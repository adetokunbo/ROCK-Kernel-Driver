/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/slab.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/printk.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_mqd_manager.h"
#include "cik_regs.h"
#include "kfd_kernel_queue.h"

/* Size of the per-pipe EOP queue */
#define CIK_HPD_EOP_BYTES_LOG2 11
#define CIK_HPD_EOP_BYTES (1U << CIK_HPD_EOP_BYTES_LOG2)

static int set_pasid_vmid_mapping(struct device_queue_manager *dqm,
					unsigned int pasid, unsigned int vmid);

static int create_compute_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd);

static int execute_queues_cpsch(struct device_queue_manager *dqm,
				bool static_queues_included);
static int unmap_queues_cpsch(struct device_queue_manager *dqm,
		enum kfd_unmap_queues_filter filter,
		uint32_t filter_param, bool reset);

static int map_queues_cpsch(struct device_queue_manager *dqm);

static int create_sdma_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd);

static void deallocate_sdma_queue(struct device_queue_manager *dqm,
				unsigned int sdma_queue_id);

static inline
enum KFD_MQD_TYPE get_mqd_type_from_queue_type(enum kfd_queue_type type)
{
	if (type == KFD_QUEUE_TYPE_SDMA)
		return KFD_MQD_TYPE_SDMA;
	return KFD_MQD_TYPE_CP;
}

unsigned int get_first_pipe(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm || !dqm->dev);
	return dqm->dev->shared_resources.first_compute_pipe;
}

unsigned int get_pipes_num(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm || !dqm->dev);
	return dqm->dev->shared_resources.compute_pipe_count;
}

static inline unsigned int get_pipes_num_cpsch(void)
{
	return PIPE_PER_ME_CP_SCHEDULING;
}

void program_sh_mem_settings(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	return dqm->dev->kfd2kgd->program_sh_mem_settings(
						dqm->dev->kgd, qpd->vmid,
						qpd->sh_mem_config,
						qpd->sh_mem_ape1_base,
						qpd->sh_mem_ape1_limit,
						qpd->sh_mem_bases);
}

static int init_doorbell_bitmap(struct device_queue_manager *dqm)
{
	unsigned int i;

	if (!KFD_IS_SOC15(dqm->dev->device_info->asic_family))
		return 0;

	dqm->doorbell_bitmap =
		kzalloc(DIV_ROUND_UP(KFD_MAX_NUM_OF_QUEUES_PER_PROCESS,
				     BITS_PER_BYTE), GFP_KERNEL);
	if (dqm->doorbell_bitmap == NULL)
		return -ENOMEM;

	/* Mask out any reserved doorbells */
	for (i = 0; i < KFD_MAX_NUM_OF_QUEUES_PER_PROCESS; i++)
		if ((dqm->dev->shared_resources.reserved_doorbell_mask & i) ==
		    dqm->dev->shared_resources.reserved_doorbell_val) {
			set_bit(i, dqm->doorbell_bitmap);
			pr_debug("reserved doorbell 0x%03x\n", i);
		}

	return 0;
}

static void uninit_doorbell_bitmap(struct device_queue_manager *dqm)
{
	kfree(dqm->doorbell_bitmap);
}

static int allocate_doorbell(struct device_queue_manager *dqm, struct queue *q)
{
	if (!KFD_IS_SOC15(dqm->dev->device_info->asic_family)) {
		/* On pre-SOC15 chips we need to use the queue ID to
		 * preserve the user mode ABI.
		 */
		q->doorbell_id = q->properties.queue_id;
	} else if (q->properties.type == KFD_QUEUE_TYPE_SDMA) {
		/* For SDMA queues on SOC15, use static doorbell
		 * assignments based on the engine and queue.
		 */
		q->doorbell_id = dqm->dev->shared_resources.sdma_doorbell
			[q->properties.sdma_engine_id]
			[q->properties.sdma_queue_id];
	} else {
		/* For CP queues on SOC15 reserve a free doorbell ID */
		unsigned int found;

		found = find_first_zero_bit(dqm->doorbell_bitmap,
					    KFD_MAX_NUM_OF_QUEUES_PER_PROCESS);
		if (found >= KFD_MAX_NUM_OF_QUEUES_PER_PROCESS) {
			pr_debug("amdkfd: No doorbells available");
			return -EBUSY;
		}
		set_bit(found, dqm->doorbell_bitmap);
		q->doorbell_id = found;
	}

	q->properties.doorbell_off =
		kfd_doorbell_id_to_offset(dqm->dev, q->process,
					  q->doorbell_id);

	return 0;
}

static void deallocate_doorbell(struct device_queue_manager *dqm,
				struct queue *q)
{
	unsigned int old;

	if (!KFD_IS_SOC15(dqm->dev->device_info->asic_family) ||
	    q->properties.type == KFD_QUEUE_TYPE_SDMA)
		return;

	old = test_and_clear_bit(q->doorbell_id, dqm->doorbell_bitmap);
	WARN_ON(!old);
}

static int allocate_vmid(struct device_queue_manager *dqm,
			struct qcm_process_device *qpd,
			struct queue *q)
{
	int bit, allocated_vmid;

	if (dqm->vmid_bitmap == 0)
		return -ENOMEM;

	bit = find_first_bit((unsigned long *)&dqm->vmid_bitmap,
				dqm->dev->vm_info.vmid_num_kfd);
	clear_bit(bit, (unsigned long *)&dqm->vmid_bitmap);

	allocated_vmid = bit + dqm->dev->vm_info.first_vmid_kfd;
	pr_debug("kfd: vmid allocation %d\n", allocated_vmid);
	qpd->vmid = allocated_vmid;
	q->properties.vmid = allocated_vmid;

	set_pasid_vmid_mapping(dqm, q->process->pasid, q->properties.vmid);
	program_sh_mem_settings(dqm, qpd);

	/* qpd->page_table_base is set earlier when register_process()
	 * is called, i.e. when the first queue is created.
	 */
	dqm->dev->kfd2kgd->set_vm_context_page_table_base(dqm->dev->kgd,
			qpd->vmid,
			qpd->page_table_base);
	/*invalidate the VM context after pasid and vmid mapping is set up*/
	kfd_flush_tlb(dqm->dev, qpd->pqm->process->pasid);

	return 0;
}

static int flush_texture_cache_nocpsch(struct kfd_dev *kdev,
				struct qcm_process_device *qpd)
{
	uint32_t len;

	if (!qpd->ib_kaddr)
		return -ENOMEM;

	len = qpd->dqm->packets.pmf->release_mem(qpd->ib_base,
						 (uint32_t *)qpd->ib_kaddr);

	return kdev->kfd2kgd->submit_ib(kdev->kgd, KGD_ENGINE_MEC1, qpd->vmid,
				qpd->ib_base, (uint32_t *)qpd->ib_kaddr, len);
}

