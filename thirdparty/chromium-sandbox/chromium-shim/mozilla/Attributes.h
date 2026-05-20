// the_gates: Minimal mozilla::Attributes shim. Used for MOZ_NEVER_INLINE and
// a couple of attribute macros referenced by Firefox's chromium-shim
// sandboxLogging.cpp etc. Mapped to MSVC/clang native equivalents.

#ifndef THE_GATES_MOZILLA_ATTRIBUTES_H_
#define THE_GATES_MOZILLA_ATTRIBUTES_H_

#if defined(_MSC_VER)
#define MOZ_NEVER_INLINE __declspec(noinline)
#define MOZ_ALWAYS_INLINE __forceinline
#else
#define MOZ_NEVER_INLINE __attribute__((noinline))
#define MOZ_ALWAYS_INLINE __attribute__((always_inline)) inline
#endif

#define MOZ_FORMAT_PRINTF(a, b)
#define MOZ_FALLTHROUGH [[fallthrough]]
#define MOZ_TRIVIAL_CTOR_DTOR
#define MOZ_RAII
#define MOZ_STACK_CLASS
#define MOZ_NONNULL(...)
#define MOZ_NONHEAP_CLASS
#define MOZ_STATIC_CLASS

#endif  // THE_GATES_MOZILLA_ATTRIBUTES_H_
