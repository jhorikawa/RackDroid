// The part of nova_kit.hpp that does not depend on the module. GPL-3.0-or-later.

#pragma once

#include "nova_kit.hpp"

#ifdef NOVA_SPLIT
#define NOVA_INLINE
#else
#define NOVA_INLINE inline
#endif


namespace nova {


NOVA_INLINE float engineRate() {
	Context* c = contextGet();
	return c && c->engine && c->engine->getSampleRate() > 0.f ? c->engine->getSampleRate() : 48000.f;
}


/* ---- The panel, laid out ---- */

struct Placed {
	Item item;
	math::Vec at;
	/** Half the height of the part, for the label above it. */
	float radius;
	float cell;
	std::string text;
};

struct Plan {
	int width = 0;
	std::vector<Placed> items;
	std::vector<math::Rect> displays;
	/** What is wrong with the panel, for whoever wrote it. */
	std::vector<std::string> problems;
};

const float TOP = 34.f;
const float BOTTOM = 366.f;
const float LABEL = 12.f;

NOVA_INLINE bool isButton(const engine::Module* m, int param) {
	const engine::ParamQuantity* pq = m->paramQuantities[param];
	const engine::SwitchQuantity* sq = dynamic_cast<const engine::SwitchQuantity*>(pq);
	return sq && sq->labels.empty() && sq->minValue == 0.f && sq->maxValue == 1.f;
}

/** The positions of a switch, 0 if the param is not one. */
NOVA_INLINE int positions(const engine::Module* m, int param) {
	const engine::ParamQuantity* pq = m->paramQuantities[param];
	const engine::SwitchQuantity* sq = dynamic_cast<const engine::SwitchQuantity*>(pq);
	if (!sq || isButton(m, param))
		return 0;
	return (int) std::lround(sq->maxValue - sq->minValue) + 1;
}

NOVA_INLINE float diameter(const engine::Module* m, const Item& i) {
	switch (i.kind) {
		case Item::KNOB:
			if (isButton(m, i.id))
				return 18.f;
			if (positions(m, i.id) == 2)
				return 21.f;
			if (positions(m, i.id) == 3)
				return 28.f;
			return 29.f;
		case Item::BIG: return 45.f;
		case Item::SMALL: return 23.f;
		case Item::TRIM: return 18.f;
		case Item::LIGHT: return 9.f;
		case Item::GAP: return 1.f;
		default: return 24.f;
	}
}

NOVA_INLINE std::string nameOf(const engine::Module* m, const Item& i) {
	if (!i.label.empty())
		return i.label;
	switch (i.kind) {
		case Item::IN: return m->inputInfos[i.id]->name;
		case Item::OUT: return m->outputInfos[i.id]->name;
		case Item::LIGHT: return m->lightInfos[i.id] ? m->lightInfos[i.id]->name : "";
		case Item::GAP: return "";
		default: return m->paramQuantities[i.id]->name;
	}
}

NOVA_INLINE float widthOf(const std::string& label) {
	return 4.7f * (float) label.size();
}

NOVA_INLINE Plan plan(const Module* m) {
	Plan p;
	std::vector<Panel::Row> rows = m->panel.rows;
	std::vector<int> used[4];
	auto kindOf = [](Item::Kind k) { return k == Item::IN ? 1 : k == Item::OUT ? 2 : k == Item::LIGHT ? 3 : 0; };
	int counts[4] = {(int) m->params.size(), (int) m->inputs.size(), (int) m->outputs.size(), (int) m->lights.size()};
	const char* words[4] = {"param", "input", "output", "light"};
	for (int k = 0; k < 4; k++)
		used[k].assign(counts[k], 0);
	for (Panel::Row& r : rows) {
		std::vector<Item> good;
		for (const Item& i : r.items) {
			int k = kindOf(i.kind);
			if (i.kind == Item::GAP) {
				good.push_back(i);
			}
			else if (i.id < 0 || i.id >= counts[k]) {
				p.problems.push_back(string::f("the panel names %s %d, but there are %d", words[k], i.id, counts[k]));
			}
			else if (used[k][i.id]++) {
				p.problems.push_back(string::f("the panel has %s %d more than once", words[k], i.id));
			}
			else {
				good.push_back(i);
			}
		}
		r.items = good;
	}
	// What the panel does not mention goes below, so that nothing is out of reach.
	std::vector<Item> rest;
	auto spill = [&](size_t perRow, bool all) {
		if (rest.size() >= perRow || (all && !rest.empty())) {
			rows.push_back({rest, 0.f});
			rest.clear();
		}
	};
	for (int i = 0; i < counts[0]; i++) {
		if (!used[0][i]) {
			bool onlyFew = m->panel.rows.empty() && counts[0] <= 3 && i == 0 && !isButton(m, i) && !positions(m, i);
			rest.push_back(onlyFew ? big(i) : knob(i));
			spill(onlyFew ? 1 : counts[0] > 6 ? 3 : 2, false);
		}
	}
	spill(0, true);
	for (int i = 0; i < counts[3]; i++) {
		if (!used[3][i])
			rest.push_back(light(i));
		spill(6, false);
	}
	spill(0, true);
	for (int i = 0; i < counts[1]; i++) {
		if (!used[1][i])
			rest.push_back(in(i));
		spill(4, false);
	}
	if (rest.size() + counts[2] > 4)
		spill(0, true);
	for (int i = 0; i < counts[2]; i++) {
		if (!used[2][i])
			rest.push_back(out(i));
		spill(4, false);
	}
	spill(0, true);

	float needWidth = 30.f;
	float needHeight = 0.f;
	std::vector<float> heights;
	for (size_t r = 0; r < rows.size(); r++) {
		if (rows[r].display > 0.f) {
			heights.push_back(rows[r].display + 6.f);
			needHeight += heights.back();
			needWidth = std::fmax(needWidth, 75.f);
			continue;
		}
		float w = 0.f;
		float h = 0.f;
		for (const Item& i : rows[r].items) {
			float d = diameter(m, i);
			w += std::fmax(d + 7.f, std::fmin(widthOf(nameOf(m, i)) + 4.f, 46.f));
			h = std::fmax(h, d + LABEL + 3.f);
		}
		heights.push_back(h);
		needHeight += h;
		needWidth = std::fmax(needWidth, w + 8.f);
	}
	int needHp = (int) std::ceil(needWidth / RACK_GRID_WIDTH);
	p.width = m->panel.width > 0 ? m->panel.width : std::max(needHp, 4);
	if (p.width < needHp)
		p.problems.push_back(string::f("the panel is %d HP wide, but its widest row needs %d", p.width, needHp));
	if (p.width > 60)
		p.problems.push_back(string::f("the panel is %d HP wide: that is too much for a small screen, 60 at the most", p.width));
	float room = BOTTOM - TOP;
	if (needHeight > room)
		p.problems.push_back(string::f("the rows of the panel need %d px, but there are %d: use fewer rows, and more in each",
			(int) std::ceil(needHeight), (int) room));
	float space = rows.empty() ? 0.f : std::fmax(0.f, (room - needHeight) / (float) rows.size());
	float y = TOP + space / 2.f;
	float panelWidth = p.width * RACK_GRID_WIDTH;
	for (size_t r = 0; r < rows.size(); r++) {
		if (rows[r].display > 0.f) {
			p.displays.push_back(math::Rect(5.f, y + 3.f, panelWidth - 10.f, rows[r].display));
		}
		else if (!rows[r].items.empty()) {
			// What a row does not need is shared out between its parts.
			std::vector<float> cells;
			float total = 0.f;
			for (const Item& i : rows[r].items) {
				cells.push_back(std::fmax(diameter(m, i) + 7.f, std::fmin(widthOf(nameOf(m, i)) + 4.f, 46.f)));
				total += cells.back();
			}
			float more = (panelWidth - 8.f - total) / (float) cells.size();
			float x = 4.f;
			for (size_t i = 0; i < cells.size(); i++) {
				const Item& item = rows[r].items[i];
				float cell = cells[i] + more;
				float d = diameter(m, item);
				// On one line, whatever their sizes.
				math::Vec at(x + cell / 2.f, y + LABEL + (heights[r] - LABEL - 3.f) / 2.f);
				p.items.push_back({item, at, d / 2.f, cell, nameOf(m, item)});
				x += cell;
			}
		}
		y += heights[r] + space;
	}
	return p;
}


/* ---- What a module is, for the test and for the app ---- */

NOVA_INLINE std::string jsonText(json_t* j) {
	char* dump = json_dumps(j, JSON_COMPACT);
	std::string s = dump ? dump : "{}";
	std::free(dump);
	json_decref(j);
	return s;
}

NOVA_INLINE std::string describe(Module* m) {
	json_t* root = json_object();
	std::vector<std::string> notes;
	json_t* params = json_array();
	for (size_t i = 0; i < m->params.size(); i++) {
		engine::ParamQuantity* pq = m->paramQuantities[i];
		json_t* j = json_object();
		json_object_set_new(j, "name", json_string(pq->name.c_str()));
		json_object_set_new(j, "min", json_real(pq->minValue));
		json_object_set_new(j, "max", json_real(pq->maxValue));
		json_object_set_new(j, "default", json_real(pq->defaultValue));
		json_array_append_new(params, j);
		if (pq->name.empty())
			notes.push_back(string::f("param %d has no name: configParam(), configSwitch() or configButton() it", (int) i));
		if (!(pq->minValue < pq->maxValue) || pq->defaultValue < pq->minValue || pq->defaultValue > pq->maxValue)
			notes.push_back(string::f("param %d \"%s\" has a range of %g to %g and starts at %g", (int) i, pq->name.c_str(),
				pq->minValue, pq->maxValue, pq->defaultValue));
	}
	json_object_set_new(root, "params", params);
	for (int k = 0; k < 2; k++) {
		json_t* ports = json_array();
		const std::vector<engine::PortInfo*>& infos = k ? m->outputInfos : m->inputInfos;
		for (size_t i = 0; i < infos.size(); i++) {
			json_array_append_new(ports, json_string(infos[i]->name.c_str()));
			if (infos[i]->name.empty())
				notes.push_back(string::f("%s %d has no name", k ? "output" : "input", (int) i));
		}
		json_object_set_new(root, k ? "outputs" : "inputs", ports);
	}
	json_object_set_new(root, "lights", json_integer(m->lights.size()));
	json_object_set_new(root, "menu", json_integer(m->menu.size()));
	Plan p = plan(m);
	json_object_set_new(root, "width", json_integer(p.width));
	json_object_set_new(root, "displays", json_integer(p.displays.size()));
	// For a picture of it (MadeModules.kt).
	auto hex = [](NVGcolor c) {
		return json_string(string::f("#%02x%02x%02x", (int) (math::clamp(c.r, 0.f, 1.f) * 255.f), (int) (math::clamp(c.g, 0.f, 1.f) * 255.f),
			(int) (math::clamp(c.b, 0.f, 1.f) * 255.f)).c_str());
	};
	json_t* panel = json_object();
	json_object_set_new(panel, "title", json_string(m->panel.title.c_str()));
	json_object_set_new(panel, "color", hex(m->panel.color));
	json_object_set_new(panel, "accent", hex(m->panel.accent));
	json_t* parts = json_array();
	for (const Placed& i : p.items) {
		const char* kind = i.item.kind == Item::IN ? "in" : i.item.kind == Item::OUT ? "out" : i.item.kind == Item::LIGHT ? "light"
			: i.item.kind == Item::GAP ? "gap" : isButton(m, i.item.id) ? "button" : positions(m, i.item.id) == 2 || positions(m, i.item.id) == 3 ? "switch" : "knob";
		json_array_append_new(parts, json_pack("[s, f, f, f, s]", kind, i.at.x, i.at.y, i.radius, i.text.c_str()));
	}
	json_object_set_new(panel, "parts", parts);
	json_t* displays = json_array();
	for (const math::Rect& r : p.displays)
		json_array_append_new(displays, json_pack("[f, f, f, f]", r.pos.x, r.pos.y, r.size.x, r.size.y));
	json_object_set_new(panel, "displays", displays);
	json_object_set_new(root, "panel", panel);
	notes.insert(notes.end(), p.problems.begin(), p.problems.end());
	json_t* notesJ = json_array();
	for (const std::string& s : notes)
		json_array_append_new(notesJ, json_string(s.c_str()));
	json_object_set_new(root, "problems", notesJ);
	return jsonText(root);
}


#ifndef NOVA_TEST

/* ---- The panel, drawn ---- */

/** In the width there is, smaller if it must be. */
NOVA_INLINE void drawLabel(NVGcontext* vg, math::Vec at, const std::string& text, float width, float size, NVGcolor color) {
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
	nvgFillColor(vg, color);
	nvgFontSize(vg, size);
	std::string s = text;
	float w = nvgTextBounds(vg, 0, 0, s.c_str(), NULL, NULL);
	if (w > width) {
		nvgFontSize(vg, std::fmax(size * width / w, size * 0.72f));
		while (s.size() > 2 && nvgTextBounds(vg, 0, 0, s.c_str(), NULL, NULL) > width)
			s.pop_back();
	}
	nvgText(vg, at.x, at.y, s.c_str(), NULL);
}


struct Face : widget::Widget {
	Plan plan;
	Panel panel;
	/** A picture of the parts too, for a panel with none on it. */
	bool parts = false;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float w = box.size.x;
		float h = box.size.y;
		nvgBeginPath(vg);
		nvgRect(vg, 0, 0, w, h);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, h, nvgLerpRGBA(panel.color, nvgRGB(255, 255, 255), 0.08f), panel.color));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgRect(vg, 0.5f, 0.5f, w - 1.f, h - 1.f);
		nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 34));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgRect(vg, 8.f, 26.f, w - 16.f, 2.f);
		nvgFillColor(vg, panel.accent);
		nvgFill(vg);

		if (APP->window && APP->window->uiFont)
			nvgFontFaceId(vg, APP->window->uiFont->handle);
		bool light = 0.3f * panel.color.r + 0.59f * panel.color.g + 0.11f * panel.color.b > 0.55f;
		NVGcolor ink = light ? nvgRGB(0x14, 0x18, 0x1e) : nvgRGB(0xf2, 0xf5, 0xf8);
		drawLabel(vg, math::Vec(w / 2.f, 20.f), panel.title, w - 12.f, 15.f, ink);

		for (const Placed& p : plan.items) {
			if (p.item.kind != Item::OUT)
				continue;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, p.at.x - 17.f, p.at.y - p.radius - LABEL - 1.f, 34.f, p.radius * 2.f + LABEL + 4.f, 4.f);
			nvgFillColor(vg, light ? nvgRGBA(0, 0, 0, 200) : nvgRGBA(255, 255, 255, 224));
			nvgFill(vg);
		}
		for (const Placed& p : plan.items) {
			bool isOut = p.item.kind == Item::OUT;
			NVGcolor c = isOut ? (light ? nvgRGB(0xff, 0xff, 0xff) : nvgRGB(0x14, 0x18, 0x1e)) : ink;
			drawLabel(vg, math::Vec(p.at.x, p.at.y - p.radius - 3.f), p.text, isOut ? 32.f : p.cell - 1.f, 10.5f, c);
		}
		for (const math::Rect& r : plan.displays) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 4.f);
			nvgFillColor(vg, nvgRGB(0x0a, 0x0d, 0x12));
			nvgFill(vg);
		}
		Widget::draw(args);
	}
};


