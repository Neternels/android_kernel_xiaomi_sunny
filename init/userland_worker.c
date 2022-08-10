// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Vlad Adumitroaie <celtare21@gmail.com>.
 *                    Adam W. Willis <return.of.octobot@gmail.com>
 *                    Cyber Knight <cyberknight755@gmail.com>
 */

#define pr_fmt(fmt) "userland_worker: " fmt
#define nt_info(fmt, ...) printk(KERN_INFO "NetErnels: " fmt, ##__VA_ARGS__)

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/security.h>
#include <linux/delay.h>
#include <linux/userland.h>

#include "../security/selinux/include/security.h"

#define INITIAL_SIZE 4
#define MAX_CHAR 128
#define DELAY 500

static char** argv;

static struct delayed_work userland_work;

unsigned int is_libcam;
module_param(is_libcam, uint, 0644);

extern bool is_inline;

static void free_memory(char** argv, int size)
{
	int i;

	for (i = 0; i < size; i++)
		kfree(argv[i]);
	kfree(argv);
}

static char** alloc_memory(int size)
{
	char** argv;
	int i;

	argv = kmalloc(size * sizeof(char*), GFP_KERNEL);
	if (!argv)
		return NULL;

	for (i = 0; i < size; i++) {
		argv[i] = kmalloc(MAX_CHAR * sizeof(char), GFP_KERNEL);
		if (!argv[i]) {
			kfree(argv);
			return NULL;
		}
	}

	return argv;
}

static int call_userland(char** argv)
{
	static char* envp[] = {
		"SHELL=/bin/sh",
		"HOME=/",
		"USER=shell",
		"TERM=xterm-256color",
		"PATH=/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:/system_ext/bin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin",
		"DISPLAY=:0",
		NULL
	};

	return call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
}

static inline int nix_sh(const char* cmd)
{
  int ret;

  strcpy(argv[0], "/system/bin/sh");
  strcpy(argv[1], "-c");
  strcpy(argv[2], cmd);
  argv[3] = NULL;
  
  ret = call_userland(argv);
  if (!ret)
    pr_info("%s executed successfully!", cmd);
  else
    pr_err("%s failed to execute! %d", cmd, ret);
  
  return ret;
}

static inline int nix_test(const char* path, bool dir)
{
        strcpy(argv[0], "/system/bin/test");
        strcpy(argv[1], (dir ? "-d" : "-f"));
        strcpy(argv[2], path);
        argv[3] = NULL;

        return call_userland(argv);
}

static void vbswap_helper(void)
{

  nix_sh("/system/bin/echo 4294967296 > /sys/devices/virtual/block/vbswap0/disksize");
  nix_sh("/system/bin/mkswap /dev/block/vbswap0");
  nix_sh("/system/bin/swapon /dev/block/vbswap0");

}

static void libcam_helper(void)
{
	if (nix_test("/system/lib64/libcameraservice.so", true)) {
		pr_info("libcameraservice exists in lib64! Using system monotonic time for buffer timestamp...");
		is_libcam = 0;
	} else {
		pr_info("libcameraservice does not exist in lib64! Using boot time for buffer timestamp...");
		is_libcam = 1;
	}
}

static void userland_worker(struct work_struct *work)
{
	bool is_enforcing;
	int retries = 0;
        const int max_retries = 25;

	argv = alloc_memory(INITIAL_SIZE);
	if (!argv) {
		pr_err("Couldn't allocate memory!");
		return;
	}

	do {
		is_enforcing = get_enforce_value();
		if (!is_enforcing)
			msleep(DELAY);
	} while (!is_enforcing && (retries++ < max_retries));

	if (is_enforcing) {
		pr_info("Setting selinux state: permissive");
		set_selinux(0);
	}

	vbswap_helper();
	libcam_helper();

	if (is_enforcing) {
		pr_info("Setting selinux state: enforcing");
		set_selinux(1);
	}

	free_memory(argv, INITIAL_SIZE);
}

static int __init userland_worker_entry(void)
{

	if (is_inline) {
                nt_info("Inline ROM detected! Killing UserLand Worker...\n");
                return 0;
        }

	INIT_DELAYED_WORK(&userland_work, userland_worker);
	queue_delayed_work(system_power_efficient_wq,
			&userland_work, DELAY);

	return 0;
}

module_init(userland_worker_entry);
