/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// the_gates: Extends the original Firefox stub with the minimal directory
// helpers chromium's app_container_base.cc and a couple of other sandbox/win
// TUs call. Chromium 137 inlines these calls; Firefox's older snapshot didn't
// need them yet. We declare them here and implement in a sibling .cpp.

#ifndef BASE_FILES_FILE_UTIL_H_
#define BASE_FILES_FILE_UTIL_H_

#include "base/base_export.h"
#include "base/files/file_path.h"

namespace base {

// Returns true if the given path exists and is a directory.
BASE_EXPORT bool DirectoryExists(const base::FilePath& path);

// Creates the directory at |path|. Returns true on success or if it already
// exists. Wraps Win32 CreateDirectoryW.
BASE_EXPORT bool CreateDirectory(const base::FilePath& path);

// Deletes |path| and everything underneath it. Returns true on success.
BASE_EXPORT bool DeletePathRecursively(const base::FilePath& path);

}  // namespace base

#endif  // BASE_FILES_FILE_UTIL_H_
