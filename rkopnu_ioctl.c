// SPDX-License-Identifier: GPL-2.0-only
/*
 * rkopnu: the rknpu ioctl ABI (what librknnrt calls) on top of the mainline
 * rocket driver machinery.
 *   Phase 1: RKNPU_ACTION probe responses.
 *   Phase 2: RKNPU_MEM_CREATE/MAP/DESTROY/SYNC via rocket's GEM/shmem + IOMMU.
 *   Phase 3 (TODO): RKNPU_SUBMIT onto rocket's job/register machinery.
 */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/iommu.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>
#include <drm/drm_vma_manager.h>

#include "rknpu_ioctl.h"
#include "rkopnu.h"
#include "rocket_drv.h"
#include "rocket_gem.h"

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
		args->value = 0;
		return 0;
	}
}

/*
 * MEM_CREATE: shmem-backed GEM object, mapped into the NPU IOMMU domain.
 * Mirrors rocket_ioctl_create_bo, adapted to the rknpu_mem_create struct.
 * obj_addr returns the (opaque) gem pointer that MEM_SYNC/DESTROY pass back
 * (matches the vendor ABI; librknnrt treats it as opaque).
 */
int rkopnu_ioctl_mem_create(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct rocket_file_priv *priv = file->driver_priv;
	struct rknpu_mem_create *args = data;
	struct drm_gem_shmem_object *shmem;
	struct rocket_gem_object *bo;
	struct drm_gem_object *gem;
	struct sg_table *sgt;
	int ret;

	shmem = drm_gem_shmem_create(dev, args->size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	gem = &shmem->base;
	bo = to_rocket_bo(gem);
	bo->driver_priv = priv;
	/*
	 * rkopnu multi-domain: honor the vendor iommu_domain_id instead of
	 * collapsing every allocation into one shared domain. This is the
	 * field librknnrt already fills in from rknn_matmul_info.iommu_domain_id
	 * (see ggml-rknpu2's IOMMUDomainManager) -- previously read and dropped
	 * on the floor here, which made every allocation land in domain 0
	 * regardless of what userspace asked for, capping total NPU-resident
	 * memory at one domain's ~4GiB IOVA aperture.
	 */
	bo->domain = rocket_iommu_domain_get(priv, args->iommu_domain_id);
	if (IS_ERR(bo->domain)) {
		ret = PTR_ERR(bo->domain);
		bo->domain = NULL;
		goto err;
	}
	bo->size = args->size;
	bo->offset = 0;

	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_put_domain;
	}

	mutex_lock(&bo->domain->mm_lock);
	ret = drm_mm_insert_node_generic(&bo->domain->mm, &bo->mm, bo->size,
					 PAGE_SIZE, 0, 0);
	mutex_unlock(&bo->domain->mm_lock);
	if (ret) {
		/* Most likely IOMMU VA-aperture exhaustion/fragmentation over a
		 * long-lived server (the "works for one, dies after N" mode at
		 * large contexts), or genuine exhaustion of this domain's ~4GiB
		 * budget if userspace kept packing it past capacity. Log the
		 * domain id too now that there's more than one. (Fable review,
		 * risk 3.) */
		dev_warn(dev->dev, "rkopnu: MEM_CREATE VA insert failed (%d) for %llu bytes (domain %d)\n",
			 ret, (unsigned long long)bo->size, bo->domain->id);
		goto err_put_domain;
	}

	ret = iommu_map_sgtable(bo->domain->domain, bo->mm.start, shmem->sgt,
				IOMMU_READ | IOMMU_WRITE);
	if (ret < 0 || (u64)ret < args->size) {
		ret = -ENOMEM;
		goto err_remove_node;
	}
	bo->size = ret;

	ret = drm_gem_handle_create(file, gem, &args->handle);
	if (ret)
		goto err_unmap;

	args->dma_addr = bo->mm.start;
	args->obj_addr = (u64)(uintptr_t)gem;
	args->size = bo->size;

	drm_gem_object_put(gem);
	return 0;

err_unmap:
	iommu_unmap(bo->domain->domain, bo->mm.start, bo->size);
err_remove_node:
	mutex_lock(&bo->domain->mm_lock);
	drm_mm_remove_node(&bo->mm);
	mutex_unlock(&bo->domain->mm_lock);
err_put_domain:
	/*
	 * Pre-existing single-domain code never dropped this reference on any
	 * error path (the fd's one domain outlived every failed allocation
	 * anyway). Now that a domain can be created lazily just for this
	 * failed BO, leaking it here would leak a whole IOMMU paging domain
	 * (page tables) per failed MEM_CREATE -- worse under exactly the
	 * memory-pressure conditions that make MEM_CREATE fail in the first
	 * place. Drop it explicitly.
	 */
	rocket_iommu_domain_put(bo->domain);
	bo->domain = NULL;
err:
	drm_gem_shmem_object_free(gem);
	return ret;
}

/* MEM_MAP: hand back the mmap fake-offset for a handle. */
int rkopnu_ioctl_mem_map(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct rknpu_mem_map *args = data;
	struct drm_gem_object *gem;
	int ret;

	gem = drm_gem_object_lookup(file, args->handle);
	if (!gem)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(gem);
	if (!ret)
		args->offset = drm_vma_node_offset_addr(&gem->vma_node);

	drm_gem_object_put(gem);
	return ret;
}

/* MEM_DESTROY: drop the handle; rocket_gem_bo_free() unmaps the IOMMU. */
int rkopnu_ioctl_mem_destroy(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct rknpu_mem_destroy *args = data;

	return drm_gem_handle_delete(file, args->handle);
}

/* MEM_SYNC: CPU<->device cache sync. obj_addr is the opaque gem pointer. */
int rkopnu_ioctl_mem_sync(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct rknpu_mem_sync *args = data;
	struct drm_gem_object *gem = (struct drm_gem_object *)(uintptr_t)args->obj_addr;
	struct rocket_gem_object *bo;

	if (!gem)
		return -EINVAL;
	bo = to_rocket_bo(gem);
	if (!bo->base.sgt)
		return 0;

	if (args->flags & RKNPU_MEM_SYNC_TO_DEVICE)
		dma_sync_sgtable_for_device(dev->dev, bo->base.sgt, DMA_TO_DEVICE);
	if (args->flags & RKNPU_MEM_SYNC_FROM_DEVICE)
		dma_sync_sgtable_for_cpu(dev->dev, bo->base.sgt, DMA_FROM_DEVICE);

	return 0;
}

/* SUBMIT: implemented in rocket_job.c (needs the static job/sched machinery). */
