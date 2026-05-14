#ifndef SANDBOXING_WIN
#define SANDBOXING_WIN

#include "core/object/ref_counted.h"

namespace sandbox {
	class BrokerServices;
}

class SandboxingWin : public RefCounted {
	GDCLASS(SandboxingWin, RefCounted);

	sandbox::BrokerServices* broker_service;

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
