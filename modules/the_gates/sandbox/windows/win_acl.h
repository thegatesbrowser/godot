/**************************************************************************/
/*  win_acl.h                                                             */
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

#ifdef WINDOWS_ENABLED

#include "core/string/ustring.h"
#include "core/templates/hash_set.h"

#include <windows.h>

// All Windows ACL/SID work for the sandbox subsystem lives behind this
// class. The stamped-paths cache is process-wide (a static member) so it
// survives across SandboxWin instances; the GDScript launcher discards and
// recreates Sandbox between gate respawns, and a per-instance cache would
// re-stamp every cycle. Methods are idempotent: re-stamping the same path
// with the same SID + mask is a no-op (multi-cycle respawn relies on this).
//
// SDDL reference: D = DACL, S = SACL, ML = mandatory label, OICI =
// OBJECT_INHERIT + CONTAINER_INHERIT, GA = GENERIC_ALL, NW = NO_WRITE_UP,
// WD = Everyone, AC = All Application Packages (S-1-15-2-1), S-1-16-0 =
// UNTRUSTED integrity SID.
class WinACL {
public:
	// Stamps Everyone:GA + All Application Packages:GA + UNTRUSTED label
	// NO_WRITE_UP on a path. On dirs, propagates via OICI; the function
	// walks the existing tree once on the first call and skips the walk
	// on subsequent calls (existence of the UNTRUSTED ACE is the probe).
	// Accepts "ipc://" prefix and strips it.
	void stamp_untrusted_label(const String &p_path);

	// Adds an explicit-allow ACE for `p_sid` on `p_path`. Idempotent per
	// (sid, path, mask, inheritance) — second call returns true without
	// touching the kernel. Cache is the same process-wide set as
	// stamp_untrusted_label; any WinACL instance sees prior stamps.
	bool grant_sid_access(PSID p_sid, const String &p_path,
			ACCESS_MASK p_mask, DWORD p_inheritance);

	// Derives the per-gate AppContainer package name from a rw_dir path.
	// Stable 32-bit hash slug; same rw_dir always yields the same package.
	static String pergate_package_name(const String &p_rw_dir);

	// AppContainer SID for the given package name. Caller frees with
	// ::FreeSid. Returns nullptr on derive failure (and logs).
	static PSID alloc_pergate_sid(const String &p_package_name);

	// All Application Packages well-known SID (S-1-15-2-1). Caller frees
	// with ::FreeSid. Returns nullptr on alloc failure.
	static PSID alloc_all_packages_sid();

private:
	// Process-wide cache. Survives SandboxWin lifecycle so each cycle
	// observes the prior cycle's stamps and skips re-stamping.
	static HashSet<String> &stamped_();
};

#endif // WINDOWS_ENABLED
