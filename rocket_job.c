// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2019 Linaro, Ltd, Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/drm_print.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/rocket_accel.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>

#include "rocket_core.h"
#include "rocket_device.h"
#include "rocket_drv.h"
#include "rocket_job.h"
#include "rocket_registers.h"
#include <linux/iosys-map.h>
#include <linux/bitops.h>
#include <drm/drm_gem_shmem_helper.h>
#include "rknpu_ioctl.h"

#define JOB_TIMEOUT_MS 500

static struct rocket_job *
to_rocket_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct rocket_job, base);
}

/*
 * rkopnu: map a core_mask to the physical core index the job commits to
 * (vendor rknpu_wait_core_index). CORE0=1, CORE1=2, CORE2=4. Multi-core masks
 * (0x3 / 0x7) commit their first PC to core 0; the sibling cores are driven
 * from the same job via subcore_task[] on core 0's completion path.
 */
static int rknpu_wait_core_index(u32 core_mask)
{
	switch (core_mask) {
	case 0x2: /* CORE1 only */
		return 1;
	case 0x4: /* CORE2 only */
		return 2;
	default:  /* CORE0, 0x3, 0x7, AUTO(0) -> core 0 */
		return 0;
	}
}

static const char *rocket_fence_get_driver_name(struct dma_fence *fence)
{
	return "rocket";
}

static const char *rocket_fence_get_timeline_name(struct dma_fence *fence)
{
	return "rockchip-npu";
}

static const struct dma_fence_ops rocket_fence_ops = {
	.get_driver_name = rocket_fence_get_driver_name,
	.get_timeline_name = rocket_fence_get_timeline_name,
};

static struct dma_fence *rocket_fence_create(struct rocket_core *core)
{
	struct dma_fence *fence;

	fence = kzalloc_obj(*fence);
	if (!fence)
		return ERR_PTR(-ENOMEM);

	dma_fence_init(fence, &rocket_fence_ops, &core->fence_lock,
		       core->fence_context, ++core->emit_seqno);

	return fence;
}

static int
rocket_copy_tasks(struct drm_device *dev,
		  struct drm_file *file_priv,
		  struct drm_rocket_job *job,
		  struct rocket_job *rjob)
{
	int ret = 0;

	if (job->task_struct_size < sizeof(struct drm_rocket_task))
		return -EINVAL;

	rjob->task_count = job->task_count;

	if (!rjob->task_count)
		return 0;

	rjob->tasks = kvmalloc_objs(*rjob->tasks, job->task_count);
	if (!rjob->tasks) {
		drm_dbg(dev, "Failed to allocate task array\n");
		return -ENOMEM;
	}

	for (int i = 0; i < rjob->task_count; i++) {
		struct drm_rocket_task task = {0};

		if (copy_from_user(&task,
				   u64_to_user_ptr(job->tasks) + i * job->task_struct_size,
				   sizeof(task))) {
			drm_dbg(dev, "Failed to copy incoming tasks\n");
			ret = -EFAULT;
			goto fail;
		}

		if (task.regcmd_count == 0) {
			drm_dbg(dev, "regcmd_count field in drm_rocket_task should be > 0.\n");
			ret = -EINVAL;
			goto fail;
		}

		rjob->tasks[i].regcmd = task.regcmd;
		rjob->tasks[i].regcmd_count = task.regcmd_count;
	}

	return 0;

fail:
	kvfree(rjob->tasks);
	return ret;
}

