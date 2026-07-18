// SPDX-License-Identifier: GPL-2.0-only
/*
 * rkopnu: the rknpu ioctl ABI (what librknnrt calls) on top of the mainline
 * rocket driver machinery. Phase 1: ACTION probe responses; MEM and SUBMIT stubbed.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>

#include "rknpu_ioctl.h"
#include "rkopnu.h"

int rkopnu_ioctl_action(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct rknpu_action *args = data;

	switch (args->flags) {
	case RKNPU_GET_HW_VERSION:
		args->value = 1179210309u; /* RK3588 rknpu core version (from dmesg); Phase 3: read live from reg */
		return 0;
	case RKNPU_GET_DRV_VERSION:
		args->value = RKNPU_GET_DRV_VERSION_CODE(0, 9, 8);
		return 0;
	case RKNPU_GET_IOMMU_EN:
		args->value = 1;
		return 0;
	case RKNPU_GET_IOMMU_DOMAIN_ID:
		args->value = 0;
		return 0;
	case RKNPU_GET_FREQ:
		args->value = 1000000000u; /* 1 GHz */
		return 0;
	case RKNPU_POWER_ON:
	case RKNPU_POWER_OFF:
	case RKNPU_ACT_RESET:
		return 0;
	case RKNPU_GET_TOTAL_SRAM_SIZE:
	case RKNPU_GET_FREE_SRAM_SIZE:
		args->value = 0;
		return 0;
	default:
		/* permissive during probe: unhandled actions succeed with 0 */
		args->value = 0;
		return 0;
	}
}

int rkopnu_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file)      { return -ENOSYS; }
int rkopnu_ioctl_mem_create(struct drm_device *dev, void *data, struct drm_file *file)  { return -ENOSYS; }
int rkopnu_ioctl_mem_map(struct drm_device *dev, void *data, struct drm_file *file)     { return -ENOSYS; }
int rkopnu_ioctl_mem_destroy(struct drm_device *dev, void *data, struct drm_file *file) { return -ENOSYS; }
int rkopnu_ioctl_mem_sync(struct drm_device *dev, void *data, struct drm_file *file)    { return -ENOSYS; }
