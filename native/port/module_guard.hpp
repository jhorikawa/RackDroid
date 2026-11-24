#pragma once
#include <cstdint>
#include <string>

/** Running code that may be wrong: a module made on this device from the chat
(made_modules.cpp, nova/kit).

A bad memory access, a failed check, an exception or a loop that never ends,
inside such a call, ends the call and nothing else. What was faulty is told on
the render thread, and the module it belongs to is not called again.

It is for mistakes, not for malice, and it cannot put right what the code had
already overwritten: the modules are compiled with bounds checks so that they
stop before that (nova/bridge/nova_build.py). */

extern "C" {

/** Any thread. Calls fn(arg); 0 if it came back, 1 if it was cut short. `owner`
is the engine::Module it belongs to, `where` one of GuardWhere. Nests.
Until guardInstall() it is a plain call. */
__attribute__((visibility("default")))
int nova_guard_call(void (*fn)(void*), void* arg, const void* owner, int where);

/** Tells that a module has gone wrong without being cut short. */
__attribute__((visibility("default")))
void nova_guard_report(const void* owner, int where, const char* text);

}

namespace rackdroid {

/** The names are in module_guard.cpp and, for the module's side, in nova_kit.hpp. */
enum GuardWhere {
	GUARD_MAKE,
	GUARD_PROCESS,
	GUARD_EVENT,
	GUARD_STATE,
	GUARD_DRAW,
	GUARD_WIDGET,
	GUARD_REMOVE,
};

struct GuardFault {
	/** The module, as it was given: it may be gone by now. */
	const void* owner = nullptr;
	int where = 0;
	/** In words: "memory access", "a check failed", "took too long", ... */
	std::string what;
	/** Where in which library of guardAddCode(), or an empty tag. */
	std::string tag;
	uintptr_t offset = 0;
};

/** Once, before the first such call. `marker` is a file that is written if the
app dies outside a guarded call while code of guardAddCode() is on the stack;
one line for each tag found there, the first with the place: "Folder 3 0x1e6a0". */
void guardInstall(const std::string& marker);

/** The library that holds `address`, under a name of one's own. */
void guardAddCode(const void* address, const std::string& tag);

/** Render thread: the next fault that has not been told yet. */
bool guardNextFault(GuardFault& fault);

const char* guardWhereName(int where);

} // namespace rackdroid
