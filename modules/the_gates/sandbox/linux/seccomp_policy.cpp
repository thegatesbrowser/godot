/**************************************************************************/
/*  seccomp_policy.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifdef LINUXBSD_ENABLED

#include "seccomp_policy.h"

#include "../signal_safe_log.h"

#include "sandbox/linux/bpf_dsl/bpf_dsl.h"
#include "sandbox/linux/system_headers/linux_seccomp.h"
#include "sandbox/linux/system_headers/linux_syscalls.h"

#include <errno.h>
#include <stdint.h>

using sandbox::bpf_dsl::Allow;
using sandbox::bpf_dsl::Error;
using sandbox::bpf_dsl::ResultExpr;
using sandbox::bpf_dsl::Trap;

namespace {

constexpr int DENIAL_TABLE_SIZE = 1024;

// One slot per syscall number; each denied syscall is logged once so a spinning
// renderer can't flood the uploaded log. Racy across threads but harmlessly so.
bool g_denial_logged[DENIAL_TABLE_SIZE] = {};

// SIGSYS handler: async-signal-safe only. Logs each denied syscall once and
// returns -EPERM so the denial stays fail-soft, exactly as SECCOMP_RET_ERRNO
// would. Resolve the number to a name offline.
intptr_t log_denied_syscall(const struct arch_seccomp_data &p_args, void *) {
	const int nr = p_args.nr;
	if (nr >= 0 && nr < DENIAL_TABLE_SIZE) {
		if (g_denial_logged[nr]) {
			return -EPERM;
		}
		g_denial_logged[nr] = true;
	}

	tg_signal_safe_log("[SECCOMP] denied syscall ", nr);
	return -EPERM;
}

} // namespace

TheGatesRendererPolicy::TheGatesRendererPolicy() = default;
TheGatesRendererPolicy::~TheGatesRendererPolicy() = default;

ResultExpr TheGatesRendererPolicy::EvaluateSyscall(int sysno) const {
	switch (sysno) {
		// Process / thread lifecycle.
		case __NR_exit:
		case __NR_exit_group:
		case __NR_clone:
		case __NR_clone3:
		case __NR_futex:
		case __NR_set_robust_list:
		case __NR_get_robust_list:
		case __NR_set_tid_address:
		case __NR_rt_sigaction:
		case __NR_rt_sigprocmask:
		case __NR_rt_sigreturn:
		case __NR_rt_sigtimedwait:
		case __NR_rt_sigsuspend:
		case __NR_rt_sigpending:
		case __NR_rt_sigqueueinfo:
		case __NR_rt_tgsigqueueinfo:
		case __NR_kill:
		case __NR_tkill:
		case __NR_tgkill:
		case __NR_pause:
		case __NR_arch_prctl:
		case __NR_prctl:
		// Process / thread identity.
		case __NR_getpid:
		case __NR_gettid:
		case __NR_getppid:
		case __NR_getuid:
		case __NR_geteuid:
		case __NR_getgid:
		case __NR_getegid:
		case __NR_getresuid:
		case __NR_getresgid:
		case __NR_getpgid:
		case __NR_getpgrp:
		case __NR_getsid:
		case __NR_getgroups:
		case __NR_setpgid:
		case __NR_setsid:
		case __NR_uname:
		case __NR_capget:
		case __NR_capset:
		// Memory + mmap.
		case __NR_brk:
		case __NR_mmap:
		case __NR_munmap:
		case __NR_mprotect:
		case __NR_mremap:
		case __NR_madvise:
		case __NR_mincore:
		case __NR_mlock:
		case __NR_munlock:
		case __NR_mlock2:
		case __NR_mlockall:
		case __NR_munlockall:
		case __NR_msync:
		case __NR_membarrier:
		case __NR_memfd_create:
		case __NR_pkey_alloc:
		case __NR_pkey_free:
		case __NR_pkey_mprotect:
		// File I/O.
		case __NR_read:
		case __NR_pread64:
		case __NR_readv:
		case __NR_preadv:
		case __NR_preadv2:
		case __NR_write:
		case __NR_pwrite64:
		case __NR_writev:
		case __NR_pwritev:
		case __NR_pwritev2:
		case __NR_open:
		case __NR_openat:
		case __NR_openat2:
		case __NR_close:
		case __NR_close_range:
		case __NR_creat:
		case __NR_lseek:
		case __NR_stat:
		case __NR_lstat:
		case __NR_fstat:
		case __NR_statx:
		case __NR_newfstatat:
		case __NR_access:
		case __NR_faccessat:
		case __NR_faccessat2:
		case __NR_fcntl:
		case __NR_flock:
		case __NR_dup:
		case __NR_dup2:
		case __NR_dup3:
		case __NR_pipe:
		case __NR_pipe2:
		case __NR_fdatasync:
		case __NR_fsync:
		case __NR_sync:
		case __NR_sync_file_range:
		case __NR_syncfs:
		case __NR_fadvise64:
		case __NR_fallocate:
		case __NR_ftruncate:
		case __NR_truncate:
		case __NR_readahead:
		case __NR_sendfile:
		case __NR_copy_file_range:
		case __NR_getdents:
		case __NR_getdents64:
		case __NR_readlink:
		case __NR_readlinkat:
		case __NR_chmod:
		case __NR_fchmod:
		case __NR_fchmodat:
		case __NR_chown:
		case __NR_fchown:
		case __NR_fchownat:
		case __NR_lchown:
		case __NR_mkdir:
		case __NR_mkdirat:
		case __NR_rmdir:
		case __NR_unlink:
		case __NR_unlinkat:
		case __NR_rename:
		case __NR_renameat:
		case __NR_renameat2:
		case __NR_link:
		case __NR_linkat:
		case __NR_symlink:
		case __NR_symlinkat:
		case __NR_chdir:
		case __NR_fchdir:
		case __NR_getcwd:
		case __NR_umask:
		case __NR_utime:
		case __NR_utimes:
		case __NR_utimensat:
		case __NR_futimesat:
		case __NR_inotify_init:
		case __NR_inotify_init1:
		case __NR_inotify_add_watch:
		case __NR_inotify_rm_watch:
		case __NR_fanotify_init:
		case __NR_fanotify_mark:
		// Sockets — only ops that work on already-opened FDs are allowed.
		// Socket CREATION (__NR_socket, __NR_socketpair, __NR_bind, __NR_listen,
		// __NR_connect) is denied so the renderer cannot make new IP sockets
		// directly. It must ask the launcher's NetworkBroker (over the
		// pre-lockdown AF_UNIX channel) for an FD, which is passed via
		// SCM_RIGHTS. sendmsg/recvmsg/sendto/recvfrom on existing FDs are
		// allowed because the broker handshake needs them.
		case __NR_accept:
		case __NR_accept4:
		case __NR_sendto:
		case __NR_recvfrom:
		case __NR_sendmsg:
		case __NR_recvmsg:
		case __NR_sendmmsg:
		case __NR_recvmmsg:
		case __NR_shutdown:
		case __NR_setsockopt:
		case __NR_getsockopt:
		case __NR_getsockname:
		case __NR_getpeername:
		// Time / sleep.
		case __NR_clock_gettime:
		case __NR_clock_getres:
		case __NR_clock_nanosleep:
		case __NR_nanosleep:
		case __NR_gettimeofday:
		case __NR_setitimer:
		case __NR_getitimer:
		case __NR_alarm:
		case __NR_times:
		// Event / fd primitives.
		case __NR_eventfd:
		case __NR_eventfd2:
		case __NR_timerfd_create:
		case __NR_timerfd_settime:
		case __NR_timerfd_gettime:
		case __NR_signalfd:
		case __NR_signalfd4:
		case __NR_epoll_create:
		case __NR_epoll_create1:
		case __NR_epoll_ctl:
		case __NR_epoll_wait:
		case __NR_epoll_pwait:
		case __NR_epoll_pwait2:
		case __NR_select:
		case __NR_pselect6:
		case __NR_poll:
		case __NR_ppoll:
		// Resource limits + scheduling.
		case __NR_getrlimit:
		case __NR_setrlimit:
		case __NR_prlimit64:
		case __NR_getrusage:
		case __NR_sysinfo:
		case __NR_sched_yield:
		case __NR_sched_getaffinity:
		case __NR_sched_setaffinity:
		case __NR_sched_getparam:
		case __NR_sched_setparam:
		case __NR_sched_getscheduler:
		case __NR_sched_setscheduler:
		case __NR_sched_get_priority_min:
		case __NR_sched_get_priority_max:
		case __NR_sched_rr_get_interval:
		case __NR_setpriority:
		case __NR_getpriority:
		case __NR_setfsuid:
		case __NR_setfsgid:
		// Misc.
		case __NR_getrandom:
		case __NR_ioctl:
		case __NR_wait4:
		case __NR_waitid:
		case __NR_rseq:
		case __NR_restart_syscall:
			return Allow();
		// io_uring_* deliberately denied: recurring sandbox-escape vector
		// (Chromium and Firefox both block it). Renderer does not use it.
		default:
			return Trap(log_denied_syscall, nullptr);
	}
}

ResultExpr TheGatesRendererPolicy::InvalidSyscall() const {
	return Error(ENOSYS);
}

#endif // LINUXBSD_ENABLED
