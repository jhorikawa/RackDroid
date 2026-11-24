/* Pause and play. See transport.hpp.
 *
 * Asked for by SELECT on a game controller, by the button beside the toolbar's
 * arrow and by the controller's main menu.
 *
 * While paused the engine is not being run, so what judges it by how it runs
 * (thread count, block size, buffer depth -- see android_main()) has to stand
 * still as well, and gets the same moment to settle afterwards as it does when
 * the screen comes back.
 */
#include <atomic>
#include <cmath>
#include <string>

#include <app/Scene.hpp>
#include <context.hpp>
#include <widget/Widget.hpp>
#include <window/Window.hpp>

#include <nanovg.h>

#include "cable_park.hpp"
#include "gamepad_nav.hpp"
#include "jni_bridge.hpp"
#include "transport.hpp"
#include "window_android.hpp"

#include <android/log.h>
#include <jni.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.transport", __VA_ARGS__)

using namespace rack;

namespace rackdroid {


static std::atomic<bool> g_paused{false};
/** Asked for from the UI thread, carried out on the render thread. */
static std::atomic<bool> g_toggleWanted{false};
/** The patch epoch as last told to transportStep(), and when the pause began. */
static int g_epoch = 0;
static int g_epochAtPause = 0;

/** Audio thread only: 1 playing, 0 silent. */
static float g_gain = 1.f;
/** Long enough not to click, short enough to answer the button at once. */
static const float FADE_SECONDS = 0.01f;


bool transportPaused() {
	return g_paused.load(std::memory_order_relaxed);
}


void transportSetPaused(bool paused) {
	if (g_paused.exchange(paused, std::memory_order_relaxed) == paused)
		return;
	g_epochAtPause = g_epoch;
	if (!paused)
		windowNoteSurfaceChange();
	windowNoteInteraction();
	notifyPaused(paused);
	LOGI("%s", paused ? "paused" : "playing");
}


void transportToggle() {
	transportSetPaused(!transportPaused());
}


void transportStep(int patchEpoch) {
	g_epoch = patchEpoch;
	if (g_toggleWanted.exchange(false, std::memory_order_relaxed))
		transportToggle();
	// A patch just opened is opened to be heard.
	if (transportPaused() && g_epoch != g_epochAtPause)
		transportSetPaused(false);
}


bool transportHeld() {
	return g_gain <= 0.f && transportPaused();
}


void transportFade(float* output, int channels, int32_t frames, int32_t sampleRate) {
	float target = transportPaused() ? 0.f : 1.f;
	if (g_gain == target || sampleRate <= 0)
		return;
	float step = 1.f / (FADE_SECONDS * sampleRate);
	for (int32_t i = 0; i < frames; i++) {
		g_gain = target > g_gain ? std::fmin(g_gain + step, target) : std::fmax(g_gain - step, target);
		for (int c = 0; c < channels; c++)
			output[i * channels + c] *= g_gain;
	}
}


struct PausedSign : widget::Widget {
	void step() override {
		if (APP->scene)
			box.size = APP->scene->box.size;
		widget::Widget::step();
	}

	void draw(const DrawArgs& args) override {
		if (!transportPaused() || !APP->window->uiFont)
			return;
		const char* text = "PAUSED";
		const NVGcolor accent = nvgRGB(0xFF, 0xB4, 0x54);
		nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, 15.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		float tw = nvgTextBounds(args.vg, 0.f, 0.f, text, NULL, NULL);
		static const float ICON = 10.f;
		float w = ICON + 8.f + tw + 28.f;
		float h = 26.f;
		float x = (box.size.x - gamepadNavPanelWidth() - w) * 0.5f;
		float y = std::fmax(cableParkTopInset(), 0.f) + 8.f;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, x, y, w, h, h * 0.5f);
		nvgFillColor(args.vg, nvgRGBA(0x1C, 0x18, 0x13, 0xEC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, accent);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		nvgFillColor(args.vg, accent);
		for (int i = 0; i < 2; i++) {
			nvgBeginPath(args.vg);
			nvgRect(args.vg, x + 14.f + i * 6.f, y + (h - ICON) * 0.5f, 3.5f, ICON);
			nvgFill(args.vg);
		}
		nvgText(args.vg, x + 14.f + ICON + 8.f, y + h * 0.5f, text, NULL);
	}
};


static PausedSign* g_sign = NULL;


void installTransport() {
	if (g_sign || !APP->scene)
		return;
	g_sign = new PausedSign;
	g_sign->box.pos = math::Vec(0.f, 0.f);
	g_sign->box.size = APP->scene->box.size;
	APP->scene->addChild(g_sign);
}


} // namespace rackdroid


extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeTransportToggle(JNIEnv*, jobject) {
	rackdroid::g_toggleWanted.store(true, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_rackdroid_MainActivity_nativeTransportPaused(JNIEnv*, jobject) {
	return rackdroid::transportPaused() ? JNI_TRUE : JNI_FALSE;
}
