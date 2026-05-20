/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// the_gates: Restored shim header, extended to chromium 137's surface.
// See win_util.cpp for the rationale.

#ifndef BASE_WIN_WIN_UTIL_H_
#define BASE_WIN_WIN_UTIL_H_

#include <windows.h>
#include <winternl.h>

#include <string>

#include "base/base_export.h"
#include "base/types/expected.h"

namespace base {
namespace win {

BASE_EXPORT std::wstring GetWindowObjectName(HANDLE handle);
BASE_EXPORT bool IsAppVerifierLoaded();
BASE_EXPORT expected<std::wstring, NTSTATUS> GetObjectTypeName(HANDLE handle);

}  // namespace win
}  // namespace base

#endif  // BASE_WIN_WIN_UTIL_H_
