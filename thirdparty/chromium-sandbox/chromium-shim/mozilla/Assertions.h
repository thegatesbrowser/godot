// the_gates: Minimal mozilla::Assertions shim for the Firefox-style sandbox
// build. Firefox's chromium-shim references mozilla/Assertions.h for MOZ_CRASH
// and MOZ_ASSERT. We map them to C runtime primitives — the chromium sandbox
// only hits these on genuinely fatal paths, where aborting is what we want.

#ifndef THE_GATES_MOZILLA_ASSERTIONS_H_
#define THE_GATES_MOZILLA_ASSERTIONS_H_

#include <cassert>
#include <cstdlib>

#if defined(_WIN32)
#include <intrin.h>
#define MOZ_CRASH_BREAK() __debugbreak()
#else
#define MOZ_CRASH_BREAK() __builtin_trap()
#endif

#define MOZ_CRASH(...) do { MOZ_CRASH_BREAK(); std::abort(); } while (0)
#define MOZ_CRASH_UNSAFE(msg) MOZ_CRASH(msg)
#define MOZ_CRASH_UNSAFE_PRINTF(...) MOZ_CRASH()

#define MOZ_ASSERT(cond, ...) assert(cond)
#define MOZ_RELEASE_ASSERT(cond, ...) do { if (!(cond)) MOZ_CRASH(); } while (0)
#define MOZ_DIAGNOSTIC_ASSERT(cond, ...) MOZ_ASSERT(cond)
#define MOZ_ASSERT_UNREACHABLE(...) MOZ_CRASH()

#endif  // THE_GATES_MOZILLA_ASSERTIONS_H_
