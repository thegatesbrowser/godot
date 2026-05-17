/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Minimal stand-in for chromium's base/debug/dump_without_crashing.cc. The
// real version surfaces a non-fatal report to the crash reporter; we have no
// reporter consuming the dump, so the calls become no-ops and the symbols
// resolve at link time.

#include "base/debug/dump_without_crashing.h"

namespace base::debug {

bool DumpWithoutCrashing(const base::Location& /*location*/,
                         base::TimeDelta /*time_between_dumps*/) {
  return false;
}

void SetDumpWithoutCrashingFunction(void (* /*function*/)()) {}

void ResetDumpWithoutCrashingThrottlingForTesting() {}

}  // namespace base::debug
