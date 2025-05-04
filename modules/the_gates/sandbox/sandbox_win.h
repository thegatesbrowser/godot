#ifndef SANDBOXING_WIN
#define SANDBOXING_WIN

#include <sandbox/win/src/sandbox.h>
#include <sandbox/win/src/sandbox_factory.h>
#include <iostream>
#include <shellapi.h>  // For CommandLineToArgvW
#include "BrokerServicesDelegateImpl.h"

#include "core/object/ref_counted.h"

class SandboxingWin : public RefCounted {
	GDCLASS(SandboxingWin, RefCounted);

public:
	int run_parent(int argc, wchar_t* argv[], sandbox::BrokerServices* broker_service);
	int run_child(int argc, wchar_t* argv[]);
	int sandbox_main(int argc, wchar_t* argv[]);

	SandboxingWin();
	~SandboxingWin();
};

#endif // SANDBOXING_WIN
