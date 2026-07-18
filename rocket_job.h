/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_JOB_H__
#define __ROCKET_JOB_H__

#include <drm/drm_drv.h>
#include <drm/gpu_scheduler.h>

#include "rocket_core.h"
#include "rocket_drv.h"

struct rocket_task {
	u64 regcmd;
	u32 regcmd_count;
};

#include "rknpu_ioctl.h"

struct rocket_job {
	struct drm_sched_job base;

	struct rocket_device *rdev;

	struct drm_gem_object **in_bos;
	struct drm_gem_object **out_bos;

	u32 in_bo_count;
	u32 out_bo_count;

	struct rocket_task *tasks;
	u32 task_count;
	u32 next_task_idx;

	/* rkopnu: rknpu submit context; per-core task range selected in hw_submit */
	struct rknpu_task *rk_tasks;              /* vmap'd task array base */
	struct rknpu_subcore_task rk_subcore[5];
	u32 rk_core_mask;
	u32 rk_use_core_num;
	u64 rk_task_base_addr;
	u32 rk_task_start;
	u32 rk_task_number;
	u32 rk_pp_en;
	u32 timeout_count;

	u64 t_push; /* rkopnu perf-trace: ktime at drm_sched push */

	/* Fence to be signaled by drm-sched once its done with the job */
	struct dma_fence *inference_done_fence;

	/* Fence to be signaled by IRQ handler when the job is complete. */
	struct dma_fence *done_fence;

	struct rocket_iommu_domain *domain;

	struct kref refcount;
};

int rocket_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);

int rocket_job_init(struct rocket_core *core);
void rocket_job_fini(struct rocket_core *core);
int rocket_job_open(struct rocket_file_priv *rocket_priv);
void rocket_job_close(struct rocket_file_priv *rocket_priv);
int rocket_job_is_idle(struct rocket_core *core);

#endif
