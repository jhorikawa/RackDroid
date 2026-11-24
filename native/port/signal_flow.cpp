/* Which way a cable goes, and how much is going through it.
 *
 * A cable in Rack looks the same from both ends, so reading a patch means
 * following every one of them to a jack and reading the jack. This puts a row
 * of arrowheads on each cable that travel from the output to the input: the
 * direction can be read even from a still picture, and how strongly they show
 * is the level of the signal. With no signal they are faint and stand still.
 *
 * With a module selected from the controller (or a jack focused), the cables
 * that come into it are blue, the ones that leave it orange, and the rest step
 * back.
 *
 * The levels are the ones Rack already keeps for the lights in the plugs, so
 * nothing is measured and nothing runs on the audio thread.
 *
 * Drawn as a child of the RackWidget, above the cables and in their layer. A
 * cable's arrowheads are one path, stroked twice (a dark edge under a light
 * core): a few dozen strokes a frame for a patch of a few dozen cables.
 */
#include <atomic>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <context.hpp>
#include <engine/Cable.hpp>
#include <engine/Module.hpp>
#include <settings.hpp>
#include <system.hpp>
#include <widget/Widget.hpp>

#include <nanovg.h>

#include "gamepad_nav.hpp"
#include "signal_flow.hpp"
#include "transport.hpp"
#include "window_android.hpp"

#include <jni.h>

using namespace rack;

namespace rackdroid {


/** Set from the UI thread (the controller's main menu). */
static std::atomic<bool> g_on{true};

/** Rack units. */
static const float SPACING = 34.f;
static const float SPEED = 62.f;
/** The plug covers the end of the cable. */
static const float END_GAP = 22.f;
/** Over this much of it, at either end, an arrowhead grows and shrinks rather
than appear and vanish. */
static const float FADE = 11.f;
/** Pieces the curve is measured in. */
static const int STEPS = 18;


/** What a cable's arrowheads remember from one frame to the next. Their place is
kept as a part of the space between two of them, and moved on by a little each
frame: worked out afresh from the time, a space that changed by a hundredth --
the signal's level, the zoom -- threw the whole row about. */
struct CableState {
	float pos = 0.f;
	int count = 0;
	float level = 0.f;
	float alpha = 0.f;
	bool started = false;
	int seen = 0;
};

static float follow(float now, float target, float dt, float seconds) {
	return now + (target - now) * (1.f - std::exp(-dt / seconds));
}


struct SignalFlow : widget::Widget {
	double lastTime = 0.0;
	/** The time the arrowheads go by: the clock's, without the unevenness with
	which frames come to be drawn. */
	double clock = 0.0;
	float period = 1.f / 60.f;
	float dt = 0.f;
	int frame = 0;
	bool moving = false;
	std::map<int64_t, CableState> states;

	void tick() {
		double now = system::getTime();
		float real = lastTime > 0.0 ? (float) (now - lastTime) : 0.f;
		lastTime = now;
		if (clock <= 0.0 || real > 0.07f) {
			clock = now;
			dt = 0.f;
			return;
		}
		period += (real - period) * 0.1f;
		dt = period + (float) (now - clock - period) * 0.15f;
		dt = math::clamp(dt, 0.f, 0.1f);
		clock += dt;
	}

	/** The same sag the cable itself is drawn with (CableWidget.cpp). */
	static math::Vec slumpOf(math::Vec a, math::Vec b) {
		float dist = a.minus(b).norm();
		math::Vec avg = a.plus(b).div(2.f);
		avg.y += (1.f - settings::cableTension) * (150.f + 1.f * dist);
		return avg;
	}

	/** 0 to 1, from the lights in the plug: a signal at 10 V is 1. */
	static float levelOf(engine::Output& out) {
		float v = 0.f;
		for (int i = 0; i < 3; i++)
			v = std::fmax(v, out.plugLights[i].getBrightness());
		return math::clamp(v, 0.f, 1.f);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 3 || !g_on.load(std::memory_order_relaxed))
			return;
		app::RackWidget* rack = APP->scene ? APP->scene->rack : NULL;
		if (!rack)
			return;
		tick();
		frame++;
		moving = false;

