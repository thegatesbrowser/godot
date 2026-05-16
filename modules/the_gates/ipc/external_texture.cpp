/**************************************************************************/
/*  external_texture.cpp                                                  */
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

#include "external_texture.h"

#ifdef WINDOWS_ENABLED
#define WIN32_LEAN_AND_MEAN
#include "thirdparty/cppzmq/zmq.hpp"
#include "zmq_runtime.h"
#include <windows.h>
#endif

#if MACOS_ENABLED
#include "thirdparty/cppzmq/zmq.hpp"
#include "zmq_runtime.h"
#include <IOSurface/IOSurface.h>
#endif

#if LINUXBSD_ENABLED
#include "flingfd.h"
#endif

bool TGExternalTexture::send_filehandle(const String &p_path) {
	ERR_FAIL_COND_V_MSG(filehandle == FileHandleInvalid, false, "Sending invalid filehandle. First create external texture");

#ifdef WINDOWS_ENABLED
	// Duplicate handle
	PackedStringArray split = p_path.split("|");
	ERR_FAIL_COND_V_MSG(split.size() != 2, false, "Invalid path. Should be 'ipc://path|targetProcessId'");
	int targetProcessId = split[1].to_int();

	HANDLE hTargetProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, targetProcessId);
	ERR_FAIL_COND_V_MSG(hTargetProcess == INVALID_HANDLE_VALUE, false, "Unable to OpenProcess from " + itos(targetProcessId));

	HANDLE hDuplicateHandle;
	bool success = DuplicateHandle(GetCurrentProcess(), (HANDLE)filehandle, hTargetProcess, &hDuplicateHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
	ERR_FAIL_COND_V_MSG(!success, false, "Unable to DuplicateHandle. Error code: " + itos(GetLastError()));

	filehandle = (FileHandle)hDuplicateHandle;
	zmq::socket_t sock(tg_zmq_context(), zmq::socket_type::pair);
	sock.connect(tg_resolve_ipc_address(split[0]).utf8().get_data());

	int64_t data = reinterpret_cast<int64_t>(filehandle);
	zmq::message_t msg(sizeof(int64_t));
	memcpy(msg.data(), &data, sizeof(int64_t));
	success = sock.send(msg, zmq::send_flags::none).has_value();
	sock.close();

	// Clean up
	CloseHandle(hTargetProcess);
	if (!success) {
		CloseHandle(hDuplicateHandle);
	}

	return success;
#elif MACOS_ENABLED
	uint32_t surfaceID = IOSurfaceGetID(filehandle);

	zmq::socket_t sock(tg_zmq_context(), zmq::socket_type::pair);
	sock.connect(tg_resolve_ipc_address(p_path).utf8().get_data());

	zmq::message_t msg(sizeof(uint32_t));
	memcpy(msg.data(), &surfaceID, sizeof(uint32_t));
	bool success = sock.send(msg, zmq::send_flags::none).has_value();
	sock.close();

	return success;
#else
	return flingfd_simple_send(p_path.utf8().get_data(), filehandle);
#endif
}

bool TGExternalTexture::recv_filehandle(const String &p_path) {
#ifdef WINDOWS_ENABLED
	zmq::socket_t sock(tg_zmq_context(), zmq::socket_type::pair);
	sock.bind(tg_resolve_ipc_address(p_path).utf8().get_data());

	zmq::message_t msg;
	if (!sock.recv(msg)) { // WARNING: BLOCKING COMMAND
		return false;
	}
	int64_t data;
	ERR_FAIL_COND_V(msg.size() != sizeof(int64_t), false);
	memcpy(&data, msg.data(), sizeof(int64_t));
	sock.close();

	filehandle = reinterpret_cast<void *>(data);
#elif MACOS_ENABLED
	zmq::socket_t sock(tg_zmq_context(), zmq::socket_type::pair);
	sock.bind(tg_resolve_ipc_address(p_path).utf8().get_data());

	zmq::message_t msg;
	if (!sock.recv(msg)) { // WARNING: BLOCKING COMMAND
		return false;
	}
	uint32_t surfaceID;
	ERR_FAIL_COND_V(msg.size() != sizeof(uint32_t), false);
	memcpy(&surfaceID, msg.data(), sizeof(uint32_t));
	sock.close();

	filehandle = IOSurfaceLookup(surfaceID);
#else
	filehandle = flingfd_simple_recv(p_path.utf8().get_data()); // WARNING: BLOCKING COMMAND
#endif
	ERR_FAIL_COND_V_MSG(filehandle == FileHandleInvalid, false, "Receive filehandle failed");

	return true;
}

