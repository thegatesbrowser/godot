/**************************************************************************/
/*  lockdown.h                                                            */
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

#ifdef LINUXBSD_ENABLED

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Order: PR_SET_NO_NEW_PRIVS → landlock → capset → seccomp. Fail-closed at
// each step. Called from Sandbox::lower_token once IPC + Vulkan are up.
// p_allow_audio / p_allow_microphone gate the runtime-dir audio sockets;
// Linux can't separate mic from output at the socket layer, so passing
// false for either denies all audio.
Error tg_apply_lockdown(const String &p_rw_dir,
		const Vector<String> &p_rw_files,
		const Vector<String> &p_ro_files,
		bool p_allow_audio,
		bool p_allow_microphone);

// Engaged landlock ABI captured at lockdown time. Returns 0 if the lockdown
// hasn't run or the kernel rejected landlock_create_ruleset. Reading
// landlock_create_ruleset post-lockdown returns EPERM (seccomp blocks it),
// so the diagnostic must consult this cached value instead.
int tg_lockdown_landlock_abi();

#endif // LINUXBSD_ENABLED