static void deallocate_vmid(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int bit = qpd->vmid - dqm->dev->vm_info.first_vmid_kfd;

	/* On GFX v7, CP doesn't flush TC at dequeue */
	if (q->device->device_info->asic_family == CHIP_HAWAII)
		if (flush_texture_cache_nocpsch(q->device, qpd))
			pr_err("kfd: Failed to flush TC\n");

	/* Release the vmid mapping */
	set_pasid_vmid_mapping(dqm, 0, qpd->vmid);

	set_bit(bit, (unsigned long *)&dqm->vmid_bitmap);
	qpd->vmid = 0;
	q->properties.vmid = 0;
}

static int create_queue_nocpsch(struct device_queue_manager *dqm,
				struct queue *q,
				struct qcm_process_device *qpd,
				int *allocated_vmid)
{
	int retval;

	BUG_ON(!dqm || !q || !qpd || !allocated_vmid);

	pr_debug("kfd: In func %s\n", __func__);
	print_queue(q);

	mutex_lock(&dqm->lock);

	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("amdkfd: Can't create new usermode queue because %d queues were already created\n",
				dqm->total_queue_count);
		mutex_unlock(&dqm->lock);
		return -EPERM;
	}

	if (list_empty(&qpd->queues_list)) {
		retval = allocate_vmid(dqm, qpd, q);
		if (retval != 0) {
			mutex_unlock(&dqm->lock);
			return retval;
		}
	}
	*allocated_vmid = qpd->vmid;
	q->properties.vmid = qpd->vmid;
	/*
	 * Eviction state logic: we only mark active queues as evicted
	 * to avoid the overhead of restoring inactive queues later
	 */
	if (qpd->evicted)
		q->properties.is_evicted = (q->properties.queue_size > 0 &&
					    q->properties.queue_percent > 0 &&
					    q->properties.queue_address != 0);

	q->properties.tba_addr = qpd->tba_addr;
	q->properties.tma_addr = qpd->tma_addr;

	if (q->properties.type == KFD_QUEUE_TYPE_COMPUTE)
		retval = create_compute_queue_nocpsch(dqm, q, qpd);
	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		retval = create_sdma_queue_nocpsch(dqm, q, qpd);

	if (retval != 0) {
		if (list_empty(&qpd->queues_list)) {
			deallocate_vmid(dqm, qpd, q);
			*allocated_vmid = 0;
		}
		mutex_unlock(&dqm->lock);
		return retval;
	}

	list_add(&q->list, &qpd->queues_list);
	if (q->properties.is_active)
		dqm->queue_count++;

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		dqm->sdma_queue_count++;

	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	mutex_unlock(&dqm->lock);
	return 0;
}

static int allocate_hqd(struct device_queue_manager *dqm, struct queue *q)
{
	bool set;
	int pipe, bit, i;

	set = false;

	for (pipe = dqm->next_pipe_to_allocate, i = 0; i < get_pipes_num(dqm);
			pipe = ((pipe + 1) % get_pipes_num(dqm)), ++i) {
		if (dqm->allocated_queues[pipe] != 0) {
			bit = find_first_bit(
				(unsigned long *)&dqm->allocated_queues[pipe],
				QUEUES_PER_PIPE);

			clear_bit(bit,
				(unsigned long *)&dqm->allocated_queues[pipe]);
			q->pipe = pipe;
			q->queue = bit;
			set = true;
			break;
		}
	}

	if (!set)
		return -EBUSY;

	pr_debug("kfd: DQM %s hqd slot - pipe (%d) queue(%d)\n",
				__func__, q->pipe, q->queue);
	/* horizontal hqd allocation */
	dqm->next_pipe_to_allocate = (pipe + 1) % get_pipes_num(dqm);

	return 0;
}

static inline void deallocate_hqd(struct device_queue_manager *dqm,
				struct queue *q)
{
	set_bit(q->queue, (unsigned long *)&dqm->allocated_queues[q->pipe]);
}

static int create_compute_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd)
{
	int retval;
	struct mqd_manager *mqd;

	BUG_ON(!dqm || !q || !qpd);

	mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_COMPUTE);
	if (mqd == NULL)
		return -ENOMEM;

	retval = allocate_hqd(dqm, q);
	if (retval != 0)
		return retval;

	retval = allocate_doorbell(dqm, q);
	if (retval)
		goto out_deallocate_hqd;

	retval = mqd->init_mqd(mqd, &q->mqd, &q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties);
	if (retval != 0)
		goto out_deallocate_doorbell;

	pr_debug("kfd: loading mqd to hqd on pipe (%d) queue (%d)\n",
			q->pipe,
			q->queue);

	dqm->dev->kfd2kgd->alloc_memory_of_scratch(
			dqm->dev->kgd, qpd->sh_hidden_private_base, qpd->vmid);

	if (!q->properties.is_active)
		return 0;

	retval = mqd->load_mqd(mqd, q->mqd, q->pipe, q->queue, &q->properties,
			       q->process->mm);
	if (retval != 0)
		goto out_uninit_mqd;

	return 0;

out_uninit_mqd:
	mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);
out_deallocate_doorbell:
	deallocate_doorbell(dqm, q);
out_deallocate_hqd:
	deallocate_hqd(dqm, q);

	return retval;
}

/* Access to DQM has to be locked before calling destroy_queue_nocpsch_locked
 * to avoid asynchronized access
 */
static int destroy_queue_nocpsch_locked(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int retval = 0;
	struct mqd_manager *mqd;

	WARN_ON(!dqm || !q || !q->mqd || !qpd);

	mqd = dqm->ops.get_mqd_manager(dqm,
		get_mqd_type_from_queue_type(q->properties.type));
	if (!mqd) {
		retval = -ENOMEM;
		goto out;
	}

	deallocate_doorbell(dqm, q);

	if (q->properties.type == KFD_QUEUE_TYPE_COMPUTE)
		deallocate_hqd(dqm, q);
	else if (q->properties.type == KFD_QUEUE_TYPE_SDMA) {
		dqm->sdma_queue_count--;
		deallocate_sdma_queue(dqm, q->sdma_id);
	} else {
		pr_debug("q->properties.type is invalid (%d)\n",
			q->properties.type);
		retval = -EINVAL;
	}
	dqm->total_queue_count--;

	retval = mqd->destroy_mqd(mqd, q->mqd,
				KFD_PREEMPT_TYPE_WAVEFRONT_RESET,
				KFD_HIQ_TIMEOUT,
				q->pipe, q->queue);
	if (retval != 0)
		goto out;

	mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);

	list_del(&q->list);
	if (list_empty(&qpd->queues_list))
		deallocate_vmid(dqm, qpd, q);
	if (q->properties.is_active)
		dqm->queue_count--;

out:
	return retval;
}

