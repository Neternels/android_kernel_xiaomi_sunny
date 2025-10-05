// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013 - 2020 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include "linux/thread_info.h"
#include "linux/mm.h"
#include <linux/version.h>
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_file.h>
#include <drm/drm_vblank.h>
#include <drm/drm_ioctl.h>
#elif KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE
#else
#include <drm/drmP.h>
#endif
#include "evdi_drm.h"
#include "evdi_drm_drv.h"
#include "evdi_params.h"
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/compiler.h>
#include <linux/platform_device.h>
#include <linux/completion.h>

#include <linux/dma-buf.h>
#include <linux/vt_kern.h>
#include <linux/workqueue.h>
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE
#include <linux/compiler_attributes.h>
#endif

#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE || defined(EL9)
MODULE_IMPORT_NS("DMA_BUF");
#endif

#if KERNEL_VERSION(5, 1, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_probe_helper.h>
#endif

struct evdi_event_update_ready_pending {
	struct drm_pending_event base;
	struct drm_evdi_event_update_ready update_ready;
};

struct evdi_event_dpms_pending {
	struct drm_pending_event base;
	struct drm_evdi_event_dpms dpms;
};

struct evdi_event_mode_changed_pending {
	struct drm_pending_event base;
	struct drm_evdi_event_mode_changed mode_changed;
};

struct evdi_event_crtc_state_pending {
	struct drm_pending_event base;
	struct drm_evdi_event_crtc_state crtc_state;
};

#define painter_lock(painter)                           \
	do {                                            \
		EVDI_VERBOSE("Painter lock\n");         \
		mutex_lock(&painter->lock);             \
	} while (0)

#define painter_unlock(painter)                         \
	do {                                            \
		EVDI_VERBOSE("Painter unlock\n");       \
		mutex_unlock(&painter->lock);           \
	} while (0)

bool evdi_painter_is_connected(struct evdi_painter *painter)
{
	return painter ? painter->is_connected : false;
}

static void evdi_painter_add_event_to_pending_list(
	struct evdi_painter *painter,
	struct drm_pending_event *event)
{
	unsigned long flags;
	struct drm_pending_event *last_event = NULL;
	struct list_head *list = NULL;

	spin_lock_irqsave(&painter->drm_device->event_lock, flags);

	list = &painter->pending_events;
	if (!list_empty(list)) {
		last_event =
		  list_last_entry(list, struct drm_pending_event, link);
	}

	list_add_tail(&event->link, list);

	spin_unlock_irqrestore(&painter->drm_device->event_lock, flags);
}

static void evdi_send_events_now(struct work_struct *work);
static void evdi_send_events_retry(struct work_struct *work);

static bool evdi_painter_flush_pending_events(struct evdi_painter *painter)
{
	unsigned long flags;
	struct drm_pending_event *event_to_be_sent = NULL;
	struct list_head *list = NULL;
	bool has_space = false;
	bool flushed_all = false;

	spin_lock_irqsave(&painter->drm_device->event_lock, flags);

	list = &painter->pending_events;
	while ((event_to_be_sent = list_first_entry_or_null(
			list, struct drm_pending_event, link))) {
		has_space = drm_event_reserve_init_locked(painter->drm_device,
		    painter->drm_filp, event_to_be_sent,
		    event_to_be_sent->event) == 0;
		if (has_space) {
			list_del_init(&event_to_be_sent->link);
			drm_send_event_locked(painter->drm_device,
					      event_to_be_sent);
		} else
			break;
	}

	flushed_all = list_empty(&painter->pending_events);
	spin_unlock_irqrestore(&painter->drm_device->event_lock, flags);

	return flushed_all;
}

static void evdi_painter_send_event(struct evdi_painter *painter,
				    struct drm_pending_event *event)
{
	if (!event) {
		EVDI_ERROR("Null drm event!");
		return;
	}

