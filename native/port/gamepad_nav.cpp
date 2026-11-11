/* Operating the rack with a game controller.
 *
 * RackDroid is built for a finger, and on a 4.5" handheld a finger is a blunt
 * tool: a drag meant to scroll moves a module or turns a knob. This gives the
 * rack a focus that the D-pad moves from control to control, the way a small
 * button-only rack does it:
 *
 *   D-pad        move the focus to the nearest knob/jack in that direction
 *   A            knob: edit (D-pad up/down coarse, left/right fine)
 *                switch: step it; a momentary button is held while A is held
 *                jack: start a cable, then A on another jack to connect
 *   B            back: leave edit, cancel the cable, and with nothing left to
 *                back out of, put the focus ring away
 *   Y            clear: reset the focused knob, unplug the focused jack
 *   right stick  turn the focused knob directly
 *   left stick   pan the rack
 *   L2 / R2      zoom out / in
 *   R3           play mode: the right stick becomes two voltages for the patch
 *                instead (see gamepad_midi.cpp). Only the right stick changes
 *                hands -- a knob can still be edited with A and the D-pad,
 *                while nothing else can pan.
 *   L1 / R1      previous / next module
 *   SELECT       cycle the zoom
 *
 * Parameters are changed through their ParamQuantity, not by faking a drag, so
 * every plugin's controls behave the same. Changes and cables go on the undo
 * history. Touch keeps working throughout.
 *
 * Everything here runs on the render thread (input is dispatched from the same
 * loop that draws), so Rack's widgets are touched directly.
 */
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/ParamWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/RackScrollWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <app/Switch.hpp>
#include <context.hpp>
#include <engine/Module.hpp>
#include <engine/ParamQuantity.hpp>
#include <engine/PortInfo.hpp>
#include <history.hpp>
#include <plugin/Model.hpp>
#include <system.hpp>
#include <widget/Widget.hpp>
#include <window/Window.hpp>

#include <nanovg.h>

#include "cable_park.hpp"
#include "gamepad_input.hpp"
#include "gamepad_nav.hpp"
#include "touch_input.hpp"
#include "window_android.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.pad", __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#endif

using namespace rack;

