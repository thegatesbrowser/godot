/**************************************************************************/
/*  zmq_runtime.cpp                                                       */
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

#include "zmq_runtime.h"

#include "core/os/os.h"
#include "thirdparty/cppzmq/zmq.hpp"

// Disable BLOCKY so zmq_ctx_term doesn't wait LINGER on sockets whose peer
// (the renderer) was SIGKILL'd — the launcher otherwise hangs forever during
// shutdown when a gate is killed mid-stream (multi-gate teardown path).
namespace {
struct TGContext {
	zmq::context_t ctx;
	TGContext() {
		ctx.set(zmq::ctxopt::blocky, 0);
	}
};
} // namespace

zmq::context_t &tg_zmq_context() {
	static TGContext s_ctx;
	return s_ctx.ctx;
}

void tg_zmq_shutdown() {
	tg_zmq_context().close();
}

String tg_resolve_ipc_address(const String &p_address) {
	const String marker = "user://";
	const int idx = p_address.find(marker);
	if (idx < 0) {
		return p_address;
	}
	const String prefix = p_address.substr(0, idx);
	const String suffix = p_address.substr(idx + marker.length());
	const String dir = tg_ipc_dir_override.is_empty()
			? OS::get_singleton()->get_user_data_dir()
			: tg_ipc_dir_override;
	String resolved = prefix + dir + "/" + suffix;
#ifdef WINDOWS_ENABLED
	return resolved.replace_char('\\', '/');
#else
	return resolved;
#endif
}
