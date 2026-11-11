/* The built-in controller as a MIDI device.
 *
 * Desktop Rack has a "Gamepad" MIDI driver (src/gamepad.cpp) that turns a
 * controller's axes and buttons into CC messages, so a stick can be patched
 * like any other voltage source: MIDI-CC to CV gives it jacks, MIDI-Map ties it
 * to a knob. That file reads the controller through GLFW, which does not exist
 * here, so this is the same driver over gamepad_input.cpp instead -- same
 * driver id (-10) and name, so a patch made on either side finds the driver on
 * the other. The CC numbers follow what GLFW gives for a standard pad there:
 *
 *   CC 0  left stick X      -10..10 V (right is positive)
 *   CC 1  left stick Y      -10..10 V (up is positive)
 *   CC 2  right stick X     held at 0: the right stick turns the focused knob
 *   CC 3  right stick Y     held at 0
 *   CC 4  L2                0..10 V
 *   CC 5  R2                0..10 V
 *   CC 6..20  buttons A B X Y L1 R1 SELECT START GUIDE L3 R3 up right down left
 *             (0 or 10 V; only R3 is sent, the rest operate the rack)
 *   CC 21 on  reserved for the tilt sensor and the touch pad
 *
 * Two differences from the desktop driver, both on purpose: the triggers rest
 * at 0 V instead of -10 V, and stick-up is the positive direction.
 *
 * The sticks and triggers only reach the patch in play mode (L3 toggles it,
 * see gamepad_nav.cpp). Outside it they pan and zoom the rack, and the patch
 * sees them at rest.
 */
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <midi.hpp>

#include "gamepad_input.hpp"
#include "gamepad_midi.hpp"
#include "gamepad_nav.hpp"

namespace rackdroid {


static const int DRIVER_ID = -10;
static const int NUM_AXES = 6;
static const int NUM_CCS = NUM_AXES + PAD_BUTTONS;
/** A stick never rests at exactly zero. Without this the patch would see a
voltage that is not quite 0 V, and a 14-bit CC would chatter. */
static const float CV_DEADZONE = 0.05f;


static float restAtZero(float v) {
	float a = std::fabs(v);
	if (a < CV_DEADZONE)
		return 0.f;
	return std::copysign((a - CV_DEADZONE) / (1.f - CV_DEADZONE), v);
}


struct PadMidiDevice : rack::midi::InputDevice {
	int16_t sent[NUM_CCS] = {};

	std::string getName() override {
		return "Built-in controller";
	}

	void step() {
		bool play = gamepadPlayMode();
		float axes[NUM_AXES] = {};
		if (play) {
			axes[0] = restAtZero(gamepadAxis(PAD_LX));
			axes[1] = -restAtZero(gamepadAxis(PAD_LY));
			axes[4] = gamepadAxis(PAD_L2);
			axes[5] = gamepadAxis(PAD_R2);
		}
		for (int i = 0; i < NUM_CCS; i++) {
			// Same encoding as Rack's own driver: fourteen bits, allowed to go
			// negative, which MIDI-CC to CV reads back as a bipolar voltage.
			int16_t value;
			if (i < NUM_AXES)
				value = (int16_t) std::max(-0x3f80, std::min(0x3f80, (int) std::round(axes[i] * 0x3f80)));
			else
				value = (play && i - NUM_AXES == PAD_R3 && gamepadButton(PAD_R3)) ? 0x3f80 : 0;
			if (value == sent[i])
				continue;
			sent[i] = value;

			rack::midi::Message msg;
			msg.setStatus(0xb);
			msg.setNote(i);
			msg.bytes[2] = (uint8_t) (value >> 7);
			onMessage(msg);
			if (i < NUM_AXES) {
				rack::midi::Message lsb;
				lsb.setStatus(0xb);
				lsb.setNote(i + 32);
				lsb.bytes[2] = (uint8_t) (value & 0x7f);
				onMessage(lsb);
			}
		}
	}
};


struct PadMidiDriver : rack::midi::Driver {
	PadMidiDevice device;

	std::string getName() override {
		return "Gamepad";
	}
	std::vector<int> getInputDeviceIds() override {
		return {0};
	}
	int getDefaultInputDeviceId() override {
		return 0;
	}
	std::string getInputDeviceName(int deviceId) override {
		return deviceId == 0 ? device.getName() : "";
	}
	rack::midi::InputDevice* subscribeInput(int deviceId, rack::midi::Input* input) override {
		if (deviceId != 0)
			return NULL;
		device.subscribe(input);
		return &device;
	}
	void unsubscribeInput(int deviceId, rack::midi::Input* input) override {
		if (deviceId == 0)
			device.unsubscribe(input);
	}
};


static PadMidiDriver* g_driver = NULL;


void gamepadMidiInit() {
	if (g_driver)
		return;
	g_driver = new PadMidiDriver;
	rack::midi::addDriver(DRIVER_ID, g_driver);
}


void gamepadMidiStep() {
	if (g_driver)
		g_driver->device.step();
}


} // namespace rackdroid
