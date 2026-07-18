/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_DEVICE_H__
#define __ROCKET_DEVICE_H__

#include <drm/drm_device.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>

#include "rocket_core.h"

struct rocket_device {
	struct drm_device ddev;

	struct mutex sched_lock;

	struct rocket_core *cores;
	unsigned int num_cores;

	/*
	 * The three RK3588 NPU cores (PD_NPUTOP/core 0, PD_NPU1/core 1,
	 * PD_NPU2/core 2) share one physical NIU/interconnect and PMU
	 * idle-request handshake. genpd has no notion that these three
	 * otherwise-independent power domains share hardware: it will happily
	 * let one domain issue an idle-request while a sibling domain still
	 * has live AXI traffic in flight, which races the shared handshake
	 * and can fault with an asynchronous SError (seen on this board under
	 * autosuspend + sustained multi-core load).
	 *
	 * Fix: never let genpd make an independent per-core gating decision.
	 * Every in-flight job, on any core, holds one reference on this
	 * refcount. While it is non-zero all three cores' power domains and
	 * clocks are held resumed as a unit (vendor rknpu driver does the
	 * same: npu0/npu1/npu2 are resumed and suspended together under one
	 * lock, never independently). Only once the last job anywhere
	 * completes does the whole unit unwind. See rocket_device_pm_get()/
	 * rocket_device_pm_put() in rocket_device.c.
	 */
	struct mutex pm_lock;
	unsigned int pm_refcount;
};

struct rocket_device *rocket_device_init(struct platform_device *pdev,
					 const struct drm_driver *rocket_drm_driver);
void rocket_device_fini(struct rocket_device *rdev);

int rocket_device_pm_get(struct rocket_device *rdev);
void rocket_device_pm_put(struct rocket_device *rdev);
void rocket_device_pm_put_noidle(struct rocket_device *rdev);
#define to_rocket_device(drm_dev) \
	((struct rocket_device *)(container_of((drm_dev), struct rocket_device, ddev)))

#endif /* __ROCKET_DEVICE_H__ */