static int destroy_queue_nocpsch(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int retval;

	BUG_ON(!dqm || !q || !q->mqd || !qpd);

	mutex_lock(&dqm->lock);
	retval = destroy_queue_nocpsch_locked(dqm, qpd, q);
	mutex_unlock(&dqm->lock);

	return retval;
}

static  bool is_queue_nocpsch(struct device_queue_manager *dqm, struct queue *q)
{
	return (dqm->sched_policy == KFD_SCHED_POLICY_NO_HWS &&
			(q->properties.type == KFD_QUEUE_TYPE_COMPUTE ||
			q->properties.type == KFD_QUEUE_TYPE_SDMA));
}

static int update_queue(struct device_queue_manager *dqm, struct queue *q)
{
	int retval;
	struct mqd_manager *mqd;
	struct kfd_process_device *pdd;

	bool prev_active = false;

	BUG_ON(!dqm || !q || !q->mqd);

	mutex_lock(&dqm->lock);

	pdd = kfd_get_process_device_data(q->device, q->process);
	if (!pdd) {
		retval = -ENODEV;
		goto out_unlock;
	}
	mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
	if (mqd == NULL) {
		retval = -ENOMEM;
		goto out_unlock;
	}
	/*
	 * Eviction state logic: we only mark active queues as evicted
	 * to avoid the overhead of restoring inactive queues later
	 */
	if (pdd->qpd.evicted > 0)
		q->properties.is_evicted = (q->properties.queue_size > 0 &&
					    q->properties.queue_percent > 0 &&
					    q->properties.queue_address != 0);

	/* save previous activity state for counters */
	prev_active = q->properties.is_active;

	/* HWS mode, unmap first to own mqd */
	if (dqm->sched_policy != KFD_SCHED_POLICY_NO_HWS) {
		retval = unmap_queues_cpsch(dqm,
				KFD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0,
				false);
		if (retval != 0) {
			pr_err("unmap queue failed");
			goto out_unlock;
		}
	} else if (is_queue_nocpsch(dqm, q) && prev_active == true) {
		retval = mqd->destroy_mqd(mqd, q->mqd,
				KFD_PREEMPT_TYPE_WAVEFRONT_DRAIN,
				KFD_UNMAP_LATENCY_MS, q->pipe, q->queue);
		if (retval != 0) {
			pr_err("destroy mqd failed");
			goto out_unlock;
		}
	}

	retval = mqd->update_mqd(mqd, q->mqd, &q->properties);

	if (is_queue_nocpsch(dqm, q)) {
		if (q->properties.is_active)
			retval = mqd->load_mqd(mqd, q->mqd, q->pipe, q->queue,
					       &q->properties, q->process->mm);
	}
	/*
	 * check active state vs. the previous state
	 * and modify counter accordingly
	 */
	if ((q->properties.is_active) && (!prev_active))
		dqm->queue_count++;
	else if ((!q->properties.is_active) && (prev_active))
		dqm->queue_count--;

	if (dqm->sched_policy != KFD_SCHED_POLICY_NO_HWS)
		retval = map_queues_cpsch(dqm);

out_unlock:
	mutex_unlock(&dqm->lock);

	return retval;
}

static struct mqd_manager *get_mqd_manager_nocpsch(
		struct device_queue_manager *dqm, enum KFD_MQD_TYPE type)
{
	struct mqd_manager *mqd;

	BUG_ON(!dqm || type >= KFD_MQD_TYPE_MAX);

	pr_debug("kfd: In func %s mqd type %d\n", __func__, type);

	mqd = dqm->mqds[type];
	if (!mqd) {
		mqd = mqd_manager_init(type, dqm->dev);
		if (mqd == NULL)
			pr_err("kfd: mqd manager is NULL");
		dqm->mqds[type] = mqd;
	}

	return mqd;
}

int process_evict_queues(struct device_queue_manager *dqm,
		struct qcm_process_device *qpd)
{
	struct queue *q, *next;
	struct mqd_manager *mqd;
	struct kfd_process_device *pdd;
	int retval = 0;

	BUG_ON(!dqm || !qpd);

	mutex_lock(&dqm->lock);
	if (qpd->evicted++ > 0) { /* already evicted, do nothing */
		mutex_unlock(&dqm->lock);
		return 0;
	}

	pdd = qpd_to_pdd(qpd);
	pr_info_ratelimited("Evicting PASID %u queues\n",
			    pdd->process->pasid);

	/* unactivate all active queues on the qpd */
	list_for_each_entry_safe(q, next, &qpd->queues_list, list) {
		mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
		if (!mqd) { /* should not be here */
			BUG();
			continue;
		}
		/* if the queue is not active anyway, it is not evicted */
		if (q->properties.is_active == true) {
			q->properties.is_evicted = true;
			q->properties.is_active = false;
		}

		if (is_queue_nocpsch(dqm, q) &&
		    q->properties.is_evicted)
			retval = mqd->destroy_mqd(mqd, q->mqd,
				KFD_PREEMPT_TYPE_WAVEFRONT_DRAIN,
				KFD_UNMAP_LATENCY_MS, q->pipe, q->queue);
		if (q->properties.is_evicted)
			dqm->queue_count--;
	}
	if (dqm->sched_policy != KFD_SCHED_POLICY_NO_HWS)
		retval = execute_queues_cpsch(dqm, qpd->is_debug);

	mutex_unlock(&dqm->lock);
	return retval;

}

int process_restore_queues(struct device_queue_manager *dqm,
		struct qcm_process_device *qpd)
{
	struct queue *q, *next;
	struct mqd_manager *mqd;
	int retval = 0;
	struct kfd_process_device *pdd;
	uint32_t pd_base;

	BUG_ON(!dqm || !qpd);

	pdd = qpd_to_pdd(qpd);
	/* Retrieve PD base */
	pd_base = dqm->dev->kfd2kgd->get_process_page_dir(pdd->vm);

	mutex_lock(&dqm->lock);
	if (qpd->evicted == 0) /* already restored, do nothing */
		goto out_unlock;

	if (qpd->evicted > 1) { /* ref count still > 0, decrement & quit */
		qpd->evicted--;
		goto out_unlock;
	}

	pr_info_ratelimited("Restoring PASID %u queues\n",
			    pdd->process->pasid);

	/* Update PD Base in QPD */
	qpd->page_table_base = pd_base;
	pr_debug("Updated PD address to 0x%08x in %s\n", pd_base, __func__);

	if (dqm->sched_policy == KFD_SCHED_POLICY_NO_HWS &&
	    !list_empty(&qpd->queues_list)) {
		dqm->dev->kfd2kgd->set_vm_context_page_table_base(
				dqm->dev->kgd,
				qpd->vmid,
				qpd->page_table_base);

		kfd_flush_tlb(dqm->dev, pdd->process->pasid);
	}

