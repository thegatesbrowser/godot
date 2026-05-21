/**************************************************************************/
/*  brokered_ip.cpp                                                       */
/**************************************************************************/

#include "brokered_ip.h"

#include "renderer_net_client.h"

extern "C" bool tg_renderer_resolve_hostname(const String &p_hostname, IP::Type p_type,
		List<IPAddress> &r_addresses) {
	if (!RendererNetClient::is_installed()) {
		return false;
	}
	return RendererNetClient::resolve_hostname(p_hostname, p_type, r_addresses);
}
