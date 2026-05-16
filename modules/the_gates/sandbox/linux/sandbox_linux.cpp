/**************************************************************************/
/*  sandbox_linux.cpp                                                     */
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

#include "sandbox_linux.h"

#include "../sandbox_policy.h"
#include "core/string/print_string.h"

#ifdef LINUXBSD_ENABLED

#include <errno.h>
#include <fcntl.h>
#include <seccomp.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// TODO Phase 3: wire landlock rules from SandboxPolicy.

extern char **environ;

namespace {

Error apply_seccomp_filter() {
	scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_TRAP);
	if (ctx == nullptr) {
		ERR_FAIL_V_MSG(FAILED, "SandboxLinux: seccomp_init failed");
	}

	static const int allowed[] = {
		SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(close), SCMP_SYS(fstat),
		SCMP_SYS(lseek), SCMP_SYS(mmap), SCMP_SYS(mprotect), SCMP_SYS(munmap),
		SCMP_SYS(brk), SCMP_SYS(rt_sigaction), SCMP_SYS(rt_sigprocmask),
		SCMP_SYS(rt_sigreturn), SCMP_SYS(ioctl), SCMP_SYS(pread64),
		SCMP_SYS(pwrite64), SCMP_SYS(readv), SCMP_SYS(writev),
		SCMP_SYS(access), SCMP_SYS(pipe), SCMP_SYS(select), SCMP_SYS(sched_yield),
		SCMP_SYS(mremap), SCMP_SYS(msync), SCMP_SYS(mincore), SCMP_SYS(madvise),
		SCMP_SYS(dup), SCMP_SYS(dup2), SCMP_SYS(pause), SCMP_SYS(nanosleep),
		SCMP_SYS(getitimer), SCMP_SYS(alarm), SCMP_SYS(setitimer),
		SCMP_SYS(getpid), SCMP_SYS(sendfile), SCMP_SYS(socket), SCMP_SYS(connect),
		SCMP_SYS(accept), SCMP_SYS(sendto), SCMP_SYS(recvfrom), SCMP_SYS(sendmsg),
		SCMP_SYS(recvmsg), SCMP_SYS(shutdown), SCMP_SYS(bind), SCMP_SYS(listen),
		SCMP_SYS(getsockname), SCMP_SYS(getpeername), SCMP_SYS(socketpair),
		SCMP_SYS(setsockopt), SCMP_SYS(getsockopt), SCMP_SYS(clone),
		SCMP_SYS(fork), SCMP_SYS(vfork), SCMP_SYS(execve), SCMP_SYS(exit),
		SCMP_SYS(wait4), SCMP_SYS(kill), SCMP_SYS(uname), SCMP_SYS(fcntl),
		SCMP_SYS(flock), SCMP_SYS(fsync), SCMP_SYS(fdatasync), SCMP_SYS(truncate),
		SCMP_SYS(ftruncate), SCMP_SYS(getdents), SCMP_SYS(getcwd), SCMP_SYS(chdir),
		SCMP_SYS(rename), SCMP_SYS(mkdir), SCMP_SYS(rmdir), SCMP_SYS(creat),
		SCMP_SYS(link), SCMP_SYS(unlink), SCMP_SYS(symlink), SCMP_SYS(readlink),
		SCMP_SYS(chmod), SCMP_SYS(fchmod), SCMP_SYS(chown), SCMP_SYS(fchown),
		SCMP_SYS(umask), SCMP_SYS(gettimeofday), SCMP_SYS(getrlimit),
		SCMP_SYS(getrusage), SCMP_SYS(sysinfo), SCMP_SYS(times), SCMP_SYS(getuid),
		SCMP_SYS(getgid), SCMP_SYS(geteuid), SCMP_SYS(getegid), SCMP_SYS(setpgid),
		SCMP_SYS(getppid), SCMP_SYS(getpgrp), SCMP_SYS(setsid), SCMP_SYS(getgroups),
		SCMP_SYS(setfsuid), SCMP_SYS(setfsgid), SCMP_SYS(getsid),
		SCMP_SYS(arch_prctl), SCMP_SYS(futex), SCMP_SYS(set_tid_address),
		SCMP_SYS(set_robust_list), SCMP_SYS(get_robust_list), SCMP_SYS(exit_group),
		SCMP_SYS(openat), SCMP_SYS(mkdirat), SCMP_SYS(fstatat64), SCMP_SYS(newfstatat),
		SCMP_SYS(unlinkat), SCMP_SYS(renameat), SCMP_SYS(linkat), SCMP_SYS(symlinkat),
		SCMP_SYS(readlinkat), SCMP_SYS(fchmodat), SCMP_SYS(faccessat),
		SCMP_SYS(pselect6), SCMP_SYS(ppoll), SCMP_SYS(epoll_pwait),
		SCMP_SYS(prlimit64), SCMP_SYS(getrandom), SCMP_SYS(memfd_create),
		SCMP_SYS(membarrier), SCMP_SYS(statx), SCMP_SYS(clock_gettime),
		SCMP_SYS(clock_nanosleep), SCMP_SYS(epoll_create), SCMP_SYS(epoll_create1),
		SCMP_SYS(epoll_ctl), SCMP_SYS(epoll_wait), SCMP_SYS(eventfd2),
		SCMP_SYS(timerfd_create), SCMP_SYS(timerfd_settime), SCMP_SYS(timerfd_gettime),
		SCMP_SYS(prctl), SCMP_SYS(setpriority), SCMP_SYS(getpriority),
		SCMP_SYS(sched_getaffinity), SCMP_SYS(sched_setaffinity),
		SCMP_SYS(sched_getparam), SCMP_SYS(sched_setparam),
	};