	/* activate all active queues on the qpd */
	list_for_each_entry_safe(q, next, &qpd->queues_list, list) {
		mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
		if (!mqd) { /* should not be here */
			BUG();
			continue;
		}
		if (q->properties.is_evicted) {
			q->properties.is_evicted = false;
			q->properties.is_active = true;

			if (dqm->sched_policy == KFD_SCHED_POLICY_NO_HWS &&
			    (q->properties.type == KFD_QUEUE_TYPE_COMPUTE ||
			     q->properties.type == KFD_QUEUE_TYPE_SDMA))
				retval = mqd->load_mqd(mqd, q->mqd, q->pipe,
						       q->queue, &q->properties,
						       q->process->mm);
			dqm->queue_count++;
		}
	}
	if (dqm->sched_policy != KFD_SCHED_POLICY_NO_HWS)
		retval = execute_queues_cpsch(dqm, false);

	if (retval == 0)
		qpd->evicted = 0;

out_unlock:
	mutex_unlock(&dqm->lock);

	return retval;
}

static int register_process(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	struct device_process_node *n;
	int retval;
	struct kfd_process_device *pdd;
	uint32_t pd_base;

	BUG_ON(!dqm || !qpd);

	pr_debug("In func %s\n", __func__);

	n = kzalloc(sizeof(struct device_process_node), GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	n->qpd = qpd;

	pdd = qpd_to_pdd(qpd);
	/* Retrieve PD base */
	pd_base = dqm->dev->kfd2kgd->get_process_page_dir(pdd->vm);

	mutex_lock(&dqm->lock);
	list_add(&n->list, &dqm->queues);

	/* Update PD Base in QPD */
	qpd->page_table_base = pd_base;
	pr_debug("Updated PD address to 0x%08x in %s\n", pd_base, __func__);

	retval = dqm->asic_ops.update_qpd(dqm, qpd);

	dqm->processes_count++;

	mutex_unlock(&dqm->lock);

	return retval;
}

static int unregister_process(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	int retval;
	struct device_process_node *cur, *next;

	BUG_ON(!dqm || !qpd);

	pr_debug("In func %s\n", __func__);

	pr_debug("qpd->queues_list is %s\n",
			list_empty(&qpd->queues_list) ? "empty" : "not empty");

	retval = 0;
	mutex_lock(&dqm->lock);

	list_for_each_entry_safe(cur, next, &dqm->queues, list) {
		if (qpd == cur->qpd) {
			list_del(&cur->list);
			kfree(cur);
			dqm->processes_count--;
			goto out;
		}
	}
	/* qpd not found in dqm list */
	retval = 1;
out:
	mutex_unlock(&dqm->lock);
	return retval;
}

static int
set_pasid_vmid_mapping(struct device_queue_manager *dqm, unsigned int pasid,
			unsigned int vmid)
{
	uint32_t pasid_mapping;

	pasid_mapping = (pasid == 0) ? 0 :
		(uint32_t)pasid |
		ATC_VMID_PASID_MAPPING_VALID;

	return dqm->dev->kfd2kgd->set_pasid_vmid_mapping(
						dqm->dev->kgd, pasid_mapping,
						vmid);
}

int init_pipelines(struct device_queue_manager *dqm,
			unsigned int pipes_num, unsigned int first_pipe)
{
	void *hpdptr;
	struct mqd_manager *mqd;
	unsigned int i, err, inx;
	uint64_t pipe_hpd_addr;

	BUG_ON(!dqm || !dqm->dev);

	pr_debug("kfd: In func %s\n", __func__);

	/*
	 * Allocate memory for the HPDs. This is hardware-owned per-pipe data.
	 * The driver never accesses this memory after zeroing it.
	 * It doesn't even have to be saved/restored on suspend/resume
	 * because it contains no data when there are no active queues.
	 */

	err = kfd_gtt_sa_allocate(dqm->dev, CIK_HPD_EOP_BYTES * pipes_num,
					&dqm->pipeline_mem);

	if (err) {
		pr_err("kfd: error allocate vidmem num pipes: %d\n",
			pipes_num);
		return -ENOMEM;
	}

	hpdptr = dqm->pipeline_mem->cpu_ptr;
	dqm->pipelines_addr = dqm->pipeline_mem->gpu_addr;

	memset(hpdptr, 0, CIK_HPD_EOP_BYTES * pipes_num);

	mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_COMPUTE);
	if (mqd == NULL) {
		kfd_gtt_sa_free(dqm->dev, dqm->pipeline_mem);
		return -ENOMEM;
	}

	for (i = 0; i < pipes_num; i++) {
		inx = i + first_pipe;
		/*
		 * HPD buffer on GTT is allocated by amdkfd, no need to waste
		 * space in GTT for pipelines we don't initialize
		 */
		pipe_hpd_addr = dqm->pipelines_addr + i * CIK_HPD_EOP_BYTES;
		pr_debug("kfd: pipeline address %llX\n", pipe_hpd_addr);
		/* = log2(bytes/4)-1 */
		dqm->dev->kfd2kgd->init_pipeline(dqm->dev->kgd, inx,
				CIK_HPD_EOP_BYTES_LOG2 - 3, pipe_hpd_addr);
	}

	return 0;
}

static void init_interrupts(struct device_queue_manager *dqm)
{
	unsigned int i;

	BUG_ON(dqm == NULL);

	for (i = 0 ; i < get_pipes_num(dqm) ; i++)
		dqm->dev->kfd2kgd->init_interrupts(dqm->dev->kgd, i);
}
static int init_scheduler(struct device_queue_manager *dqm)
{
	int retval;

	BUG_ON(!dqm);

	pr_debug("kfd: In %s\n", __func__);

	retval = init_pipelines(dqm, get_pipes_num(dqm), get_first_pipe(dqm));
	return retval;
}

static int initialize_nocpsch(struct device_queue_manager *dqm)
{
	int i, ret;

	BUG_ON(!dqm);

	pr_debug("kfd: In func %s num of pipes: %d\n",
			__func__, get_pipes_num(dqm));

	dqm->allocated_queues = kcalloc(get_pipes_num(dqm),
					sizeof(unsigned int), GFP_KERNEL);
	if (!dqm->allocated_queues)
		return -ENOMEM;

	ret = init_doorbell_bitmap(dqm);
	if (ret) {
		kfree(dqm->allocated_queues);
		return ret;
	}

	mutex_init(&dqm->lock);
	INIT_LIST_HEAD(&dqm->queues);
	dqm->queue_count = dqm->next_pipe_to_allocate = 0;
	dqm->sdma_queue_count = 0;

	for (i = 0; i < get_pipes_num(dqm); i++)
		dqm->allocated_queues[i] = (1 << QUEUES_PER_PIPE) - 1;

	dqm->vmid_bitmap = (1 << dqm->dev->vm_info.vmid_num_kfd) - 1;
	dqm->sdma_bitmap = (1 << CIK_SDMA_QUEUES) - 1;

	init_scheduler(dqm);
	return 0;
}

