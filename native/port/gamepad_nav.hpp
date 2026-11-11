#pragma once

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

/** Play mode: the left stick and the triggers are voltages for the patch
(gamepad_midi.cpp) instead of panning and zooming the rack. L3 toggles it. */
bool gamepadPlayMode();

} // namespace rackdroid
