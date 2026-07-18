/* SPDX-License-Identifier: GPL-2.0-only */
/* rkopnu: rknpu-ABI ioctl handler prototypes (rkopnu_ioctl.c). */
#ifndef __RKOPNU_H__
#define __RKOPNU_H__
struct drm_device; struct drm_file;
int rkopnu_ioctl_action(struct drm_device *dev, void *data, struct drm_file *file);
int rkopnu_ioctl_submit(struct drm_device *dev, void *data, struct drm_file *file);
int rkopnu_ioctl_mem_create(struct drm_device *dev, void *data, struct drm_file *file);
int rkopnu_ioctl_mem_map(struct drm_device *dev, void *data, struct drm_file *file);
int rkopnu_ioctl_mem_destroy(struct drm_device *dev, void *data, struct drm_file *file);
int rkopnu_ioctl_mem_sync(struct drm_device *dev, void *data, struct drm_file *file);
#endif