Error TGExternalTexture::create(const RD::TextureFormat &p_format, const RD::TextureView &p_view, const Vector<Vector<uint8_t>> &p_data) {
	view = p_view;
	format = p_format;
	rid = RD::get_singleton()->external_texture_create(p_format, p_view, &filehandle, p_data);
	ERR_FAIL_COND_V_MSG(!rid.is_valid(), ERR_CANT_CREATE, "Unable to create external texture");
	ERR_FAIL_COND_V_MSG(filehandle == FileHandleInvalid, ERR_CANT_CREATE, "Unable to export filehandle");

	return OK;
}

Error TGExternalTexture::import(const RD::TextureFormat &p_format, const RD::TextureView &p_view) {
	ERR_FAIL_COND_V_MSG(filehandle == FileHandleInvalid, ERR_UNAVAILABLE, "filehandle is no valid. Receive filehandle first");

	view = p_view;
	format = p_format;
	rid = RD::get_singleton()->external_texture_import(format, view, filehandle);
	ERR_FAIL_COND_V_MSG(!rid.is_valid(), ERR_CANT_CREATE, "Unable to import external texture from filehandle");

	return OK;
}

Error TGExternalTexture::copy_from_screen() {
	ERR_FAIL_COND_V_MSG(!rid.is_valid(), ERR_UNAVAILABLE, "ExternalTexture is not valid. Create or import first");

	Vector3 size = {
		static_cast<float>(format.width),
		static_cast<float>(format.height),
		static_cast<float>(format.depth)
	};
	const Vector3 zero = { 0, 0, 0 };

	return RD::get_singleton()->screen_copy(rid, zero, size, 0, 0);
}

Error TGExternalTexture::_create(const Ref<RDTextureFormat> &p_format, const Ref<RDTextureView> &p_view, const TypedArray<PackedByteArray> &p_data) {
	ERR_FAIL_COND_V(p_format.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_view.is_null(), ERR_INVALID_PARAMETER);

	Vector<Vector<uint8_t>> data;
	for (int i = 0; i < p_data.size(); i++) {
		Vector<uint8_t> byte_slice = p_data[i];
		ERR_FAIL_COND_V(byte_slice.is_empty(), ERR_INVALID_PARAMETER);
		data.push_back(byte_slice);
	}

	return create(RD::_get_base(p_format), RD::_get_base(p_view), data);
}

Error TGExternalTexture::_import(const Ref<RDTextureFormat> &p_format, const Ref<RDTextureView> &p_view) {
	ERR_FAIL_COND_V(p_format.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_view.is_null(), ERR_INVALID_PARAMETER);

	return import(RD::_get_base(p_format), RD::_get_base(p_view));
}

Error TGExternalTexture::_copy(RID p_texture, bool p_from) {
	ERR_FAIL_COND_V_MSG(!rid.is_valid(), ERR_UNAVAILABLE, "ExternalTexture is not valid. Create or import first");
	ERR_FAIL_COND_V_MSG(!p_texture.is_valid(), ERR_INVALID_PARAMETER, "Parameter texture is not valid");

	Vector3 size = {
		static_cast<float>(format.width),
		static_cast<float>(format.height),
		static_cast<float>(format.depth)
	};
	const Vector3 zero = { 0, 0, 0 };

	if (p_from) {
		return RD::get_singleton()->texture_copy(p_texture, rid, zero, zero, size, 0, 0, 0, 0);
	} else {
		return RD::get_singleton()->texture_copy(rid, p_texture, zero, zero, size, 0, 0, 0, 0);
	}
}

void TGExternalTexture::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "format", "view"), &TGExternalTexture::_create, DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("send_filehandle", "path"), &TGExternalTexture::send_filehandle);
	ClassDB::bind_method(D_METHOD("copy_to", "texture"), &TGExternalTexture::copy_to);
	ClassDB::bind_method(D_METHOD("copy_from", "texture"), &TGExternalTexture::copy_from);
	ClassDB::bind_method(D_METHOD("get_rid"), &TGExternalTexture::get_rid);
}

TGExternalTexture::TGExternalTexture() {
}

TGExternalTexture::~TGExternalTexture() {
	if (rid.is_valid()) {
		RD::get_singleton()->free(rid);
		print_line("Texture freed " + itos(rid.get_id()));
	}
}
