/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright 2024-2025 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_DRV_H__
#define __ROCKET_DRV_H__

#include <drm/drm_mm.h>
#include <drm/gpu_scheduler.h>
#include <linux/iosys-map.h>

#include "rocket_device.h"

extern const struct dev_pm_ops rocket_pm_ops;

/*
 * rkopnu multi-domain: the vendor rknpu ABI (rknpu_mem_create.iommu_domain_id,
 * rknpu_submit.iommu_domain_id -- see rknpu_ioctl.h) lets a client spread
 * allocations across multiple IOMMU paging domains. Each domain is a fully
 * independent page-table root, so each gets its own ~4GiB IOVA aperture (the
 * vendor rknn_matmul API's B-matrix offsets are int32, which is the real
 * ceiling -- not the IOMMU hardware, which is per-domain). Before this, every
 * allocation on a given fd landed in the single domain created at open(), so
 * userspace's domain-spreading (ggml-rknpu2's IOMMUDomainManager) was a
 * no-op and any model with >~4GiB of NPU-resident weights hit ENOSPC. Each
 * domain now owns its own drm_mm IOVA allocator (mm/mm_lock below), matching
 * its own independent address space -- a shared allocator across domains
 * would double-book identical IOVA ranges in different page tables, which is
 * fine as far as the IOMMU is concerned but wrong for this driver's
 * bookkeeping (VA aperture accounting, exhaustion diagnostics).
 */
struct rocket_iommu_domain {
	struct iommu_domain *domain;
	struct kref kref;

	/* vendor iommu_domain_id this slot was created for (0..ROCKET_MAX_IOMMU_DOMAINS-1) */
	s32 id;

	struct drm_mm mm;
	struct mutex mm_lock;
};

/*
 * Matches ggml-rknpu2's IOMMUDomainManager, which dynamically assigns
 * domain ids 0..15 (see assign_domain_memory() in ggml-rknpu2.cpp). Any
 * out-of-range or unset (old-caller) id falls back to domain 0.
 */
#define ROCKET_MAX_IOMMU_DOMAINS 16

struct rocket_file_priv {
	struct rocket_device *rdev;

	/*
	 * Lazily-created pool of per-fd IOMMU domains, indexed by the vendor
	 * iommu_domain_id. Slots are NULL until first referenced by a
	 * MEM_CREATE or SUBMIT that names them. domains_lock protects the
	 * pool array itself (slot install/lookup); each domain's own
	 * mm_lock protects its drm_mm, and its kref its lifetime.
	 */
	struct mutex domains_lock;
	struct rocket_iommu_domain *domains[ROCKET_MAX_IOMMU_DOMAINS];

	struct drm_sched_entity sched_entity;

	/* rkopnu correctness: one drm_sched entity bound to each single core, so a
	 * SUBMIT is pinned to the physical core selected by core_mask (the vendor
	 * commits the PC to that exact core, whose subcore_task[] slot is the only
	 * one librknnrt filled). Letting drm_sched pick an arbitrary core reads the
	 * wrong subcore slot -> out-of-bounds task index -> oops. */
	struct drm_sched_entity core_entity[3];
};

/*
 * Returns a referenced (kref_get'd) domain for domain_id, creating it on
 * first use. Caller must rocket_iommu_domain_put() when done. Can return
 * ERR_PTR (e.g. -ENOMEM from iommu_paging_domain_alloc) since domain
 * creation is now lazy instead of guaranteed-at-open -- callers that were
 * written against the old infallible single-domain version need updating,
 * not just recompiling.
 */
struct rocket_iommu_domain *rocket_iommu_domain_get(struct rocket_file_priv *rocket_priv, s32 domain_id);
void rocket_iommu_domain_put(struct rocket_iommu_domain *domain);

/* Tears down every domain this fd ever created. Call from postclose, after
 * DRM core has already released this file's GEM objects (so every domain's
 * drm_mm is provably empty -- see rocket_open()/rocket_postclose() in
 * rocket_drv.c for the ordering this relies on). */
void rocket_iommu_domains_close(struct rocket_file_priv *rocket_priv);

#endif