static void uninitialize_nocpsch(struct device_queue_manager *dqm)
{
	int i;

	BUG_ON(!dqm);

	BUG_ON(dqm->queue_count > 0 || dqm->processes_count > 0);

	uninit_doorbell_bitmap(dqm);
	kfree(dqm->allocated_queues);
	for (i = 0 ; i < KFD_MQD_TYPE_MAX ; i++)
		kfree(dqm->mqds[i]);
	mutex_destroy(&dqm->lock);
	kfd_gtt_sa_free(dqm->dev, dqm->pipeline_mem);
}

static int start_nocpsch(struct device_queue_manager *dqm)
{
	init_interrupts(dqm);
	return pm_init(&dqm->packets, dqm, dqm->dev->mec_fw_version);
}

static int stop_nocpsch(struct device_queue_manager *dqm)
{
	pm_uninit(&dqm->packets);
	return 0;
}

static int allocate_sdma_queue(struct device_queue_manager *dqm,
				unsigned int *sdma_queue_id)
{
	int bit;

	if (dqm->sdma_bitmap == 0)
		return -ENOMEM;

	bit = find_first_bit((unsigned long *)&dqm->sdma_bitmap,
				CIK_SDMA_QUEUES);

	clear_bit(bit, (unsigned long *)&dqm->sdma_bitmap);
	*sdma_queue_id = bit;

	return 0;
}

static void deallocate_sdma_queue(struct device_queue_manager *dqm,
				unsigned int sdma_queue_id)
{
	if (sdma_queue_id >= CIK_SDMA_QUEUES)
		return;
	set_bit(sdma_queue_id, (unsigned long *)&dqm->sdma_bitmap);
}

static int create_sdma_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd)
{
	struct mqd_manager *mqd;
	int retval;

	mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_SDMA);
	if (!mqd)
		return -ENOMEM;

	retval = allocate_sdma_queue(dqm, &q->sdma_id);
	if (retval != 0)
		return retval;

	q->properties.sdma_queue_id = q->sdma_id / CIK_SDMA_QUEUES_PER_ENGINE;
	q->properties.sdma_engine_id = q->sdma_id % CIK_SDMA_QUEUES_PER_ENGINE;

	retval = allocate_doorbell(dqm, q);
	if (retval)
		goto out_deallocate_sdma_queue;

	pr_debug("kfd: sdma id is:    %d\n", q->sdma_id);
	pr_debug("     sdma queue id: %d\n", q->properties.sdma_queue_id);
	pr_debug("     sdma engine id: %d\n", q->properties.sdma_engine_id);

	dqm->asic_ops.init_sdma_vm(dqm, q, qpd);
	retval = mqd->init_mqd(mqd, &q->mqd, &q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties);
	if (retval != 0)
		goto out_deallocate_doorbell;

	retval = mqd->load_mqd(mqd, q->mqd, 0, 0, &q->properties, NULL);
	if (retval != 0)
		goto out_uninit_mqd;

	return 0;

out_uninit_mqd:
	mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);
out_deallocate_doorbell:
	deallocate_doorbell(dqm, q);
out_deallocate_sdma_queue:
	deallocate_sdma_queue(dqm, q->sdma_id);

	return retval;
}

/*
 * Device Queue Manager implementation for cp scheduler
 */

static int set_sched_resources(struct device_queue_manager *dqm)
{
	struct scheduling_resources res;
	unsigned int queue_num, queue_mask;

	BUG_ON(!dqm);

	pr_debug("kfd: In func %s\n", __func__);

	queue_num = get_pipes_num_cpsch() * QUEUES_PER_PIPE;
	queue_mask = (1 << queue_num) - 1;
	res.vmid_mask = dqm->dev->shared_resources.compute_vmid_bitmap;
	res.queue_mask = queue_mask << (get_first_pipe(dqm) * QUEUES_PER_PIPE);
	res.gws_mask = res.oac_mask = res.gds_heap_base =
						res.gds_heap_size = 0;

	pr_debug("kfd: scheduling resources:\n"
			"      vmid mask: 0x%8X\n"
			"      queue mask: 0x%8llX\n",
			res.vmid_mask, res.queue_mask);

	return pm_send_set_resources(&dqm->packets, &res);
}

static int initialize_cpsch(struct device_queue_manager *dqm)
{
	int retval;

	BUG_ON(!dqm);

	pr_debug("kfd: In func %s num of pipes: %d\n",
			__func__, get_pipes_num_cpsch());

	retval = init_doorbell_bitmap(dqm);
	if (retval)
		return retval;

	mutex_init(&dqm->lock);
	INIT_LIST_HEAD(&dqm->queues);
	dqm->queue_count = dqm->processes_count = 0;
	dqm->sdma_queue_count = 0;
	dqm->active_runlist = false;
	dqm->sdma_bitmap = (1 << CIK_SDMA_QUEUES) - 1;
	retval = dqm->asic_ops.init_cpsch(dqm);
	if (retval != 0)
		goto fail_init_pipelines;

	return 0;

fail_init_pipelines:
	uninit_doorbell_bitmap(dqm);
	mutex_destroy(&dqm->lock);
	return retval;
}

static int start_cpsch(struct device_queue_manager *dqm)
{
	int retval;

	BUG_ON(!dqm);

	retval = 0;

	retval = pm_init(&dqm->packets, dqm, dqm->dev->mec_fw_version);
	if (retval != 0)
		goto fail_packet_manager_init;

	retval = set_sched_resources(dqm);
	if (retval != 0)
		goto fail_set_sched_resources;

	pr_debug("kfd: allocating fence memory\n");

	/* allocate fence memory on the gart */
	retval = kfd_gtt_sa_allocate(dqm->dev, sizeof(*dqm->fence_addr),
					&dqm->fence_mem);

	if (retval != 0)
		goto fail_allocate_vidmem;

	dqm->fence_addr = dqm->fence_mem->cpu_ptr;
	dqm->fence_gpu_addr = dqm->fence_mem->gpu_addr;

	init_interrupts(dqm);

	mutex_lock(&dqm->lock);
	execute_queues_cpsch(dqm, false);
	mutex_unlock(&dqm->lock);

	return 0;
fail_allocate_vidmem:
fail_set_sched_resources:
	pm_uninit(&dqm->packets);
fail_packet_manager_init:
	return retval;
}

static int stop_cpsch(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm);

	mutex_lock(&dqm->lock);

	unmap_queues_cpsch(dqm, KFD_UNMAP_QUEUES_FILTER_ALL_QUEUES, 0, false);

	mutex_unlock(&dqm->lock);

	kfd_gtt_sa_free(dqm->dev, dqm->fence_mem);
	pm_uninit(&dqm->packets);

	return 0;
}

static int create_kernel_queue_cpsch(struct device_queue_manager *dqm,
					struct kernel_queue *kq,
					struct qcm_process_device *qpd)
{
	BUG_ON(!dqm || !kq || !qpd);

