/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Shim for chromium's base/metrics/field_trial_params.h. Chromium 137's
// version uses raw_ptr<> internally without including base/memory/raw_ptr.h,
// which fails to compile in our minimal base/ subset. The sandbox build never
// reads a FeatureParam value at runtime; the few headers (sys_info.h, etc.)
// that reference the type only need a forward declaration.

#ifndef BASE_METRICS_FIELD_TRIAL_PARAMS_H_
#define BASE_METRICS_FIELD_TRIAL_PARAMS_H_

namespace base {

template <typename T>
class FeatureParam;

}  // namespace base

#endif  // BASE_METRICS_FIELD_TRIAL_PARAMS_H_
