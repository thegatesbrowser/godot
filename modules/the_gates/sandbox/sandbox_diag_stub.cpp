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

#include "sandbox/win/src/sandbox_policy_diagnostic.h"
#include "sandbox/win/src/sandbox_policy_base.h"
#include "base/location.h"
#include "base/time/time.h"

// base::debug::DumpWithoutCrashing is referenced by chromium 137's win_util.cc
// (via SCOPED_CRASH_KEY_NUMBER) and by some intercept paths. Firefox's shim
// has it as a no-op stub via patch 37; we inline that here.
namespace base::debug {
bool DumpWithoutCrashing(const base::Location& /*location*/,
                         base::TimeDelta /*time_between_dumps*/) {
  return false;
}
}  // namespace base::debug

namespace sandbox {

// Empty ctor — we don't snapshot anything. The fields stay at their default
// values from the header. PolicyBase's `friend` access is what would make a
// full snapshot work, but we don't need it.
PolicyDiagnostic::PolicyDiagnostic(PolicyBase* /*policy*/) {}

PolicyDiagnostic::~PolicyDiagnostic() = default;

const std::string& PolicyDiagnostic::JsonString() const {
  if (!json_string_) {
    json_string_.emplace();
  }
  return *json_string_;
}

}  // namespace sandbox
