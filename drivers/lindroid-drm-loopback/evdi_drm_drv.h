/* SPDX-License-Identifier: GPL-2.0-only
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

#ifndef EVDI_DRV_H
#define EVDI_DRV_H

#include <linux/module.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0) || defined(EL8) || defined(EL9)
#include <linux/xarray.h>
#define EVDI_HAVE_XARRAY	1
#else
#undef EVDI_HAVE_XARRAY
#endif
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vblank.h>
#else
#include <drm/drmP.h>
#endif
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE || defined(EL8) || defined(EL9)
#include <drm/drm_framebuffer.h>
#else
#include <drm/drm_irq.h>
#endif
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_gem.h>
#include <drm/drm_framebuffer.h>

#include "evdi_debug.h"
#include "evdi_drm.h"
#include "tests/evdi_test.h"

#define EVDI_WAIT_TIMEOUT (10*HZ)
#define EVDI_MAX_FDS   32
#define EVDI_MAX_INTS  256

struct evdi_fbdev;
struct evdi_painter;

struct evdi_device {
	struct drm_device *ddev;
	struct drm_connector *conn;

	uint32_t pixel_area_limit;
	uint32_t pixel_per_second_limit;

	struct evdi_fbdev *fbdev;
	struct evdi_painter *painter;

	int dev_index;
	enum poll_event_type poll_event;
	void *poll_data;
	int poll_data_size;
	wait_queue_head_t poll_ioct_wq;
	wait_queue_head_t poll_response_ioct_wq;
	struct mutex poll_lock;
	atomic_t poll_stopping;
	struct completion poll_completion;
	int last_buf_add_id;
	void *last_got_buff;
	spinlock_t event_lock;
	struct list_head event_queue;
#if defined(EVDI_HAVE_XARRAY)
	struct xarray event_xa;
#else
	struct idr event_idr;
#endif
	atomic_t next_event_id;
};

struct evdi_event {
	enum poll_event_type type;
	void *data;
	void *reply_data;
	int poll_id;
	bool on_queue;
	struct drm_file *owner;
	struct list_head list;
	struct evdi_device *evdi;
#if defined(EVDI_HAVE_XARRAY)
	struct rcu_head	rcu;
#endif
};

struct evdi_gem_object {
	struct drm_gem_object base;
	struct page **pages;
	atomic_t pages_pin_count;
	struct mutex pages_lock;
	void *vmapping;
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(EL8)
	bool vmap_is_iomem;
#endif
	bool vmap_is_vmram;
	struct sg_table *sg;
};

#define to_evdi_bo(x) container_of(x, struct evdi_gem_object, base)

struct evdi_framebuffer {
	struct drm_framebuffer base;
	struct evdi_gem_object *obj;
	bool active;
	int gralloc_buf_id;
	struct drm_file *owner;
};

#define MAX_DIRTS 16

struct evdi_painter {
	bool is_connected;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_rate;

	struct mutex lock;
	struct drm_clip_rect dirty_rects[MAX_DIRTS];
	int num_dirts;
	struct evdi_framebuffer *scanout_fb;

	struct drm_file *drm_filp;
	struct drm_device *drm_device;

	bool was_update_requested;
	bool needs_full_modeset;
	struct drm_crtc *crtc;
	struct drm_pending_vblank_event *vblank;

	struct list_head pending_events;
	struct work_struct send_events_now;
	struct delayed_work send_events_retry;

	struct notifier_block vt_notifier;
	int fg_console;
};

struct evdi_gralloc_buf {
	int version;
	int numFds;
	int numInts;
	struct file *data_files[EVDI_MAX_FDS];
	int data_ints[EVDI_MAX_INTS];
	struct file *memfd_file;
};

struct evdi_gralloc_buf_user {
	int version;
	int numFds;
	int numInts;
	int data[128];
};

#define to_evdi_fb(x) container_of(x, struct evdi_framebuffer, base)


struct evdi_event *evdi_create_event(struct evdi_device *evdi, enum poll_event_type type, void *data, struct drm_file *file);
void evdi_event_unlink_and_free(struct evdi_device *evdi, struct evdi_event *event);

int evdi_poll_ioctl(struct drm_device *drm_dev, void *data,
                    struct drm_file *file);

/* modeset */
void evdi_modeset_init(struct drm_device *dev);
void evdi_modeset_cleanup(struct drm_device *dev);
int evdi_connector_init(struct drm_device *dev, struct drm_encoder *encoder);