static void rocket_job_hw_submit(struct rocket_core *core, struct rocket_job *job)
{
	unsigned int extra_bit;
	int ci = core->index;
	int ts, tn;
	struct rknpu_task *first, *last;

	if (atomic_read(&core->reset.pending))
		return;

	job->next_task_idx++;

	/* RK3588 (num_irqs>1): per-core task range comes from subcore_task[] */
	if (job->rk_use_core_num == 3) {
		ts = job->rk_subcore[ci + 2].task_start;
		tn = job->rk_subcore[ci + 2].task_number;
	} else {
		ts = job->rk_subcore[ci].task_start;
		tn = job->rk_subcore[ci].task_number;
	}
	/* Fall back to the whole-job range if the subcore slot is empty or its
	 * range escapes the job's declared [task_start, task_start+task_number).
	 * Defends against reading an unfilled/garbage subcore_task[] slot, which
	 * would index rk_tasks[] out of the vmap'd BO and oops. */
	if (tn == 0 || ts < job->rk_task_start ||
	    ts + tn > job->rk_task_start + job->rk_task_number) {
		ts = job->rk_task_start;
		tn = job->rk_task_number;
	}
	first = &job->rk_tasks[ts];
	last = &job->rk_tasks[ts + tn - 1];

	/* enable the PP pipeline (vendor rknpu enable writes = rocket CNA/CORE S_POINTER) */
	rocket_pc_writel(core, BASE_ADDRESS, 0x1);
	extra_bit = 0x10000000 * core->index;
	rocket_cna_writel(core, S_POINTER, CNA_S_POINTER_POINTER_PP_EN(1) |
					   CNA_S_POINTER_EXECUTER_PP_EN(1) |
					   CNA_S_POINTER_POINTER_PP_MODE(1) | extra_bit);
	rocket_core_writel(core, S_POINTER, CORE_S_POINTER_POINTER_PP_EN(1) |
					    CORE_S_POINTER_EXECUTER_PP_EN(1) |
					    CORE_S_POINTER_POINTER_PP_MODE(1) | extra_bit);

	/* PC program (vendor rknpu_job_subcore_commit_pc, rk3588: scale=2, extra=4, bits=12) */
	rocket_pc_writel(core, BASE_ADDRESS, (u32)first->regcmd_addr);
	rocket_pc_writel(core, REGISTER_AMOUNTS,
			 (first->regcfg_amount + 4 + 2 - 1) / 2 - 1);
	rocket_pc_writel(core, INTERRUPT_MASK, last->int_mask);
	rocket_pc_writel(core, INTERRUPT_CLEAR, first->int_mask);
	rocket_pc_writel(core, TASK_CON, ((0x6 | job->rk_pp_en) << 12) | tn);
	rocket_pc_writel(core, TASK_DMA_BASE_ADDR, (u32)job->rk_task_base_addr);
	rocket_pc_writel(core, OPERATION_ENABLE, 0x1);
	rocket_pc_writel(core, OPERATION_ENABLE, 0x0);
}

static int rocket_acquire_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct drm_sched_job *job,
					bool is_write)
{
	int i, ret;

	for (i = 0; i < bo_count; i++) {
		ret = dma_resv_reserve_fences(bos[i]->resv, 1);
		if (ret)
			return ret;

		ret = drm_sched_job_add_implicit_dependencies(job, bos[i],
							      is_write);
		if (ret)
			return ret;
	}

	return 0;
}

static void rocket_attach_object_fences(struct drm_gem_object **bos,
					int bo_count,
					struct dma_fence *fence)
{
	int i;

	for (i = 0; i < bo_count; i++)
		dma_resv_add_fence(bos[i]->resv, fence, DMA_RESV_USAGE_WRITE);
}