	if (!painter->drm_filp) {
		EVDI_VERBOSE("Painter is not connected!");
		drm_event_cancel_free(painter->drm_device, event);
		return;
	}

	if (!painter->drm_device) {
		EVDI_WARN("Painter is not connected to drm device!");
		drm_event_cancel_free(painter->drm_device, event);
		return;
	}

	if (!painter->is_connected) {
		EVDI_WARN("Painter is not connected!");
		drm_event_cancel_free(painter->drm_device, event);
		return;
	}

	evdi_painter_add_event_to_pending_list(painter, event);

	if (evdi_painter_flush_pending_events(painter))
		return;
	schedule_work(&painter->send_events_now);
}

static struct drm_pending_event *create_update_ready_event(void)
{
	struct evdi_event_update_ready_pending *event;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		EVDI_ERROR("Failed to create update ready event");
		return NULL;
	}

	event->update_ready.base.type = DRM_EVDI_EVENT_UPDATE_READY;
	event->update_ready.base.length = sizeof(event->update_ready);
	event->base.event = &event->update_ready.base;
	return &event->base;
}

static void evdi_painter_send_update_ready(struct evdi_painter *painter)
{
	struct drm_pending_event *event = create_update_ready_event();

	evdi_painter_send_event(painter, event);
}

static struct drm_pending_event *create_dpms_event(int mode)
{
	struct evdi_event_dpms_pending *event;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		EVDI_ERROR("Failed to create dpms event");
		return NULL;
	}

	event->dpms.base.type = DRM_EVDI_EVENT_DPMS;
	event->dpms.base.length = sizeof(event->dpms);
	event->dpms.mode = mode;
	event->base.event = &event->dpms.base;
	return &event->base;
}

static void evdi_painter_send_dpms(struct evdi_painter *painter, int mode)
{
	struct drm_pending_event *event = create_dpms_event(mode);

	EVDI_TEST_HOOK(evdi_testhook_painter_send_dpms(mode));
	evdi_painter_send_event(painter, event);
}

static struct drm_pending_event *create_mode_changed_event(
	struct drm_display_mode *current_mode,
	int32_t bits_per_pixel,
	uint32_t pixel_format)
{
	struct evdi_event_mode_changed_pending *event;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		EVDI_ERROR("Failed to create mode changed event");
		return NULL;
	}

	event->mode_changed.base.type = DRM_EVDI_EVENT_MODE_CHANGED;
	event->mode_changed.base.length = sizeof(event->mode_changed);

	event->mode_changed.hdisplay = current_mode->hdisplay;
	event->mode_changed.vdisplay = current_mode->vdisplay;
	event->mode_changed.vrefresh = drm_mode_vrefresh(current_mode);
	event->mode_changed.bits_per_pixel = bits_per_pixel;
	event->mode_changed.pixel_format = pixel_format;

	event->base.event = &event->mode_changed.base;
	return &event->base;
}

static void evdi_painter_send_mode_changed(
	struct evdi_painter *painter,
	struct drm_display_mode *current_mode,
	int32_t bits_per_pixel,
	uint32_t pixel_format)
{
	struct drm_pending_event *event = create_mode_changed_event(
		current_mode, bits_per_pixel, pixel_format);

	evdi_painter_send_event(painter, event);
}

struct drm_clip_rect evdi_painter_framebuffer_size(
	struct evdi_painter *painter)
{
	struct drm_clip_rect rect = {0, 0, 0, 0};
	struct evdi_framebuffer *efb = NULL;

	if (painter == NULL) {
		EVDI_WARN("Painter is not connected!");
		return rect;
	}

	painter_lock(painter);
	efb = painter->scanout_fb;
	if (!efb) {
		if (painter->is_connected)
			EVDI_WARN("Scanout buffer not set.");
		goto unlock;
	}
	rect.x1 = 0;
	rect.y1 = 0;
	rect.x2 = efb->base.width;
	rect.y2 = efb->base.height;
unlock:
	painter_unlock(painter);
	return rect;
}

