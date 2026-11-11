/* Game controller input.
 *
 * Upstream RackDroid is touch-only: key events are never looked at, and a
 * joystick's motion events fall into the touch code, which treats them as a
 * finger. This file takes both before the touch code sees them, keeps the
 * controller's state, and hands button edges to the focus navigation in
 * gamepad_nav.cpp.
 *
 * Measured on a Retroid Pocket Nova ("Retroid Pocket Controller", sources
 * KEYBOARD | GAMEPAD | JOYSTICK): the sticks are X/Y and Z/RZ, the analog
 * triggers BRAKE and GAS, and the D-pad arrives as the HAT_X/HAT_Y axes, not
 * as DPAD keys. Other pads send the D-pad as keys and the triggers as
 * LTRIGGER/RTRIGGER, so both forms are read.
 */
#include <cmath>

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>

#include "gamepad_input.hpp"
#include "gamepad_nav.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.pad", __VA_ARGS__)

namespace rackdroid {


static bool g_button[PAD_BUTTONS] = {};
static float g_axis[PAD_AXES] = {};
static bool g_seen = false;
// The D-pad as last read from the HAT axes, kept apart from DPAD keys so one
// source releasing does not cancel the other.
static bool g_hat[4] = {};

static const float HAT_THRESHOLD = 0.5f;


static void setButton(int button, bool down) {
	if (button < 0 || button >= PAD_BUTTONS || g_button[button] == down)
		return;
	g_button[button] = down;
	gamepadNavButton(button, down);
}


static int buttonForKey(int32_t key) {
	switch (key) {
		case AKEYCODE_BUTTON_A: return PAD_A;
		case AKEYCODE_BUTTON_B: return PAD_B;
		case AKEYCODE_BUTTON_X: return PAD_X;
		case AKEYCODE_BUTTON_Y: return PAD_Y;
		case AKEYCODE_BUTTON_L1: return PAD_L1;
		case AKEYCODE_BUTTON_R1: return PAD_R1;
		case AKEYCODE_BUTTON_SELECT: return PAD_SELECT;
		case AKEYCODE_BUTTON_START: return PAD_START;
		case AKEYCODE_BUTTON_MODE: return PAD_GUIDE;
		case AKEYCODE_BUTTON_THUMBL: return PAD_L3;
		case AKEYCODE_BUTTON_THUMBR: return PAD_R3;
		case AKEYCODE_DPAD_UP: return PAD_UP;
		case AKEYCODE_DPAD_RIGHT: return PAD_RIGHT;
		case AKEYCODE_DPAD_DOWN: return PAD_DOWN;
		case AKEYCODE_DPAD_LEFT: return PAD_LEFT;
		default: return -1;
	}
}


static bool isTriggerKey(int32_t key) {
	// Sent alongside the analog axis by pads that have one; the axis is what
	// is read, so these are only swallowed.
	return key == AKEYCODE_BUTTON_L2 || key == AKEYCODE_BUTTON_R2;
}


static int handleKey(AInputEvent* event) {
	int32_t source = AInputEvent_getSource(event);
	int32_t key = AKeyEvent_getKeyCode(event);
	int32_t action = AKeyEvent_getAction(event);
	bool fromPad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD
		|| (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK
		|| (source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
	int button = buttonForKey(key);
	if (button < 0) {
		if (fromPad && isTriggerKey(key)) {
			g_seen = true;
			return 1;
		}
		return 0;
	}
	// A hardware keyboard's arrow keys share the DPAD key codes. They are not
	// a controller's, and Rack has its own use for them.
	if (!fromPad)
		return 0;
	if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP)
		return 1;
	// Android repeats a held key by itself; the navigation times its own.
	if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(event) > 0)
		return 1;
	if (!g_seen)
		LOGI("controller seen: first key %d from source 0x%x", key, source);
	g_seen = true;
	LOGI("key %d %s -> button %d", key, action == AKEY_EVENT_ACTION_DOWN ? "down" : "up", button);
	setButton(button, action == AKEY_EVENT_ACTION_DOWN);
	return 1;
}


static int handleMotion(AInputEvent* event) {
	int32_t source = AInputEvent_getSource(event);
	if ((source & AINPUT_SOURCE_CLASS_JOYSTICK) == 0)
		return 0;
	if (!g_seen)
		LOGI("controller seen: first motion from source 0x%x", source);
	g_seen = true;

	g_axis[PAD_LX] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
	g_axis[PAD_LY] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
	g_axis[PAD_RX] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
	g_axis[PAD_RY] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
	g_axis[PAD_L2] = std::fmax(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0),
		AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0));
	g_axis[PAD_R2] = std::fmax(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0),
		AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0));

	float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
	float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
	bool hat[4] = {hy < -HAT_THRESHOLD, hx > HAT_THRESHOLD, hy > HAT_THRESHOLD, hx < -HAT_THRESHOLD};
	static const int hatButton[4] = {PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT};
	for (int i = 0; i < 4; i++) {
		if (hat[i] == g_hat[i])
			continue;
		g_hat[i] = hat[i];
		setButton(hatButton[i], hat[i]);
	}
	return 1;
}


int gamepadHandleEvent(AInputEvent* event) {
	switch (AInputEvent_getType(event)) {
		case AINPUT_EVENT_TYPE_KEY: return handleKey(event);
		case AINPUT_EVENT_TYPE_MOTION: return handleMotion(event);
		default: return 0;
	}
}


bool gamepadButton(int button) {
	return button >= 0 && button < PAD_BUTTONS && g_button[button];
}


float gamepadAxis(int axis) {
	return axis >= 0 && axis < PAD_AXES ? g_axis[axis] : 0.f;
}


bool gamepadSeen() {
	return g_seen;
}


void gamepadInputReset() {
	for (int i = 0; i < PAD_BUTTONS; i++)
		setButton(i, false);
	for (int i = 0; i < 4; i++)
		g_hat[i] = false;
	for (int i = 0; i < PAD_AXES; i++)
		g_axis[i] = 0.f;
}


} // namespace rackdroid
