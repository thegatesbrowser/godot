/**************************************************************************/
/*  win_acl.cpp                                                           */
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

#include "win_acl.h"

#include "../socket_acl.h"

#ifdef WINDOWS_ENABLED

#include "core/string/print_string.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <string>

namespace {

// Everyone GA + All Application Packages GA + UNTRUSTED label NO_WRITE_UP,
// OICI so directories propagate to children. Used by stamp_untrusted_label.
const wchar_t *kBaselineSddl =
		L"D:(A;OICI;GA;;;WD)(A;OICI;GA;;;AC)S:(ML;OICI;NW;;;S-1-16-0)";

String strip_ipc_prefix(const String &p_path) {
	const String prefix = "ipc://";
	if (p_path.begins_with(prefix)) {
		return p_path.substr(prefix.length());
	}
	return p_path;
}

const wchar_t *as_wide(const Char16String &p_storage) {
	return (const wchar_t *)p_storage.get_data();
}

void stamp_path(const wchar_t *p_path, PACL p_dacl, PACL p_sacl) {
	const DWORD r = ::SetNamedSecurityInfoW(
			const_cast<LPWSTR>(p_path),
			SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION,
			nullptr, nullptr,
			p_dacl, p_sacl);
	if (r != ERROR_SUCCESS) {
		print_line(vformat("WinACL: SetNamedSecurityInfo(%s) failed (win=%d)",
				String::utf16((const char16_t *)p_path), (int)r));
	}
}

void stamp_recursive(const std::wstring &p_dir, PACL p_dacl, PACL p_sacl) {
	stamp_path(p_dir.c_str(), p_dacl, p_sacl);

	std::wstring pattern = p_dir + L"\\*";
	WIN32_FIND_DATAW fd;
	HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
			continue;
		}
		std::wstring child = p_dir + L"\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			stamp_recursive(child, p_dacl, p_sacl);
		} else {
			stamp_path(child.c_str(), p_dacl, p_sacl);
		}
	} while (::FindNextFileW(h, &fd));
	::FindClose(h);
}