		// Physical pixels to a rack unit: zoomed out, the arrowheads keep a
		// size that can be seen and stand further apart.
		float xform[6];
		nvgCurrentTransform(args.vg, xform);
		float scale = xform[0];
		if (scale <= 0.f)
			return;
		float grow = math::clamp(1.5f / scale, 1.f, 3.f);

		CableFocus focus;
		bool focused = gamepadNavCableFocus(focus);
		nvgSave(args.vg);
		nvgLineCap(args.vg, NVG_ROUND);
		nvgLineJoin(args.vg, NVG_ROUND);
		for (app::CableWidget* cw : rack->getCompleteCables())
			drawCable(args, cw, focused ? &focus : NULL, grow);
		nvgRestore(args.vg);
		if (moving)
			windowNoteAnimation();
		if (frame % 600 == 0) {
			for (auto it = states.begin(); it != states.end();)
				it = it->second.seen + 600 < frame ? states.erase(it) : std::next(it);
		}
	}

	void drawCable(const DrawArgs& args, app::CableWidget* cw, const CableFocus* focus, float grow) {
		engine::Cable* cable = cw->getCable();
		if (!cable || !cable->outputModule || !cable->inputModule)
			return;
		math::Vec a = cw->getOutputPos();
		math::Vec b = cw->getInputPos();
		math::Vec c = slumpOf(a, b);
		// A quadratic curve lies inside the triangle of its points.
		float minX = std::fmin(a.x, std::fmin(b.x, c.x)) - 10.f, maxX = std::fmax(a.x, std::fmax(b.x, c.x)) + 10.f;
		float minY = std::fmin(a.y, std::fmin(b.y, c.y)) - 10.f, maxY = std::fmax(a.y, std::fmax(b.y, c.y)) + 10.f;
		if (!args.clipBox.intersects(math::Rect(minX, minY, maxX - minX, maxY - minY)))
			return;
		// The ends of the cable, as it is drawn, are off the jacks' centres.
		a = a.plus(c.minus(a).normalize().mult(14.f));
		b = b.plus(c.minus(b).normalize().mult(14.f));

		engine::Output& out = cable->outputModule->outputs[cable->outputId];
		bool silent = out.getChannels() == 0;
		float level = silent ? 0.f : levelOf(out);
		bool incoming = focus && cable->inputModule->id == focus->moduleId
			&& (focus->portType < 0 || (focus->portType == engine::Port::INPUT && cable->inputId == focus->portId));
		bool outgoing = focus && cable->outputModule->id == focus->moduleId
			&& (focus->portType < 0 || (focus->portType == engine::Port::OUTPUT && cable->outputId == focus->portId));

		CableState& st = states[cable->id];
		st.seen = frame;
		if (!st.started) {
			st.started = true;
			// Each cable at a phase of its own: in step, a bundle of them reads
			// as one wave across instead of as many lines along.
			st.pos = (float) ((uint64_t) cable->id % 97) / 97.f;
			st.level = level;
		}
		// The lights in the plugs flicker with the sound. Up at once, down
		// slowly: what is wanted is how strong the signal is, not its wave.
		st.level = follow(st.level, level, dt, level > st.level ? 0.06f : 0.5f);

		// Quiet signals should not vanish: half the range is spent on the
		// first tenth of it.
		float shown = std::sqrt(st.level);
		float alpha = silent ? 0.22f : 0.45f + 0.55f * shown;
		NVGcolor core = nvgRGB(0xFF, 0xFF, 0xFF);
		if (incoming || outgoing) {
			core = outgoing ? nvgRGB(0xFF, 0xB0, 0x4A) : nvgRGB(0x7C, 0xD4, 0xFF);
			alpha = std::fmax(alpha, 0.75f);
		}
		else if (focus && focus->portType < 0) {
			// Only for a whole module: stepping from jack to jack, the rest of
			// the rack should not flicker.
			alpha *= 0.4f;
		}
		st.alpha = st.alpha <= 0.f ? alpha : follow(st.alpha, alpha, dt, 0.12f);
		alpha = st.alpha;
		float size = (3.1f + 1.7f * shown) * grow;
		if (incoming || outgoing)
			size *= 1.2f;
		float width = 1.7f * grow;

		// Along the curve by distance, not by parameter: the arrowheads stay
		// evenly spaced however it bends.
		math::Vec pts[STEPS + 1];
		float at[STEPS + 1];
		at[0] = 0.f;
		pts[0] = a;
		for (int i = 1; i <= STEPS; i++) {
			float t = (float) i / STEPS, u = 1.f - t;
			pts[i] = a.mult(u * u).plus(c.mult(2.f * u * t)).plus(b.mult(t * t));
			at[i] = at[i - 1] + pts[i].minus(pts[i - 1]).norm();
		}
		float length = at[STEPS];
		// Nothing here may depend on the signal: it is where they stand.
		float gap = std::fmin(END_GAP - 14.f + 3.f * grow, length * 0.2f);
		// A whole number of them, evenly spaced: the row moves on without ever
		// leaving a short cable empty.
		float usable = length - 2.f * gap;
		if (usable <= 0.f)
			return;
		// One more or one fewer only once it is clearly called for, not back
		// and forth about the middle.
		float ideal = usable / (SPACING * grow);
		if (st.count < 1 || std::fabs(ideal - st.count) > 0.7f)
			st.count = (int) std::fmax(1.f, std::round(ideal));
		int count = st.count;
		float spacing = usable / count;
		if (!silent && !transportPaused()) {
			st.pos += SPEED * grow * dt / spacing;
			st.pos -= std::floor(st.pos);
			moving = true;
		}
		float fade = std::fmin(FADE * grow, spacing * 0.4f);

		nvgBeginPath(args.vg);
		bool any = false;
		int seg = 1;
		auto arrow = [&](float d) {
			while (seg < STEPS && at[seg] < d)
				seg++;
			float run = at[seg] - at[seg - 1];
			float f = run > 0.f ? (d - at[seg - 1]) / run : 0.f;
			math::Vec p = pts[seg - 1].plus(pts[seg].minus(pts[seg - 1]).mult(f));
			math::Vec dir = pts[seg].minus(pts[seg - 1]);
			float n = dir.norm();
			if (n <= 0.f)
				return;
			dir = dir.div(n);
			float full = math::clamp(std::fmin(d - gap, gap + usable - d) / fade, 0.f, 1.f);
			if (full < 0.35f)
				return;
			float s = size * full;
			math::Vec side(-dir.y, dir.x);
			math::Vec tip = p.plus(dir.mult(s * 0.6f));
			math::Vec l = p.minus(dir.mult(s * 0.6f)).plus(side.mult(s));
			math::Vec r = p.minus(dir.mult(s * 0.6f)).minus(side.mult(s));
			nvgMoveTo(args.vg, l.x, l.y);
			nvgLineTo(args.vg, tip.x, tip.y);
			nvgLineTo(args.vg, r.x, r.y);
			any = true;
		};
		for (int i = 0; i < count; i++)
			arrow(gap + (i + st.pos) * spacing);
		if (!any)
			return;
		nvgStrokeColor(args.vg, nvgRGBAf(0.f, 0.f, 0.f, alpha * 0.75f));
		nvgStrokeWidth(args.vg, width + 1.6f * grow);
		nvgStroke(args.vg);
		nvgStrokeColor(args.vg, nvgTransRGBAf(core, alpha));
		nvgStrokeWidth(args.vg, width);
		nvgStroke(args.vg);
	}
};


static SignalFlow* g_flow = NULL;


void installSignalFlow() {
	if (g_flow || !APP->scene || !APP->scene->rack)
		return;
	app::RackWidget* rack = APP->scene->rack;
	g_flow = new SignalFlow;
	// Never culled: the cables can be anywhere on the rack.
	g_flow->box = rack->box.zeroPos();
	rack->addChildAbove(g_flow, rack->getCableContainer());
}


} // namespace rackdroid


extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeSetSignalFlow(JNIEnv*, jobject, jboolean on) {
	rackdroid::g_on.store(on == JNI_TRUE, std::memory_order_relaxed);
}
