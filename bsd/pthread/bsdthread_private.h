/*
 * Copyright (c) 2017 Apple, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef _PTHREAD_BSDTHREAD_PRIVATE_H_
#define _PTHREAD_BSDTHREAD_PRIVATE_H_

#if XNU_KERNEL_PRIVATE && !defined(__PTHREAD_EXPOSE_INTERNALS__)
#define __PTHREAD_EXPOSE_INTERNALS__ 1
#endif // XNU_KERNEL_PRIVATE

#ifdef __PTHREAD_EXPOSE_INTERNALS__

/* pthread bsdthread_ctl sysctl commands */
/* bsdthread_ctl(BSDTHREAD_CTL_SET_QOS, thread_port, tsd_entry_addr, 0) */
#define BSDTHREAD_CTL_SET_QOS                           0x10
/* bsdthread_ctl(BSDTHREAD_CTL_GET_QOS, thread_port, 0, 0) */
#define BSDTHREAD_CTL_GET_QOS                           0x20
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_OVERRIDE_START, thread_port, priority, 0) */
#define BSDTHREAD_CTL_QOS_OVERRIDE_START        0x40
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_OVERRIDE_END, thread_port, 0, 0) */
#define BSDTHREAD_CTL_QOS_OVERRIDE_END          0x80
/* bsdthread_ctl(BSDTHREAD_CTL_SET_SELF, priority, voucher, flags) */
#define BSDTHREAD_CTL_SET_SELF                          0x100
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_OVERRIDE_RESET, 0, 0, 0) */
#define BSDTHREAD_CTL_QOS_OVERRIDE_RESET        0x200
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_OVERRIDE_DISPATCH, thread_port, priority, 0) */
#define BSDTHREAD_CTL_QOS_OVERRIDE_DISPATCH     0x400
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_DISPATCH_ASYNCHRONOUS_OVERRIDE_ADD, thread_port, priority, resource) */
#define BSDTHREAD_CTL_QOS_DISPATCH_ASYNCHRONOUS_OVERRIDE_ADD            0x401
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_DISPATCH_ASYNCHRONOUS_OVERRIDE_RESET, 0|1 (?reset_all), resource, 0) */
#define BSDTHREAD_CTL_QOS_DISPATCH_ASYNCHRONOUS_OVERRIDE_RESET          0x402
/* bsdthread_ctl(BSDTHREAD_CTL_QOS_MAX_PARALLELISM, priority, flags, 0) */
#define BSDTHREAD_CTL_QOS_MAX_PARALLELISM       0x800
/*
 * bsdthread_ctl(BSDTHREAD_CTL_WORKQ_ALLOW_KILL, enable, 0, 0)
 * It only affects the calling thread. Regular UNIX calls still need to be
 * used to manipulate signal mask of the calling thread to allow delivery
 * of a specific signal to it.
 * It is typically used in abort paths so it does not need to worry about
 * preserving sigmask across the thread's re-use. See workq_thread_return.
 */
#define BSDTHREAD_CTL_WORKQ_ALLOW_KILL 0x1000
/* bsdthread_ctl(BSDTHREAD_CTL_DISPATCH_APPLY_ATTR, flags, val1, val2) */
#define BSDTHREAD_CTL_DISPATCH_APPLY_ATTR 0x2000
/*
 * bsdthread_ctl(BSDTHREAD_CTL_WORKQ_ALLOW_SIGMASK, sigmask, 0, 0)
 * This is a process wide configuration (as opposed to ALLOW_KILL) that
 * provides the calling process an ability to send signals to all its
 * pthread workqueue threads.
 * Regular UNIX calls still need to be used to manipulate signal mask of
 * each individual pthread worker thread to allow delivery of a specific
 * signal to that thread.
 * The @sigmask specified here is used internally by workqueue subsystem
 * to preserve sigmask of pthread workqueue threads across their re-use.
 * See workq_thread_return.
 */

#define BSDTHREAD_CTL_WORKQ_ALLOW_SIGMASK 0x4000

/* Flags for BSDTHREAD_CTL_QOS_MAX_PARALLELISM */
#define _PTHREAD_QOS_PARALLELISM_COUNT_LOGICAL 0x1
#define _PTHREAD_QOS_PARALLELISM_REALTIME      0x2
#define _PTHREAD_QOS_PARALLELISM_CLUSTER_SHARED_RSRC      0x4


/* Flags for BSDTHREAD_CTL_DISPATCH_APPLY_ATTR */
#define _PTHREAD_DISPATCH_APPLY_ATTR_CLUSTER_SHARED_RSRC_SET    0x1
#define _PTHREAD_DISPATCH_APPLY_ATTR_CLUSTER_SHARED_RSRC_CLEAR  0x2


#endif // __PTHREAD_EXPOSE_INTERNALS__
#endif // _PTHREAD_BSDTHREAD_PRIVATE_H_
