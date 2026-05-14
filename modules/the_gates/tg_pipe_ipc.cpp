/**************************************************************************/
/*  tg_pipe_ipc.cpp                                                       */
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

#include "tg_pipe_ipc.h"

#include "core/io/marshalls.h"
#include "core/os/os.h"

namespace {
constexpr int FRAME_HEADER_SIZE = 4;
} // namespace

String TgPipeIpc::make_read_address() const {
	return role == ROLE_BIND ? base_address + ".c2s" : base_address + ".s2c";
}

String TgPipeIpc::make_write_address() const {
	return role == ROLE_BIND ? base_address + ".s2c" : base_address + ".c2s";
}

void TgPipeIpc::mark_activity() {
	last_activity_usec = OS::get_singleton()->get_ticks_usec();
}

bool TgPipeIpc::try_open() {
	if (connected) {
		return true;
	}
	Error read_error = OK;
	Error write_error = OK;
	read_pipe = FileAccess::open(make_read_address(), FileAccess::READ_WRITE, &read_error);
	write_pipe = FileAccess::open(make_write_address(), FileAccess::READ_WRITE, &write_error);
	if (read_error != OK || write_error != OK || read_pipe.is_null() || write_pipe.is_null()) {
		close_handles();
		return false;
	}
	connected = true;
	mark_activity();
	return true;
}

void TgPipeIpc::close_handles() {
	connected = false;
	read_pipe.unref();
	write_pipe.unref();
}

bool TgPipeIpc::bind(const String &p_address) {
	close();
	role = ROLE_BIND;
	base_address = p_address;
	return try_open();
}

bool TgPipeIpc::connect(const String &p_address) {
	close();
	role = ROLE_CONNECT;
	base_address = p_address;
	return try_open();
}

void TgPipeIpc::queue_message(const Vector<uint8_t> &p_payload) {
	const uint32_t size = (uint32_t)p_payload.size();
	Vector<uint8_t> framed;
	framed.resize(FRAME_HEADER_SIZE + (int)size);
	encode_uint32(size, framed.ptrw());
	if (size > 0) {
		memcpy(framed.ptrw() + FRAME_HEADER_SIZE, p_payload.ptr(), size);
	}
	send_queue.push_back(framed);
}

bool TgPipeIpc::pump_write() {
	if (!connected) {
		return false;
	}
	while (!send_queue.is_empty()) {
		const Vector<uint8_t> &msg = send_queue[0];
		write_pipe->store_buffer(msg.ptr(), msg.size());
		const Error err = write_pipe->get_error();
		if (err == OK) {
			send_queue.remove_at(0);
			mark_activity();
			continue;
		}
		if (err == ERR_BUSY) {
			return true;
		}
		close_handles();
		return false;
	}
	return true;
}

bool TgPipeIpc::pump_read() {
	if (!connected) {
		return false;
	}
	while (true) {
		const uint64_t available = read_pipe->get_length();
		if (available == 0) {
			break;
		}
		const int prev_size = recv_stream.size();
		recv_stream.resize(prev_size + (int)available);
		const uint64_t got = read_pipe->get_buffer(recv_stream.ptrw() + prev_size, available);
		if (got == 0) {
			recv_stream.resize(prev_size);
			break;
		}
		if (got != available) {
			recv_stream.resize(prev_size + (int)got);
		}
		mark_activity();
	}
	extract_frames();
	return true;
}

void TgPipeIpc::extract_frames() {
	int offset = 0;
	while (recv_stream.size() - offset >= FRAME_HEADER_SIZE) {
		const uint8_t *header = recv_stream.ptr() + offset;
		const uint32_t payload_size = decode_uint32(header);
		const int frame_size = FRAME_HEADER_SIZE + (int)payload_size;
		if (recv_stream.size() - offset < frame_size) {
			break;
		}
		Vector<uint8_t> payload;
		payload.resize((int)payload_size);
		if (payload_size > 0) {
			memcpy(payload.ptrw(), header + FRAME_HEADER_SIZE, payload_size);
		}
		recv_queue.push_back(payload);
		offset += frame_size;
	}
	if (offset > 0) {
		const int remaining = recv_stream.size() - offset;
		if (remaining > 0) {
			memmove(recv_stream.ptrw(), recv_stream.ptr() + offset, remaining);
		}
		recv_stream.resize(remaining);
	}
}

bool TgPipeIpc::poll() {
	if (!connected && !base_address.is_empty()) {
		try_open();
	}
	if (!connected) {
		return false;
	}
	const bool write_ok = pump_write();
	const bool read_ok = pump_read();
	return write_ok && read_ok;
}

bool TgPipeIpc::pop_message(Vector<uint8_t> &r_message) {
	if (recv_queue.is_empty()) {
		return false;
	}
	r_message = recv_queue[0];
	recv_queue.remove_at(0);
	return true;
}

bool TgPipeIpc::is_connected(uint64_t p_idle_timeout_usec) const {
	if (!connected) {
		return false;
	}
	if (p_idle_timeout_usec == 0 || last_activity_usec == 0) {
		return true;
	}
	return OS::get_singleton()->get_ticks_usec() - last_activity_usec <= p_idle_timeout_usec;
}

void TgPipeIpc::close() {
	close_handles();
	recv_stream.clear();
	recv_queue.clear();
	send_queue.clear();
	last_activity_usec = 0;
}
