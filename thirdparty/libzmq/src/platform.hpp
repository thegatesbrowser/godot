#ifndef __PLATFORM_HPP_INCLUDED__
#define __PLATFORM_HPP_INCLUDED__

// Upstream libzmq generates platform.hpp via autoconf or cmake. We vendor
// the source tree without that build step, so the project's SConscript
// (modules/the_gates/SCsub) is the single source of truth for the
// ZMQ_HAVE_* / HAVE_* feature macros — see its env_zmq branches.
//
// This file is left empty so it can be #included by every TU without
// hardcoding a platform. The original upstream stub force-defined
// ZMQ_HAVE_WINDOWS unconditionally, which broke Linux/macOS builds.

#endif
