// Nova Modular - GPL-3.0-or-later.
//
// The Mutable Instruments modules VCV's Audible Instruments does not have,
// taken from BrickRack (https://github.com/andrewfrangella/brickrack), which
// runs the original firmware DSP. See adapter.hpp for how they become Rack
// modules, and plugin.json for the names: like Audible Instruments, the
// modules carry descriptive names and say what they are based on in their
// description.

#include "plugin.hpp"

#include "adapter.hpp"

Plugin* pluginInstance;

/** play.cpp: not from BrickRack, but this fork's own. */
plugin::Model* createPlayModel();

void init(Plugin* p) {
	pluginInstance = p;

	auto add = [&](NovaModelOptions options) {
		if (plugin::Model* model = createNovaModel(options))
			p->addModel(model);
	};

	// Plaits with firmware 1.2's 24 models. Its wrapper renders 48 kHz blocks
	// straight into the engine, hence the native rate; one voice per channel.
	add({"macro-osc-2", "Plaits24", 48000.f, 16});
	// Beads picks its own codec rate (24/32/48 kHz) and converts itself.
	add({"texture-synth-2", "Beads"});
	// Peaks' firmware runs at 48 kHz, in step with BrickRack's engine.
	add({"dual-trigger", "Peaks", 48000.f});
	add({"chiptune-osc", "Edges"});
	add({"dual-multimode-filter", "Blades"});
	p->addModel(createPlayModel());
}