static int rocket_job_push(struct rocket_job *job)
{
	struct rocket_device *rdev = job->rdev;
	struct drm_gem_object **bos;
	struct ww_acquire_ctx acquire_ctx;
	int ret = 0;

	bos = kvmalloc_array(job->in_bo_count + job->out_bo_count, sizeof(void *),
			     GFP_KERNEL);
	memcpy(bos, job->in_bos, job->in_bo_count * sizeof(void *));
	memcpy(&bos[job->in_bo_count], job->out_bos, job->out_bo_count * sizeof(void *));

	ret = drm_gem_lock_reservations(bos, job->in_bo_count + job->out_bo_count, &acquire_ctx);
	if (ret)
		goto err;

	scoped_guard(mutex, &rdev->sched_lock) {
		drm_sched_job_arm(&job->base);

		job->inference_done_fence = dma_fence_get(&job->base.s_fence->finished);

		ret = rocket_acquire_object_fences(job->in_bos, job->in_bo_count, &job->base, false);
		if (ret)
			goto err_unlock;

		ret = rocket_acquire_object_fences(job->out_bos, job->out_bo_count, &job->base, true);
		if (ret)
			goto err_unlock;

		kref_get(&job->refcount); /* put by scheduler job completion */

		drm_sched_entity_push_job(&job->base);
	}

	rocket_attach_object_fences(job->out_bos, job->out_bo_count, job->inference_done_fence);

err_unlock:
	drm_gem_unlock_reservations(bos, job->in_bo_count + job->out_bo_count, &acquire_ctx);
err:
	kvfree(bos);

	return ret;
}

static void rocket_job_cleanup(struct kref *ref)
{
	struct rocket_job *job = container_of(ref, struct rocket_job,
						refcount);
	unsigned int i;

	rocket_iommu_domain_put(job->domain);

	dma_fence_put(job->done_fence);
	dma_fence_put(job->inference_done_fence);

	if (job->in_bos) {
		for (i = 0; i < job->in_bo_count; i++)
			drm_gem_object_put(job->in_bos[i]);

		kvfree(job->in_bos);
	}

	if (job->out_bos) {
		for (i = 0; i < job->out_bo_count; i++)
			drm_gem_object_put(job->out_bos[i]);

		kvfree(job->out_bos);
	}

	kvfree(job->tasks);

	kfree(job);
}

static void rocket_job_put(struct rocket_job *job)
{
	kref_put(&job->refcount, rocket_job_cleanup);
}

static void rocket_job_free(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);

	drm_sched_job_cleanup(sched_job);

	rocket_job_put(job);
}

static struct rocket_core *sched_to_core(struct rocket_device *rdev,
					 struct drm_gpu_scheduler *sched)
{
	unsigned int core;

	for (core = 0; core < rdev->num_cores; core++) {
		if (&rdev->cores[core].sched == sched)
			return &rdev->cores[core];
	}

	return NULL;
}

static struct dma_fence *rocket_job_run(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);
	struct rocket_device *rdev = job->rdev;
	struct rocket_core *core = sched_to_core(rdev, sched_job->sched);
	struct dma_fence *fence = NULL;
	int attach_err = 0;
	int ret;

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	/*
	 * Nothing to execute: can happen if the job has finished while
	 * we were resetting the NPU.
	 */
	if (job->next_task_idx == job->task_count)
		return NULL;

	fence = rocket_fence_create(core);
	if (IS_ERR(fence))
		return fence;

	if (job->done_fence)
		dma_fence_put(job->done_fence);
	job->done_fence = dma_fence_get(fence);

	/*
	 * All three NPU cores share one physical NIU/interconnect and PMU
	 * idle-request handshake; resume (and later release) their power
	 * domains and clocks as a single unit rather than per-core, or a
	 * sibling core's independent gating can race this core's in-flight
	 * DMA traffic. See rocket_device_pm_get() in rocket_device.c.
	 */
	ret = rocket_device_pm_get(rdev);
	if (ret < 0)
		return fence;

	/*
	 * rkopnu perf: keep the IOMMU domain attached across jobs (the vendor
	 * does); a per-submit attach/detach is a TLB flush * 2028 submits.
	 * Re-attach only on a domain change. The attach/detach of attached_domain
	 * is done under job_lock so it is serialized against the detach in
	 * rocket_job_close() and rocket_reset() (Fable review).
	 */

	scoped_guard(mutex, &core->job_lock) {
		if (core->attached_domain != job->domain) {
			if (core->attached_domain)
				iommu_detach_group(NULL, core->iommu_group);
			attach_err = iommu_attach_group(job->domain->domain,
							core->iommu_group);
			core->attached_domain = attach_err ? NULL : job->domain;
		}
		if (!attach_err) {
			core->in_flight_job = job;
			rocket_job_hw_submit(core, job);
		}
	}

	/*
	 * On attach failure the job never reached hw_submit, so no IRQ will ever
	 * complete it. Unwind the PM ref taken above and fail the fence now
	 * instead of leaving a phantom "submitted" job to time out (which would
	 * also leak the PM ref, since rocket_reset only unwinds when in_flight_job
	 * is set). See Fable review.
	 */
	if (attach_err) {
		rocket_device_pm_put(rdev);
		dma_fence_set_error(fence, attach_err);
		dma_fence_signal(fence);
	}

	return fence;
}

