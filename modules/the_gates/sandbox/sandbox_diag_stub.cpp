/**************************************************************************/
/*  sandbox_diag_stub.cpp                                                 */
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

// the_gates: Stub for sandbox::PolicyDiagnostic.
//
// The chromium sandbox/win/src/sandbox_policy_diagnostic.cc builds a JSON
// representation of the policy via base::Value, base::DictValue, base::
// ListValue, base::WriteJson — none of which we want to vendor (they pull
// in base/json/ and base/values.cc with their own dependencies).
//
// We don't use BrokerServices::GetPolicyDiagnostics() — our sandbox wrapper
// never asks the broker for its policy back. Stub the symbols so the link
// succeeds; JsonString() returns the empty string.

#include "base/location.h"
#include "base/time/time.h"
#include "sandbox/win/src/sandbox_policy_base.h"
#include "sandbox/win/src/sandbox_policy_diagnostic.h"

// base::debug::DumpWithoutCrashing is referenced by chromium 137's win_util.cc
// (via SCOPED_CRASH_KEY_NUMBER) and by some intercept paths. Firefox's shim
// has it as a no-op stub via patch 37; we inline that here.
namespace base::debug {
bool DumpWithoutCrashing(const base::Location & /*location*/,
		base::TimeDelta /*time_between_dumps*/) {
	return false;
}
} // namespace base::debug

namespace sandbox {

// Empty ctor — we don't snapshot anything. The fields stay at their default
// values from the header. PolicyBase's `friend` access is what would make a
// full snapshot work, but we don't need it.
PolicyDiagnostic::PolicyDiagnostic(PolicyBase * /*policy*/) {}

PolicyDiagnostic::~PolicyDiagnostic() = default;

const std::string &PolicyDiagnostic::JsonString() const {
	if (!json_string_) {
		json_string_.emplace();
	}
	return *json_string_;
}

} // namespace sandbox