struct Screen : widget::Widget {
	Module* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module || module->stopped)
			return;
		nvgSave(args.vg);
		nvgIntersectScissor(args.vg, 0, 0, box.size.x, box.size.y);
		if (APP->window && APP->window->uiFont)
			nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		if (!guarded(module, DRAW, [&] { module->draw(args.vg, box.size); }))
			module->stop();
		nvgRestore(args.vg);
	}
};


struct Cover : widget::Widget {
	Module* module = nullptr;

	void draw(const DrawArgs& args) override {
		if (!module || !module->stopped)
			return;
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGBA(60, 0, 0, 150));
		nvgFill(args.vg);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, box.size.y / 2.f - 14.f, box.size.x, 28.f);
		nvgFillColor(args.vg, nvgRGB(0xc6, 0x28, 0x28));
		nvgFill(args.vg);
		if (APP->window && APP->window->uiFont)
			nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		drawLabel(args.vg, math::Vec(box.size.x / 2.f, box.size.y / 2.f + 5.f), "STOPPED", box.size.x - 4.f, 15.f, nvgRGB(255, 255, 255));
	}
};


struct ValueSlider : ui::Slider {
	struct Value : Quantity {
		MenuEntry entry;
		void setValue(float v) override { *entry.value = math::clamp(v, entry.min, entry.max); }
		float getValue() override { return *entry.value; }
		float getMinValue() override { return entry.min; }
		float getMaxValue() override { return entry.max; }
		float getDefaultValue() override { return entry.standard; }
		std::string getLabel() override { return entry.label; }
		std::string getUnit() override { return entry.unit; }
	};
	ValueSlider(const MenuEntry& e) {
		Value* v = new Value;
		v->entry = e;
		quantity = v;
		box.size.x = 220.f;
	}
	~ValueSlider() { delete quantity; }
};


