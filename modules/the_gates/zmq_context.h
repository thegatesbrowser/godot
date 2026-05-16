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

#pragma once

#include "core/os/os.h"
#include "core/string/ustring.h"
#include "thirdparty/cppzmq/zmq.hpp"

inline zmq::context_t ctx;

extern String tg_ipc_dir_override;

// TODO: launcher should own renderer's working dir, removing this override
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