namespace rackdroid {


enum FocusKind { FOCUS_NONE = 0, FOCUS_PARAM, FOCUS_INPUT, FOCUS_OUTPUT };

struct Focus {
	int64_t moduleId = -1;
	int kind = FOCUS_NONE;
	int id = -1;
	bool operator==(const Focus& o) const {
		return moduleId == o.moduleId && kind == o.kind && id == o.id;
	}
	bool isPort() const { return kind == FOCUS_INPUT || kind == FOCUS_OUTPUT; }
};

struct Item {
	Focus focus;
	/** Centre in the rack's own coordinates: independent of zoom and scroll. */
	math::Vec pos;
};


// --- state ------------------------------------------------------------------

static Focus g_focus;
/** Where the focus last was in each module, so L1/R1 return to it. */
static std::map<int64_t, Focus> g_remembered;
/** The ring is shown. Cleared by a touch, set by any button. */
static bool g_active = false;
static bool g_editing = false;
/** A momentary button is being held down by A. */
static bool g_momentary = false;
static bool g_cabling = false;
static Focus g_cableFrom;
/** The rack is scrolling after the focus. */
static bool g_follow = false;
/** The left stick or the triggers moved the view since the focus last moved,
so the focus may have been left behind off screen. */
static bool g_viewMoved = false;
static bool g_playMode = false;
/** When the mode last changed, to say so on screen for a moment. */
static double g_modeChanged = -1e9;

static int g_dirHeld = -1;
static double g_repeatAt = 0.0;
static double g_lastStep = 0.0;

/** An edit in progress, to become one undo step. */
static bool g_histOpen = false;
static Focus g_histFocus;
static float g_histOld = 0.f;
static double g_histTouched = 0.0;
/** Fractions of a step the right stick has built up on a snapping control. */
static float g_snapAcc = 0.f;

static const double REPEAT_DELAY = 0.32;
static const double REPEAT_MOVE = 0.09;
static const double REPEAT_EDIT = 0.035;
static const float STICK_DEADZONE = 0.18f;
static const float TRIGGER_DEADZONE = 0.08f;
/** Scene units kept clear around the focused control when scrolling to it. */
static const float FOLLOW_MARGIN = 34.f;
/** The module palette and the status strip sit over the bottom of the rack. */
static const float BOTTOM_INSET = 92.f;
static const float ZOOM_LEVELS[] = {1.f, 2.f, 0.5f};


// --- lookups ----------------------------------------------------------------

static app::RackWidget* rackWidget() {
	return APP->scene ? APP->scene->rack : NULL;
}


static app::ParamWidget* paramOf(const Focus& f) {
	if (f.kind != FOCUS_PARAM || !rackWidget())
		return NULL;
	app::ModuleWidget* mw = rackWidget()->getModule(f.moduleId);
	return mw ? mw->getParam(f.id) : NULL;
}


static app::PortWidget* portOf(const Focus& f) {
	if (!f.isPort() || !rackWidget())
		return NULL;
	app::ModuleWidget* mw = rackWidget()->getModule(f.moduleId);
	if (!mw)
		return NULL;
	return f.kind == FOCUS_INPUT ? mw->getInput(f.id) : mw->getOutput(f.id);
}


static widget::Widget* widgetOf(const Focus& f) {
	if (f.kind == FOCUS_PARAM)
		return paramOf(f);
	return portOf(f);
}


/** Shown all the way up to its module: plugins hide controls by hiding a
parent (alternate panels, collapsed sections). */
static bool shownIn(widget::Widget* w, widget::Widget* top) {
	for (widget::Widget* p = w; p && p != top; p = p->parent) {
		if (!p->visible)
			return false;
	}
	return w->box.size.x > 0.f && w->box.size.y > 0.f;
}


static void collect(app::ModuleWidget* mw, std::vector<Item>& out) {
	if (!mw || !mw->module)
		return;
	app::RackWidget* rack = rackWidget();
	for (app::ParamWidget* pw : mw->getParams()) {
		if (!pw->getParamQuantity() || !shownIn(pw, mw))
			continue;
		Item it;
		it.focus.moduleId = mw->module->id;
		it.focus.kind = FOCUS_PARAM;
		it.focus.id = pw->paramId;
		it.pos = pw->getRelativeOffset(pw->box.size.mult(0.5f), rack);
		out.push_back(it);
	}
	for (app::PortWidget* port : mw->getPorts()) {
		if (!shownIn(port, mw))
			continue;
		Item it;
		it.focus.moduleId = mw->module->id;
		it.focus.kind = port->type == engine::Port::INPUT ? FOCUS_INPUT : FOCUS_OUTPUT;
		it.focus.id = port->portId;
		it.pos = port->getRelativeOffset(port->box.size.mult(0.5f), rack);
		out.push_back(it);
	}
}


/** While a cable is being run, only jacks that could take its other end. */
static bool allowed(const Focus& f) {
	if (!g_cabling)
		return true;
	if (f == g_cableFrom)
		return true;
	return f.isPort() && f.kind != g_cableFrom.kind;
}


// --- undo -------------------------------------------------------------------

static void closeHistory() {
	if (!g_histOpen)
		return;
	g_histOpen = false;
	app::ParamWidget* pw = paramOf(g_histFocus);
	engine::ParamQuantity* pq = pw ? pw->getParamQuantity() : NULL;
	if (!pq)
		return;
	float now = pq->getValue();
	if (now == g_histOld)
		return;
	history::ParamChange* h = new history::ParamChange;
	h->name = "move knob";
	h->moduleId = g_histFocus.moduleId;
	h->paramId = g_histFocus.id;
	h->oldValue = g_histOld;
	h->newValue = now;
	APP->history->push(h);
}


static void setParam(engine::ParamQuantity* pq, float value) {
	if (!g_histOpen || !(g_histFocus == g_focus)) {
		closeHistory();
		g_histOpen = true;
		g_histFocus = g_focus;
		g_histOld = pq->getValue();
	}
	g_histTouched = system::getTime();
	pq->setValue(math::clamp(value, pq->getMinValue(), pq->getMaxValue()));
}


// --- focus ------------------------------------------------------------------

static void releaseMomentary() {
	if (!g_momentary)
		return;
	g_momentary = false;
	app::ParamWidget* pw = paramOf(g_focus);
	engine::ParamQuantity* pq = pw ? pw->getParamQuantity() : NULL;
	if (pq)
		pq->setMin();
}


static void setFocus(const Focus& f) {
	if (f == g_focus)
		return;
	releaseMomentary();
	closeHistory();
	g_editing = false;
	g_snapAcc = 0.f;
	g_focus = f;
	g_remembered[f.moduleId] = f;
	g_follow = true;
	g_viewMoved = false;
}


/** The part of the rack view nothing sits on top of: below the toolbar, above
the palette and the status strip. Scene units. */
static math::Rect clearView() {
	app::RackScrollWidget* rs = APP->scene->rackScroll;
	math::Vec o = rs->getAbsoluteOffset(math::Vec(0.f, 0.f));
	float top = std::fmax(o.y, cableParkTopInset());
	float bottom = o.y + rs->box.size.y - BOTTOM_INSET;
	if (bottom < top)
		top = bottom = (top + bottom) * 0.5f;
	return math::Rect(o.x, top, rs->box.size.x, bottom - top);
}


/** Focuses the control nearest the middle of what is on screen. */
static void pickNearCentre() {
	app::RackWidget* rack = rackWidget();
	if (!rack || !APP->scene->rackScroll)
		return;
	math::Vec centre = clearView().getCenter();
	float best = INFINITY;
	Focus pick;
	for (app::ModuleWidget* mw : rack->getModules()) {
		std::vector<Item> items;
		collect(mw, items);
		for (const Item& it : items) {
			if (!allowed(it.focus))
				continue;
			widget::Widget* w = widgetOf(it.focus);
			if (!w)
				continue;
			float d = w->getAbsoluteOffset(w->box.size.mult(0.5f)).minus(centre).square();
			if (d < best) {
				best = d;
				pick = it.focus;
			}
		}
	}
	if (pick.kind == FOCUS_NONE)
		return;
	if (pick == g_focus) {
		g_follow = true;
		g_viewMoved = false;
		return;
	}
	setFocus(pick);
}


/** Makes sure there is a live focus, on screen. Returns false when one had to
be picked, so the press that asked only reveals the ring instead of also acting
on a control the user was not looking at. */
static bool ensureFocus() {
	widget::Widget* w = widgetOf(g_focus);
	if (w && allowed(g_focus)) {
		if (!g_viewMoved)
			return true;
		// The view was panned or zoomed by hand. If that left the focus off
		// screen, the next press starts from what is being looked at now.
		g_viewMoved = false;
		if (clearView().contains(w->getAbsoluteOffset(w->box.size.mult(0.5f))))
			return true;
	}
	pickNearCentre();
	return false;
}


/** Best item from `items` lying in direction `dir` of `from`. With `cone`,
only those within about sixty degrees of it. */
static bool pickToward(const std::vector<Item>& items, math::Vec from, math::Vec dir,
		bool cone, Focus& out) {
	float best = INFINITY;
	bool found = false;
	for (const Item& it : items) {
		if (it.focus == g_focus || !allowed(it.focus))
			continue;
		math::Vec d = it.pos.minus(from);
		float along = d.dot(dir);
		float ortho = std::fabs(d.x * dir.y - d.y * dir.x);
		if (along < 2.f)
			continue;
		if (cone && ortho > along * 1.8f)
			continue;
		float score = along + ortho * 2.f;
		if (score < best) {
			best = score;
			out = it.focus;
			found = true;
		}
	}
	return found;
}


static void moveFocus(math::Vec dir) {
	app::RackWidget* rack = rackWidget();
	widget::Widget* cur = widgetOf(g_focus);
	if (!rack || !cur)
		return;
	math::Vec from = cur->getRelativeOffset(cur->box.size.mult(0.5f), rack);
	std::vector<Item> own, others;
	for (app::ModuleWidget* mw : rack->getModules()) {
		if (!mw->module)
			continue;
		collect(mw, mw->module->id == g_focus.moduleId ? own : others);
	}
	Focus next;
	// Inside the module first; past its edge, on to whatever lies beyond; and
	// only then something in the module that is merely off to one side.
	if (pickToward(own, from, dir, true, next) ||
		pickToward(others, from, dir, true, next) ||
		pickToward(own, from, dir, false, next))
		setFocus(next);
}


static void stepModule(int delta) {
	app::RackWidget* rack = rackWidget();
	if (!rack)
		return;
	std::vector<app::ModuleWidget*> mods;
	for (app::ModuleWidget* mw : rack->getModules()) {
		if (mw->module)
			mods.push_back(mw);
	}
	if (mods.empty())
		return;
	std::sort(mods.begin(), mods.end(), [](app::ModuleWidget* a, app::ModuleWidget* b) {
		if (a->box.pos.y != b->box.pos.y)
			return a->box.pos.y < b->box.pos.y;
		return a->box.pos.x < b->box.pos.x;
	});
	int n = (int) mods.size();
	int at = 0;
	for (int i = 0; i < n; i++) {
		if (mods[i]->module->id == g_focus.moduleId)
			at = i;
	}
	// A module with nothing to focus (a blank, a notes panel) is stepped over.
	for (int tries = 0; tries < n; tries++) {
		at = ((at + delta) % n + n) % n;
		app::ModuleWidget* mw = mods[at];
		auto mem = g_remembered.find(mw->module->id);
		if (mem != g_remembered.end() && widgetOf(mem->second) && allowed(mem->second)) {
			setFocus(mem->second);
			return;
		}
		std::vector<Item> items;
		collect(mw, items);
		// Top-left control, knobs before jacks.
		const Item* first = NULL;
		for (const Item& it : items) {
			if (!allowed(it.focus))
				continue;
			bool better = !first
				|| (it.focus.kind == FOCUS_PARAM) > (first->focus.kind == FOCUS_PARAM)
				|| ((it.focus.kind == FOCUS_PARAM) == (first->focus.kind == FOCUS_PARAM)
					&& (it.pos.y < first->pos.y - 4.f
						|| (std::fabs(it.pos.y - first->pos.y) <= 4.f && it.pos.x < first->pos.x)));
			if (better)
				first = &it;
		}
		if (first) {
			setFocus(first->focus);
			return;
		}
	}
}


// --- actions ----------------------------------------------------------------

static bool isSwitch(app::ParamWidget* pw) {
	return dynamic_cast<app::Switch*>(pw) != NULL;
}


static void adjustFocused(int button) {
	app::ParamWidget* pw = paramOf(g_focus);
	engine::ParamQuantity* pq = pw ? pw->getParamQuantity() : NULL;
	if (!pq || !pq->isBounded())
		return;
	float range = pq->getMaxValue() - pq->getMinValue();
	float sign = (button == PAD_UP || button == PAD_RIGHT) ? 1.f : -1.f;
	bool coarse = button == PAD_UP || button == PAD_DOWN;
	float step;
	if (pq->snapEnabled)
		step = coarse ? std::fmax(1.f, std::round(range / 8.f)) : 1.f;
	else
		step = coarse ? range / 20.f : range / 200.f;
	float base = pq->snapEnabled ? std::round(pq->getValue()) : pq->getValue();
	setParam(pq, base + sign * step);
}


static void pressSwitch(app::ParamWidget* pw) {
	engine::ParamQuantity* pq = pw->getParamQuantity();
	app::Switch* sw = dynamic_cast<app::Switch*>(pw);
	if (!pq || !sw)
		return;
	if (sw->momentary) {
		pq->setMax();
		g_momentary = true;
		return;
	}
	float oldValue = pq->getValue();
	if (pq->isMax())
		pq->setMin();
	else
		pq->setValue(std::round(pq->getValue()) + 1.f);
	float newValue = pq->getValue();
	if (oldValue == newValue)
		return;
	history::ParamChange* h = new history::ParamChange;
	h->name = "move switch";
	h->moduleId = g_focus.moduleId;
	h->paramId = g_focus.id;
	h->oldValue = oldValue;
	h->newValue = newValue;
	APP->history->push(h);
}


static void connectCable(const Focus& a, const Focus& b) {
	app::PortWidget* pa = portOf(a);
	app::PortWidget* pb = portOf(b);
	if (!pa || !pb || pa->type == pb->type)
		return;
	app::PortWidget* in = pa->type == engine::Port::INPUT ? pa : pb;
	app::PortWidget* out = pa->type == engine::Port::INPUT ? pb : pa;
	if (rackWidget()->getCable(out, in))
		return;
	app::CableWidget* cw = new app::CableWidget;
	cw->color = rackWidget()->getNextCableColor();
	cw->inputPort = in;
	cw->outputPort = out;
	cw->updateCable();
	if (!cw->isComplete()) {
		delete cw;
		return;
	}
	rackWidget()->addCable(cw);
	history::CableAdd* h = new history::CableAdd;
	h->setCable(cw);
	APP->history->push(h);
}


static void unplug(app::PortWidget* port) {
	std::vector<app::CableWidget*> cables = rackWidget()->getCompleteCablesOnPort(port);
	if (cables.empty())
		return;
	history::ComplexAction* all = new history::ComplexAction;
	all->name = "remove cables";
	for (app::CableWidget* cw : cables) {
		history::CableRemove* h = new history::CableRemove;
		h->setCable(cw);
		all->push(h);
		rackWidget()->removeCable(cw);
		delete cw;
	}
	APP->history->push(all);
}


static void pressA() {
	if (!ensureFocus())
		return;
	if (g_focus.kind == FOCUS_PARAM) {
		app::ParamWidget* pw = paramOf(g_focus);
		if (isSwitch(pw)) {
			pressSwitch(pw);
			return;
		}
		g_editing = !g_editing;
		if (!g_editing)
			closeHistory();
		return;
	}
	if (!g_cabling) {
		g_cabling = true;
		g_cableFrom = g_focus;
		return;
	}
	if (!(g_focus == g_cableFrom))
		connectCable(g_cableFrom, g_focus);
	g_cabling = false;
}


static void pressB() {
	if (g_cabling) {
		g_cabling = false;
		if (widgetOf(g_cableFrom))
			setFocus(g_cableFrom);
		return;
	}
	if (g_editing) {
		g_editing = false;
		closeHistory();
		return;
	}
	// Nothing left to back out of: the ring goes away. The focus itself is
	// kept, so the D-pad brings it back where it was.
	releaseMomentary();
	closeHistory();
	g_dirHeld = -1;
	g_follow = false;
	g_active = false;
}


static void pressY() {
	if (!ensureFocus())
		return;
	if (g_focus.isPort()) {
		// Not while a cable is being run from here: B cancels that.
		app::PortWidget* port = g_cabling ? NULL : portOf(g_focus);
		if (port)
			unplug(port);
		return;
	}
	app::ParamWidget* pw = paramOf(g_focus);
	if (!pw)
		return;
	closeHistory();
	pw->resetAction();
}


static void cycleZoom() {
	app::RackScrollWidget* rs = APP->scene ? APP->scene->rackScroll : NULL;
	if (!rs)
		return;
	float zoom = rs->getZoom();
	int n = (int) (sizeof(ZOOM_LEVELS) / sizeof(ZOOM_LEVELS[0]));
	int at = 0;
	float best = INFINITY;
	for (int i = 0; i < n; i++) {
		float d = std::fabs(std::log2(zoom) - std::log2(ZOOM_LEVELS[i]));
		if (d < best) {
			best = d;
			at = i;
		}
	}
	rs->setZoom(ZOOM_LEVELS[(at + 1) % n]);
	g_follow = true;
}


static void pressDirection(int button) {
	if (!ensureFocus())
		return;
	if (g_editing && g_focus.kind == FOCUS_PARAM) {
		adjustFocused(button);
		return;
	}
	switch (button) {
		case PAD_UP: moveFocus(math::Vec(0.f, -1.f)); break;
		case PAD_DOWN: moveFocus(math::Vec(0.f, 1.f)); break;
		case PAD_LEFT: moveFocus(math::Vec(-1.f, 0.f)); break;
		case PAD_RIGHT: moveFocus(math::Vec(1.f, 0.f)); break;
		default: break;
	}
}


void gamepadNavButton(int button, bool down) {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	windowNoteInteraction();
	bool isDir = button == PAD_UP || button == PAD_DOWN || button == PAD_LEFT || button == PAD_RIGHT;

	if (!down) {
		if (isDir && g_dirHeld == button)
			g_dirHeld = -1;
		if (button == PAD_A)
			releaseMomentary();
		return;
	}

	// Buttons that have nothing to do with the focus work whether or not the
	// ring is showing, and do not bring it out.
	if (button == PAD_R3) {
		g_playMode = !g_playMode;
		g_modeChanged = system::getTime();
		LOGI("play mode %s", g_playMode ? "on" : "off");
		return;
	}
	if (button == PAD_SELECT) {
		cycleZoom();
		return;
	}

	if (!g_active) {
		// The ring is put away. B leaves it so; any other button brings it
		// back, and does only that -- acting on a control nobody can see
		// would be a surprise.
		if (button == PAD_B)
			return;
		g_active = true;
		if (isDir) {
			g_dirHeld = button;
			g_repeatAt = system::getTime() + REPEAT_DELAY;
		}
		ensureFocus();
		g_follow = true;
		return;
	}

	if (isDir) {
		g_dirHeld = button;
		g_repeatAt = system::getTime() + REPEAT_DELAY;
		pressDirection(button);
		return;
	}
	switch (button) {
		case PAD_A: pressA(); break;
		case PAD_B: pressB(); break;
		case PAD_Y: pressY(); break;
		case PAD_L1: if (ensureFocus()) stepModule(-1); break;
		case PAD_R1: if (ensureFocus()) stepModule(1); break;
		default: break;
	}
}


bool gamepadPlayMode() {
	return g_playMode;
}


void gamepadNavNoteTouch() {
	if (!g_active)
		return;
	g_active = false;
	g_dirHeld = -1;
	releaseMomentary();
	closeHistory();
	g_editing = false;
	g_cabling = false;
	g_follow = false;
}


// --- per frame --------------------------------------------------------------

static float shapeStick(float v) {
	float a = std::fabs(v);
	if (a < STICK_DEADZONE)
		return 0.f;
	float t = (a - STICK_DEADZONE) / (1.f - STICK_DEADZONE);
	return std::copysign(t * t, v);
}


static void turnWithStick(float dt) {
	// In play mode the right stick belongs to the patch.
	if (g_playMode || !g_active || g_focus.kind != FOCUS_PARAM)
		return;
	// Up and right turn the knob up; sideways is a tenth as fast, for trimming.
	float amount = -shapeStick(gamepadAxis(PAD_RY)) * 0.55f + shapeStick(gamepadAxis(PAD_RX)) * 0.055f;
	if (amount == 0.f) {
		g_snapAcc = 0.f;
		return;
	}
	app::ParamWidget* pw = paramOf(g_focus);
	engine::ParamQuantity* pq = pw ? pw->getParamQuantity() : NULL;
	if (!pq || !pq->isBounded())
		return;
	// A momentary button has nothing to turn.
	app::Switch* sw = dynamic_cast<app::Switch*>(pw);
	if (sw && sw->momentary)
		return;
	windowNoteInteraction();
	float range = pq->getMaxValue() - pq->getMinValue();
	if (pq->snapEnabled) {
		// A few steps a second at full tilt however many positions there are:
		// a three-way switch should not flick end to end.
		g_snapAcc += amount * dt * std::fmax(range * 1.2f, 6.f);
		float whole = std::trunc(g_snapAcc);
		if (whole != 0.f) {
			g_snapAcc -= whole;
			setParam(pq, std::round(pq->getValue()) + whole);
		}
		return;
	}
	setParam(pq, pq->getValue() + amount * dt * range);
}


static float shapeTrigger(float v) {
	if (v < TRIGGER_DEADZONE)
		return 0.f;
	return (v - TRIGGER_DEADZONE) / (1.f - TRIGGER_DEADZONE);
}


/** The left stick pans the rack and the triggers zoom it, so the view can be
moved without a finger -- which on a small screen grabs a module or a knob as
often as it grabs the background. */
static void panAndZoom(float dt) {
	app::RackScrollWidget* rs = APP->scene->rackScroll;
	if (!rs)
		return;
	math::Vec pan(shapeStick(gamepadAxis(PAD_LX)), shapeStick(gamepadAxis(PAD_LY)));
	if (pan.x != 0.f || pan.y != 0.f) {
		// A screen and a half a second at full tilt, at any zoom.
		float speed = std::fmax(rs->box.size.x, rs->box.size.y) * 1.5f;
		rs->offset = rs->offset.plus(pan.mult(speed * dt));
		g_follow = false;
		g_viewMoved = true;
		windowNoteInteraction();
	}
	float pull = shapeTrigger(gamepadAxis(PAD_R2)) - shapeTrigger(gamepadAxis(PAD_L2));
	if (pull != 0.f) {
		float zoom = rs->getZoom();
		// Two octaves end to end (0.5x to 2x) in a little over a second.
		float next = math::clamp(zoom * std::pow(2.f, pull * 1.6f * dt), 0.25f, MAX_RACK_ZOOM);
		if (next != zoom) {
			rs->setZoom(next);
			touchNoteZooming();
			g_follow = false;
			g_viewMoved = true;
		}
		windowNoteInteraction();
	}
}


static void followFocus(float dt) {
	if (!g_follow || !g_active)
		return;
	widget::Widget* w = widgetOf(g_focus);
	app::RackScrollWidget* rs = APP->scene->rackScroll;
	if (!w || !rs) {
		g_follow = false;
		return;
	}
	math::Vec p = w->getAbsoluteOffset(w->box.size.mult(0.5f));
	math::Rect view = clearView();
	float left = view.pos.x + FOLLOW_MARGIN;
	float right = view.pos.x + view.size.x - FOLLOW_MARGIN;
	float top = view.pos.y + FOLLOW_MARGIN;
	float bottom = view.pos.y + view.size.y - FOLLOW_MARGIN;
	if (bottom < top) {
		// Hardly any room between the toolbar and the palette: aim for the
		// middle of what there is.
		top = bottom = (top + bottom) * 0.5f;
	}
	math::Vec d(0.f, 0.f);
	if (p.x < left)
		d.x = p.x - left;
	else if (p.x > right)
		d.x = p.x - right;
	if (p.y < top)
		d.y = p.y - top;
	else if (p.y > bottom)
		d.y = p.y - bottom;
	if (std::fabs(d.x) < 0.5f && std::fabs(d.y) < 0.5f) {
		g_follow = false;
		return;
	}
	float k = 1.f - std::exp(-dt * 14.f);
	rs->offset = rs->offset.plus(d.mult(k));
	windowNoteInteraction();
}


void gamepadNavStep() {
	if (!APP || !APP->scene || !APP->scene->rack)
		return;
	double now = system::getTime();
	float dt = g_lastStep > 0.0 ? (float) (now - g_lastStep) : 0.f;
	g_lastStep = now;
	if (dt > 0.1f)
		dt = 0.1f;

	// The focused control can vanish under us: its module deleted, another
	// patch loaded.
	if (g_focus.kind != FOCUS_NONE && !widgetOf(g_focus)) {
		g_focus = Focus();
		g_editing = false;
		g_momentary = false;
		g_histOpen = false;
	}
	if (g_cabling && !widgetOf(g_cableFrom))
		g_cabling = false;

	if (g_dirHeld >= 0 && now >= g_repeatAt) {
		bool editing = g_editing && g_focus.kind == FOCUS_PARAM;
		g_repeatAt = now + (editing ? REPEAT_EDIT : REPEAT_MOVE);
		pressDirection(g_dirHeld);
		windowNoteInteraction();
	}

	turnWithStick(dt);
	panAndZoom(dt);

	// The stick leaves no "done" behind it: an edit it made becomes an undo
	// step once the knob has been left alone for a moment.
	if (g_histOpen && !g_editing && now - g_histTouched > 0.6)
		closeHistory();

	followFocus(dt);
}


// --- drawing ----------------------------------------------------------------

static const NVGcolor RING = nvgRGB(0xFF, 0xDA, 0x9F);
static const NVGcolor RING_EDIT = nvgRGB(0xFF, 0x8A, 0x3D);
static const NVGcolor RING_CABLE = nvgRGB(0x6F, 0xD6, 0xFF);


static std::string describe(const Focus& f) {
	app::RackWidget* rack = rackWidget();
	app::ModuleWidget* mw = rack ? rack->getModule(f.moduleId) : NULL;
	std::string text = mw && mw->model ? mw->model->name : "";
	if (f.kind == FOCUS_PARAM) {
		app::ParamWidget* pw = paramOf(f);
		engine::ParamQuantity* pq = pw ? pw->getParamQuantity() : NULL;
		if (pq) {
			text += "  -  " + pq->getLabel();
			std::string value = pq->getDisplayValueString() + pq->getUnit();
			if (!value.empty())
				text += "   " + value;
		}
	}
	else if (f.isPort()) {
		app::PortWidget* port = portOf(f);
		engine::PortInfo* info = port ? port->getPortInfo() : NULL;
		text += f.kind == FOCUS_INPUT ? "  -  IN " : "  -  OUT ";
		if (info)
			text += info->name;
		if (port) {
			size_t n = rack->getCompleteCablesOnPort(port).size();
			if (n > 0)
				text += "   (" + std::to_string(n) + (n == 1 ? " cable)" : " cables)");
		}
	}
	return text;
}


struct GamepadOverlay : widget::Widget {
	void step() override {
		if (APP->scene)
			box.size = APP->scene->box.size;
		widget::Widget::step();
	}