struct Widget : app::ModuleWidget {
	Widget(Module* m, Module* preview, const std::string& slug) {
		setModule(m);
		// A panel in the list of modules has no module behind it.
		Module* t = m ? m : preview;
		Plan p = plan(t);
		box.size = math::Vec(p.width * RACK_GRID_WIDTH, RACK_GRID_HEIGHT);

		widget::FramebufferWidget* fb = new widget::FramebufferWidget;
		fb->box.size = box.size;
		Face* face = new Face;
		face->box.size = box.size;
		face->plan = p;
		face->panel = t->panel;
		if (face->panel.title.empty())
			face->panel.title = string::uppercase(slug);
		fb->addChild(face);
		addChild(fb);

		for (const math::Rect& r : p.displays) {
			Screen* s = new Screen;
			s->box = r;
			s->module = t;
			addChild(s);
		}
		for (const Placed& i : p.items)
			addPart(t, m, i);

		Cover* cover = new Cover;
		cover->box.size = box.size;
		cover->module = m;
		addChild(cover);
	}

	/** As Rack's, with the module's own destructor guarded too. */
	~Widget() {
		clearChildren();
		engine::Module* m = module;
		if (!m)
			return;
		APP->engine->removeModule(m);
		module = NULL;
		guarded(m, REMOVE, [&] { delete m; });
	}

