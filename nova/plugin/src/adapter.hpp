// Nova Modular - GPL-3.0-or-later.
//
// Runs a BrickRack module (src/br/modules/*.cc) as a VCV Rack module.
//
// BrickRack's module model is deliberately close to Rack's -- params, ports
// and lights, one process() per engine sample -- and each module describes
// its panel as a list of knobs, jacks and labels in millimetres. So one
// generic Rack Module and one generic ModuleWidget are enough: the module
// files are compiled as they are, firmware DSP and all.

#pragma once

#include <string>

#include "plugin.hpp"

struct NovaModelOptions {
	/** The BrickRack model to wrap (Model::slug in its .cc). */
	std::string brSlug;
	/** The Rack model slug, as listed in plugin.json. */
	std::string slug;
	/** Engine rate the wrapper was written for, or 0 if it follows the
	engine. BrickRack's engine always runs at 48 kHz and a few wrappers
	(the ones whose firmware runs at 48 kHz too) rely on it; Rack's rate is
	the user's choice. Those run at this rate and are converted. */
	float nativeRate = 0.f;
	/** Polyphony: one instance per channel, up to this many. */
	int maxVoices = 1;
};

/** NULL (with a warning in the log) if the BrickRack model is not linked in. */
plugin::Model* createNovaModel(const NovaModelOptions& options);