	void drawRing(const DrawArgs& args, widget::Widget* w, NVGcolor col, float width, bool fill) {
		math::Vec o = w->getAbsoluteOffset(math::Vec(0.f, 0.f));
		math::Vec e = w->getAbsoluteOffset(w->box.size);
		float bw = e.x - o.x, bh = e.y - o.y;
		if (bw <= 0.f || bh <= 0.f)
			return;
		float pad = std::fmax(3.f, std::fmin(bw, bh) * 0.12f);
		float r = std::fmin(bw, bh) * 0.5f + pad;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, o.x - pad, o.y - pad, bw + pad * 2.f, bh + pad * 2.f, r);
		if (fill) {
			nvgFillColor(args.vg, nvgTransRGBA(col, 0x38));
			nvgFill(args.vg);
		}
		// A dark line under the bright one: the ring has to read on cream
		// panels as well as on black ones.
		nvgStrokeColor(args.vg, nvgRGBA(0, 0, 0, 0xB0));
		nvgStrokeWidth(args.vg, width + 2.f);
		nvgStroke(args.vg);
		nvgStrokeColor(args.vg, col);
		nvgStrokeWidth(args.vg, width);
		nvgStroke(args.vg);
	}

	void drawStrip(const DrawArgs& args, const std::string& text, NVGcolor accent) {
		if (text.empty() || !APP->window->uiFont)
			return;
		nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, 15.f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		float bounds[4];
		nvgTextBounds(args.vg, 0.f, 0.f, text.c_str(), NULL, bounds);
		float tw = bounds[2] - bounds[0];
		float w = std::fmin(tw + 28.f, box.size.x - 16.f);
		float h = 26.f;
		float x = (box.size.x - w) * 0.5f;
		float y = box.size.y - BOTTOM_INSET + 4.f;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, x, y, w, h, h * 0.5f);
		nvgFillColor(args.vg, nvgRGBA(0x1C, 0x18, 0x13, 0xEC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, accent);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		nvgScissor(args.vg, x + 8.f, y, w - 16.f, h);
		nvgFillColor(args.vg, nvgRGB(0xF4, 0xEC, 0xDC));
		nvgText(args.vg, box.size.x * 0.5f, y + h * 0.5f, text.c_str(), NULL);
		nvgResetScissor(args.vg);
	}

