/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Shim of chromium's base/features.h. The only reference from sandbox/linux
// lives inside a `#if BUILDFLAG(ENABLE_MUTEX_PRIORITY_INHERITANCE)` block
// that resolves to 0 in our config; an empty header satisfies the
// unconditional include without dragging chromium's feature machinery in.

#ifndef BASE_FEATURES_H_
#define BASE_FEATURES_H_

#endif  // BASE_FEATURES_H_
