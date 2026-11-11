#pragma once

struct AInputEvent;

namespace rackdroid {

/** Buttons of the built-in controller, in the order GLFW's standard gamepad
mapping uses, so the same indices can later feed Rack's "Gamepad" MIDI driver
and line up with a controller plugged into desktop Rack. */
enum PadButton {
	PAD_A = 0,
	PAD_B,
	PAD_X,
	PAD_Y,
	PAD_L1,
	PAD_R1,
	PAD_SELECT,
	PAD_START,
	PAD_GUIDE,
	PAD_L3,
	PAD_R3,
	PAD_UP,
	PAD_RIGHT,
	PAD_DOWN,
	PAD_LEFT,
	PAD_BUTTONS
};

/** Axes, again in GLFW's order. Sticks are -1..1 (Y down positive, as Android
reports them); triggers are 0..1. */
enum PadAxis {
	PAD_LX = 0,
	PAD_LY,
	PAD_RX,
	PAD_RY,
	PAD_L2,
	PAD_R2,
	PAD_AXES
};

/** Decodes a key or joystick event from a game controller. Returns 1 if the
event was a controller's and has been consumed, 0 for everything else (touch,
mouse, the BACK key), which the caller passes on as before. Render thread. */
int gamepadHandleEvent(AInputEvent* event);

bool gamepadButton(int button);
float gamepadAxis(int axis);

/** True once a controller has sent anything in this session. */
bool gamepadSeen();

} // namespace rackdroid
