// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * Based on parts on udlfb.c:
 * Copyright (C) 2009 its respective authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <linux/version.h>
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
#include <drm/drmP.h>
#endif
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_atomic.h>
#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_damage_helper.h>
#endif
#include "evdi_drm_drv.h"
#include "evdi_debug.h"


struct evdi_fbdev {
	struct drm_fb_helper helper;
	struct evdi_framebuffer efb;
	struct list_head fbdev_list;
	const struct fb_ops *fb_ops;
	int fb_count;
};

struct drm_clip_rect evdi_framebuffer_sanitize_rect(
				const struct evdi_framebuffer *fb,
				const struct drm_clip_rect *dirty_rect)
{
	struct drm_clip_rect rect = *dirty_rect;

	if (rect.x1 > rect.x2) {
		unsigned short tmp = rect.x2;

		EVDI_WARN("Wrong clip rect: x1 > x2\n");
		rect.x2 = rect.x1;
		rect.x1 = tmp;
	}

	if (rect.y1 > rect.y2) {
		unsigned short tmp = rect.y2;

		EVDI_WARN("Wrong clip rect: y1 > y2\n");
		rect.y2 = rect.y1;
		rect.y1 = tmp;
	}


	if (rect.x1 > fb->base.width) {
		EVDI_DEBUG("Wrong clip rect: x1 > fb.width\n");
		rect.x1 = fb->base.width;
	}

	if (rect.y1 > fb->base.height) {
		EVDI_DEBUG("Wrong clip rect: y1 > fb.height\n");
		rect.y1 = fb->base.height;
	}

	if (rect.x2 > fb->base.width) {
		EVDI_DEBUG("Wrong clip rect: x2 > fb.width\n");
		rect.x2 = fb->base.width;
	}

	if (rect.y2 > fb->base.height) {
		EVDI_DEBUG("Wrong clip rect: y2 > fb.height\n");
		rect.y2 = fb->base.height;
	}

	return rect;
}

#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
#define DRM_MODESET_ACQUIRE_INTERRUPTIBLE BIT(0)
#endif

#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
/*
 * Function taken from
 * https://lore.kernel.org/dri-devel/20180905233901.2321-5-drawat@vmware.com/
 */
static int evdi_user_framebuffer_dirty(
		struct drm_framebuffer *fb,
		__maybe_unused struct drm_file *file_priv,
		__always_unused unsigned int flags,
		__always_unused unsigned int color,
		__always_unused struct drm_clip_rect *clips,
		__always_unused unsigned int num_clips)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	struct drm_device *dev = efb->base.dev;

	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_state *state;
	struct drm_plane *plane;
	int ret = 0;

	EVDI_CHECKPT();

	drm_modeset_acquire_init(&ctx,
		/*
		 * When called from ioctl, we are interruptable,
		 * but not when called internally (ie. defio worker)
		 */
		file_priv ? DRM_MODESET_ACQUIRE_INTERRUPTIBLE :	0);

	state = drm_atomic_state_alloc(fb->dev);
	if (!state) {
		ret = -ENOMEM;
		goto out;
	}
	state->acquire_ctx = &ctx;

retry:

	drm_for_each_plane(plane, fb->dev) {
		struct drm_plane_state *plane_state;

		if (plane->state->fb != fb)
			continue;

		/*
		 * Even if it says 'get state' this function will create and
		 * initialize state if it does not exists. We use this property
		 * to force create state.
		 */
		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto out;
		}
	}

	ret = drm_atomic_commit(state);

out:
	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
#if KERNEL_VERSION(4, 15, 0) <= LINUX_VERSION_CODE
		ret = drm_modeset_backoff(&ctx);
#else
		ret = drm_modeset_backoff_interruptible(&ctx);
#endif
		if (!ret)
			goto retry;
	}

	if (state)
		drm_atomic_state_put(state);

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}
#endif

static int evdi_user_framebuffer_create_handle(struct drm_framebuffer *fb,
					       struct drm_file *file_priv,
					       unsigned int *handle)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	efb->owner = file_priv;
	return drm_gem_handle_create(file_priv, &efb->obj->base, handle);
}

