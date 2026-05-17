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
#include "core/os/thread.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"

class SandboxPolicy;

class Sandbox : public RefCounted {
	GDCLASS(Sandbox, RefCounted);

	struct VerifyJob;
	VerifyJob *current_job = nullptr;
	Thread verify_thread;

	static void _verify_thread_func(void *p_userdata);
	void _verify_done();

protected:
	static void _bind_methods();

	// Per-platform synchronous verify. Called from a worker thread by
	// verify_binary(); see _verify_thread_func. Returns OK / ERR_UNAUTHORIZED.
	virtual Error _verify_binary_impl(const String &p_path) = 0;

public:
	// Null Ref<> on builds with no backend (e.g. Windows without tg_sandbox).
	static Ref<Sandbox> create();

	// Returns { pid, thread_id } on success, empty Dictionary on failure.
	virtual Dictionary spawn_target(const Ref<SandboxPolicy> &p_policy,
			const String &p_executable, const Vector<String> &p_arguments) = 0;
	virtual void apply_renderer_acl(const String &p_path) = 0;

	// Non-blocking. Kicks the hashing / signature check onto a worker thread
	// and returns a Signal the caller can `await`; the signal fires once with
	// the verify Error. Returns an empty Signal if another verify is already
	// in flight on this broker.
	Signal verify_binary(const String &p_path);

	virtual bool is_target_running() const = 0;
	virtual Error kill_target() = 0;

	// Fail-closed: a non-OK return MUST abort the caller.
	virtual Error lower_token() = 0;
	virtual bool is_target() const = 0;

	Sandbox() = default;
	virtual ~Sandbox();
};