	pr_debug("kfd: In func %s\n", __func__);

	mutex_lock(&dqm->lock);
	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("amdkfd: Can't create new kernel queue because %d queues were already created\n",
				dqm->total_queue_count);
		mutex_unlock(&dqm->lock);
		return -EPERM;
	}

	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	list_add(&kq->list, &qpd->priv_queue_list);
	dqm->queue_count++;
	qpd->is_debug = true;
	execute_queues_cpsch(dqm, false);
	mutex_unlock(&dqm->lock);

	return 0;
}

static void destroy_kernel_queue_cpsch(struct device_queue_manager *dqm,
					struct kernel_queue *kq,
					struct qcm_process_device *qpd)
{
	BUG_ON(!dqm || !kq);

	pr_debug("kfd: In %s\n", __func__);

	mutex_lock(&dqm->lock);
	/* here we actually preempt the DIQ */
	list_del(&kq->list);
	dqm->queue_count--;
	qpd->is_debug = false;
	execute_queues_cpsch(dqm, true);
	/*
	 * Unconditionally decrement this counter, regardless of the queue's
	 * type.
	 */
	dqm->total_queue_count--;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);
	mutex_unlock(&dqm->lock);
}

static int create_queue_cpsch(struct device_queue_manager *dqm, struct queue *q,
			struct qcm_process_device *qpd, int *allocate_vmid)
{
	int retval;
	struct mqd_manager *mqd;

	BUG_ON(!dqm || !q || !qpd);

	retval = 0;

	if (allocate_vmid)
		*allocate_vmid = 0;

	mutex_lock(&dqm->lock);

	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("amdkfd: Can't create new usermode queue because %d queues were already created\n",
				dqm->total_queue_count);
		retval = -EPERM;
		goto out_unlock;
	}

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA) {
		retval = allocate_sdma_queue(dqm, &q->sdma_id);
		if (retval != 0)
			goto out_unlock;
		q->properties.sdma_queue_id =
			q->sdma_id / CIK_SDMA_QUEUES_PER_ENGINE;
		q->properties.sdma_engine_id =
			q->sdma_id % CIK_SDMA_QUEUES_PER_ENGINE;
	}

	retval = allocate_doorbell(dqm, q);
	if (retval)
		goto out_deallocate_sdma_queue;

	mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));

	if (mqd == NULL) {
		retval = -ENOMEM;
		goto out_deallocate_doorbell;
	}
	/*
	 * Eviction state logic: we only mark active queues as evicted
	 * to avoid the overhead of restoring inactive queues later
	 */
	if (qpd->evicted)
		q->properties.is_evicted = (q->properties.queue_size > 0 &&
					    q->properties.queue_percent > 0 &&
					    q->properties.queue_address != 0);

	dqm->asic_ops.init_sdma_vm(dqm, q, qpd);

	q->properties.tba_addr = qpd->tba_addr;
	q->properties.tma_addr = qpd->tma_addr;
	retval = mqd->init_mqd(mqd, &q->mqd, &q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties);
	if (retval != 0)
		goto out_deallocate_doorbell;

	list_add(&q->list, &qpd->queues_list);
	if (q->properties.is_active) {
		dqm->queue_count++;
		retval = execute_queues_cpsch(dqm, false);
	}

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
			dqm->sdma_queue_count++;
	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;

	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	mutex_unlock(&dqm->lock);
	return retval;

out_deallocate_doorbell:
	deallocate_doorbell(dqm, q);
out_deallocate_sdma_queue:
	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		deallocate_sdma_queue(dqm, q->sdma_id);
out_unlock:
	mutex_unlock(&dqm->lock);

	return retval;
}

int amdkfd_fence_wait_timeout(unsigned int *fence_addr,
				unsigned int fence_value,
				unsigned long timeout_ms)
{
	unsigned long end_jiffies;

	BUG_ON(!fence_addr);

	end_jiffies = (timeout_ms * HZ / 1000) + jiffies;

	while (*fence_addr != fence_value) {
		if (time_after(jiffies, end_jiffies)) {
			pr_err("kfd: qcm fence wait loop timeout expired\n");
			return -ETIME;
		}
		schedule();
	}

	return 0;
}

static int unmap_sdma_queues(struct device_queue_manager *dqm,
				unsigned int sdma_engine)
{
	return pm_send_unmap_queue(&dqm->packets, KFD_QUEUE_TYPE_SDMA,
			KFD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES, 0, false,
			sdma_engine);
}

/* dqm->lock mutex has to be locked before calling this function */
static int map_queues_cpsch(struct device_queue_manager *dqm)
{
	int retval;

	if (dqm->queue_count <= 0 || dqm->processes_count <= 0) {
		retval = 0;
		return retval;
	}

	if (dqm->active_runlist) {
		retval = 0;
		return retval;
	}

	retval = pm_send_runlist(&dqm->packets, &dqm->queues);
	if (retval != 0) {
		pr_err("failed to execute runlist");
		return retval;
	}
	dqm->active_runlist = true;

	return retval;
}

/* dqm->lock mutex has to be locked before calling this function */
static int unmap_queues_cpsch(struct device_queue_manager *dqm,
		enum kfd_unmap_queues_filter filter,
		uint32_t filter_param, bool reset)
{
	int retval;

	BUG_ON(!dqm);

	retval = 0;

	if (!dqm->active_runlist)
		return retval;

	pr_debug("kfd: Before destroying queues, sdma queue count is : %u\n",
		dqm->sdma_queue_count);

	if (dqm->sdma_queue_count > 0) {
		unmap_sdma_queues(dqm, 0);
		unmap_sdma_queues(dqm, 1);
	}

	retval = pm_send_unmap_queue(&dqm->packets, KFD_QUEUE_TYPE_COMPUTE,
			filter, filter_param, reset, 0);
	if (retval != 0)
		return retval;

	*dqm->fence_addr = KFD_FENCE_INIT;
	pm_send_query_status(&dqm->packets, dqm->fence_gpu_addr,
				KFD_FENCE_COMPLETED);
	/* should be timed out */
	retval = amdkfd_fence_wait_timeout(dqm->fence_addr, KFD_FENCE_COMPLETED,
				QUEUE_PREEMPT_DEFAULT_TIMEOUT_MS);
	if (retval != 0) {
		pr_err("kfd: unmapping queues failed.");
		return retval;
	}

	pm_release_ib(&dqm->packets);
	dqm->active_runlist = false;

	return retval;
}