static void evdi_user_framebuffer_destroy(struct drm_framebuffer *fb)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	struct drm_device *dev = efb->base.dev;
	struct evdi_device *evdi = dev->dev_private;
	struct evdi_event *event;
	
	EVDI_CHECKPT();
	if (efb->obj)
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE || defined(EL8)
		drm_gem_object_put(&efb->obj->base);
#else
		drm_gem_object_put_unlocked(&efb->obj->base);
#endif
	drm_framebuffer_cleanup(fb);

	if (!evdi_painter_is_connected(evdi->painter)) {
		kfree(efb);
		return;
	}

	event = evdi_create_event(evdi, destroy_buf, &efb->gralloc_buf_id, efb->owner);
	if (!event)
		return;

	wake_up_interruptible(&evdi->poll_ioct_wq);
	kfree(efb);
}

int evdi_atomic_helper_dirtyfb(struct drm_framebuffer *framebuffer,
		     struct drm_file *file_priv, unsigned flags,
		     unsigned color, struct drm_clip_rect *clips,
		     unsigned num_clips){
	return 0;
}

static const struct drm_framebuffer_funcs evdifb_funcs = {
	.create_handle = evdi_user_framebuffer_create_handle,
	.destroy = evdi_user_framebuffer_destroy,
#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
	.dirty = evdi_atomic_helper_dirtyfb,
#else
	.dirty = evdi_user_framebuffer_dirty,
#endif
};

static int
evdi_framebuffer_init(struct drm_device *dev,
		      struct evdi_framebuffer *efb,
		      const struct drm_mode_fb_cmd2 *mode_cmd,
		      struct evdi_gem_object *obj)
{
	efb->obj = obj;
	drm_helper_mode_fill_fb_struct(dev, &efb->base, mode_cmd);
	return drm_framebuffer_init(dev, &efb->base, &evdifb_funcs);
}

int evdi_fb_get_bpp(uint32_t format)
{
	const struct drm_format_info *info = drm_format_info(format);

	if (!info)
		return 0;
	return info->cpp[0] * 8;
}

struct drm_framebuffer *evdi_fb_user_fb_create(
					struct drm_device *dev,
					struct drm_file *file,
					const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *obj;
	struct evdi_framebuffer *efb;
	int ret;
	uint32_t size;
	int bpp = evdi_fb_get_bpp(mode_cmd->pixel_format);
	uint32_t handle;
	ssize_t bytes_read;
	struct file *memfd_file;
	int id;
	loff_t pos;

	size = mode_cmd->offsets[0] + mode_cmd->pitches[0] * mode_cmd->height;
	size = ALIGN(size, PAGE_SIZE);

	if (bpp != 32) {
		EVDI_ERROR("Unsupported bpp (%d)\n", bpp);
		return ERR_PTR(-EINVAL);
	}

	evdi_gem_create(file, dev, size, &handle);
	obj = drm_gem_object_lookup(file, handle);
	if (obj == NULL)
		return ERR_PTR(-ENOENT);
	if (size > obj->size) {
		DRM_ERROR("object size not sufficient for fb %d %zu %u %d %d\n",
			  size, obj->size, mode_cmd->offsets[0],
			  mode_cmd->pitches[0], mode_cmd->height);
		goto err_no_mem;
	}
	efb = kzalloc(sizeof(*efb), GFP_KERNEL);
	if (efb == NULL)
		goto err_no_mem;
	efb->base.obj[0] = obj;

	memfd_file = fget(mode_cmd->handles[0]);
	if (!memfd_file) {
		EVDI_ERROR("Failed to open fake fb: %d\n", mode_cmd->handles[0]);
		return ERR_PTR(-EINVAL);
	}

	pos = 0;
	bytes_read = kernel_read(memfd_file, &id, sizeof(id), &pos);
	if (bytes_read != sizeof(id)) {
		EVDI_ERROR("Failed to read id from memfd, bytes_read=%zd\n", bytes_read);
		return ERR_PTR(-EIO);
	}

	efb->gralloc_buf_id = id;
	ret = evdi_framebuffer_init(dev, efb, mode_cmd, to_evdi_bo(obj));

	if (ret)
		goto err_inval;
	return &efb->base;

 err_no_mem:
	drm_gem_object_put(obj);
	return ERR_PTR(-ENOMEM);
 err_inval:
	kfree(efb);
	drm_gem_object_put(obj);
	return ERR_PTR(-EINVAL);
}
