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
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/version.h>
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_vblank.h>
#include <drm/drm_damage_helper.h>
#elif KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE
#include <drm/drm_damage_helper.h>
#else
#include <drm/drmP.h>
#endif
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_atomic_helper.h>
#include "evdi_drm.h"
#include "evdi_drm_drv.h"
#include "evdi_params.h"
#include "evdi_debug.h"
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_gem_atomic_helper.h>
#else
#include <drm/drm_gem_framebuffer_helper.h>
#endif

static void evdi_crtc_dpms(__always_unused struct drm_crtc *crtc,
			   __always_unused int mode)
{
	EVDI_CHECKPT();
}

static void evdi_crtc_disable(__always_unused struct drm_crtc *crtc)
{
	EVDI_CHECKPT();
	drm_crtc_vblank_off(crtc);
}

static void evdi_crtc_destroy(struct drm_crtc *crtc)
{
	EVDI_CHECKPT();
	drm_crtc_cleanup(crtc);
	kfree(crtc);
}

static void evdi_crtc_commit(__always_unused struct drm_crtc *crtc)
{
	EVDI_CHECKPT();
}

static void evdi_crtc_set_nofb(__always_unused struct drm_crtc *crtc)
{
}

static void evdi_crtc_atomic_flush(
	struct drm_crtc *crtc
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
	, struct drm_atomic_state *state
#else
	, __always_unused struct drm_crtc_state *old_state
#endif
	)
{
	struct drm_crtc_state *crtc_state;
	struct evdi_device *evdi;
	bool notify_mode_changed;
	bool notify_dpms;
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
#else
	crtc_state = crtc->state;
#endif
	evdi = crtc->dev->dev_private;
	notify_mode_changed = crtc_state->active &&
			   (crtc_state->mode_changed || evdi_painter_needs_full_modeset(evdi->painter));
	notify_dpms = crtc_state->active_changed || evdi_painter_needs_full_modeset(evdi->painter);

	if (notify_mode_changed)
		evdi_painter_mode_changed_notify(evdi, &crtc_state->adjusted_mode);

	if (notify_dpms)
		evdi_painter_dpms_notify(evdi->painter,
			crtc_state->active ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);

	evdi_painter_set_vblank(evdi->painter, crtc, crtc_state->event);
	evdi_painter_send_update_ready_if_needed(evdi->painter);
//	int ret = wait_event_interruptible(evdi->poll_response_ioct_wq, !evdi->poll_done);

//	if (ret < 0) {
		// Process is likely beeing killed at this point RIP btw :(, so assume there are no more events
	//	pr_err("evdi_crtc_atomic_flush: Wait interrupted by signal\n");
		//evdi->poll_event = none;
		//evdi->poll_done = false;
		//return;
	//}
	//evdi->poll_done = false;
	evdi_painter_send_vblank(evdi->painter);
	crtc_state->event = NULL;
}

static struct drm_crtc_helper_funcs evdi_helper_funcs = {
	.mode_set_nofb  = evdi_crtc_set_nofb,
	.atomic_flush   = evdi_crtc_atomic_flush,

	.dpms           = evdi_crtc_dpms,
	.commit         = evdi_crtc_commit,
	.disable        = evdi_crtc_disable
};

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
static int evdi_enable_vblank(__always_unused struct drm_crtc *crtc)
{
	return 1;
}

static void evdi_disable_vblank(__always_unused struct drm_crtc *crtc)
{
}
#endif

void evdi_vblank(struct evdi_device *evdi) {
}

int evdi_atomic_helper_page_flip(struct drm_crtc *crtc,
				struct drm_framebuffer *fb,
				struct drm_pending_vblank_event *event,
				uint32_t flags,
				struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_device *dev;
	struct evdi_device *evdi;
	struct evdi_framebuffer *efb;
	struct evdi_event *ev_event;
	dev = crtc->dev;
	evdi = dev->dev_private;
	efb = evdi->painter->scanout_fb;

	ev_event = evdi_create_event(evdi, swap_to, &efb->gralloc_buf_id, event->base.file_priv);
	if (!ev_event)
		return -ENOMEM;

	wake_up_interruptible(&evdi->poll_ioct_wq);

	return drm_atomic_helper_page_flip(crtc, fb, event, flags, ctx);
}

static const struct drm_crtc_funcs evdi_crtc_funcs = {
	.reset                  = drm_atomic_helper_crtc_reset,
	.destroy                = evdi_crtc_destroy,
	.set_config             = drm_atomic_helper_set_config,
	.page_flip              = evdi_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
	.enable_vblank          = evdi_enable_vblank,
	.disable_vblank         = evdi_disable_vblank,
#endif
};

