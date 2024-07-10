#ifndef SANDBOXING
#define SANDBOXING

#include "core/object/ref_counted.h"

class Sandboxing : public RefCounted {
	GDCLASS(Sandboxing, RefCounted);

public:
	static Error sandbox();

	Sandboxing();
	~Sandboxing();
};

#endif // SANDBOXING