	for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
		if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, allowed[i], 0) != 0) {
			seccomp_release(ctx);
			ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux: seccomp_rule_add failed for syscall %d", allowed[i]));
		}
	}

	if (seccomp_load(ctx) != 0) {
		seccomp_release(ctx);
		ERR_FAIL_V_MSG(FAILED, "SandboxLinux: seccomp_load failed");
	}

	seccomp_release(ctx);
	return OK;
}

} // namespace

#endif // LINUXBSD_ENABLED

Dictionary SandboxLinux::spawn_target(const Ref<SandboxPolicy> &p_policy,
		const String &p_executable, const Vector<String> &p_arguments) {
	Dictionary result;
#ifdef LINUXBSD_ENABLED
	ERR_FAIL_COND_V_MSG(p_policy.is_null(), result,
			"SandboxLinux::spawn_target requires a non-null SandboxPolicy");

	CharString exe_cs = p_executable.utf8();
	Vector<CharString> arg_storage;
	arg_storage.push_back(exe_cs);
	for (int i = 0; i < p_arguments.size(); ++i) {
		arg_storage.push_back(p_arguments[i].utf8());
	}

	Vector<char *> argv;
	for (int i = 0; i < arg_storage.size(); ++i) {
		argv.push_back(const_cast<char *>(arg_storage[i].get_data()));
	}
	argv.push_back(nullptr);

	setenv("TG_TARGET", "1", 1);

	pid_t pid = 0;
	if (posix_spawn(&pid, exe_cs.get_data(), nullptr, nullptr, argv.ptrw(), environ) != 0) {
		unsetenv("TG_TARGET");
		ERR_FAIL_V_MSG(result, vformat("SandboxLinux::spawn_target: posix_spawn failed errno=%d", errno));
	}
	unsetenv("TG_TARGET");

	target_pid = (int64_t)pid;
	result["pid"] = target_pid;
#else
	(void)p_policy;
	(void)p_executable;
	(void)p_arguments;
#endif
	return result;
}

void SandboxLinux::apply_renderer_acl(const String &p_path) {
	// No-op: AF_UNIX socket perms inherit umask + parent dir on POSIX.
	(void)p_path;
}

Error SandboxLinux::verify_binary(const String &p_path) {
	// TODO Phase 3: SHA-256 against tg_signature_pin.
	(void)p_path;
	print_line("[VERIFY-BYPASSED] SandboxLinux: signature_verify not yet implemented");
	return OK;
}

bool SandboxLinux::is_target_running() const {
#ifdef LINUXBSD_ENABLED
	if (target_pid == 0) {
		return false;
	}
	// TODO Phase 3: pid reuse — kill(pid, 0) lies after the child dies
	// and the kernel rebinds the pid. Replace with pidfd_open or waitpid.
	return ::kill((pid_t)target_pid, 0) == 0;
#else
	return false;
#endif
}

Error SandboxLinux::kill_target() {
#ifdef LINUXBSD_ENABLED
	if (target_pid == 0) {
		return ERR_DOES_NOT_EXIST;
	}
	const pid_t pid = (pid_t)target_pid;
	if (::kill(pid, SIGTERM) != 0) {
		ERR_FAIL_V_MSG(FAILED, vformat("SandboxLinux::kill_target: kill failed errno=%d", errno));
	}
	// Non-blocking reap; clears target_pid so future probes return false.
	// If the child hasn't exited yet, init reaps the zombie on launcher exit.
	int status = 0;
	::waitpid(pid, &status, WNOHANG);
	target_pid = 0;
	return OK;
#else
	return ERR_UNAVAILABLE;
#endif
}

Error SandboxLinux::lower_token() {
#ifdef LINUXBSD_ENABLED
	// TODO Phase 3: landlock + user-namespace before seccomp_load.
	return apply_seccomp_filter();
#else
	return ERR_UNAVAILABLE;
#endif
}

bool SandboxLinux::is_target() const {
#ifdef LINUXBSD_ENABLED
	const char *flag = ::getenv("TG_TARGET");
	return flag != nullptr && flag[0] == '1';
#else
	return false;
#endif
}