static void evdi_plane_atomic_update(struct drm_plane *plane,
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE || defined(EL8)
				     struct drm_atomic_state *atom_state
#else
				     struct drm_plane_state *old_state
#endif
		)
{
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE || defined(EL8)
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(atom_state, plane);
#else
#endif
	struct drm_plane_state *state;
	struct evdi_device *evdi;
	struct evdi_painter *painter;
	struct drm_crtc *crtc;

	if (!plane || !plane->state) {
		EVDI_WARN("Plane state is null\n");
		return;
	}

	if (!plane->dev || !plane->dev->dev_private) {
		EVDI_WARN("Plane device is null\n");
		return;
	}

	state = plane->state;
	evdi = plane->dev->dev_private;
	painter = evdi->painter;
	crtc = state->crtc;

	if (!old_state->crtc && state->crtc)
		evdi_painter_dpms_notify(evdi->painter, DRM_MODE_DPMS_ON);
	else if (old_state->crtc && !state->crtc)
		evdi_painter_dpms_notify(evdi->painter, DRM_MODE_DPMS_OFF);

	if (state->fb) {
		struct drm_framebuffer *fb = state->fb;
		struct drm_framebuffer *old_fb = old_state->fb;
		struct evdi_framebuffer *efb = to_evdi_fb(fb);

		if (!old_fb && crtc)
			evdi_painter_force_full_modeset(painter);

		if (old_fb &&
		    fb->format && old_fb->format &&
		    fb->format->format != old_fb->format->format)
			evdi_painter_force_full_modeset(painter);

		if (fb != old_fb ||
		    evdi_painter_needs_full_modeset(painter)) {

			evdi_painter_set_scanout_buffer(painter, efb);

		};
	}
}

static const struct drm_plane_helper_funcs evdi_plane_helper_funcs = {
	.atomic_update = evdi_plane_atomic_update,
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE || defined(EL8)
	.prepare_fb = drm_gem_plane_helper_prepare_fb
#else
	.prepare_fb = drm_gem_fb_prepare_fb
#endif
};

static const struct drm_plane_funcs evdi_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const uint32_t formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
};

static struct drm_plane *evdi_create_plane(
		struct drm_device *dev,
		enum drm_plane_type type,
		const struct drm_plane_helper_funcs *helper_funcs)
{
	struct drm_plane *plane;
	int ret;
	char *plane_type = "primary";

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);
	if (plane == NULL) {
		EVDI_ERROR("Failed to allocate %s plane\n", plane_type);
		return NULL;
	}
	plane->format_default = true;

	ret = drm_universal_plane_init(dev,
				       plane,
				       0xFF,
				       &evdi_plane_funcs,
				       formats,
				       ARRAY_SIZE(formats),
				       NULL,
				       type,
				       NULL
				       );

	if (ret) {
		EVDI_ERROR("Failed to initialize %s plane\n", plane_type);
		kfree(plane);
		return NULL;
	}

	drm_plane_helper_add(plane, helper_funcs);

	return plane;
}

static int evdi_crtc_init(struct drm_device *dev)
{
	struct drm_crtc *crtc = NULL;
	struct drm_plane *primary_plane = NULL;
	int status = 0;

	EVDI_CHECKPT();
	crtc = kzalloc(sizeof(struct drm_crtc), GFP_KERNEL);
	if (crtc == NULL)
		return -ENOMEM;

	primary_plane = evdi_create_plane(dev, DRM_PLANE_TYPE_PRIMARY,
					  &evdi_plane_helper_funcs);

#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
	drm_plane_enable_fb_damage_clips(primary_plane);
#endif

	status = drm_crtc_init_with_planes(dev, crtc,
					   primary_plane, NULL,
					   &evdi_crtc_funcs,
					   NULL
					   );

	EVDI_DEBUG("drm_crtc_init: %d p%p\n", status, primary_plane);
	drm_crtc_helper_add(crtc, &evdi_helper_funcs);

	return 0;
}

int evdi_atomic_helper_commit(struct drm_device * dev, struct drm_atomic_state * state, bool nonblock)
{
	EVDI_VERBOSE("evdi_atomic_helper_commit\n");
	return drm_atomic_helper_commit(dev, state, nonblock);
}
static const struct drm_mode_config_funcs evdi_mode_funcs = {
	.fb_create = evdi_fb_user_fb_create,
#if KERNEL_VERSION(6, 11, 0) < LINUX_VERSION_CODE || defined(EL8)
#else
	.output_poll_changed = NULL,
#endif
	.atomic_commit = evdi_atomic_helper_commit,
	.atomic_check = drm_atomic_helper_check
};

void evdi_modeset_init(struct drm_device *dev)
{
	struct drm_encoder *encoder;

	EVDI_CHECKPT();

	drm_mode_config_init(dev);

	dev->mode_config.min_width = 64;
	dev->mode_config.min_height = 64;

	dev->mode_config.max_width = 7680;
	dev->mode_config.max_height = 4320;

	dev->mode_config.prefer_shadow = 0;
	dev->mode_config.preferred_depth = 24;

	dev->mode_config.funcs = &evdi_mode_funcs;

	evdi_crtc_init(dev);

	encoder = evdi_encoder_init(dev);

	evdi_connector_init(dev, encoder);

	drm_mode_config_reset(dev);
}

void evdi_modeset_cleanup(__maybe_unused struct drm_device *dev)
{
#if KERNEL_VERSION(5, 8, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
	drm_mode_config_cleanup(dev);
#endif
}
