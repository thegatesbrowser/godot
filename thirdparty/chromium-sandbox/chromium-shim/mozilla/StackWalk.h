// the_gates: Minimal mozilla::StackWalk shim. The Firefox sandboxLogging.cpp
// uses CallerPC() to capture the PC of the function calling LogBlocked, for
// stack-trace logging.
//
// We provide a no-op equivalent — under our build the sandbox logging callback
// isn't installed, so the PC value is never inspected.

#ifndef THE_GATES_MOZILLA_STACKWALK_H_
#define THE_GATES_MOZILLA_STACKWALK_H_

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace mozilla {

// Return address of the calling function. Approximates the Mozilla helper.
static inline void* CallerPC() {
#if defined(_MSC_VER)
  return _ReturnAddress();
#else
  return __builtin_return_address(0);
#endif
}

}  // namespace mozilla

// Firefox's chromium-shim files call CallerPC() unqualified — provide an
// alias in the global namespace.
using mozilla::CallerPC;

#endif  // THE_GATES_MOZILLA_STACKWALK_H_
