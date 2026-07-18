// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/drm_drv.h>
#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>

#include "rocket_device.h"

struct rocket_device *rocket_device_init(struct platform_device *pdev,
					 const struct drm_driver *rocket_drm_driver)
{
	struct device *dev = &pdev->dev;
	struct device_node *core_node;
	struct rocket_device *rdev;
	struct drm_device *ddev;
	unsigned int num_cores = 0;
	int err;

	rdev = devm_drm_dev_alloc(dev, rocket_drm_driver, struct rocket_device, ddev);
	if (IS_ERR(rdev))
		return rdev;

	ddev = &rdev->ddev;
	dev_set_drvdata(dev, rdev);

	for_each_compatible_node(core_node, NULL, "rockchip,rk3588-rknn-core")
		if (of_device_is_available(core_node))
			num_cores++;

	rdev->cores = devm_kcalloc(dev, num_cores, sizeof(*rdev->cores), GFP_KERNEL);
	if (!rdev->cores)
		return ERR_PTR(-ENOMEM);

	dma_set_max_seg_size(dev, UINT_MAX);

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return ERR_PTR(err);

	err = devm_mutex_init(dev, &rdev->sched_lock);
	if (err)
		return ERR_PTR(-ENOMEM);

	err = devm_mutex_init(dev, &rdev->pm_lock);
	if (err)
		return ERR_PTR(-ENOMEM);

	err = drm_dev_register(ddev, 0);
	if (err)
		return ERR_PTR(err);

	return rdev;
}

void rocket_device_fini(struct rocket_device *rdev)
{
	WARN_ON(rdev->num_cores > 0);

	drm_dev_unregister(&rdev->ddev);
}

/*
 * Resume all cores' power domains and clocks, in core-index order (core 0 /
 * PD_NPUTOP first, matching genpd's own subdomain-requires-master ordering
 * and the vendor rknpu driver's npu0 -> npu1 -> npu2 power-on sequence).
 * Caller holds rdev->pm_lock.
 */
static int rocket_device_pm_resume_unit(struct rocket_device *rdev)
{
	unsigned int i;
	int ret;

	for (i = 0; i < rdev->num_cores; i++) {
		ret = pm_runtime_resume_and_get(rdev->cores[i].dev);
		if (ret < 0) {
			dev_err(rdev->cores[i].dev,
				"failed to resume NPU power unit (%d)\n", ret);
			while (i-- > 0) {
				pm_runtime_mark_last_busy(rdev->cores[i].dev);
				pm_runtime_put_autosuspend(rdev->cores[i].dev);
			}
			return ret;
		}
	}

	return 0;
}

/*
 * Release all cores' power domains and clocks as a unit. Caller holds
 * rdev->pm_lock and has already verified pm_refcount is 0, i.e. no core has
 * any job in flight.
 */
static void rocket_device_pm_release_unit(struct rocket_device *rdev)
{
	unsigned int i;

	/*
	 * No IOMMU-suspend poll here (tried and reverted - see commit
	 * "accel/rocket: drop the IOMMU-suspend poll from unit release").
	 * genpd itself already refuses to power off a domain until every
	 * device attached to it - a core AND its separate rockchip-iommu
	 * device, linked via a DL_FLAG_PM_RUNTIME device link - reports
	 * itself runtime-suspended (that is genpd's own per-domain
	 * device_count bookkeeping, not something this driver needs to
	 * duplicate). genpd also already refuses to power off a master
	 * domain (PD_NPUTOP) while a subdomain (PD_NPU1/PD_NPU2) is still on.
	 *
	 * Release core 0 last anyway, so its autosuspend delay is what
	 * ultimately paces the whole unit's gate-down, matching the vendor
	 * driver's npu2 -> npu1 -> npu0 power-off order.
	 */
	for (i = rdev->num_cores; i-- > 0; ) {
		/*
		 * Passive visibility only: a plain status read, no poll, no
		 * wait, not on the gating critical path.
		 */
		if (rdev->cores[i].iommu_dev)
			dev_dbg(rdev->cores[i].dev, "iommu %s suspended=%d at unit release\n",
				dev_name(rdev->cores[i].iommu_dev),
				pm_runtime_status_suspended(rdev->cores[i].iommu_dev));

		pm_runtime_mark_last_busy(rdev->cores[i].dev);
		pm_runtime_put_autosuspend(rdev->cores[i].dev);
	}
}

int rocket_device_pm_get(struct rocket_device *rdev)
{
	int ret = 0;

	mutex_lock(&rdev->pm_lock);
	if (rdev->pm_refcount == 0) {
		ret = rocket_device_pm_resume_unit(rdev);
		if (ret == 0)
			rdev->pm_refcount++;
	} else {
		rdev->pm_refcount++;
	}
	mutex_unlock(&rdev->pm_lock);

	return ret;
}

void rocket_device_pm_put(struct rocket_device *rdev)
{
	mutex_lock(&rdev->pm_lock);
	if (WARN_ON(rdev->pm_refcount == 0)) {
		mutex_unlock(&rdev->pm_lock);
		return;
	}

	if (--rdev->pm_refcount == 0)
		rocket_device_pm_release_unit(rdev);
	mutex_unlock(&rdev->pm_lock);
}

void rocket_device_pm_put_noidle(struct rocket_device *rdev)
{
	unsigned int i;

	mutex_lock(&rdev->pm_lock);
	if (WARN_ON(rdev->pm_refcount == 0)) {
		mutex_unlock(&rdev->pm_lock);
		return;
	}

	/*
	 * Error/timeout unwind: balance the refcount without deferring via
	 * autosuspend or waiting on the IOMMU poll. If this brings the unit
	 * to idle, a plain immediate put is enough; the next
	 * rocket_device_pm_get() will bring it back up cleanly regardless of
	 * whatever transient state the domains are left in.
	 */
	if (--rdev->pm_refcount == 0) {
		for (i = 0; i < rdev->num_cores; i++)
			pm_runtime_put_noidle(rdev->cores[i].dev);
	}
	mutex_unlock(&rdev->pm_lock);
}