static void rocket_job_handle_irq(struct rocket_core *core)
{
	/*
	 * No pm_runtime_mark_last_busy(core->dev) here: the whole-unit
	 * release path (rocket_device_pm_put() -> rocket_device_pm_release_unit())
	 * marks last-busy on every core right before its put, at the point
	 * the last in-flight job anywhere actually completes. That is the
	 * meaningful "unit went idle" timestamp for the shared autosuspend
	 * delay, not this core's own completion time.
	 */
	rocket_pc_writel(core, OPERATION_ENABLE, 0x0);
	rocket_pc_writel(core, INTERRUPT_CLEAR, 0xffffffff);

	scoped_guard(mutex, &core->job_lock)
		if (core->in_flight_job) {
			if (core->in_flight_job->next_task_idx < core->in_flight_job->task_count) {
				rocket_job_hw_submit(core, core->in_flight_job);
				return;
			}

			/* rkopnu perf: leave the domain attached (see rocket_job_run) */
			dma_fence_signal(core->in_flight_job->done_fence);
			rocket_device_pm_put(core->rdev);
			core->in_flight_job = NULL;
		}
}

static void
rocket_reset(struct rocket_core *core, struct drm_sched_job *bad)
{
	if (!atomic_read(&core->reset.pending))
		return;

	drm_sched_stop(&core->sched, bad);

	/*
	 * Remaining interrupts have been handled, but we might still have
	 * stuck jobs. Let's make sure the PM counters stay balanced by
	 * manually calling pm_runtime_put_noidle().
	 */
	scoped_guard(mutex, &core->job_lock) {
		if (core->in_flight_job)
			rocket_device_pm_put_noidle(core->rdev);

		if (core->attached_domain) {
			iommu_detach_group(NULL, core->iommu_group);
			core->attached_domain = NULL;
		}

		core->in_flight_job = NULL;
	}

	/* Proceed with reset now. */
	rocket_core_reset(core);

	/* NPU has been reset, we can clear the reset pending bit. */
	atomic_set(&core->reset.pending, 0);

	/* Restart the scheduler */
	drm_sched_start(&core->sched, 0);
}

static enum drm_gpu_sched_stat rocket_job_timedout(struct drm_sched_job *sched_job)
{
	struct rocket_job *job = to_rocket_job(sched_job);
	struct rocket_device *rdev = job->rdev;
	struct rocket_core *core = sched_to_core(rdev, sched_job->sched);

	dev_err(core->dev, "NPU job timed out (#%u); int_raw=0x%x int_status=0x%x",
		job->timeout_count + 1,
		rocket_pc_readl(core, INTERRUPT_RAW_STATUS),
		rocket_pc_readl(core, INTERRUPT_STATUS));

	atomic_set(&core->reset.pending, 1);
	rocket_reset(core, sched_job);

