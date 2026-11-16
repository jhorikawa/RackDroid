# Nova Modular (5 modules) for RackDroid: included by native/CMakeLists.txt.
#
# The Mutable Instruments modules Audible Instruments does not have -- Beads,
# Peaks, Edges, Blades and Plaits with firmware 1.2's 24 models -- taken from
# BrickRack, which runs the original firmware DSP. GPL-3.0-or-later; firmware
# MIT (Edges' tables GPL). Panels are drawn by the plugin, so it ships no art.
#
# The very same sources build for desktop Rack with the Makefile next to this
# file, so a patch made here opens there. The source list and the flags
# mirror that Makefile (and BrickRack's CMakeLists.txt, which both follow).

set(NOVA_DIR ${CMAKE_CURRENT_LIST_DIR})
set(NOVA_EU ${NOVA_DIR}/eurorack)

file(GLOB NOVA_SOURCES
	${NOVA_DIR}/src/*.cpp
	${NOVA_DIR}/src/br/modules/*.cc
	${NOVA_EU}/plaits/dsp/*.cc
	${NOVA_EU}/plaits/dsp/*/*.cc)
list(APPEND NOVA_SOURCES
	${NOVA_DIR}/src/br/core/module.cc
	${NOVA_DIR}/src/br/core/model.cc
	${NOVA_EU}/stmlib/dsp/units.cc
	${NOVA_EU}/stmlib/dsp/atan.cc
	${NOVA_EU}/stmlib/utils/random.cc
	${NOVA_EU}/plaits/resources.cc
	${NOVA_EU}/beads/dsp/delay_clock.cc
	${NOVA_EU}/beads/dsp/delay_controller.cc
	${NOVA_EU}/beads/dsp/mu_law.cc
	${NOVA_EU}/beads/dsp/processor.cc
	${NOVA_EU}/beads/dsp/synth.cc
	${NOVA_EU}/beads/dsp/trigger_generator.cc
	${NOVA_EU}/beads/resources.cc
	${NOVA_EU}/peaks/drums/bass_drum.cc
	${NOVA_EU}/peaks/drums/fm_drum.cc
	${NOVA_EU}/peaks/drums/high_hat.cc
	${NOVA_EU}/peaks/drums/snare_drum.cc
	${NOVA_EU}/peaks/modulations/lfo.cc
	${NOVA_EU}/peaks/modulations/multistage_envelope.cc
	${NOVA_EU}/peaks/number_station/number_station.cc
	${NOVA_EU}/peaks/pulse_processor/pulse_randomizer.cc
	${NOVA_EU}/peaks/pulse_processor/pulse_shaper.cc
	${NOVA_EU}/peaks/processors.cc
	${NOVA_EU}/peaks/resources.cc)

add_simple_plugin(plugin_nova ${NOVA_DIR} ${NOVA_SOURCES})
# The firmware is written for 32-bit ARM GCC 4.x: TEST is its own desktop
# configuration, and the prelude supplies the headers old toolchains pulled
# in by themselves.
target_compile_options(plugin_nova PRIVATE
	-DTEST -include ${NOVA_DIR}/src/br/core/mi_prelude.h
	-I${NOVA_EU} -I${NOVA_DIR}/src/br
	-fno-strict-aliasing)
# BrickRack is C++17 (the rest of the plugins keep Rack's C++11).
set_target_properties(plugin_nova PROPERTIES CXX_STANDARD 17)
