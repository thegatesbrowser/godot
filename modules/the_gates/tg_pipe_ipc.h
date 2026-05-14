/**************************************************************************/
/*  tg_pipe_ipc.h                                                         */
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

#ifndef TG_PIPE_IPC_H
#define TG_PIPE_IPC_H

#include "core/string/ustring.h"
#include "core/templates/vector.h"

#ifndef WINDOWS_ENABLED
#include "core/io/file_access.h"
#endif

class TgPipeIpc {
public:
	enum Role {
		ROLE_BIND,
		ROLE_CONNECT,
	};

private:
	String base_address;
	Role role = ROLE_CONNECT;

#ifdef WINDOWS_ENABLED
	// Raw Win32 HANDLEs stored as void* to avoid leaking <windows.h>
	// into every translation unit that includes this header.
	void *read_handle = nullptr;
	void *write_handle = nullptr;
#else
	Ref<FileAccess> read_pipe;
	Ref<FileAccess> write_pipe;
#endif

	Vector<uint8_t> recv_stream;
	Vector<Vector<uint8_t>> recv_queue;
	Vector<Vector<uint8_t>> send_queue;
	uint64_t last_activity_usec = 0;
	bool connected = false;

	String make_read_address() const;
	String make_write_address() const;
	bool try_open();
	void close_handles();
	bool pump_read();
	bool pump_write();
	void extract_frames();
	void mark_activity();

public:
	bool bind(const String &p_address);
	bool connect(const String &p_address);
	void queue_message(const Vector<uint8_t> &p_payload);
	bool poll();
	bool pop_message(Vector<uint8_t> &r_message);
	bool is_connected(uint64_t p_idle_timeout_usec = 0) const;
	void close();
};

#endif // TG_PIPE_IPC_H
