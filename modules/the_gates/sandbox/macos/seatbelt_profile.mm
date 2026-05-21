/**************************************************************************/
/*  seatbelt_profile.mm                                                   */
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

#include "seatbelt_profile.h"

#ifdef MACOS_ENABLED

namespace {

const char kSeatbeltBase[] = R"SANDBOX_LITERAL(
  ; Per-gate user-data dir
  (if (string=? hasProfileDir "TRUE")
    (allow file-read* file-write*
      (subpath profileDir)))

  ; MoltenVK's first vkCreatePipelineCache talks to com.apple.MTLCompilerService
  ; over XPC and blocks indefinitely if it can't reach its on-disk cache at
  ; $DARWIN_USER_CACHE_DIR/com.apple.metal/<gpu-id>/.
  (when userCacheDir
    (allow file-read* file-write*
      (subpath (string-append userCacheDir "/com.apple.metal"))))

  ; GPU / IOKit
  (allow iokit-get-properties)
  (allow iokit-open
    (iokit-user-client-class "AGXDeviceUserClient")
    (iokit-user-client-class "AGXSharedUserClient"))

  ; HID, WM, system queries
  (allow mach-lookup
    (global-name "com.apple.iohideventsystem")
    (global-name "com.apple.logd.events")
    (global-name "com.apple.windowmanager.server")
    (global-name "com.apple.dock.fullscreen")
    (global-name "com.apple.GameController.gamecontrollerd.app")
    (global-name "com.apple.PowerManagement.control")
    (global-name "com.apple.SecurityServer")) ; mbedtls SecTrust* during HTTPS
  (allow mach-register (global-name "com.apple.coredrag"))
  (allow sysctl-read (sysctl-name "kern.willshutdown"))
  (allow user-preference-read (preference-domain "com.apple.coregraphics"))

  ; external_texture bind path; renderer binds, launcher connects.
  (allow file-read* file-write*
    (literal "/private/tmp/external_texture"))

  ; The network-broker control channel is an inherited socketpair end; no
  ; filesystem path involved, so nothing to allow here.

  ; Network policy: allow network-outbound + network-inbound + network-bind
  ; (so operations on broker-handed FDs work — sendto/recvfrom for UDP,
  ; send/recv for TCP). Do NOT call (system-network) — that primitive is
  ; what Apple's App Sandbox base profile uses to enable socket() creation
  ; in the first place. Without (system-network), raw socket(AF_INET) in
  ; the renderer is denied by the base profile's (moz-deny default).
  ; Inherited FDs from SCM_RIGHTS survive because the syscall hook for
  ; "create a new socket" doesn't fire on already-open FDs.
  ;
  ; Reference: /System/Library/Sandbox/Profiles/appsandbox-common.sb
  ;   (define (network-client) (system-network) (allow network-outbound (remote ip)) ...)
  ; Both pieces are needed to use the network normally; we only enable
  ; the second.
  (allow network-outbound (remote ip))
  (allow network-inbound (local ip))
  (allow network-bind (local ip))
  ; Deny EVERY xnu syscall that creates a new network file descriptor.
  ; Inherited FDs from SCM_RIGHTS (broker-passed) and pre-lockdown zmq
  ; sockets survive — these hooks fire only at the syscall entry point,
  ; not on operations (sendto/recvfrom/send/recv/read/write) against
  ; existing FDs.
  ;
  ; Each deny catches every invocation path: libsystem wrappers, direct
  ; libc syscall() calls, and inline-assembly traps (the kernel hook is
  ; at the syscall entry, not at the libc level).
  ;
  ; xnu bsd/kern/syscalls.master numbers (verified against current main):
  (deny syscall-unix (syscall-number 97))   ; SYS_socket
  (deny syscall-unix (syscall-number 135))  ; SYS_socketpair
  (deny syscall-unix (syscall-number 450))  ; SYS_socket_delegate (private SPI)
  (deny syscall-unix (syscall-number 502))  ; SYS_necp_client_action
  (deny syscall-unix (syscall-number 522))  ; SYS_necp_session_open
  (deny syscall-unix (syscall-number 510))  ; SYS___channel_open (Network.framework)
  ;
  ; Also deny syscalls that retarget an existing socket FD. UDP allows
  ; ::connect() multiple times to switch peers; without this deny, a
  ; renderer GDExtension could call libc connect(broker_fd, private_addr)
  ; to bypass the broker's per-open CIDR enforcement. Linux's seccomp
  ; filter already denies __NR_connect / __NR_bind post-lockdown — these
  ; bring macOS to parity.
  (deny syscall-unix (syscall-number 98))   ; SYS_connect
  (deny syscall-unix (syscall-number 104))  ; SYS_bind
  (deny syscall-unix (syscall-number 106))  ; SYS_listen
  (deny syscall-unix (syscall-number 30))   ; SYS_accept
  (deny syscall-unix (syscall-number 409))  ; SYS_accept_nocancel
  (deny syscall-unix (syscall-number 446))  ; SYS_connect_nocancel
)SANDBOX_LITERAL";

const char kSeatbeltAudioOutput[] = R"SANDBOX_LITERAL(
  (allow ipc-posix-shm-read* ipc-posix-shm-write-data
    (ipc-posix-name-regex #"^AudioIO"))
  (allow mach-lookup
    (global-name "com.apple.audio.coreaudiod")
    (global-name "com.apple.audio.audiohald")
    (global-name "com.apple.audio.SandboxHelper")
    (global-name "com.apple.audio.AUHostingService")
    (global-name "com.apple.audio.AudioSession")
    (global-name "com.apple.audioanalyticsd"))
  (allow iokit-open
    (iokit-user-client-class "IOAudioEngineUserClient")
    (iokit-user-client-class "IOAudioControlUserClient"))
  (allow file-read* (subpath "/Library/Audio/Plug-Ins"))
)SANDBOX_LITERAL";

const char kSeatbeltMicrophone[] = R"SANDBOX_LITERAL(
  (allow device-microphone)
)SANDBOX_LITERAL";

} // namespace

String tg_build_seatbelt_addend(bool p_allow_audio, bool p_allow_microphone) {
	String out = kSeatbeltBase;
	if (p_allow_audio) {
		out += kSeatbeltAudioOutput;
	}
	if (p_allow_microphone) {
		out += kSeatbeltMicrophone;
	}
	return out;
}

#endif // MACOS_ENABLED
