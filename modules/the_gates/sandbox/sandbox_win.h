#ifndef SANDBOXING_WIN
#define SANDBOXING_WIN

#include "core/object/ref_counted.h"

class SandboxingWin : public RefCounted {
	GDCLASS(SandboxingWin, RefCounted);

public:
	int run_parent(List<String> args);
	int run_child();
	int run(List<String> args);

	SandboxingWin();
	~SandboxingWin();
};

#endif // SANDBOXING_WIN
