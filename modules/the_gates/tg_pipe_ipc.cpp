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

#include "core/os/os.h"

String TgPipeIpc::normalize_address(const String &p_address) const {
	if (p_address.begins_with("pipe://")) {
		return p_address;
	}
	if (p_address.begins_with("ipc://")) {
		return "pipe://" + p_address.substr(6);
	}
	if (p_address.begins_with("/")) {
		return "pipe://" + p_address;
	}
	return "pipe://" + p_address;
}

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

	if (read_error != OK || write_error != OK || read_pipe.is_null() || write_pipe.is_null() || !read_pipe->is_open() || !write_pipe->is_open()) {
		read_pipe.unref();
		write_pipe.unref();
		connected = false;
		return false;
	}

	connected = true;
	mark_activity();
	return true;
}

bool TgPipeIpc::bind(const String &p_address) {
	close();
	role = ROLE_BIND;
	base_address = normalize_address(p_address);
	return try_open();
}

bool TgPipeIpc::connect(const String &p_address) {
	close();
	role = ROLE_CONNECT;
	base_address = normalize_address(p_address);
	return try_open();
}

void TgPipeIpc::push_frame(const Vector<uint8_t> &p_payload) {
	uint32_t size = (uint32_t)p_payload.size();
	Vector<uint8_t> framed;
	framed.resize((int)size + 4);
	uint8_t *dst = framed.ptrw();
	dst[0] = (uint8_t)(size & 0xFF);
	dst[1] = (uint8_t)((size >> 8) & 0xFF);
	dst[2] = (uint8_t)((size >> 16) & 0xFF);
	dst[3] = (uint8_t)((size >> 24) & 0xFF);
	if (size > 0) {
		memcpy(dst + 4, p_payload.ptr(), size);
	}
	send_queue.push_back(framed);
}

void TgPipeIpc::queue_message(const Vector<uint8_t> &p_payload) {
	push_frame(p_payload);
}

bool TgPipeIpc::pump_write() {
	if (!connected || write_pipe.is_null()) {
		return false;
	}

	while (!send_queue.is_empty()) {
		const Vector<uint8_t> &msg = send_queue[0];
		write_pipe->store_buffer(msg.ptr(), msg.size());
		Error write_error = write_pipe->get_error();
		if (write_error == OK) {
			send_queue.remove_at(0);
			mark_activity();
			continue;
		}
		if (write_error == ERR_BUSY) {
			return true;
		}
		connected = false;
		read_pipe.unref();
		write_pipe.unref();
		return false;
	}

	return true;
}

bool TgPipeIpc::pump_read() {
	if (!connected || read_pipe.is_null()) {
		return false;
	}

	while (true) {
		uint64_t available = read_pipe->get_length();
		if (available == 0) {
			break;
		}

		Vector<uint8_t> chunk;
		chunk.resize((int)available);
		uint64_t read_size = read_pipe->get_buffer(chunk.ptrw(), available);
		if (read_size == 0) {
			break;
		}

		if (read_size != available) {
			chunk.resize((int)read_size);
		}
		recv_stream.append_array(chunk);
		mark_activity();
	}

	while (recv_stream.size() >= 4) {
		const uint8_t *src = recv_stream.ptr();
		uint32_t payload_size = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
		uint64_t frame_size = 4 + payload_size;
		if ((uint64_t)recv_stream.size() < frame_size) {
			break;
		}

		Vector<uint8_t> payload;
		payload.resize((int)payload_size);
		if (payload_size > 0) {
			memcpy(payload.ptrw(), src + 4, payload_size);
		}
		recv_queue.push_back(payload);

		Vector<uint8_t> rest;
		rest.resize(recv_stream.size() - (int)frame_size);
		if (!rest.is_empty()) {
			memcpy(rest.ptrw(), src + frame_size, rest.size());
		}
		recv_stream = rest;
	}

	return true;
}

bool TgPipeIpc::poll() {
	if (!connected && !base_address.is_empty()) {
		try_open();
	}
	if (!connected) {
		return false;
	}
	bool write_ok = pump_write();
	bool read_ok = pump_read();
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
	connected = false;
	if (read_pipe.is_valid()) {
		read_pipe->close();
	}
	if (write_pipe.is_valid()) {
		write_pipe->close();
	}
	read_pipe.unref();
	write_pipe.unref();
	recv_stream.clear();
	recv_queue.clear();
	send_queue.clear();
	last_activity_usec = 0;
}
