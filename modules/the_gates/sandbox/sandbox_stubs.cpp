/**************************************************************************/
/*  sandbox_stubs.cpp                                                     */
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

// the_gates: catch-all stub for chromium 137 symbols the sandbox lib calls
// transitively but our wrapper never exercises. Each stub uses the real
// chromium headers so signatures stay in lockstep with whatever the .cc that
// references them sees.

#include <windows.h>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/memory/protected_memory.h"
#include "base/path_service.h"
#include "base/task/current_thread.h"
#include "base/threading/hang_watcher.h"

namespace base {

// ----- FilePath helpers chromium 137 added that Firefox's shim doesn't ----

bool FilePath::IsAbsolute() const {
	const StringType &v = value();
	if (v.size() >= 2 && v[1] == L':' &&
			((v[0] >= L'A' && v[0] <= L'Z') || (v[0] >= L'a' && v[0] <= L'z'))) {
		return true;
	}
	return v.size() >= 2 && v[0] == L'\\' && v[1] == L'\\';
}

bool FilePath::operator==(const FilePath &other) const {
	return value() == other.value();
}

// ----- GetPageSize -----

size_t GetPageSize() {
	SYSTEM_INFO info{};
	::GetSystemInfo(&info);
	return info.dwPageSize ? info.dwPageSize : 4096u;
}

// ----- CommandLine — no-op so time_win.cc's MaybeAddHighResolutionTimeTicks
//       check returns false and nothing else happens. We don't store any
//       state.

// We never make InitializedForCurrentProcess() return true, so ForCurrentProcess
// is dead code. Return nullptr — saves us from having to drag in the chromium
// command_line.cc constructor + storage. The only call site in our build
// (time_win.cc:614) gates on InitializedForCurrentProcess first.
CommandLine *CommandLine::ForCurrentProcess() {
	return nullptr;
}
bool CommandLine::InitializedForCurrentProcess() {
	return false;
}

bool CommandLine::HasSwitch(const char *const /*sw*/) const {
	return false;
}

std::string CommandLine::GetSwitchValueASCII(std::string_view /*sw*/) const {
	return std::string();
}

void CommandLine::AppendSwitchASCII(std::string_view, std::string_view) {}

// ----- PathService — fail every lookup; we never use it -----
bool PathService::Get(int /*key*/, FilePath * /*path*/) {
	return false;
}

// ----- HangWatcher — sandbox never trips it; no-op -----
void HangWatcher::InvalidateActiveExpectations() {}

// ----- CurrentIOThread / CurrentUIThread — sandbox is its own thread -----
bool CurrentIOThread::IsSet() {
	return false;
}
bool CurrentUIThread::IsSet() {
	return false;
}

// ----- AutoWritableMemory — code-page protection toggles. Interceptors call
// these when patching ntdll thunks. SetMemoryReadWrite/ReadOnly used to be
// inline in the header in older chromium; 137 moved them to .cc. Provide
// safe defaults: actually flip page protection.
bool AutoWritableMemoryBase::SetMemoryReadOnly(void *start, void *end) {
	if (start == nullptr || end == nullptr || end <= start) {
		return true;
	}
	DWORD old = 0;
	return ::VirtualProtect(start,
				   static_cast<SIZE_T>(static_cast<char *>(end) -
						   static_cast<char *>(start)),
				   PAGE_EXECUTE_READ, &old) != FALSE;
}
bool AutoWritableMemoryBase::SetMemoryReadWrite(void *start, void *end) {
	if (start == nullptr || end == nullptr || end <= start) {
		return true;
	}
	DWORD old = 0;
	return ::VirtualProtect(start,
				   static_cast<SIZE_T>(static_cast<char *>(end) -
						   static_cast<char *>(start)),
				   PAGE_EXECUTE_READWRITE, &old) != FALSE;
}

namespace internal {
void CheckMemoryReadOnly(const void * /*p*/) {}
} // namespace internal

} // namespace base

// ----- switches:: constants chromium command_line callers reference -----
namespace switches {
extern const char kForceHighResTimeTicks[];
const char kForceHighResTimeTicks[] = "force-high-res-time-ticks";
} //namespace switches

// PR_ParseTimeString + double_conversion now provided by their real .cc
// files (vendored under base/third_party/{nspr,double_conversion}/), no
// stubs needed.