	/* Defense-in-depth: don't retry a stuck job forever (that races teardown
	 * -> drm_mm/list corruption -> Oops on close). Give up after a few. */
	if (++job->timeout_count > 3) {
		dev_err(core->dev, "NPU job stuck after %u timeouts; abandoning\n",
			job->timeout_count);
		return DRM_GPU_SCHED_STAT_ENODEV;
	}

	return DRM_GPU_SCHED_STAT_RESET;
}

static void rocket_reset_work(struct work_struct *work)
{
	struct rocket_core *core;

	core = container_of(work, struct rocket_core, reset.work);
	rocket_reset(core, NULL);
}

static const struct drm_sched_backend_ops rocket_sched_ops = {
	.run_job = rocket_job_run,
	.timedout_job = rocket_job_timedout,
	.free_job = rocket_job_free
};

static irqreturn_t rocket_job_irq_handler_thread(int irq, void *data)
{
	struct rocket_core *core = data;

	rocket_job_handle_irq(core);

	return IRQ_HANDLED;
}

static irqreturn_t rocket_job_irq_handler(int irq, void *data)
{
	struct rocket_core *core = data;
	u32 raw_status = rocket_pc_readl(core, INTERRUPT_RAW_STATUS);

	WARN_ON(raw_status & PC_INTERRUPT_RAW_STATUS_DMA_READ_ERROR);
	WARN_ON(raw_status & PC_INTERRUPT_RAW_STATUS_DMA_WRITE_ERROR);

	/* rkopnu: wake on ANY raw interrupt - librknnrt's matmul completion lands on
	 * bits 30/31, not the DPU_0/1 bits (8/9) mainline rocket was RE'd against. */
	if (!raw_status)
		return IRQ_NONE;

	rocket_pc_writel(core, INTERRUPT_MASK, 0x0);

	return IRQ_WAKE_THREAD;
}

int rocket_job_init(struct rocket_core *core)
{
	struct drm_sched_init_args args = {
		.ops = &rocket_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(JOB_TIMEOUT_MS),
		.name = dev_name(core->dev),
		.dev = core->dev,
	};
	int ret;

	INIT_WORK(&core->reset.work, rocket_reset_work);
	spin_lock_init(&core->fence_lock);
	mutex_init(&core->job_lock);

	core->irq = platform_get_irq(to_platform_device(core->dev), 0);
	if (core->irq < 0)
		return core->irq;

	ret = devm_request_threaded_irq(core->dev, core->irq,
					rocket_job_irq_handler,
					rocket_job_irq_handler_thread,
					IRQF_SHARED, dev_name(core->dev),
					core);
	if (ret) {
		dev_err(core->dev, "failed to request job irq");
		return ret;
	}

	core->reset.wq = alloc_ordered_workqueue("rocket-reset-%d", 0, core->index);
	if (!core->reset.wq)
		return -ENOMEM;

	core->fence_context = dma_fence_context_alloc(1);

	args.timeout_wq = core->reset.wq;
	ret = drm_sched_init(&core->sched, &args);
	if (ret) {
		dev_err(core->dev, "Failed to create scheduler: %d.", ret);
		goto err_sched;
	}

	return 0;

err_sched:
	drm_sched_fini(&core->sched);

	destroy_workqueue(core->reset.wq);
	return ret;
}

void rocket_job_fini(struct rocket_core *core)
{
	drm_sched_fini(&core->sched);

	cancel_work_sync(&core->reset.work);
	destroy_workqueue(core->reset.wq);
}