	/** Which mode the right stick is in: said in full for a moment after R3,
	then kept as a small tag for as long as play mode lasts, since a stick that
	has stopped turning knobs needs explaining. */
	void drawMode(const DrawArgs& args) {
		bool fresh = system::getTime() - g_modeChanged < 2.5;
		if (!fresh && !g_playMode)
			return;
		if (!APP->window->uiFont)
			return;
		std::string text = !fresh ? "PLAY"
			: g_playMode ? "PLAY:  right stick = CV"
			: "EDIT:  right stick turns the knob";
		NVGcolor accent = g_playMode ? nvgRGB(0x8D, 0xE0, 0x7A) : RING;
		nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, 13.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		float bounds[4];
		nvgTextBounds(args.vg, 0.f, 0.f, text.c_str(), NULL, bounds);
		float w = bounds[2] - bounds[0] + 20.f;
		float h = 22.f;
		float x = box.size.x - w - 10.f;
		float y = std::fmax(cableParkTopInset(), 0.f) + 8.f;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, x, y, w, h, h * 0.5f);
		nvgFillColor(args.vg, nvgRGBA(0x1C, 0x18, 0x13, 0xEC));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, accent);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		nvgFillColor(args.vg, accent);
		nvgText(args.vg, x + 10.f, y + h * 0.5f, text.c_str(), NULL);
	}

	void draw(const DrawArgs& args) override {
		drawMode(args);
		widget::Widget* w = g_active ? widgetOf(g_focus) : NULL;
		if (!w) {
			widget::Widget::draw(args);
			return;
		}
		NVGcolor col = g_cabling ? RING_CABLE : (g_editing || g_momentary) ? RING_EDIT : RING;
		if (g_cabling) {
			widget::Widget* from = widgetOf(g_cableFrom);
			if (from) {
				drawRing(args, from, RING_CABLE, 2.f, true);
				if (from != w) {
					math::Vec a = from->getAbsoluteOffset(from->box.size.mult(0.5f));
					math::Vec b = w->getAbsoluteOffset(w->box.size.mult(0.5f));
					nvgBeginPath(args.vg);
					nvgMoveTo(args.vg, a.x, a.y);
					nvgLineTo(args.vg, b.x, b.y);
					nvgStrokeColor(args.vg, nvgTransRGBA(RING_CABLE, 0xC0));
					nvgStrokeWidth(args.vg, 2.5f);
					nvgStroke(args.vg);
				}
			}
		}
		drawRing(args, w, col, g_editing ? 3.f : 2.2f, g_editing || g_momentary);

		std::string text = describe(g_focus);
		if (g_cabling)
			text = "CABLE:  " + text;
		else if (g_editing)
			text = "EDIT:  " + text;
		drawStrip(args, text, col);
		widget::Widget::draw(args);
	}
};


static GamepadOverlay* g_overlay = NULL;


void installGamepadNav() {
	if (g_overlay || !APP->scene)
		return;
	g_overlay = new GamepadOverlay;
	g_overlay->box.pos = math::Vec(0.f, 0.f);
	g_overlay->box.size = APP->scene->box.size;
	APP->scene->addChild(g_overlay);
}


} // namespace rackdroid
