// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/of_platform.h>
#include <linux/reset.h>

#include "rocket_core.h"
#include "rocket_job.h"

static const char * const rocket_opp_clk_names[] = { "npu", NULL };
static const char * const rocket_opp_regulator_names[] = { "npu", NULL };

static void rocket_core_clk_disable(void *data)
{
	struct rocket_core *core = data;

	clk_bulk_disable_unprepare(ARRAY_SIZE(core->clks), core->clks);
}

int rocket_core_init(struct rocket_core *core)
{
	struct dev_pm_opp_config opp_config = {
		.clk_names = rocket_opp_clk_names,
		.regulator_names = rocket_opp_regulator_names,
	};
	struct device *dev = core->dev;
	struct platform_device *pdev = to_platform_device(dev);
	u32 version;
	int err = 0;

	core->resets[0].id = "srst_a";
	core->resets[1].id = "srst_h";
	err = devm_reset_control_bulk_get_exclusive(&pdev->dev, ARRAY_SIZE(core->resets),
						    core->resets);
	if (err)
		return dev_err_probe(dev, err, "failed to get resets for core %d\n", core->index);

	core->clks[0].id = "aclk";
	core->clks[1].id = "hclk";
	core->clks[2].id = "npu";
	core->clks[3].id = "pclk";
	err = devm_clk_bulk_get(dev, ARRAY_SIZE(core->clks), core->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to get clocks for core %d\n", core->index);

	/*
	 * The RK3588 PMU power-up sequence for PD_NPUTOP/PD_NPU1/PD_NPU2
	 * (power on, memory init, NIU idle-release handshake) requires the
	 * domain's bus/core clocks to be running, but with runtime PM the
	 * genpd transition happens *before* this driver's runtime_resume
	 * callback gets to enable them. The power-domain nodes only list the
	 * shared root clocks, so a gated power-on brings the domain up with
	 * the per-core ACLK/HCLK branch gates disabled: the NPU logic powers
	 * up unclocked and in an undefined state. Jobs then hang without
	 * faulting, and the next idle-request handshake on 'nputop' times
	 * out, ending in a synchronous external abort when the IOMMU resume
	 * touches its registers behind the still-idled NIU.
	 *
	 * The vendor driver (rknpu) enables the full NPU clock set before any
	 * of the three domains is powered up and disables it only after they
	 * are all down. Mirror that by keeping the clocks enabled for as long
	 * as the driver is bound; the power savings come from the domains
	 * gating, which is unaffected.
	 */
	err = clk_bulk_prepare_enable(ARRAY_SIZE(core->clks), core->clks);
	if (err)
		return dev_err_probe(dev, err, "failed to enable clocks for core %d\n", core->index);

	err = devm_add_action_or_reset(dev, rocket_core_clk_disable, core);
	if (err)
		return err;

	err = devm_pm_opp_set_config(dev, &opp_config);
	if (err)
		return dev_err_probe(dev, err, "failed to set OPP config for core %d\n", core->index);

	err = devm_pm_opp_of_add_table(dev);
	if (err)
		return dev_err_probe(dev, err, "failed to add OPP table for core %d\n", core->index);

	core->pc_iomem = devm_platform_ioremap_resource_byname(pdev, "pc");
	if (IS_ERR(core->pc_iomem)) {
		dev_err(dev, "couldn't find PC registers %ld\n", PTR_ERR(core->pc_iomem));
		return PTR_ERR(core->pc_iomem);
	}

	core->cna_iomem = devm_platform_ioremap_resource_byname(pdev, "cna");
	if (IS_ERR(core->cna_iomem)) {
		dev_err(dev, "couldn't find CNA registers %ld\n", PTR_ERR(core->cna_iomem));
		return PTR_ERR(core->cna_iomem);
	}

	core->core_iomem = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(core->core_iomem)) {
		dev_err(dev, "couldn't find CORE registers %ld\n", PTR_ERR(core->core_iomem));
		return PTR_ERR(core->core_iomem);
	}

	dma_set_max_seg_size(dev, UINT_MAX);

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return err;

	core->iommu_group = iommu_group_get(dev);

	/*
	 * Look up the struct device for this core's rockchip-iommu so the
	 * whole-unit power-down path (rocket_device_pm_put()) can confirm it
	 * has actually finished its own (asynchronous) runtime suspend before
	 * letting any power domain gate. See the comment there for why.
	 */
	{
		struct device_node *iommu_np;
		struct platform_device *iommu_pdev;

		iommu_np = of_parse_phandle(pdev->dev.of_node, "iommus", 0);
		if (iommu_np) {
			iommu_pdev = of_find_device_by_node(iommu_np);
			of_node_put(iommu_np);
			if (iommu_pdev)
				core->iommu_dev = &iommu_pdev->dev;
		}
		if (!core->iommu_dev)
			dev_warn(dev, "couldn't resolve iommu device for core %d\n", core->index);
	}

	err = rocket_job_init(core);
	if (err) {
		iommu_group_put(core->iommu_group);
		core->iommu_group = NULL;
		return err;
	}

	pm_runtime_use_autosuspend(dev);

	/*
	 * As this NPU will be most often used as part of a media pipeline that
	 * ends presenting in a display, choose 50 ms (~3 frames at 60Hz) as an
	 * autosuspend delay as that will keep the device powered up while the
	 * pipeline is running.
	 */
	pm_runtime_set_autosuspend_delay(dev, 50);

	pm_runtime_enable(dev);

	err = pm_runtime_resume_and_get(dev);
	if (err) {
		rocket_core_fini(core);
		return err;
	}

	err = dev_pm_opp_set_rate(dev, 1000000000);
	if (err) {
		pm_runtime_put_autosuspend(dev);
		rocket_core_fini(core);
		return dev_err_probe(dev, err,
				     "failed to set NPU OPP rate for core %d\n", core->index);
	}

	version = rocket_pc_readl(core, VERSION);
	version += rocket_pc_readl(core, VERSION_NUM) & 0xffff;

	/*
	 * Never gate: deliberately do NOT release this reference. Mainline
	 * genpd's gate->resume sequencing for RK3588_PD_NPUTOP has a
	 * reproducible resume-side fault - the idle-request ACK never
	 * arrives ("failed to get ack on domain nputop, val=0xa1fff"),
	 * whether the preceding idle window is 50ms or several minutes -
	 * that survived three independent, source-verified fix attempts on
	 * this branch: cross-core whole-unit gating (41b54eff/8f15b3b5),
	 * a post-power-on settle delay grounded in RK3576's vendor table
	 * (bdc8cda2), and vendor-matched mem-reset timing/barriers
	 * (de1c70cb, byte-for-byte diffed against rockchip-linux/kernel
	 * drivers/soc/rockchip/pm_domains.c). All three landed cleanly and
	 * none changed the outcome, on both a 50ms rapid cadence and a
	 * genuinely cold (>10 minute idle, directly verified via
	 * runtime_status before the resume attempt) single gate->resume.
	 *
	 * The vendor BSP never hits this at all because it bypasses genpd
	 * entirely: rockchip_do_pmu_set_power_domain() is called directly,
	 * synchronously, from the vendor's own rknpu_power_on()/_off(),
	 * with no deferred/async runtime-PM machinery in between. That
	 * architecture is not portable into a mainline genpd-based driver
	 * as an incremental patch - it would mean reimplementing the whole
	 * PMU sequencing driver-side, bypassing genpd altogether.
	 *
	 * Until upstream resolves the actual genpd defect, disable runtime
	 * gating for this domain by holding it resumed for the lifetime of
	 * the module: keep this initial probe-time reference forever
	 * (balanced by a single matching put in rocket_core_fini() on
	 * module removal). rocket_device_pm_get()/put() (rocket_device.c)
	 * still runs its own per-job get/put on top of this unconditionally
	 * held reference - it just never drives the usage count to zero,
	 * so genpd never attempts the fault-prone transition. The NPU
	 * stays powered for as long as the module is loaded; that is the
	 * accepted, documented cost of a working NPU until the upstream
	 * gap is closed.
	 */

	dev_info(dev, "Rockchip NPU core %d version: %d\n", core->index, version);

	return 0;
}

void rocket_core_fini(struct rocket_core *core)
{
	/* Balance the permanent never-gate reference taken in rocket_core_init(). */
	pm_runtime_put_noidle(core->dev);

	pm_runtime_dont_use_autosuspend(core->dev);
	pm_runtime_disable(core->dev);
	if (core->iommu_dev) {
		put_device(core->iommu_dev);
		core->iommu_dev = NULL;
	}
	iommu_group_put(core->iommu_group);
	core->iommu_group = NULL;
	rocket_job_fini(core);
}

void rocket_core_reset(struct rocket_core *core)
{
	reset_control_bulk_assert(ARRAY_SIZE(core->resets), core->resets);

	udelay(10);

	reset_control_bulk_deassert(ARRAY_SIZE(core->resets), core->resets);
}
