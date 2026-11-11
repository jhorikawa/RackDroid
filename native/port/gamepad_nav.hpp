#pragma once

#include <cstdint>

namespace rackdroid {

/** A controller button changed state (PadButton in gamepad_input.hpp).
Called by gamepad_input.cpp on the render thread. */
void gamepadNavButton(int button, bool down);

/** Per-frame work: D-pad repeat, the right stick turning the focused knob,
scrolling the rack after the focus. Call once per rendered frame, before
Window::step(). */
void gamepadNavStep();

/** Adds the focus ring and the status strip to the scene. Call once, after
the patch is launched. */
void installGamepadNav();

/** A finger took over: the ring steps out of the way until the next button. */
void gamepadNavNoteTouch();

/** A module was just added: the focus goes to it, so what follows a pick from
the controller's module menu is on the new module. */
void gamepadNavFocusModule(int64_t moduleId);

/** Play mode: the right stick is two voltages for the patch (gamepad_midi.cpp)
instead of turning the focused knob. R3 toggles it. */
bool gamepadPlayMode();

} // namespace rackdroid
