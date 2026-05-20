/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Shim of chromium's base/allocator/partition_alloc_features.h. Including
// chromium's real header pulls in the entire partition_allocator tree, which
// the sandbox build doesn't need; the references from sandbox/linux live
// inside a `#if BUILDFLAG(ENABLE_MUTEX_PRIORITY_INHERITANCE)` block that
// resolves to 0 in our config, so an empty header is sufficient.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOC_FEATURES_H_
#define BASE_ALLOCATOR_PARTITION_ALLOC_FEATURES_H_

#endif  // BASE_ALLOCATOR_PARTITION_ALLOC_FEATURES_H_
