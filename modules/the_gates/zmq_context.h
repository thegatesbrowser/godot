/**************************************************************************/
/*  zmq_context.h                                                         */
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

#ifndef ZMQ_CONTEXT_H
#define ZMQ_CONTEXT_H

#include "core/os/os.h"
#include "core/string/ustring.h"
#include "thirdparty/cppzmq/zmq.hpp"

inline zmq::context_t ctx;

// Defined in main/main.cpp, populated by the --tg-ipc-dir CLI arg the
// launcher passes when spawning the renderer. Empty in the launcher
// process (which uses its own OS::get_user_data_dir() as the fallback).
extern String tg_ipc_dir_override;

// Resolve a zmq address whose path component is `user://...` to an absolute
// `ipc://<dir>/...` address. Returns p_address unchanged if no `user://`
// marker is present.
//
// `<dir>` comes from tg_ipc_dir_override (set by --tg-ipc-dir) if present,
// otherwise falls back to OS::get_user_data_dir(). The override exists
// because the renderer's OS::get_user_data_dir() resolves to the loaded
// gate's project name ("TheGates Tutorial" etc.), not the launcher's
// "TheGates", so the two processes don't naturally agree on a path.
//
// TODO: longer-term, the launcher should own the renderer's whole working
// directory (including the sockets it creates), removing the need for this
// override at all.
inline String tg_resolve_ipc_address(const String &p_address) {
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
	return resolved.replace("\\", "/");
}

#endif // ZMQ_CONTEXT_H
