// the_gates: Map mozilla::UniquePtr to std::unique_ptr. Firefox's
// chromium-shim/base/scoped_native_library.h uses UniquePtr; this gives it the
// interface it expects.

#ifndef THE_GATES_MOZILLA_UNIQUEPTR_H_
#define THE_GATES_MOZILLA_UNIQUEPTR_H_

#include <memory>

namespace mozilla {
template <typename T, typename Deleter = std::default_delete<T>>
using UniquePtr = std::unique_ptr<T, Deleter>;

template <typename T, typename... Args>
inline UniquePtr<T> MakeUnique(Args&&... args) {
  return std::make_unique<T>(std::forward<Args>(args)...);
}
}  // namespace mozilla

#endif  // THE_GATES_MOZILLA_UNIQUEPTR_H_