static void evdi_send_vblank(struct drm_crtc *crtc,
			     struct drm_pending_vblank_event *vblank)
{
	if (crtc && vblank) {
		unsigned long flags = 0;

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, vblank);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

void evdi_painter_send_vblank(struct evdi_painter *painter)
{
	EVDI_CHECKPT();

	evdi_send_vblank(painter->crtc, painter->vblank);

	painter->crtc = NULL;
	painter->vblank = NULL;
	//Attempt flush at vblank for reducing latency and wakeups
	(void)evdi_painter_flush_pending_events(painter);
}

void evdi_painter_set_vblank(
	struct evdi_painter *painter,
	struct drm_crtc *crtc,
	struct drm_pending_vblank_event *vblank)
{
	EVDI_CHECKPT();

	if (painter) {
		painter_lock(painter);

		evdi_painter_send_vblank(painter);

		if (painter->num_dirts > 0 && painter->is_connected) {
			painter->crtc = crtc;
			painter->vblank = vblank;
		} else {
			evdi_send_vblank(crtc, vblank);
		}

		painter_unlock(painter);
	} else {
		evdi_send_vblank(crtc, vblank);
	}
}

void evdi_painter_send_update_ready_if_needed(struct evdi_painter *painter)
{
	EVDI_CHECKPT();
	if (painter) {
		painter_lock(painter);
#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE || defined(EL8)
		if (painter->was_update_requested && painter->num_dirts) {
#else
		if (painter->was_update_requested) {
#endif
			evdi_painter_send_update_ready(painter);
			painter->was_update_requested = false;
		}

		painter_unlock(painter);
	} else {
		EVDI_WARN("Painter does not exist!");
	}
}

static const char * const dpms_str[] = { "on", "standby", "suspend", "off" };

void evdi_painter_dpms_notify(struct evdi_painter *painter, int mode)
{
	const char *mode_str;

	if (!painter) {
		EVDI_WARN("Painter does not exist!");
		return;
	}

	if (!painter->is_connected)
		return;

	switch (mode) {
	case DRM_MODE_DPMS_ON:
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE
		fallthrough;
#endif
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		mode_str = dpms_str[mode];
		break;
	default:
		mode_str = "unknown";
	};
	EVDI_INFO("(card%d) Notifying display power state: %s\n",
		   painter->drm_device->primary->index, mode_str);
	evdi_painter_send_dpms(painter, mode);
}

static void evdi_log_pixel_format(uint32_t pixel_format,
		char *buf, size_t size)
{
#if KERNEL_VERSION(5, 14, 0) <= LINUX_VERSION_CODE || defined(EL8)
	snprintf(buf, size, "pixel format %p4cc", &pixel_format);
#else
	struct drm_format_name_buf format_name;

	drm_get_format_name(pixel_format, &format_name);
	snprintf(buf, size, "pixel format %s", format_name.str);
#endif
}

void evdi_painter_mode_changed_notify(struct evdi_device *evdi,
				      struct drm_display_mode *new_mode)
{
	struct evdi_painter *painter = evdi->painter;
	struct drm_framebuffer *fb;
	int bits_per_pixel;
	uint32_t pixel_format;
	char buf[100];

	if (painter == NULL)
		return;

	painter_lock(painter);
	fb = &painter->scanout_fb->base;
	if (fb == NULL) {
		painter_unlock(painter);
		return;
	}

	bits_per_pixel = fb->format->cpp[0] * 8;
	pixel_format = fb->format->format;
	painter_unlock(painter);

	evdi_log_pixel_format(pixel_format, buf, sizeof(buf));
	EVDI_INFO("(card%d) Notifying mode changed: %dx%d@%d; bpp %d; %s",
		   evdi->dev_index, new_mode->hdisplay, new_mode->vdisplay,
		   drm_mode_vrefresh(new_mode), bits_per_pixel, buf);

	evdi_painter_send_mode_changed(painter,
				       new_mode,
				       bits_per_pixel,
				       pixel_format);
	painter->needs_full_modeset = false;
}

static void evdi_painter_events_cleanup(struct evdi_painter *painter)
{
	struct drm_pending_event *event, *temp;
	unsigned long flags;

	spin_lock_irqsave(&painter->drm_device->event_lock, flags);
	list_for_each_entry_safe(event, temp, &painter->pending_events, link) {
		list_del(&event->link);
		kfree(event);
	}
	spin_unlock_irqrestore(&painter->drm_device->event_lock, flags);

	cancel_delayed_work_sync(&painter->send_events_retry);
	cancel_work_sync(&painter->send_events_now);
}

static int
evdi_painter_connect(struct evdi_device *evdi,
		     int width, int height, int refresh_rate,
		     struct drm_file *file, __always_unused int dev_index)
{
	struct evdi_painter *painter = evdi->painter;
	char buf[100];

	evdi_log_process(buf, sizeof(buf));

	if (painter->drm_filp)
		EVDI_WARN("(card%d) Double connect - replacing %p with %p\n",
			  evdi->dev_index, painter->drm_filp, file);

	painter_lock(painter);

	painter->width = width;
	painter->height = height;
	painter->refresh_rate = refresh_rate;
	painter->drm_filp = file;
	painter->is_connected = true;
	painter->needs_full_modeset = true;

	painter_unlock(painter);

	if (evdi)
		atomic_set(&evdi->poll_stopping, 0);

	EVDI_INFO("(card%d) Connected with %s\n", evdi->dev_index, buf);

	drm_kms_helper_hotplug_event(evdi->ddev);

	return 0;
}

static int evdi_painter_disconnect(struct evdi_device *evdi,
	struct drm_file *file)
{
	struct evdi_painter *painter = evdi->painter;
	char buf[100];

	EVDI_CHECKPT();

	painter_lock(painter);

	if (file != painter->drm_filp) {
		painter_unlock(painter);
		return -EFAULT;
	}

	if (painter->scanout_fb) {
		drm_framebuffer_put(&painter->scanout_fb->base);
		painter->scanout_fb = NULL;
	}

	painter->is_connected = false;

	evdi_log_process(buf, sizeof(buf));
	EVDI_INFO("(card%d) Disconnected from %s\n", evdi->dev_index, buf);
	evdi_painter_events_cleanup(painter);

	evdi_painter_send_vblank(painter);

	painter->drm_filp = NULL;

	painter->was_update_requested = false;

	painter_unlock(painter);

	if (evdi) {
		atomic_set(&evdi->poll_stopping, 1);
		wake_up_interruptible_all(&evdi->poll_ioct_wq);
		wake_up_all(&evdi->poll_response_ioct_wq);
	}

	drm_helper_hpd_irq_event(evdi->ddev);
	return 0;
}

void evdi_painter_close(struct evdi_device *evdi, struct drm_file *file)
{
	EVDI_CHECKPT();

	if (evdi->painter && file == evdi->painter->drm_filp)
		evdi_painter_disconnect(evdi, file);
}

int evdi_painter_connect_ioctl(struct drm_device *drm_dev, void *data,
			       struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct evdi_painter *painter = evdi->painter;
	struct drm_evdi_connect *cmd = data;
	int ret;
	EVDI_CHECKPT();
	if (painter) {
		if (cmd->connected)
			ret = evdi_painter_connect(evdi,
					     cmd->width,
					     cmd->height,
					     cmd->refresh_rate,
					     file,
					     cmd->dev_index);
		else
			ret = evdi_painter_disconnect(evdi, file);

		if (ret) {
			EVDI_WARN("(card%d)(pid=%d) disconnect failed\n",
				  evdi->dev_index, (int)task_pid_nr(current));
		}
		return ret;
	}
	EVDI_WARN("(card%d) Painter does not exist!", evdi->dev_index);
	return -ENODEV;
}

int evdi_painter_request_update_ioctl(struct drm_device *drm_dev,
				      __always_unused void *data,
				      __always_unused struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;
	struct evdi_painter *painter = evdi->painter;
	int result = 0;

	if (painter) {
		painter_lock(painter);

		if (painter->was_update_requested) {
			EVDI_WARN
			  ("(card%d) Update was already requested - ignoring\n",
			   evdi->dev_index);
		} else {
			if (painter->num_dirts > 0)
				result = 1;
			else
				painter->was_update_requested = true;
		}

		painter_unlock(painter);

		return result;
	} else {
		return -ENODEV;
	}
}

static void evdi_send_events_now(struct work_struct *work)
{
	struct evdi_painter *painter =
		container_of(work, struct evdi_painter, send_events_now);
	if (evdi_painter_flush_pending_events(painter))
		return;

	//If flush fails, back off without stacking
	if (!delayed_work_pending(&painter->send_events_retry))
		schedule_delayed_work(&painter->send_events_retry, msecs_to_jiffies(2));
}

static void evdi_send_events_retry(struct work_struct *work)
{
	struct evdi_painter *painter =
		container_of(to_delayed_work(work), struct evdi_painter, send_events_retry);
	if (evdi_painter_flush_pending_events(painter))
		return;

	// Ensure forward progress in worst case
	schedule_delayed_work(&painter->send_events_retry, msecs_to_jiffies(2));
}

int evdi_painter_init(struct evdi_device *dev)
{
	EVDI_CHECKPT();
	dev->painter = kzalloc(sizeof(*dev->painter), GFP_KERNEL);
	if (dev->painter) {
		mutex_init(&dev->painter->lock);
		dev->painter->width = 0;
		dev->painter->height = 0;
		dev->painter->refresh_rate = 0;
		dev->painter->needs_full_modeset = true;
		dev->painter->crtc = NULL;
		dev->painter->vblank = NULL;
		dev->painter->drm_device = dev->ddev;

		INIT_LIST_HEAD(&dev->painter->pending_events);
		INIT_WORK(&dev->painter->send_events_now,
			evdi_send_events_now);
		INIT_DELAYED_WORK(&dev->painter->send_events_retry,
			evdi_send_events_retry);
		return 0;
	}
	return -ENOMEM;
}

void evdi_painter_cleanup(struct evdi_painter *painter)
{
	EVDI_CHECKPT();
	if (!painter) {
		EVDI_WARN("Painter does not exist\n");
		return;
	}

	painter_lock(painter);
	painter->width = 0;
	painter->height = 0;
	painter->refresh_rate = 0;
	if (painter->scanout_fb)
		drm_framebuffer_put(&painter->scanout_fb->base);
	painter->scanout_fb = NULL;

	evdi_painter_send_vblank(painter);

	evdi_painter_events_cleanup(painter);

	painter->drm_device = NULL;
	painter_unlock(painter);
	kfree(painter);
}

void evdi_painter_set_scanout_buffer(struct evdi_painter *painter,
				     struct evdi_framebuffer *newfb)
{
	struct evdi_framebuffer *oldfb = NULL;

	if (newfb)
		drm_framebuffer_get(&newfb->base);

	painter_lock(painter);

	oldfb = painter->scanout_fb;
	painter->scanout_fb = newfb;

	painter_unlock(painter);

	if (oldfb)
		drm_framebuffer_put(&oldfb->base);
}

bool evdi_painter_needs_full_modeset(struct evdi_painter *painter)
{
	return painter ? painter->needs_full_modeset : false;
}


void evdi_painter_force_full_modeset(struct evdi_painter *painter)
{
	if (painter)
		painter->needs_full_modeset = true;
}
