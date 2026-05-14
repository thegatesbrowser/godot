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

#ifdef WINDOWS_ENABLED
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

constexpr DWORD PIPE_BUFFER_SIZE = 4096;
constexpr DWORD PIPE_MAX_INSTANCES = 1;

enum WinIoStatus {
	WIN_OK,
	WIN_BUSY, // Transient: peer not attached yet, would-block, buffer full.
	WIN_FATAL, // Handle is unusable for further I/O.
};

String to_win_pipe_path(const String &p_address) {
	return String("\\\\.\\pipe\\LOCAL\\") + p_address.replace("pipe://", "").replace("/", "_");
}

HANDLE open_win_pipe(const String &p_address) {
	const String win_path = to_win_pipe_path(p_address);
	const Char16String wpath = win_path.utf16();
	HANDLE h = CreateFileW((LPCWSTR)wpath.get_data(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
		SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
		return h;
	}
	h = CreateNamedPipeW((LPCWSTR)wpath.get_data(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, PIPE_MAX_INSTANCES, PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return nullptr;
	}
	ConnectNamedPipe(h, nullptr);
	return h;
}

void close_win_pipe(void *&h) {
	if (h != nullptr && h != INVALID_HANDLE_VALUE) {
		CloseHandle((HANDLE)h);
	}
	h = nullptr;
}

WinIoStatus classify_win_error(DWORD p_win_err) {
	if (p_win_err == ERROR_BROKEN_PIPE || p_win_err == ERROR_INVALID_HANDLE) {
		return WIN_FATAL;
	}
	return WIN_BUSY;
}

WinIoStatus peek_win_pipe(HANDLE p_h, uint64_t &r_available) {
	DWORD avail = 0;
	if (!PeekNamedPipe(p_h, nullptr, 0, nullptr, &avail, nullptr)) {
		r_available = 0;
		return classify_win_error(GetLastError());
	}
	r_available = avail;
	return WIN_OK;
}

WinIoStatus read_win_pipe(HANDLE p_h, uint8_t *p_dst, uint64_t p_len, uint64_t &r_read) {
	DWORD got = 0;
	if (!ReadFile(p_h, p_dst, (DWORD)p_len, &got, nullptr)) {
		r_read = got;
		return classify_win_error(GetLastError());
	}
	r_read = got;
	return got == 0 ? WIN_BUSY : WIN_OK;
}

WinIoStatus write_win_pipe(HANDLE p_h, const uint8_t *p_src, uint64_t p_len) {
	DWORD wrote = 0;
	if (!WriteFile(p_h, p_src, (DWORD)p_len, &wrote, nullptr)) {
		return classify_win_error(GetLastError());
	}
	// PIPE_NOWAIT writes are atomic: either all bytes go or none.
	return wrote == p_len ? WIN_OK : WIN_BUSY;
}

} // namespace
#endif // WINDOWS_ENABLED

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

#ifdef WINDOWS_ENABLED
	if (read_handle == nullptr) {
		read_handle = open_win_pipe(make_read_address());
	}
	if (write_handle == nullptr) {
		write_handle = open_win_pipe(make_write_address());
	}
	if (read_handle == nullptr || write_handle == nullptr) {
		close_handles();
		return false;
	}
#else
	Error read_error = OK;
	Error write_error = OK;
	read_pipe = FileAccess::open(make_read_address(), FileAccess::READ_WRITE, &read_error);
	write_pipe = FileAccess::open(make_write_address(), FileAccess::READ_WRITE, &write_error);
	if (read_error != OK || write_error != OK || read_pipe.is_null() || write_pipe.is_null()) {
		close_handles();
		return false;
	}
#endif

	connected = true;
	mark_activity();
	return true;
}

void TgPipeIpc::close_handles() {
	connected = false;
#ifdef WINDOWS_ENABLED
	close_win_pipe(read_handle);
	close_win_pipe(write_handle);
#else
	read_pipe.unref();
	write_pipe.unref();
#endif
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
#ifdef WINDOWS_ENABLED
		const WinIoStatus status = write_win_pipe((HANDLE)write_handle, msg.ptr(), (uint64_t)msg.size());
		const bool ok = (status == WIN_OK);
		const bool busy = (status == WIN_BUSY);
#else
		write_pipe->store_buffer(msg.ptr(), msg.size());
		const Error err = write_pipe->get_error();
		const bool ok = (err == OK);
		const bool busy = (err == ERR_BUSY);
#endif
		if (ok) {
			send_queue.remove_at(0);
			mark_activity();
			continue;
		}
		if (busy) {
			return true;
		}
		// Fatal: drop both handles so the next poll() retries from scratch.
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
		uint64_t available = 0;
#ifdef WINDOWS_ENABLED
		if (peek_win_pipe((HANDLE)read_handle, available) != WIN_OK || available == 0) {
			break;
		}
#else
		available = read_pipe->get_length();
		if (available == 0) {
			break;
		}
#endif
		const int prev_size = recv_stream.size();
		recv_stream.resize(prev_size + (int)available);
		uint8_t *dst = recv_stream.ptrw() + prev_size;

		uint64_t got = 0;
#ifdef WINDOWS_ENABLED
		read_win_pipe((HANDLE)read_handle, dst, available, got);
#else
		got = read_pipe->get_buffer(dst, available);
#endif
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