// True iff path's mandatory label SACL contains the UNTRUSTED SID (S-1-16-0).
bool has_untrusted_label(const wchar_t *p_path) {
	PSECURITY_DESCRIPTOR psd = nullptr;
	PACL psacl = nullptr;
	const DWORD r = ::GetNamedSecurityInfoW(
			const_cast<LPWSTR>(p_path),
			SE_FILE_OBJECT,
			LABEL_SECURITY_INFORMATION,
			nullptr, nullptr,
			nullptr, &psacl,
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

String sid_to_string(PSID p_sid) {
	PWSTR sid_str = nullptr;
	if (!::ConvertSidToStringSidW(p_sid, &sid_str) || sid_str == nullptr) {
		return String();
	}
	const String out = String::utf16((const char16_t *)sid_str);
	::LocalFree(sid_str);
	return out;
}

} // namespace

void tg_apply_untrusted_acl(const String &p_path) {
	const String path = strip_ipc_prefix(p_path);
	if (path.is_empty()) {
		return;
	}

	PSECURITY_DESCRIPTOR psd = nullptr;
	if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
				kBaselineSddl, SDDL_REVISION_1, &psd, nullptr)) {
		print_line(vformat("tg_apply_untrusted_acl: ConvertStringSecurityDescriptor failed (win=%d)",
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
	const wchar_t *path_w = as_wide(path_utf16);
	const DWORD attrs = ::GetFileAttributesW(path_w);
	if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		if (!has_untrusted_label(path_w)) {
			stamp_recursive(std::wstring(path_w), pdacl, psacl);
		}
	} else {
		stamp_path(path_w, pdacl, psacl);
	}

	::LocalFree(psd);
}

void WinACL::stamp_untrusted_label(const String &p_path) {
	// Cached wrapper: re-stamping the same path is a no-op after the first
	// call in this process. Avoids paying the AV-scan SetSecurityInfo cost
	// on every multi-cycle respawn. The bare uncached form lives at
	// tg_apply_untrusted_acl (free function) for launcher-side IPC bind sites.
	HashSet<String> &cache = stamped_();
	const String key = "stamp|" + p_path;
	if (cache.has(key)) {
		return;
	}
	tg_apply_untrusted_acl(p_path);
	cache.insert(key);
}

bool WinACL::grant_sid_access(PSID p_sid, const String &p_path,
		ACCESS_MASK p_mask, DWORD p_inheritance) {
	if (p_sid == nullptr || p_path.is_empty()) {
		return false;
	}

	const String key = sid_to_string(p_sid) + "|" + p_path +
			"|" + itos((int64_t)p_mask) + "|" + itos((int64_t)p_inheritance);
	HashSet<String> &cache = stamped_();
	if (cache.has(key)) {
		return true;
	}

	const Char16String utf16 = p_path.replace_char('/', '\\').utf16();
	const wchar_t *path_w = as_wide(utf16);

	PACL old_dacl = nullptr;
	PSECURITY_DESCRIPTOR sd = nullptr;
	DWORD r = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(path_w), SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
	if (r != ERROR_SUCCESS) {
		ERR_PRINT(vformat("WinACL: GetNamedSecurityInfo(%s) failed (win=%d)", p_path, (int)r));
		return false;
	}

	EXPLICIT_ACCESSW ea = {};
	ea.grfAccessPermissions = p_mask;
	ea.grfAccessMode = GRANT_ACCESS;
	ea.grfInheritance = p_inheritance;
	ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	ea.Trustee.ptstrName = (LPWSTR)p_sid;

	PACL new_dacl = nullptr;
	r = ::SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
	if (r != ERROR_SUCCESS) {
		::LocalFree(sd);
		ERR_PRINT(vformat("WinACL: SetEntriesInAcl(%s) failed (win=%d)", p_path, (int)r));
		return false;
	}

	r = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path_w), SE_FILE_OBJECT,
			DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);

	::LocalFree(new_dacl);
	::LocalFree(sd);
	if (r != ERROR_SUCCESS) {
		ERR_PRINT(vformat("WinACL: SetNamedSecurityInfo(%s) failed (win=%d)", p_path, (int)r));
		return false;
	}
	cache.insert(key);
	return true;
}

HashSet<String> &WinACL::stamped_() {
	static HashSet<String> s;
	return s;
}

String WinACL::pergate_package_name(const String &p_rw_dir) {
	const uint32_t h = p_rw_dir.replace_char('\\', '/').hash();
	return vformat("TheGates.Renderer.gate-%08x", (int64_t)h);
}

PSID WinACL::alloc_pergate_sid(const String &p_package_name) {
	if (p_package_name.is_empty()) {
		return nullptr;
	}
	const Char16String utf16 = p_package_name.utf16();
	PSID sid = nullptr;
	const HRESULT hr = ::DeriveAppContainerSidFromAppContainerName(
			as_wide(utf16), &sid);
	if (FAILED(hr)) {
		ERR_PRINT(vformat("WinACL: DeriveAppContainerSidFromAppContainerName(%s) failed (hr=0x%x)",
				p_package_name, (int)hr));
		return nullptr;
	}
	return sid;
}

PSID WinACL::alloc_all_packages_sid() {
	PSID sid = nullptr;
	SID_IDENTIFIER_AUTHORITY app_auth = SECURITY_APP_PACKAGE_AUTHORITY;
	if (!::AllocateAndInitializeSid(&app_auth, 2,
				SECURITY_APP_PACKAGE_BASE_RID,
				SECURITY_BUILTIN_PACKAGE_ANY_PACKAGE,
				0, 0, 0, 0, 0, 0, &sid)) {
		return nullptr;
	}
	return sid;
}

#endif // WINDOWS_ENABLED
