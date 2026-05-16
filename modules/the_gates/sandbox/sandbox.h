/**************************************************************************/
/*  sandbox.h                                                             */
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

class SandboxPolicy;

class Sandbox : public RefCounted {
	GDCLASS(Sandbox, RefCounted);

protected:
	static void _bind_methods();

public:
	// Returns the platform-appropriate Sandbox impl, or a null Ref<> on
	// platforms/builds with no backend (a launcher built with --no-sandbox,
	// or a platform with no implementation yet).
	static Ref<Sandbox> create();

	// Broker-side (called from the launcher process).
	// Returns Dictionary { pid, thread_id, process_handle, thread_handle }
	// on success, empty Dictionary on failure.
	virtual Dictionary spawn_target(const Ref<SandboxPolicy> &p_policy,
			const String &p_executable, const Vector<String> &p_arguments) = 0;
	virtual void apply_renderer_acl(const String &p_path) = 0;

	// Fail-closed Authenticode / codesign / detached-signature check on the
	// renderer binary before spawn. Returns ERR_UNAUTHORIZED on any failure
	// (invalid signature, wrong thumbprint, missing cert).
	virtual Error verify_binary(const String &p_path) = 0;

	// Liveness of the most recently spawned target. The launcher used to ask
	// the engine via OS.is_process_running(pid); that required a fork-side
	// hook in OS_Windows for sandbox-spawned children. The Sandbox instance
	// holds its own handle and answers locally — no engine touchpoint.
	virtual bool is_target_running() const = 0;
	virtual Error kill_target() = 0;

	// Target-side (called from the renderer process). Fail-closed: returns
	// non-OK if the lockdown step fails — caller MUST abort.
	virtual Error lower_token() = 0;
	virtual bool is_target() const = 0;

	Sandbox() = default;
	virtual ~Sandbox() = default;
};
