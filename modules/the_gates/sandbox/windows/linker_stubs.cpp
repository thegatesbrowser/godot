/**************************************************************************/
/*  linker_stubs.cpp                                                      */
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

// Catch-all stubs for chromium-sandbox library symbols our wrapper never
// exercises. Each stub uses the real chromium header so the signature stays
// in lockstep with whatever the upstream .cc that calls it expects.

#include <windows.h>

#include <shlobj.h>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/location.h"
#include "base/memory/protected_memory.h"
#include "base/path_service.h"
#include "base/task/current_thread.h"
#include "base/threading/hang_watcher.h"
#include "base/time/time.h"
#include "sandbox/win/src/sandbox_policy_base.h"
#include "sandbox/win/src/sandbox_policy_diagnostic.h"

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

// ----- PathService — chromium's AppContainerBase::CreateProfile needs
// DIR_LOCAL_APP_DATA to find the per-user packages directory. SHGetKnownFolderPath
// with FOLDERID_LocalAppData gives us that without dragging chromium's full
// PathService machinery. Other keys aren't called by code we link.
bool PathService::Get(int key, FilePath *path) {
	// base/base_paths_win.h: PATH_WIN_START = 100, then enum increments by 1
	// for each subsequent entry. DIR_LOCAL_APP_DATA is the 12th after
	// PATH_WIN_START (101=DIR_WINDOWS, ..., 112=DIR_LOCAL_APP_DATA). Hard-coded
	// to avoid pulling base_paths_win.h into the stub.
	constexpr int kDirLocalAppData = 100 + 12;
	if (key != kDirLocalAppData || path == nullptr) {
		return false;
	}
	PWSTR raw = nullptr;
	const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
	if (FAILED(hr) || raw == nullptr) {
		if (raw != nullptr) {
			::CoTaskMemFree(raw);
		}
		return false;
	}
	*path = FilePath(raw);
	::CoTaskMemFree(raw);
	return true;
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

// DumpWithoutCrashing — chromium 137's win_util.cc references this via
// SCOPED_CRASH_KEY_NUMBER and some intercept paths. Firefox's shim has it
// as a no-op stub via patch 37.
namespace base::debug {
bool DumpWithoutCrashing(const base::Location & /*location*/,
		base::TimeDelta /*time_between_dumps*/) {
	return false;
}
} // namespace base::debug

// PolicyDiagnostic — the chromium sandbox_policy_diagnostic.cc builds a JSON
// representation of the policy via base::Value/DictValue/ListValue, which we
// don't want to vendor. We never call BrokerServices::GetPolicyDiagnostics();
// stub so the link succeeds and JsonString() returns empty.
namespace sandbox {

PolicyDiagnostic::PolicyDiagnostic(PolicyBase * /*policy*/) {}

PolicyDiagnostic::~PolicyDiagnostic() = default;

const std::string &PolicyDiagnostic::JsonString() const {
	if (!json_string_) {
		json_string_.emplace();
	}
	return *json_string_;
}

} // namespace sandbox