int rocket_job_open(struct rocket_file_priv *rocket_priv)
{
	struct rocket_device *rdev = rocket_priv->rdev;
	struct drm_gpu_scheduler **scheds = kmalloc_objs(*scheds,
							 rdev->num_cores);
	unsigned int core;
	int ret;

	for (core = 0; core < rdev->num_cores; core++)
		scheds[core] = &rdev->cores[core].sched;

	ret = drm_sched_entity_init(&rocket_priv->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL,
				    scheds,
				    rdev->num_cores, NULL);
	if (WARN_ON(ret))
		return ret;

	/* rkopnu: per-core entities so a SUBMIT can be pinned to the core
	 * selected by core_mask (see rocket_file_priv::core_entity). */
	for (core = 0; core < rdev->num_cores && core < 3; core++) {
		struct drm_gpu_scheduler *sched = &rdev->cores[core].sched;

		ret = drm_sched_entity_init(&rocket_priv->core_entity[core],
					    DRM_SCHED_PRIORITY_NORMAL,
					    &sched, 1, NULL);
		if (WARN_ON(ret))
			return ret;
	}

	return 0;
}

void rocket_job_close(struct rocket_file_priv *rocket_priv)
{
	struct rocket_device *rdev = rocket_priv->rdev;
	struct drm_sched_entity *entity = &rocket_priv->sched_entity;
	unsigned int core, i;

	for (core = 0; core < rdev->num_cores && core < 3; core++)
		drm_sched_entity_destroy(&rocket_priv->core_entity[core]);

	kfree(entity->sched_list);
	drm_sched_entity_destroy(entity);

	/*
	 * The attach-once optimization (rocket_job_run) leaves this file's IOMMU
	 * domain attached to whatever cores last ran its jobs, detaching only on
	 * reset. The entities are now destroyed so no further jobs will run;
	 * detach the domain from any core still holding it before rocket_postclose
	 * frees it. Skipping this frees the domain while it is still attached
	 * (rk_iommu_domain_free WARN) and leaves core->attached_domain dangling ->
	 * use-after-free / panic when the next client's job touches it.
	 *
	 * rkopnu multi-domain: a core's attached_domain can now be any one of
	 * this fd's (up to 16) domains, not the single one it used to have --
	 * check them all. A core can only ever have one domain attached at a
	 * time, so at most one of these can match; break once found.
	 */
	for (core = 0; core < rdev->num_cores; core++) {
		struct rocket_core *rcore = &rdev->cores[core];

		scoped_guard(mutex, &rcore->job_lock) {
			for (i = 0; i < ROCKET_MAX_IOMMU_DOMAINS; i++) {
				if (rocket_priv->domains[i] &&
				    rcore->attached_domain == rocket_priv->domains[i]) {
					iommu_detach_group(NULL, rcore->iommu_group);
					rcore->attached_domain = NULL;
					break;
				}
			}
		}
	}
}

int rocket_job_is_idle(struct rocket_core *core)
{
	/* If there are any jobs in this HW queue, we're not idle */
	if (atomic_read(&core->sched.credit_count))
		return false;

	return true;
}

static int rocket_ioctl_submit_job(struct drm_device *dev, struct drm_file *file,
				   struct drm_rocket_job *job)
{
	struct rocket_device *rdev = to_rocket_device(dev);
	struct rocket_file_priv *file_priv = file->driver_priv;
	struct rocket_job *rjob = NULL;
	int ret = 0;

	if (job->task_count == 0)
		return -EINVAL;

	rjob = kzalloc_obj(*rjob);
	if (!rjob)
		return -ENOMEM;

	kref_init(&rjob->refcount);

	rjob->rdev = rdev;

	ret = drm_sched_job_init(&rjob->base,
				 &file_priv->sched_entity,
				 1, NULL, file->client_id);
	if (ret)
		goto out_put_job;

	ret = rocket_copy_tasks(dev, file, job, rjob);
	if (ret)
		goto out_cleanup_job;

	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(job->in_bo_handles),
				     job->in_bo_handle_count, &rjob->in_bos);
	if (ret)
		goto out_cleanup_job;

	rjob->in_bo_count = job->in_bo_handle_count;

	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(job->out_bo_handles),
				     job->out_bo_handle_count, &rjob->out_bos);
	if (ret)
		goto out_cleanup_job;

	rjob->out_bo_count = job->out_bo_handle_count;

	/* rkopnu multi-domain: native rocket uAPI has no domain-id field;
	 * always domain 0 (see the matching comment in rocket_gem.c -- this
	 * ioctl is unregistered/unreachable today, kept correct not deleted). */
	rjob->domain = rocket_iommu_domain_get(file_priv, 0);
	if (IS_ERR(rjob->domain)) {
		ret = PTR_ERR(rjob->domain);
		rjob->domain = NULL;
		goto out_cleanup_job;
	}

	ret = rocket_job_push(rjob);
	if (ret)
		goto out_cleanup_job;

