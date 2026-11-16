package org.rackdroid

/**
 * Which Mutable Instruments module a model is based on, if any.
 *
 * "Mutable Instruments" is a trademark, so the ports go by descriptive names:
 * Plaits is "Macro Oscillator 2", Rings is "Resonator", Clouds is "Texture
 * Synthesizer". Anyone who knows the hardware looks for the hardware's name,
 * and the module pickers (GamepadMenu, ModulePalette) had no way to show it.
 *
 * The plugins do say it, in each module's description ("Based on Mutable
 * Instruments Plaits"), so the name is read from there rather than kept in a
 * table that a new pack would silently be missing from. The table below only
 * corrects the descriptions that name the wrong thing.
 */
object MutableOrigin {
	private val named = Regex("""Mutable Instruments(?:\s*\(TM\))?\s+([A-Z][A-Za-z0-9]+)""")

	/** Model key ("pluginSlug/modelSlug") to name, where the description is
	 * not enough: both of Audible Instruments' Tides say "Tides". */
	private val corrections = mapOf(
		"AudibleInstruments/Tides" to "Tides (2014)",
		"AudibleInstruments/Tides2" to "Tides (2018)",
	)

	/** "Plaits", "Rings"... or null for a module that is not such a port. */
	fun of(key: String, description: String): String? =
		corrections[key] ?: named.find(description)?.groupValues?.get(1)
}
