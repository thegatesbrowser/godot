/**************************************************************************/
/*  UtilityProcessSandboxing.h                                            */
/*  Minimal shim for vendored Firefox sandbox code; only the              */
/*  SandboxingKind enum is referenced by the stripped Sandbox.mm.         */
/**************************************************************************/

#pragma once

#include <cstdint>

namespace mozilla {
namespace ipc {

enum class SandboxingKind : uint64_t {
	GENERIC_UTILITY = 0,
};

} // namespace ipc
} // namespace mozilla