out_cleanup_job:
	if (ret)
		drm_sched_job_cleanup(&rjob->base);
out_put_job:
	rocket_job_put(rjob);

	return ret;
}

int rocket_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_rocket_submit *args = data;
	struct drm_rocket_job *jobs;
	int ret = 0;
	unsigned int i = 0;

	if (args->job_count == 0)
		return 0;

	if (args->job_struct_size < sizeof(struct drm_rocket_job)) {
		drm_dbg(dev, "job_struct_size field in drm_rocket_submit struct is too small.\n");
		return -EINVAL;
	}

	if (args->reserved != 0) {
		drm_dbg(dev, "Reserved field in drm_rocket_submit struct should be 0.\n");
		return -EINVAL;
	}

	jobs = kvmalloc_objs(*jobs, args->job_count);
	if (!jobs) {
		drm_dbg(dev, "Failed to allocate incoming job array\n");
		return -ENOMEM;
	}

	for (i = 0; i < args->job_count; i++) {
		if (copy_from_user(&jobs[i],
				   u64_to_user_ptr(args->jobs) + i * args->job_struct_size,
				   sizeof(*jobs))) {
			ret = -EFAULT;
			drm_dbg(dev, "Failed to copy incoming job array\n");
			goto exit;
		}
	}


	for (i = 0; i < args->job_count; i++)
		rocket_ioctl_submit_job(dev, file, &jobs[i]);

exit:
	kvfree(jobs);

	return ret;
}

/*
 * rkopnu Phase 3: the rknpu RKNPU_SUBMIT ioctl. Reads the rknpu_task[] out of
 * the task BO (obj_addr handed back by MEM_CREATE), translates each into a
 * rocket_task (regcmd_addr->regcmd, regcfg_amount->regcmd_count -- same RK3588
 * PC register-command format rocket_job_hw_submit already drives), pushes onto
 * rocket's drm_sched, and blocks until the NPU IRQ completes it (the rknpu ABI
 * is synchronous). Single-core first cut; core_mask multi-core is Phase 3b.
 */
