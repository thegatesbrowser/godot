/**************************************************************************/
/*  sandbox_diagnostics.h                                                 */
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

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"

// Target-side self-report of the sandbox state observed from inside the
// renderer process. Per-platform fields (Windows token + mitigations,
// Linux seccomp/landlock, macOS Seatbelt) plus a common canaries dict.
//
// The autonomous harness reads the SANDBOX-DIAG-BEGIN/END block emitted by
// to_json_block() and cross-checks against broker_policy.json the broker
// wrote at spawn time.
class SandboxDiagnostics {
public:
	// pack_path: absolute path of the renderer's .pck, used for the
	// pck-read canary. Empty means "skip the canary."
	explicit SandboxDiagnostics(const String &p_pack_path) :
			pack_path(p_pack_path) {}

	Dictionary to_dict() const;
	String to_json_block() const;
	Error write_verify_file(const String &p_path) const;

private:
	String pack_path;
};
