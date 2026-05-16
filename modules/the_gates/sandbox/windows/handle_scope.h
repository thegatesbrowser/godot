/**************************************************************************/
/*  handle_scope.h                                                        */
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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// RAII wrapper for a Win32 HANDLE. Closes on destruction unless the handle
// is INVALID_HANDLE_VALUE or nullptr. Move-only. Use release() to transfer
// ownership (the caller takes responsibility for closing).
class HandleScope {
public:
	HandleScope() = default;
	explicit HandleScope(HANDLE p_handle) :
			handle(p_handle) {}

	HandleScope(HandleScope &&p_other) noexcept :
			handle(p_other.release()) {}

	HandleScope &operator=(HandleScope &&p_other) noexcept {
		if (this != &p_other) {
			reset(p_other.release());
		}
		return *this;
	}

	HandleScope(const HandleScope &) = delete;
	HandleScope &operator=(const HandleScope &) = delete;

	~HandleScope() {
		reset();
	}

	HANDLE get() const { return handle; }

	bool is_valid() const {
		return handle != INVALID_HANDLE_VALUE && handle != nullptr;
	}

	HANDLE release() {
		HANDLE h = handle;
		handle = INVALID_HANDLE_VALUE;
		return h;
	}

	void reset(HANDLE p_handle = INVALID_HANDLE_VALUE) {
		if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
			::CloseHandle(handle);
		}
		handle = p_handle;
	}

private:
	HANDLE handle = INVALID_HANDLE_VALUE;
};