int rkopnu_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct rocket_device *rdev = to_rocket_device(dev);
	struct rocket_file_priv *file_priv = file->driver_priv;
	struct rknpu_submit *args = data;
	struct drm_gem_object *task_gem;
	struct rknpu_task *tasks;
	struct rocket_job *rjob;
	struct iosys_map map;
	int ret;

	if (args->task_number == 0)
		return 0;

	task_gem = (struct drm_gem_object *)(uintptr_t)args->task_obj_addr;
	if (!task_gem)
		return -EINVAL;

	/*
	 * task_obj_addr is a raw kernel pointer handed back by MEM_CREATE; take a
	 * reference so a concurrent MEM_DESTROY / fd close can't free the BO under
	 * the vmap and the in-flight rk_tasks deref (Fable review). vmap only pins
	 * pages, not the gem object.
	 */
	drm_gem_object_get(task_gem);

	/* Map the task BO for this submit only. librknnrt drives the 3 cores from
	 * concurrent threads, each with its own task BO, so a shared per-file vmap
	 * cache races (one thread's vunmap strands another's in-flight rk_tasks).
	 * The ioctl blocks on the fence below until the job completes, so this
	 * per-submit mapping stays valid across the worker's hw_submit. */
	ret = drm_gem_vmap(task_gem, &map);
	if (ret) {
		drm_gem_object_put(task_gem);
		return ret;
	}
	tasks = (struct rknpu_task *)map.vaddr;

	rjob = kzalloc_obj(*rjob);
	if (!rjob) {
		ret = -ENOMEM;
		goto out_vunmap;
	}
	kref_init(&rjob->refcount);
	rjob->rdev = rdev;

	/* Pin to the physical core selected by core_mask so hw_submit reads the
	 * matching subcore_task[] slot (see rocket_file_priv::core_entity). */
	{
		int ci = rknpu_wait_core_index(args->core_mask);

		if (ci >= rdev->num_cores)
			ci = 0;
		ret = drm_sched_job_init(&rjob->base, &file_priv->core_entity[ci],
					 1, NULL, file->client_id);
	}
	if (ret)
		goto out_put_job;

	rjob->task_count = 1; /* one PC program per core drives its subcore range */
	rjob->tasks = NULL;
	rjob->rk_tasks = tasks;
	memcpy(rjob->rk_subcore, args->subcore_task, sizeof(rjob->rk_subcore));
	rjob->rk_core_mask = args->core_mask;
	rjob->rk_use_core_num = hweight32(args->core_mask & 0x7);
	rjob->rk_task_base_addr = args->task_base_addr;
	rjob->rk_task_start = args->task_start;
	rjob->rk_task_number = args->task_number;
	rjob->rk_pp_en = (args->flags & RKNPU_JOB_PINGPONG) ? 1 : 0;

	rjob->in_bo_count = 0;
	rjob->out_bo_count = 0;
	/*
	 * rkopnu multi-domain: this is the field librknnrt actually threads
	 * through from rknn_matmul_info.iommu_domain_id at rknn_matmul_create()
	 * time, for the *same* matmul context whose weight buffer was created
	 * against that domain id via MEM_CREATE. Honoring it here is what
	 * makes rocket_job_run()'s existing "reattach only on domain change"
	 * logic (rocket_job.c) actually switch domains instead of always
	 * seeing the same one.
	 */
	rjob->domain = rocket_iommu_domain_get(file_priv, (s32)args->iommu_domain_id);
	if (IS_ERR(rjob->domain)) {
		/* drm_sched_job_init() already succeeded above (this check runs
		 * after it), so unlike the two out_put_job jumps elsewhere in
		 * this function, this one must go through out_cleanup so
		 * drm_sched_job_cleanup() actually runs -- straight to
		 * out_put_job here would skip it and leak/imbalance whatever
		 * job_init set up. */
		ret = PTR_ERR(rjob->domain);
		rjob->domain = NULL;
		goto out_cleanup;
	}

	ret = rocket_job_push(rjob);
	if (ret)
		goto out_cleanup;

	/*
	 * Bounded wait: the rknpu ABI is synchronous, but an unbounded
	 * non-interruptible wait leaves an unkillable D-state task if the NPU
	 * wedges and even reset fails to signal the fence -> SIGKILL from a
	 * crashing client can't unstick it and reboot hangs. drm_sched already
	 * resets a stuck job within ~2s (500ms * up to 4); cap the wait past that
	 * so a genuine wedge fails the request instead of the box. (Fable review.)
	 */
	if (rjob->inference_done_fence) {
		long tout = dma_fence_wait_timeout(rjob->inference_done_fence,
						   false, msecs_to_jiffies(5000));
		if (tout <= 0)
			drm_warn(dev, "rkopnu: submit wait unfinished (%ld)\n", tout);
	}

	args->task_counter = args->task_number;
	ret = 0;

out_cleanup:
	if (ret)
		drm_sched_job_cleanup(&rjob->base);
out_put_job:
	rocket_job_put(rjob);
out_vunmap:
	drm_gem_vunmap(task_gem, &map);
	drm_gem_object_put(task_gem);
	return ret;
}