/* dqm->lock mutex has to be locked before calling this function */
static int execute_queues_cpsch(struct device_queue_manager *dqm,
				bool static_queues_included)
{
	int retval;
	enum kfd_unmap_queues_filter filter;

	BUG_ON(!dqm);

	pr_debug("In function %s\n", __func__);

	filter = static_queues_included ?
			KFD_UNMAP_QUEUES_FILTER_ALL_QUEUES :
			KFD_UNMAP_QUEUES_FILTER_DYNAMIC_QUEUES;

	retval = unmap_queues_cpsch(dqm, filter, 0, false);
	if (retval != 0) {
		pr_err("kfd: the cp might be in an unrecoverable state due to an unsuccessful queues preemption");
		return retval;
	}

	retval = map_queues_cpsch(dqm);

	return retval;
}

static int destroy_queue_cpsch(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int retval;
	struct mqd_manager *mqd;
	bool preempt_all_queues;

	BUG_ON(!dqm || !qpd || !q);

	preempt_all_queues = false;

	retval = 0;

	/* remove queue from list to prevent rescheduling after preemption */
	mutex_lock(&dqm->lock);

	if (qpd->is_debug) {
		/*
		 * error, currently we do not allow to destroy a queue
		 * of a currently debugged process
		 */
		retval = -EBUSY;
		goto failed_try_destroy_debugged_queue;

	}

	mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
	if (!mqd) {
		retval = -ENOMEM;
		goto failed;
	}

	deallocate_doorbell(dqm, q);

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA) {
		dqm->sdma_queue_count--;
		deallocate_sdma_queue(dqm, q->sdma_id);
	}

	list_del(&q->list);
	if (q->properties.is_active)
		dqm->queue_count--;

	retval = execute_queues_cpsch(dqm, false);

	mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);

	/*
	 * Unconditionally decrement this counter, regardless of the queue's
	 * type
	 */
	dqm->total_queue_count--;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	mutex_unlock(&dqm->lock);

	return retval;

failed:
failed_try_destroy_debugged_queue:

	mutex_unlock(&dqm->lock);
	return retval;
}

/*
 * Low bits must be 0000/FFFF as required by HW, high bits must be 0 to
 * stay in user mode.
 */
#define APE1_FIXED_BITS_MASK 0xFFFF80000000FFFFULL
/* APE1 limit is inclusive and 64K aligned. */
#define APE1_LIMIT_ALIGNMENT 0xFFFF

static bool set_cache_memory_policy(struct device_queue_manager *dqm,
				   struct qcm_process_device *qpd,
				   enum cache_policy default_policy,
				   enum cache_policy alternate_policy,
				   void __user *alternate_aperture_base,
				   uint64_t alternate_aperture_size)
{
	bool retval = true;

	pr_debug("kfd: In func %s\n", __func__);

	if (!dqm->asic_ops.set_cache_memory_policy)
		return retval;

	mutex_lock(&dqm->lock);

	if (alternate_aperture_size == 0) {
		/* base > limit disables APE1 */
		qpd->sh_mem_ape1_base = 1;
		qpd->sh_mem_ape1_limit = 0;
	} else {
		/*
		 * In FSA64, APE1_Base[63:0] = { 16{SH_MEM_APE1_BASE[31]},
		 *			SH_MEM_APE1_BASE[31:0], 0x0000 }
		 * APE1_Limit[63:0] = { 16{SH_MEM_APE1_LIMIT[31]},
		 *			SH_MEM_APE1_LIMIT[31:0], 0xFFFF }
		 * Verify that the base and size parameters can be
		 * represented in this format and convert them.
		 * Additionally restrict APE1 to user-mode addresses.
		 */

		uint64_t base = (uintptr_t)alternate_aperture_base;
		uint64_t limit = base + alternate_aperture_size - 1;

		if (limit <= base)
			goto out;

		if ((base & APE1_FIXED_BITS_MASK) != 0)
			goto out;

		if ((limit & APE1_FIXED_BITS_MASK) != APE1_LIMIT_ALIGNMENT)
			goto out;

		qpd->sh_mem_ape1_base = base >> 16;
		qpd->sh_mem_ape1_limit = limit >> 16;
	}

	retval = dqm->asic_ops.set_cache_memory_policy(
			dqm,
			qpd,
			default_policy,
			alternate_policy,
			alternate_aperture_base,
			alternate_aperture_size);

	if ((dqm->sched_policy == KFD_SCHED_POLICY_NO_HWS) && (qpd->vmid != 0))
		program_sh_mem_settings(dqm, qpd);

	pr_debug("kfd: sh_mem_config: 0x%x, ape1_base: 0x%x, ape1_limit: 0x%x\n",
		qpd->sh_mem_config, qpd->sh_mem_ape1_base,
		qpd->sh_mem_ape1_limit);

	mutex_unlock(&dqm->lock);
	return retval;

out:
	mutex_unlock(&dqm->lock);
	return false;
}

static int set_trap_handler(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				uint64_t tba_addr,
				uint64_t tma_addr)
{
	uint64_t *tma;

	if (dqm->dev->cwsr_enabled) {
		/* Jump from CWSR trap handler to user trap */
		tma = (uint64_t *)(qpd->cwsr_kaddr + dqm->dev->tma_offset);
		tma[0] = tba_addr;
		tma[1] = tma_addr;
	} else {
		qpd->tba_addr = tba_addr;
		qpd->tma_addr = tma_addr;
	}

	return 0;
}

static int process_termination_nocpsch(struct device_queue_manager *dqm,
		struct qcm_process_device *qpd)
{
	struct queue *q, *next;
	struct device_process_node *cur, *next_dpn;
	int retval = 0;

	mutex_lock(&dqm->lock);

	/* Clear all user mode queues */
	list_for_each_entry_safe(q, next, &qpd->queues_list, list) {
		retval = destroy_queue_nocpsch_locked(dqm, qpd, q);
		if (retval)
			goto out;
	}

	/* Unregister process */
	list_for_each_entry_safe(cur, next_dpn, &dqm->queues, list) {
		if (qpd == cur->qpd) {
			list_del(&cur->list);
			kfree(cur);
			dqm->processes_count--;
			break;
		}
	}

out:
	mutex_unlock(&dqm->lock);
	return retval;
}


static int process_termination_cpsch(struct device_queue_manager *dqm,
		struct qcm_process_device *qpd)
{
	int retval;
	struct queue *q, *next;
	struct kernel_queue *kq, *kq_next;
	struct mqd_manager *mqd;
	struct device_process_node *cur, *next_dpn;

	retval = 0;

	mutex_lock(&dqm->lock);

	/* Clean all kernel queues */
	list_for_each_entry_safe(kq, kq_next, &qpd->priv_queue_list, list) {
		list_del(&kq->list);
		dqm->queue_count--;
		qpd->is_debug = false;
		dqm->total_queue_count--;
	}

	/* Clear all user mode queues */
	list_for_each_entry(q, &qpd->queues_list, list) {
		if (q->properties.type == KFD_QUEUE_TYPE_SDMA) {
			dqm->sdma_queue_count--;
			deallocate_sdma_queue(dqm, q->sdma_id);
		}

		if (q->properties.is_active)
			dqm->queue_count--;

		dqm->total_queue_count--;
	}