struct drm_encoder *evdi_encoder_init(struct drm_device *dev);

int evdi_driver_open(struct drm_device *drm_dev, struct drm_file *file);
void evdi_driver_preclose(struct drm_device *dev, struct drm_file *file_priv);
void evdi_driver_postclose(struct drm_device *dev, struct drm_file *file_priv);

struct drm_framebuffer *evdi_fb_user_fb_create(
				struct drm_device *dev,
				struct drm_file *file,
				const struct drm_mode_fb_cmd2 *mode_cmd);
int evdi_gem_create(struct drm_file *file,
		struct drm_device *dev, uint64_t size, uint32_t *handle_p);
int evdi_dumb_create(struct drm_file *file_priv,
		     struct drm_device *dev, struct drm_mode_create_dumb *args);
int evdi_gem_mmap(struct drm_file *file_priv,
		  struct drm_device *dev, uint32_t handle, uint64_t *offset);

void evdi_gem_free_object(struct drm_gem_object *gem_obj);
struct evdi_gem_object *evdi_gem_alloc_object(struct drm_device *dev,
					      size_t size);
uint32_t evdi_gem_object_handle_lookup(struct drm_file *filp,
				      struct drm_gem_object *obj);

struct sg_table *evdi_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *
evdi_prime_import_sg_table(struct drm_device *dev,
			   struct dma_buf_attachment *attach,
			   struct sg_table *sg);

int evdi_gem_vmap(struct evdi_gem_object *obj);
void evdi_gem_vunmap(struct evdi_gem_object *obj);
int evdi_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma);

#if KERNEL_VERSION(4, 17, 0) <= LINUX_VERSION_CODE
vm_fault_t evdi_gem_fault(struct vm_fault *vmf);
#else
int evdi_gem_fault(struct vm_fault *vmf);
#endif

bool evdi_painter_is_connected(struct evdi_painter *painter);
void evdi_painter_close(struct evdi_device *evdi, struct drm_file *file);
void evdi_painter_send_vblank(struct evdi_painter *painter);
void evdi_painter_set_vblank(struct evdi_painter *painter,
			     struct drm_crtc *crtc,
			     struct drm_pending_vblank_event *vblank);
void evdi_painter_send_update_ready_if_needed(struct evdi_painter *painter);
void evdi_painter_dpms_notify(struct evdi_painter *painter, int mode);
void evdi_painter_mode_changed_notify(struct evdi_device *evdi,
				      struct drm_display_mode *mode);
unsigned int evdi_painter_poll(struct file *filp,
			       struct poll_table_struct *wait);

int evdi_painter_status_ioctl(struct drm_device *drm_dev, void *data,
			      struct drm_file *file);
int evdi_painter_connect_ioctl(struct drm_device *drm_dev, void *data,
			       struct drm_file *file);
int evdi_painter_request_update_ioctl(struct drm_device *drm_dev, void *data,
				      struct drm_file *file);

int evdi_painter_init(struct evdi_device *evdi);
void evdi_painter_cleanup(struct evdi_painter *painter);
void evdi_painter_set_scanout_buffer(struct evdi_painter *painter,
				     struct evdi_framebuffer *buffer);

struct drm_clip_rect evdi_framebuffer_sanitize_rect(
			const struct evdi_framebuffer *fb,
			const struct drm_clip_rect *rect);

struct drm_device *evdi_drm_device_create(struct device *parent);
int evdi_drm_device_remove(struct drm_device *dev);

bool evdi_painter_needs_full_modeset(struct evdi_painter *painter);
void evdi_painter_force_full_modeset(struct evdi_painter *painter);
struct drm_clip_rect evdi_painter_framebuffer_size(struct evdi_painter *painter);

int evdi_fb_get_bpp(uint32_t format);
void evdi_event_free(struct evdi_event *e);
#endif
