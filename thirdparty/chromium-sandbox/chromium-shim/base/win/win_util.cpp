/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// the_gates: Minimal win_util implementation. Provides only the surface
// sandbox/win/src/* TUs (notably scoped_handle.cc) actually call —
// GetObjectTypeName, IsAppVerifierLoaded, GetWindowObjectName. NOT a full
// port of chromium's win_util.cc (NativeLibrary/RegKey-method-heavy).

#include "base/win/win_util.h"

#include <windows.h>
#include <winternl.h>
#include <ntstatus.h>

#include "base/logging.h"
#include "base/strings/string_util.h"

namespace base {
namespace win {

const char kApplicationVerifierDllName[] = "verifier.dll";

std::wstring GetWindowObjectName(HANDLE handle) {
  std::wstring object_name;
  DWORD size = 0;
  ::GetUserObjectInformationW(handle, UOI_NAME, nullptr, 0, &size);
  if (!size) {
    return object_name;
  }
  if (!::GetUserObjectInformationW(
          handle, UOI_NAME, WriteInto(&object_name, size / sizeof(wchar_t)),
          size, &size)) {
    return std::wstring();
  }
  return object_name;
}

bool IsAppVerifierLoaded() {
  return ::GetModuleHandleA(kApplicationVerifierDllName) != nullptr;
}

namespace {

// PUBLIC_OBJECT_TYPE_INFORMATION isn't in <winternl.h>; declare the layout we
// need. ObjectTypeInformation = 2 per ntddk.h.
struct PUBLIC_OBJECT_TYPE_INFORMATION_t {
  UNICODE_STRING TypeName;
  ULONG Reserved[22];
};

bool IsHandleValid(HANDLE handle) {
  return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

}  // namespace

expected<std::wstring, NTSTATUS> GetObjectTypeName(HANDLE handle) {
  if (!IsHandleValid(handle)) {
    return unexpected<NTSTATUS>(STATUS_INVALID_HANDLE);
  }
  constexpr size_t kMaxTypeNameLength = 31;
  constexpr size_t kBufferSize =
      sizeof(PUBLIC_OBJECT_TYPE_INFORMATION_t) +
      (kMaxTypeNameLength + 1) * sizeof(wchar_t);
  alignas(PUBLIC_OBJECT_TYPE_INFORMATION_t) unsigned char buffer[kBufferSize] = {};
  auto* info = reinterpret_cast<PUBLIC_OBJECT_TYPE_INFORMATION_t*>(buffer);
  ULONG returned = 0;
  // ObjectTypeInformation == 2
  NTSTATUS status = ::NtQueryObject(handle,
                                    static_cast<OBJECT_INFORMATION_CLASS>(2),
                                    info,
                                    static_cast<ULONG>(sizeof(buffer)),
                                    &returned);
  if (status != STATUS_SUCCESS) {
    return unexpected<NTSTATUS>(status);
  }
  return std::wstring(info->TypeName.Buffer,
                      info->TypeName.Length / sizeof(wchar_t));
}

}  // namespace win
}  // namespace base