	/* Unregister process */
	list_for_each_entry_safe(cur, next_dpn, &dqm->queues, list) {
		if (qpd == cur->qpd) {
			list_del(&cur->list);
			kfree(cur);
			dqm->processes_count--;
			break;
		}
	}

	retval = execute_queues_cpsch(dqm, true);

	/* lastly, free mqd resources */
	list_for_each_entry_safe(q, next, &qpd->queues_list, list) {
		mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
		if (!mqd) {
			mutex_unlock(&dqm->lock);
			return -ENOMEM;
		}
		list_del(&q->list);
		mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);
	}

	mutex_unlock(&dqm->lock);
	return retval;
}

struct device_queue_manager *device_queue_manager_init(struct kfd_dev *dev)
{
	struct device_queue_manager *dqm;

	BUG_ON(!dev);

	pr_debug("kfd: loading device queue manager\n");

	dqm = kzalloc(sizeof(struct device_queue_manager), GFP_KERNEL);
	if (!dqm)
		return NULL;

	switch (dev->device_info->asic_family) {
	case CHIP_HAWAII:
	case CHIP_TONGA:
		dqm->sched_policy = KFD_SCHED_POLICY_NO_HWS;
		break;
	default:
		dqm->sched_policy = sched_policy;
		break;
	}

	dqm->dev = dev;
	switch (dqm->sched_policy) {
	case KFD_SCHED_POLICY_HWS:
	case KFD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION:
		/* initialize dqm for cp scheduling */
		dqm->ops.create_queue = create_queue_cpsch;
		dqm->ops.initialize = initialize_cpsch;
		dqm->ops.start = start_cpsch;
		dqm->ops.stop = stop_cpsch;
		dqm->ops.destroy_queue = destroy_queue_cpsch;
		dqm->ops.update_queue = update_queue;
		dqm->ops.get_mqd_manager = get_mqd_manager_nocpsch;
		dqm->ops.register_process = register_process;
		dqm->ops.unregister_process = unregister_process;
		dqm->ops.uninitialize = uninitialize_nocpsch;
		dqm->ops.create_kernel_queue = create_kernel_queue_cpsch;
		dqm->ops.destroy_kernel_queue = destroy_kernel_queue_cpsch;
		dqm->ops.set_cache_memory_policy = set_cache_memory_policy;
		dqm->ops.set_trap_handler = set_trap_handler;
		dqm->ops.process_termination = process_termination_cpsch;
		break;
	case KFD_SCHED_POLICY_NO_HWS:
		/* initialize dqm for no cp scheduling */
		dqm->ops.start = start_nocpsch;
		dqm->ops.stop = stop_nocpsch;
		dqm->ops.create_queue = create_queue_nocpsch;
		dqm->ops.destroy_queue = destroy_queue_nocpsch;
		dqm->ops.update_queue = update_queue;
		dqm->ops.get_mqd_manager = get_mqd_manager_nocpsch;
		dqm->ops.register_process = register_process;
		dqm->ops.unregister_process = unregister_process;
		dqm->ops.initialize = initialize_nocpsch;
		dqm->ops.uninitialize = uninitialize_nocpsch;
		dqm->ops.set_cache_memory_policy = set_cache_memory_policy;
		dqm->ops.set_trap_handler = set_trap_handler;
		dqm->ops.process_termination = process_termination_nocpsch;
		break;
	default:
		BUG();
		break;
	}

	switch (dev->device_info->asic_family) {
	case CHIP_CARRIZO:
		device_queue_manager_init_vi(&dqm->asic_ops);
		break;

	case CHIP_KAVERI:
		device_queue_manager_init_cik(&dqm->asic_ops);
		break;

	case CHIP_HAWAII:
		device_queue_manager_init_cik_hawaii(&dqm->asic_ops);
		break;

	case CHIP_TONGA:
	case CHIP_FIJI:
	case CHIP_POLARIS10:
	case CHIP_POLARIS11:
		device_queue_manager_init_vi_tonga(&dqm->asic_ops);
		break;

	case CHIP_VEGA10:
		device_queue_manager_init_v9_vega10(&dqm->asic_ops);
		break;
	}

	if (dqm->ops.initialize(dqm) != 0) {
		kfree(dqm);
		return NULL;
	}

	return dqm;
}

void device_queue_manager_uninit(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm);

	dqm->ops.uninitialize(dqm);
	kfree(dqm);
}

int kfd_process_vm_fault(struct device_queue_manager *dqm,
				unsigned int pasid)
{
	struct kfd_process_device *pdd;
	struct kfd_process *p = kfd_lookup_process_by_pasid(pasid);
	int ret = 0;

	if (!p)
		return -EINVAL;
	pdd = kfd_get_process_device_data(dqm->dev, p);
	if (pdd)
		ret = process_evict_queues(dqm, &pdd->qpd);
	kfd_unref_process(p);

	return ret;
}

static void seq_reg_dump(struct seq_file *m,
			 uint32_t (*dump)[2], uint32_t n_regs)
{
	uint32_t i, count;

	for (i = 0, count = 0; i < n_regs; i++) {
		if (count == 0 ||
		    dump[i-1][0] + sizeof(uint32_t) != dump[i][0]) {
			seq_printf(m, "%s    %08x: %08x",
				   i ? "\n" : "",
				   dump[i][0], dump[i][1]);
			count = 7;
		} else {
			seq_printf(m, " %08x", dump[i][1]);
			count--;
		}
	}

	seq_puts(m, "\n");
}

int device_queue_manager_debugfs_hqds(struct seq_file *m, void *data)
{
	struct device_queue_manager *dqm = data;
	uint32_t (*dump)[2], n_regs;
	int pipe, queue;
	int r = 0;

	for (pipe = 0; pipe < get_pipes_num(dqm); pipe++) {
		for (queue = 0; queue < QUEUES_PER_PIPE; queue++) {
			r = dqm->dev->kfd2kgd->hqd_dump(
				dqm->dev->kgd, pipe, queue, &dump, &n_regs);
			if (r != 0)
				break;

			seq_printf(m, "  CP Pipe %d, Queue %d\n",
				  pipe, queue);
			seq_reg_dump(m, dump, n_regs);

			kfree(dump);
		}
	}

	for (pipe = 0; pipe < CIK_SDMA_ENGINE_NUM; pipe++) {
		for (queue = 0; queue < CIK_SDMA_QUEUES_PER_ENGINE; queue++) {
			r = dqm->dev->kfd2kgd->hqd_sdma_dump(
				dqm->dev->kgd, pipe, queue, &dump, &n_regs);
			if (r != 0)
				break;

			seq_printf(m, "  SDMA Engine %d, RLC %d\n",
				  pipe, queue);
			seq_reg_dump(m, dump, n_regs);

			kfree(dump);
		}
	}

	return r;
}
