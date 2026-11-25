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
 *   CC 0  left stick X *    -10..10 V (right is positive)
 *   CC 1  left stick Y *    -10..10 V (up is positive)
 *   CC 2  right stick X     -10..10 V
 *   CC 3  right stick Y     -10..10 V
 *   CC 4  L2 *              0..10 V
 *   CC 5  R2 *              0..10 V
 *   CC 6..20  buttons A B X Y L1 R1 SELECT START GUIDE L3 R3 up right down left
 *             (0 or 10 V; L3 as it is, the others *, and never SELECT, START,
 *             GUIDE and R3, the play-mode switch)
 *   CC 21  tilt X            -10..10 V (right edge down is positive)
 *   CC 22  tilt Y            -10..10 V (top edge away from you is positive)
 *   CC 23  touch pad X       0..10 V (left to right)
 *   CC 24  touch pad Y       0..10 V (bottom to top)
 *   CC 25  touch pad gate    0 or 10 V (a finger is on the pad)
 *   CC 26  twist             -10..10 V (how fast it is turned like a wheel; right is positive)
 *   CC 27  shake             0..10 V
 *   CC 28  shaken            0 or 10 V
 *   CC 29..31  touch pad, second finger: X, Y, gate
 *   CC 64..71  drum pads     0 or 10 V
 *   CC 72..79  drum pads, how high on the pad: 0..10 V
 *   CC 80..83  faders        0..10 V
 *
 * One difference from the desktop driver, on purpose: stick-up is the positive
 * direction.
 *
 * All but what is touched only reaches the patch in play mode (R3 toggles it, see
 * gamepad_nav.cpp); outside it the patch sees it at rest. What is marked * has
 * another use on the rack, and is the patch's only if the patch uses it
 * (play_claims.cpp). The touch surfaces are their own switch: they send whenever
 * they are on screen.
 *
 * Movement and touch are measured on the Kotlin side (TiltSensor.kt,
 * MotionSensor.kt, XyPad.kt, PlayPads.kt) and arrive through the JNI calls at the
 * bottom of this file.
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
#include "play_claims.hpp"

namespace rackdroid {


static const int DRIVER_ID = -10;
static const int NUM_AXES = 6;
static const int CC_TILT = NUM_AXES + PAD_BUTTONS;
static const int CC_PAD = CC_TILT + 2;
static const int CC_TWIST = CC_PAD + 3;
static const int CC_SHAKE = CC_TWIST + 1;
static const int CC_PAD2 = CC_SHAKE + 2;
static const int CC_DRUM = 64;
static const int DRUMS = 8;
static const int CC_DRUM_VALUE = CC_DRUM + DRUMS;
static const int CC_FADER = CC_DRUM_VALUE + DRUMS;
static const int NUM_CCS = PLAY_CCS;

// What comes from the Java UI thread, 0..1 or -1..1 by its CC; read on the render
// thread.
static std::atomic<float> g_outer[NUM_CCS] = {};
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

	/** Between -1 and 1. */
	static float level(int cc, bool play) {
		float outer = g_outer[cc].load(std::memory_order_relaxed);
		if ((cc >= CC_PAD && cc < CC_PAD + 3) || cc >= CC_PAD2)
			return outer;
		if (!play)
			return 0.f;
		switch (cc) {
			case 0: return playClaimed(cc) ? restAtZero(gamepadAxis(PAD_LX)) : 0.f;
			case 1: return playClaimed(cc) ? -restAtZero(gamepadAxis(PAD_LY)) : 0.f;
			case 2: return restAtZero(gamepadAxis(PAD_RX));
			case 3: return -restAtZero(gamepadAxis(PAD_RY));
			case 4: return playClaimed(cc) ? restAtZero(gamepadAxis(PAD_L2)) : 0.f;
			case 5: return playClaimed(cc) ? restAtZero(gamepadAxis(PAD_R2)) : 0.f;
			default: break;
		}
		if (cc >= CC_TILT)
			return outer;
		int button = cc - NUM_AXES;
		if (button == PAD_L3)
			return gamepadButton(button) ? 1.f : 0.f;
		return playButton(button) && playClaimed(cc) && gamepadButton(button) ? 1.f : 0.f;
	}

	void step() {
		bool play = gamepadPlayMode();
		for (int i = 0; i < NUM_CCS; i++) {
			// Same encoding as Rack's own driver: fourteen bits, allowed to go
			// negative, which MIDI-CC to CV reads back as a bipolar voltage.
			bool gate = (i >= NUM_AXES && i < CC_TILT) || i == CC_PAD + 2 || i == CC_SHAKE + 1 || i == CC_PAD2 + 2
				|| (i >= CC_DRUM && i < CC_DRUM_VALUE);
			// The other half of a CC has room only below 32.
			bool analog = !gate && i < 32;
			float level = this->level(i, play);
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


static void outer(int cc, float value) {
	if (cc >= 0 && cc < rackdroid::PLAY_CCS)
		rackdroid::g_outer[cc].store(value, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadTilt(JNIEnv*, jobject, jfloat x, jfloat y) {
	outer(rackdroid::CC_TILT, x);
	outer(rackdroid::CC_TILT + 1, y);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadMotion(JNIEnv*, jobject, jfloat twist, jfloat shake, jboolean shaken) {
	outer(rackdroid::CC_TWIST, twist);
	outer(rackdroid::CC_SHAKE, shake);
	outer(rackdroid::CC_SHAKE + 1, shaken ? 1.f : 0.f);
}

/** finger 0 or 1 */
extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadPad(JNIEnv*, jobject, jint finger, jfloat x, jfloat y, jboolean down) {
	int cc = finger == 0 ? rackdroid::CC_PAD : rackdroid::CC_PAD2;
	outer(cc, x);
	outer(cc + 1, y);
	outer(cc + 2, down ? 1.f : 0.f);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadDrum(JNIEnv*, jobject, jint pad, jfloat value, jboolean down) {
	if (pad < 0 || pad >= rackdroid::DRUMS)
		return;
	if (down)
		outer(rackdroid::CC_DRUM_VALUE + pad, value);
	outer(rackdroid::CC_DRUM + pad, down ? 1.f : 0.f);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeGamepadFader(JNIEnv*, jobject, jint fader, jfloat value) {
	if (fader >= 0 && fader < 4)
		outer(rackdroid::CC_FADER + fader, value);
}
