/*
 * Copyright (c) 2000-2005 Apple Computer, Inc. All rights reserved.
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
/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)select.h	8.2 (Berkeley) 1/4/94
 */

#ifndef _SYS_SELECT_H_
#define _SYS_SELECT_H_

#include <sys/appleapiopts.h>
#include <sys/cdefs.h>
#include <sys/_types.h>

/*
 * [XSI] The <sys/select.h> header shall define the fd_set type as a structure.
 * The timespec structure shall be defined as described in <time.h>
 * The <sys/select.h> header shall define the timeval structure.
 */
#include <sys/_types/_fd_def.h>
#include <sys/_types/_timespec.h>
#include <sys/_types/_timeval.h>

/*
 * The time_t and suseconds_t types shall be defined as described in
 * <sys/types.h>
 * The sigset_t type shall be defined as described in <signal.h>
 */
#include <sys/_types/_time_t.h>
#include <sys/_types/_suseconds_t.h>
#include <sys/_types/_sigset_t.h>

/*
 * [XSI] FD_CLR, FD_ISSET, FD_SET, FD_ZERO may be declared as a function, or
 *	 defined as a macro, or both
 * [XSI] FD_SETSIZE shall be defined as a macro
 */

/*
 * Select uses bit masks of file descriptors in longs.  These macros
 * manipulate such bit fields (the filesystem macros use chars).  The
 * extra protection here is to permit application redefinition above
 * the default size.
 */
#include <sys/_types/_fd_setsize.h>
#include <sys/_types/_fd_set.h>
#include <sys/_types/_fd_clr.h>
#include <sys/_types/_fd_isset.h>
#include <sys/_types/_fd_zero.h>

#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
#include <sys/_types/_fd_copy.h>
#endif  /* (!_POSIX_C_SOURCE || _DARWIN_C_SOURCE) */

#ifdef KERNEL
#include <sys/kernel_types.h>
#include <kern/waitq.h>
#include <sys/event.h>

/*
 * Used to maintain information about processes that wish to be
 * notified when I/O becomes possible.
 */
#ifdef KERNEL_PRIVATE
struct selinfo {
	union {
		struct  waitq si_waitq; /* waitq for wait/wakeup */
		uint8_t si_waitq_storage[WQ_OPAQUE_SIZE]; /* Opaque and "real" versions of waitq has different sizes
		                                           * defined in Mach and BSD layers,
		                                           * allocating extra storage to mitigate that */
	};
	struct  klist si_note;          /* JMM - temporary separation */
	u_int   si_flags;               /* see below */
};

#define SI_COLL         0x0001          /* obsolete */
#define SI_RECORDED     0x0004          /* obsolete */
#define SI_INITED       0x0008          /* obsolete */
#define SI_CLEAR        0x0010          /* obsolete */
#define SI_KNPOSTING    0x0020          /* posting to knotes */
#define SI_SELSPEC      0x0040          /* has spec_filtops knote hooked */

#else
struct selinfo;
#endif

__BEGIN_DECLS

extern int selwait;

/*
 * Now these are the laws of VNOP_SELECT, as old and as true as the sky,
 * And the device that shall keep it may prosper, but the device that shall
 * break it must receive ENODEV:
 *
 * 1. Take a lock to protect against other selects on the same vnode.
 * 2. Return 1 if data is ready to be read.
 * 3. Return 0 and call `selrecord` on a handy `selinfo` structure if there
 *    is no data.
 * 4. Call `selwakeup` when the vnode has an active `selrecord` and data
 *    can be read or written (depending on the seltype).
 * 5. If there's a `selrecord` and no corresponding `selwakeup`, but the
 *    vnode is going away, call `selthreadclear`.
 */
void    selrecord(proc_t selector, struct selinfo *, void *);
void    selwakeup(struct selinfo *);
void    selthreadclear(struct selinfo *);

#if XNU_KERNEL_PRIVATE
struct  knote;
struct  _select;
void    select_cleanup_uthread(struct _select *);

#define SELSPEC_RECORD_MARKER   ((struct select_set *)-1)
typedef void (^selspec_record_hook_t)(struct selinfo *sip);
void selspec_attach(struct knote *, struct selinfo *);
void selspec_detach(struct knote *);
#endif

__END_DECLS

#else /* !KERNEL */

__BEGIN_DECLS

#ifndef  __MWERKS__
int      pselect(int, fd_set * __restrict, fd_set * __restrict,
    fd_set * __restrict, const struct timespec * __restrict,
    const sigset_t * __restrict)
#if defined(_DARWIN_C_SOURCE) || defined(_DARWIN_UNLIMITED_SELECT)
__DARWIN_EXTSN_C(pselect)
#else /* !_DARWIN_C_SOURCE && !_DARWIN_UNLIMITED_SELECT */
#  if defined(__LP64__) && !__DARWIN_NON_CANCELABLE
__DARWIN_1050(pselect)
#  else /* !__LP64__ || __DARWIN_NON_CANCELABLE */
__DARWIN_ALIAS_C(pselect)
#  endif /* __LP64__ && !__DARWIN_NON_CANCELABLE */
#endif /* _DARWIN_C_SOURCE || _DARWIN_UNLIMITED_SELECT */
;
#endif /* __MWERKS__ */

__END_DECLS

#include <sys/_select.h>        /* select() prototype */

#endif /* ! KERNEL */

#endif /* !_SYS_SELECT_H_ */
