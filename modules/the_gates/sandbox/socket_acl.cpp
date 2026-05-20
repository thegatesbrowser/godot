/**************************************************************************/
/*  socket_acl.cpp                                                        */
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

#include "socket_acl.h"

#ifdef WINDOWS_ENABLED

#include "core/string/print_string.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <string>

// DACL: Everyone GENERIC_ALL. SACL: UNTRUSTED (S-1-16-0) mandatory label
// with NO_WRITE_UP. OICI = OBJECT_INHERIT + CONTAINER_INHERIT — applied to
// a directory the ACEs propagate to new files and subdirs; on a plain file
// the inheritance flags are ignored.
static const wchar_t *kSocketSddl =
		L"D:(A;OICI;GA;;;WD)S:(ML;OICI;NW;;;S-1-16-0)";

namespace {

void stamp_path(const wchar_t *path, PACL pdacl, PACL psacl) {
	const DWORD r = ::SetNamedSecurityInfoW(
			const_cast<LPWSTR>(path),
			SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
			/*owner=*/nullptr, /*group=*/nullptr,
			pdacl, psacl);
	if (r != ERROR_SUCCESS) {
		print_line(vformat("tg_apply_untrusted_acl: "
						   "SetNamedSecurityInfo(%s) failed (win=%d)",
				String::utf16((const char16_t *)path), (int)r));
	}
}

void stamp_recursive(const std::wstring &dir, PACL pdacl, PACL psacl) {
	stamp_path(dir.c_str(), pdacl, psacl);

	std::wstring pattern = dir + L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
			continue;
		}
		std::wstring child = dir + L"\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			stamp_recursive(child, pdacl, psacl);
		} else {
			stamp_path(child.c_str(), pdacl, psacl);
		}
	} while (::FindNextFileW(h, &fd));
	::FindClose(h);
}

// True iff path's mandatory label SACL contains the UNTRUSTED SID (S-1-16-0).
bool has_untrusted_label(const wchar_t *path) {
	PSECURITY_DESCRIPTOR psd = nullptr;
	PACL psacl = nullptr;
	const DWORD r = ::GetNamedSecurityInfoW(
			const_cast<LPWSTR>(path),
			SE_FILE_OBJECT,
			LABEL_SECURITY_INFORMATION,
			/*owner=*/nullptr, /*group=*/nullptr,
			/*dacl=*/nullptr, &psacl,
			&psd);
	if (r != ERROR_SUCCESS || psacl == nullptr) {
		if (psd != nullptr) {
			::LocalFree(psd);
		}
		return false;
	}

	bool found = false;
	for (WORD i = 0; i < psacl->AceCount; ++i) {
		LPVOID ace = nullptr;
		if (!::GetAce(psacl, i, &ace) || ace == nullptr) {
			continue;
		}
		const ACE_HEADER *header = static_cast<const ACE_HEADER *>(ace);
		if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
			continue;
		}
		const SYSTEM_MANDATORY_LABEL_ACE *label_ace =
				static_cast<const SYSTEM_MANDATORY_LABEL_ACE *>(ace);
		const PSID sid = (PSID)&label_ace->SidStart;
		PWSTR sid_str = nullptr;
		if (::ConvertSidToStringSidW(sid, &sid_str)) {
			if (wcscmp(sid_str, L"S-1-16-0") == 0) {
				found = true;
			}
			::LocalFree(sid_str);
		}
		if (found) {
			break;
		}
	}

	::LocalFree(psd);
	return found;
}

} // namespace

void tg_apply_untrusted_acl(const String &p_path) {
	String path = p_path;
	const String prefix = "ipc://";
	if (path.begins_with(prefix)) {
		path = path.substr(prefix.length());
	}
	if (path.is_empty()) {
		return;
	}

	PSECURITY_DESCRIPTOR psd = nullptr;
	if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
				kSocketSddl, SDDL_REVISION_1, &psd, nullptr)) {
		print_line(vformat("tg_apply_untrusted_acl: "
						   "ConvertStringSecurityDescriptor failed (win=%d)",
				(int)::GetLastError()));
		return;
	}

	BOOL dacl_present = FALSE, dacl_defaulted = FALSE;
	BOOL sacl_present = FALSE, sacl_defaulted = FALSE;
	PACL pdacl = nullptr;
	PACL psacl = nullptr;
	::GetSecurityDescriptorDacl(psd, &dacl_present, &pdacl, &dacl_defaulted);
	::GetSecurityDescriptorSacl(psd, &sacl_present, &psacl, &sacl_defaulted);

	const Char16String path_utf16 = path.utf16();
	const wchar_t *path_w = (const wchar_t *)path_utf16.get_data();
	const DWORD attrs = ::GetFileAttributesW(path_w);
	if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		// SetNamedSecurityInfo on an OICI'd dir propagates ACEs to every
		// child; skip on dirs already labeled to avoid the full child walk.
		if (!has_untrusted_label(path_w)) {
			stamp_recursive(std::wstring(path_w), pdacl, psacl);
		}
	} else {
		stamp_path(path_w, pdacl, psacl);
	}

	::LocalFree(psd);
}

#else // !WINDOWS_ENABLED

void tg_apply_untrusted_acl(const String & /*p_path*/) {
	// No-op: MIC is a Windows specific. On Linux / macOS the AF_UNIX socket
	// file permissions inherit from the process umask and the parent dir,
	// which is enough — the launcher's user_data_dir is already in the
	// user's home tree.
}

#endif // WINDOWS_ENABLED
