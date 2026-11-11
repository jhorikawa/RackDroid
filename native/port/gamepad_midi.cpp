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
 *   CC 0  left stick X      held at 0: the left stick pans the rack
 *   CC 1  left stick Y      held at 0
 *   CC 2  right stick X     -10..10 V (right is positive)
 *   CC 3  right stick Y     -10..10 V (up is positive)
 *   CC 4  L2                held at 0: the triggers zoom the rack
 *   CC 5  R2                held at 0
 *   CC 6..20  buttons A B X Y L1 R1 SELECT START GUIDE L3 R3 up right down left
 *             (0 or 10 V; only L3 is sent, as a gate -- the rest operate the
 *             rack, and R3 is the play-mode switch)
 *   CC 21  tilt X            -10..10 V (right edge down is positive)
 *   CC 22  tilt Y            -10..10 V (top edge away from you is positive)
 *   CC 23  touch pad X       0..10 V (left to right)
 *   CC 24  touch pad Y       0..10 V (bottom to top)
 *   CC 25  touch pad gate    0 or 10 V (a finger is on the pad)
 *
 * One difference from the desktop driver, on purpose: stick-up is the positive
 * direction.
 *
 * The right stick and the tilt only reach the patch in play mode (R3 toggles
 * it, see gamepad_nav.cpp). Outside it the stick turns the focused knob, the
 * tilt sensor is off, and the patch sees both at rest. The touch pad is its
 * own switch: it sends whenever it is on screen.
 *
 * Tilt and the pad are measured on the Kotlin side (TiltSensor.kt, XyPad.kt)
 * and arrive through the two JNI calls at the bottom of this file.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include <jni.h>

#include <midi.hpp>

#include "gamepad_input.hpp"
#include "gamepad_midi.hpp"
#include "gamepad_nav.hpp"

namespace rackdroid {


static const int DRIVER_ID = -10;
static const int NUM_AXES = 6;
static const int CC_TILT_X = NUM_AXES + PAD_BUTTONS;
static const int CC_TILT_Y = CC_TILT_X + 1;
static const int CC_PAD_X = CC_TILT_X + 2;
static const int CC_PAD_Y = CC_TILT_X + 3;
static const int CC_PAD_GATE = CC_TILT_X + 4;
static const int NUM_CCS = CC_PAD_GATE + 1;

// Written from the Java UI thread, read on the render thread.
static std::atomic<float> g_tiltX{0.f};
static std::atomic<float> g_tiltY{0.f};
static std::atomic<float> g_padX{0.f};
static std::atomic<float> g_padY{0.f};
static std::atomic<bool> g_padDown{false};
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
			axes[2] = restAtZero(gamepadAxis(PAD_RX));
			axes[3] = -restAtZero(gamepadAxis(PAD_RY));
		}
		for (int i = 0; i < NUM_CCS; i++) {
			// Same encoding as Rack's own driver: fourteen bits, allowed to go
			// negative, which MIDI-CC to CV reads back as a bipolar voltage.
			bool analog = true;
			float level = 0.f;
			if (i < NUM_AXES) {
				level = axes[i];
			}
			else if (i == CC_TILT_X) {
				level = play ? g_tiltX.load(std::memory_order_relaxed) : 0.f;
			}
			else if (i == CC_TILT_Y) {
				level = play ? g_tiltY.load(std::memory_order_relaxed) : 0.f;
			}
			else if (i == CC_PAD_X) {
				level = g_padX.load(std::memory_order_relaxed);
			}
			else if (i == CC_PAD_Y) {
				level = g_padY.load(std::memory_order_relaxed);
			}
			else {
				analog = false;
				bool on = i == CC_PAD_GATE ? g_padDown.load(std::memory_order_relaxed)
					: play && i - NUM_AXES == PAD_L3 && gamepadButton(PAD_L3);
				level = on ? 1.f : 0.f;
			}
			int16_t value = (int16_t) std::max(-0x3f80, std::min(0x3f80, (int) std::round(level * 0x3f80)));
			if (value == sent[i])
				continue;
			sent[i] = value;

			rack::midi::Message msg;
			msg.setStatus(0xb);
			msg.setNote(i);
			msg.bytes[2] = (uint8_t) (value >> 7);
			onMessage(msg);
			if (analog) {
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


extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadTilt(JNIEnv*, jobject, jfloat x, jfloat y) {
	rackdroid::g_tiltX.store(x, std::memory_order_relaxed);
	rackdroid::g_tiltY.store(y, std::memory_order_relaxed);
}


extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadPad(JNIEnv*, jobject, jfloat x, jfloat y, jboolean down) {
	rackdroid::g_padX.store(x, std::memory_order_relaxed);
	rackdroid::g_padY.store(y, std::memory_order_relaxed);
	rackdroid::g_padDown.store(down, std::memory_order_relaxed);
}