	void addPart(Module* t, Module* m, const Placed& i) {
		int id = i.item.id;
		switch (i.item.kind) {
			case Item::IN:
				addInput(createInputCentered<PJ301MPort>(i.at, m, id));
				return;
			case Item::OUT:
				addOutput(createOutputCentered<PJ301MPort>(i.at, m, id));
				return;
			case Item::LIGHT:
				addChild(createLightCentered<MediumLight<GreenLight>>(i.at, m, id));
				return;
			case Item::GAP:
				return;
			case Item::BIG:
				addParam(createParamCentered<RoundBigBlackKnob>(i.at, m, id));
				return;
			case Item::SMALL:
				addParam(createParamCentered<RoundSmallBlackKnob>(i.at, m, id));
				return;
			case Item::TRIM:
				addParam(createParamCentered<Trimpot>(i.at, m, id));
				return;
			default:
				break;
		}
		if (isButton(t, id))
			addParam(createParamCentered<VCVButton>(i.at, m, id));
		else if (positions(t, id) == 2)
			addParam(createParamCentered<CKSS>(i.at, m, id));
		else if (positions(t, id) == 3)
			addParam(createParamCentered<CKSSThree>(i.at, m, id));
		else
			addParam(createParamCentered<RoundBlackKnob>(i.at, m, id));
	}

	void appendContextMenu(ui::Menu* menu) override {
		Module* m = static_cast<Module*>(module);
		if (!m || m->menu.empty())
			return;
		menu->addChild(new ui::MenuSeparator);
		for (const MenuEntry& e : m->menu) {
			if (e.choice)
				menu->addChild(createIndexPtrSubmenuItem(e.label, e.choices, e.choice));
			else if (e.flag)
				menu->addChild(createBoolPtrMenuItem(e.label, "", e.flag));
			else if (e.value)
				menu->addChild(new ValueSlider(e));
		}
	}
};


NOVA_INLINE app::ModuleWidget* makeWidget(Module* m, Module* preview, const std::string& slug) {
	return new Widget(m, preview, slug);
}

#endif


} // namespace nova
