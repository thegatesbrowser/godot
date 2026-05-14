/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// the_gates: Minimal implementations for the file_util.h declarations our
// shim adds. Wraps Win32 directly — same surface chromium 137's
// app_container_base.cc was written against.

#include "base/files/file_util.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace base {

bool DirectoryExists(const base::FilePath& path) {
  DWORD attrs = ::GetFileAttributesW(path.value().c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool CreateDirectory(const base::FilePath& path) {
  // Create one directory at a time, walking up parents if needed. Matches
  // base::CreateDirectory's recursive semantic.
  if (DirectoryExists(path)) {
    return true;
  }
  base::FilePath parent = path.DirName();
  if (!parent.empty() && parent != path && !DirectoryExists(parent)) {
    if (!CreateDirectory(parent)) {
      return false;
    }
  }
  if (::CreateDirectoryW(path.value().c_str(), nullptr)) {
    return true;
  }
  return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool DeletePathRecursively(const base::FilePath& path) {
  // SHFileOperation expects a double-null-terminated string.
  std::wstring buf = path.value();
  buf.push_back(L'\0');
  buf.push_back(L'\0');

  SHFILEOPSTRUCTW op = {};
  op.wFunc = FO_DELETE;
  op.pFrom = buf.c_str();
  op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  return ::SHFileOperationW(&op) == 0;
}

}  // namespace base
