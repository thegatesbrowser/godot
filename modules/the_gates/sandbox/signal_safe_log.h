/**************************************************************************/
/*  signal_safe_log.h                                                     */
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

#pragma once

#include <stdint.h>

#ifdef WINDOWS_ENABLED
#include <windows.h>
#else
#include <unistd.h>
#endif

// One platform write of raw bytes to stderr. The renderer's stderr is captured
// into the log the launcher uploads, so the line survives an imminent crash.
inline void tg_signal_safe_emit(const char *p_buf, int p_len) {
#ifdef WINDOWS_ENABLED
	DWORD written = 0;
	WriteFile(GetStdHandle(STD_ERROR_HANDLE), p_buf, (DWORD)p_len, &written, nullptr);
#else
	const ssize_t w = ::write(STDERR_FILENO, p_buf, (size_t)p_len);
	(void)w;
#endif
}

// Format "<prefix><value>\n" with a decimal value into a stack buffer and emit
// it — no allocation, so it is safe to call from a seccomp trap or a crash
// signal handler.
inline void tg_signal_safe_log(const char *p_prefix, long p_value) {
	char buf[96];
	int len = 0;
	for (const char *c = p_prefix; *c != '\0' && len < 64; ++c) {
		buf[len++] = *c;
	}

	long n = p_value;
	if (n < 0) {
		buf[len++] = '-';
		n = -n;
	}
	char digits[20];
	int d = 0;
	do {
		digits[d++] = (char)('0' + (int)(n % 10));
		n /= 10;
	} while (n > 0);
	while (d > 0) {
		buf[len++] = digits[--d];
	}
	buf[len++] = '\n';

	tg_signal_safe_emit(buf, len);
}

// Same, but the value is lowercase hex (put any "0x" in the prefix). Takes a
// uintptr_t so a 64-bit crash address survives on LLP64 (Windows). Used for the
// Windows backtrace.
inline void tg_signal_safe_log_hex(const char *p_prefix, uintptr_t p_value) {
	char buf[96];
	int len = 0;
	for (const char *c = p_prefix; *c != '\0' && len < 64; ++c) {
		buf[len++] = *c;
	}

	char digits[16];
	int d = 0;
	do {
		const int nibble = (int)(p_value & 0xF);
		digits[d++] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
		p_value >>= 4;
	} while (p_value != 0);
	while (d > 0) {
		buf[len++] = digits[--d];
	}
	buf[len++] = '\n';

	tg_signal_safe_emit(buf, len);
}
