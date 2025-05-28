#ifndef SANDBOXING_WIN
#define SANDBOXING_WIN

#include "core/object/ref_counted.h"

namespace sandbox {
	class BrokerServices;
	class TargetConfig;
}

class SandboxingWin : public RefCounted {
	GDCLASS(SandboxingWin, RefCounted);

	sandbox::BrokerServices* broker_service;

	Error add_app_container_profile_to_config(sandbox::TargetConfig* config);
	String get_app_container_profile_name(const String& appcontainer_id);

protected:
	static void _bind_methods();

public:
	Error spawn_target(const Vector<String> &p_arguments);
	Error lower_token();

	bool is_target() { return broker_service == nullptr; }

	SandboxingWin();
	~SandboxingWin();
};

#endif // SANDBOXING_WIN
