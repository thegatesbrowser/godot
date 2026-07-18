/**************************************************************************/
/*  crash_logger.cpp                                                      */
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

#include "crash_logger.h"

#include "signal_safe_log.h"

#if defined(LINUXBSD_ENABLED) || defined(MACOS_ENABLED)

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

namespace {

// Fatal, synchronously-generated signals worth a log line. SIGSYS is omitted —
// the seccomp filter's trap handler owns it.
const int CRASH_SIGNALS[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

// Crash signal handler: async-signal-safe only. Logs the signal and a
// best-effort backtrace, then re-raises. SA_RESETHAND already restored the
// default disposition, so the process dies with the real signal (and a core).
void crash_handler(int p_sig) {
	tg_signal_safe_log("[RENDERER-CRASH] signal=", p_sig);

	void *frames[64];
	const int count = backtrace(frames, 64);
	backtrace_symbols_fd(frames, count, STDERR_FILENO);

	::raise(p_sig);
}

} // namespace

void tg_install_crash_logger() {
	// Warm backtrace() so it loads libgcc_s now; its first call is not
	// async-signal-safe and must not happen inside the handler.
	void *warm[1];
	(void)backtrace(warm, 1);

	struct sigaction sa = {};
	sa.sa_handler = crash_handler;
	sa.sa_flags = SA_NODEFER | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for (size_t i = 0; i < sizeof(CRASH_SIGNALS) / sizeof(CRASH_SIGNALS[0]); i++) {
		::sigaction(CRASH_SIGNALS[i], &sa, nullptr);
	}
}

#elif defined(WINDOWS_ENABLED)

#include <windows.h>

namespace {

// Top-level exception filter: runs when nothing else handles a fatal exception,
// just before the process dies. Logs the exception code and a raw-address
// backtrace (symbolize offline against the .pdb), then lets the default handler
// produce the crash dump.
LONG WINAPI crash_handler(EXCEPTION_POINTERS *p_info) {
	tg_signal_safe_log_hex("[RENDERER-CRASH] exception=0x", (uintptr_t)p_info->ExceptionRecord->ExceptionCode);

	void *frames[64];
	const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
	for (USHORT i = 0; i < count; ++i) {
		tg_signal_safe_log_hex("  at 0x", (uintptr_t)frames[i]);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void tg_install_crash_logger() {
	SetUnhandledExceptionFilter(crash_handler);
}

#else

void tg_install_crash_logger() {}

#endif
