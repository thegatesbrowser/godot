/**************************************************************************/
/*  sandbox_policy.h                                                      */
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

// Cross-platform sandbox policy description. Each Sandbox impl translates
// these into native primitives: Windows -> chromium TargetPolicy + DACL/MIC;
// Linux -> seccomp + landlock; macOS -> Seatbelt profile.
class SandboxPolicy : public RefCounted {
	GDCLASS(SandboxPolicy, RefCounted);

	String rw_dir;
	PackedStringArray rw_files;
	PackedStringArray ro_files;
	String child_stdout_log_path;
	bool block_private_networks = true;
	bool allow_audio = true;
	bool allow_microphone = true;
	int integrity_floor = 0;

protected:
	static void _bind_methods();

public:
	void set_rw_dir(const String &p_path) { rw_dir = p_path; }
	String get_rw_dir() const { return rw_dir; }

	void add_rw_file(const String &p_path) { rw_files.push_back(p_path); }
	PackedStringArray get_rw_files() const { return rw_files; }
	void set_rw_files(const PackedStringArray &p_files) { rw_files = p_files; }

	void add_ro_file(const String &p_path) { ro_files.push_back(p_path); }
	PackedStringArray get_ro_files() const { return ro_files; }
	void set_ro_files(const PackedStringArray &p_files) { ro_files = p_files; }

	void set_child_stdout_log_path(const String &p_path) { child_stdout_log_path = p_path; }
	String get_child_stdout_log_path() const { return child_stdout_log_path; }

	// Engages the OS-level filter that drops RFC 1918 / loopback / link-local
	// outbound from the renderer. Default true: each platform's spawn_target
	// refuses to spawn if the filter cannot be confirmed active.
	void set_block_private_networks(bool p_block) { block_private_networks = p_block; }
	bool is_private_networks_blocked() const { return block_private_networks; }

	void set_allow_audio(bool p_allow) { allow_audio = p_allow; }
	bool is_audio_allowed() const { return allow_audio; }

	void set_allow_microphone(bool p_allow) { allow_microphone = p_allow; }
	bool is_microphone_allowed() const { return allow_microphone; }

	// Platform-specific severity dial: Windows passes IntegrityLevel enum
	// values, Linux uses 0 for "default seccomp+landlock", macOS is binary
	// (Seatbelt active or not). Default 0 means "tightest available."
	void set_integrity_floor(int p_level) { integrity_floor = p_level; }
	int get_integrity_floor() const { return integrity_floor; }

	Dictionary to_dict() const;

	SandboxPolicy() = default;
	~SandboxPolicy() = default;
};
