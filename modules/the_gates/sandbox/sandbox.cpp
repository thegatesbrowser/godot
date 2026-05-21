/**************************************************************************/
/*  sandbox.cpp                                                           */
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

#include "sandbox.h"

#include "../network/network_broker.h"
#include "sandbox_policy.h"

#include "core/object/worker_thread_pool.h"

#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
#include "windows/sandbox_win.h"
#elif defined(TG_SANDBOX) && defined(LINUXBSD_ENABLED)
#include "linux/sandbox_linux.h"
#elif defined(TG_SANDBOX) && defined(MACOS_ENABLED)
#include "macos/sandbox_macos.h"
#endif

struct Sandbox::VerifyJob {
	Ref<Sandbox> self;
	String path;
	Error result = OK;
};

Ref<Sandbox> Sandbox::create() {
#if defined(TG_SANDBOX) && defined(WINDOWS_ENABLED)
	return Ref<Sandbox>(memnew(SandboxWin));
#elif defined(TG_SANDBOX) && defined(LINUXBSD_ENABLED)
	return Ref<Sandbox>(memnew(SandboxLinux));
#elif defined(TG_SANDBOX) && defined(MACOS_ENABLED)
	return Ref<Sandbox>(memnew(SandboxMacOS));
#else
	return Ref<Sandbox>();
#endif
}

Signal Sandbox::verify_binary(const String &p_path) {
	ERR_FAIL_COND_V_MSG(current_job != nullptr, Signal(),
			"Sandbox::verify_binary: previous verify still in flight; ignoring");

	VerifyJob *job = memnew(VerifyJob);
	job->self = Ref<Sandbox>(this);
	job->path = p_path;
	current_job = job;

	verify_thread.start(&Sandbox::_verify_thread_func, job);
	return Signal(this, SNAME("verify_finished"));
}

void Sandbox::_verify_thread_func(void *p_userdata) {
	VerifyJob *job = static_cast<VerifyJob *>(p_userdata);
	job->result = job->self->_verify_binary_impl(job->path);
	callable_mp(job->self.ptr(), &Sandbox::_verify_done).call_deferred();
}

void Sandbox::_verify_done() {
	ERR_FAIL_NULL(current_job);

	// memdelete(current_job) below drops the last Ref<Sandbox> if the launcher
	// already let go of the broker; keep `this` alive across emit + return.
	Ref<Sandbox> keep_alive(this);

	verify_thread.wait_to_finish();

	const Error err = current_job->result;
	memdelete(current_job);
	current_job = nullptr;

	emit_signal(SNAME("verify_finished"), (int)err);
}

void Sandbox::start_broker(int p_launcher_fd, void *p_target_handle) {
	if (network_broker.is_null()) {
		network_broker = Ref<NetworkBroker>(memnew(NetworkBroker));
	}
	if (p_target_handle != nullptr) {
		network_broker->set_target_process_handle(p_target_handle);
	}
	const Error e = network_broker->start(p_launcher_fd);
	if (e != OK) {
		network_broker = Ref<NetworkBroker>();
	}
}

void Sandbox::_drain_broker_task(void *p_userdata) {
	Ref<NetworkBroker> *handoff = static_cast<Ref<NetworkBroker> *>(p_userdata);
	memdelete(handoff);
}

void Sandbox::stop_broker() {
	if (network_broker.is_null()) {
		return;
	}
	// Synchronous getaddrinfo / ::connect inside the service thread aren't
	// canceled by closing the control FD, so a direct join can stall for the
	// kernel's TCP-connect timeout (macOS: 75 s). Move the join off-thread.
	network_broker->request_shutdown();
	Ref<NetworkBroker> *handoff = memnew(Ref<NetworkBroker>(network_broker));
	network_broker = Ref<NetworkBroker>();
	WorkerThreadPool::get_singleton()->add_native_task(
			&Sandbox::_drain_broker_task, handoff, false, "TheGates: drain network broker");
}

Dictionary Sandbox::network_state() const {
	if (network_broker.is_null()) {
		Dictionary out;
		out["status"] = "stopped";
		return out;
	}
	return network_broker->state();
}

void Sandbox::_bind_methods() {
	ClassDB::bind_static_method("Sandbox", D_METHOD("create"), &Sandbox::create);
	ClassDB::bind_method(D_METHOD("spawn_target", "policy", "executable", "arguments"),
			&Sandbox::spawn_target);
	ClassDB::bind_method(D_METHOD("apply_renderer_acl", "path"), &Sandbox::apply_renderer_acl);
	ClassDB::bind_method(D_METHOD("verify_binary", "path"), &Sandbox::verify_binary);
	ClassDB::bind_method(D_METHOD("is_target_running"), &Sandbox::is_target_running);
	ClassDB::bind_method(D_METHOD("kill_target"), &Sandbox::kill_target);
	ClassDB::bind_method(D_METHOD("lower_token"), &Sandbox::lower_token);
	ClassDB::bind_method(D_METHOD("is_target"), &Sandbox::is_target);
	ClassDB::bind_method(D_METHOD("network_state"), &Sandbox::network_state);

	ADD_SIGNAL(MethodInfo("verify_finished", PropertyInfo(Variant::INT, "err")));
}

Sandbox::~Sandbox() {
	if (verify_thread.is_started()) {
		verify_thread.wait_to_finish();
	}
	if (current_job != nullptr) {
		memdelete(current_job);
		current_job = nullptr;
	}
}
