/**************************************************************************/
/*  brokered_ip.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/*                                                                        */
/*  Renderer-side DNS shim. The renderer's sandbox forbids socket() so    */
/*  the engine's default getaddrinfo path cannot reach the resolver. The  */
/*  hook below routes hostname resolution through the launcher's broker  */
/*  using the same control channel as socket creation.                    */
/*                                                                        */
/*  Wired in via a #ifdef TG_RENDERER hook in core/io/ip.cpp; no separate */
/*  install step needed — RendererNetClient::install() makes both paths   */
/*  active at once.                                                       */
/**************************************************************************/

#pragma once

#include "core/io/ip.h"
#include "core/io/ip_address.h"
#include "core/string/ustring.h"
#include "core/templates/list.h"

// Called from core/io/ip.cpp under #ifdef TG_RENDERER. Returns true when
// the broker resolved the hostname and filled r_addresses; false on broker
// error so the engine's default path runs (which will then fail closed
// because the sandbox denies socket()).
extern "C" bool tg_renderer_resolve_hostname(const String &p_hostname, IP::Type p_type,
		List<IPAddress> &r_addresses);
