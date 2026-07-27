// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include <mach/mach_types.h>
#include <mach/kmod.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

#ifndef RTW_VERSION
#define RTW_VERSION "0.0.0"
#endif

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(org.realtekrtl8822c.driver.RealtekRTL8822C, RTW_VERSION, _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
