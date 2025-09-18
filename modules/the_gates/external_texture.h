/**************************************************************************/
/*  external_texture.h                                                    */
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
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_device_binds.h"

#ifdef WINDOWS_ENABLED
static const String FILEHANDLE_PATH("ipc://renderer/external_texture");
#elif MACOS_ENABLED
static const String FILEHANDLE_PATH("ipc:///tmp/external_texture");
#else
static const String FILEHANDLE_PATH("/tmp/external_texture");
#endif

class TGExternalTexture : public RefCounted {
	GDCLASS(TGExternalTexture, RefCounted);

	RID rid;
	RD::TextureView view;
	RD::TextureFormat format;
	FileHandle filehandle = FileHandleInvalid;

	Error _copy(RID p_texture, bool p_from);

protected:
	static void _bind_methods();
	Error _create(const Ref<RDTextureFormat> &p_format, const Ref<RDTextureView> &p_view, const TypedArray<PackedByteArray> &p_data);
	Error _import(const Ref<RDTextureFormat> &p_format, const Ref<RDTextureView> &p_view);

public:
	Error create(const RD::TextureFormat &p_format, const RD::TextureView &p_view, const Vector<Vector<uint8_t>> &p_data = Vector<Vector<uint8_t>>());
	Error import(const RD::TextureFormat &p_format, const RD::TextureView &p_view);
	bool send_filehandle(const String &p_path = FILEHANDLE_PATH);
	bool recv_filehandle(const String &p_path = FILEHANDLE_PATH);

	Error copy_to(RID p_texture) { return _copy(p_texture, false); }
	Error copy_from(RID p_texture) { return _copy(p_texture, true); }
	Error copy_from_screen();

	RID get_rid() const { return rid; }

	TGExternalTexture();
	~TGExternalTexture();
};
