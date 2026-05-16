/**************************************************************************/
/*  sandbox_win.h                                                         */
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

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"

namespace sandbox {
class BrokerServices;
}

class SandboxingWin : public RefCounted {
	GDCLASS(SandboxingWin, RefCounted);

	sandbox::BrokerServices *broker_service = nullptr;
	// Per-process — BrokerServices is a process singleton (SandboxFactory),
	// so calling Init() twice fails with SBOX_ERROR_UNEXPECTED_CALL (8).
	// Static so it's shared across multiple SandboxingWin instances (the
	// launcher creates one per gate spawn — first one Inits, rest reuse).
	static bool broker_initialized;

protected:
	static void _bind_methods();

public:
	// Broker-side: spawn an executable as a locked-down child. Returns a
	// Dictionary {pid, handle} on success or {} on failure.
	//
	// p_stdout_log_path: file the target's stdout + stderr write to. Opened
	// in the broker at Medium IL so the locked-down child has a writeable
	// handle even though its own token can't open files under %APPDATA%.
	//
	// p_rw_dir / p_rw_files / p_ro_files: the file-access allow-list.
	// p_rw_dir is required and gets recursive R/W via single-asterisk
	// globs up to 6 levels deep. rw_files and ro_files are exact file
	// paths. Anything outside this list is denied.
	Dictionary spawn_target(const String &p_executable, const Vector<String> &p_arguments,
			const String &p_stdout_log_path = String(),
			const String &p_rw_dir = String(),
			const Vector<String> &p_rw_files = Vector<String>(),
			const Vector<String> &p_ro_files = Vector<String>());

	// Target-side: drop the impersonation token, apply the delayed integrity
	// level, and finalize the lockdown. Called from main.cpp in the renderer
	// after all initial OS access is done.
	Error lower_token();

	// Broker-side: stamp `p_path` (a directory or file) with an UNTRUSTED
	// mandatory label + Everyone GENERIC_ALL DACL so a sandbox target at
	// INTEGRITY_LEVEL_UNTRUSTED can write to it. Without this, MIC blocks
	// writes from UNTRUSTED into objects the broker (Medium IL) created.
	void apply_untrusted_acl(const String &p_path);

	// True iff this process is a sandbox target (was spawned by a broker that
	// already wrote shared state into us). Determined by whether
	// SandboxFactory::GetBrokerServices() returned non-null when we
	// constructed.
	bool is_target() const { return broker_service == nullptr; }

	SandboxingWin();
	~SandboxingWin();
};
