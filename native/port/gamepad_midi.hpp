#pragma once

namespace rackdroid {

/** Registers the "Gamepad" MIDI driver: the built-in controller as a source
of control voltages, through Rack's own MIDI-CC and MIDI-Map modules. Call
once, after midi::init(). */
void gamepadMidiInit();

/** Sends whatever changed since the last call. Call once per frame, on the
render thread. */
void gamepadMidiStep();

} // namespace rackdroid
