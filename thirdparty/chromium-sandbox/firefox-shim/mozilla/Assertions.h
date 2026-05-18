/**************************************************************************/
/*  Assertions.h                                                          */
/*  Minimal mozilla:: assertion shim for vendored Firefox sandbox code.   */
/**************************************************************************/

#pragma once

#include "core/error/error_macros.h"

#define MOZ_ASSERT(...) DEV_ASSERT(__VA_ARGS__)
#define MOZ_RELEASE_ASSERT(cond, ...) CRASH_COND_MSG(!(cond), "MOZ_RELEASE_ASSERT failed")
#define MOZ_CRASH(...) CRASH_NOW_MSG("MOZ_CRASH")
