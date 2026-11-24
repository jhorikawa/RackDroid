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

/** Another window took the keys: nothing is held any more, and nothing that
acts on release (a shoulder button's tap) fires for a release that never came. */
void gamepadNavCancelHeld();

/** A module was just added: the focus goes to it, so what follows a pick from
the controller's module menu is on the new module. */
void gamepadNavFocusModule(int64_t moduleId);

/** Scrolls until the module can be seen beside the chat's panel, if that is up.
The focus stays where it is. A touch or a button stops it; while the user's own
hands are on the rack it is not begun. */
void gamepadNavShow(int64_t moduleId);

/** Any thread: this many pixels on the right are covered by the chat's panel, and
at the bottom by its keyboard. */
void gamepadNavPanelInset(int rightPx, int bottomPx);

/** Render thread: how much of the scene on the right that is, in its units. */
float gamepadNavPanelWidth();

/** What the cables should be told apart for (signal_flow.cpp): the module
selected, or the module and jack focused. False when there is neither. */
struct CableFocus {
	int64_t moduleId = -1;
	/** -1 the whole module, else engine::Port::Type (INPUT 0, OUTPUT 1). */
	int portType = -1;
	int portId = -1;
};
bool gamepadNavCableFocus(CableFocus& out);

/** Play mode: the right stick is two voltages for the patch (gamepad_midi.cpp)
instead of turning the focused knob. R3 toggles it. */
bool gamepadPlayMode();

} // namespace rackdroid
