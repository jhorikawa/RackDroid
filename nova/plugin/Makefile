# Nova Modular as a plugin for desktop VCV Rack 2 (Mac, Linux, Windows).
#
# RackDroid builds the same sources with CMake (native/CMakeLists.txt,
# plugin_nova). This Makefile is the ordinary Rack plugin build, so patches
# made on the Nova open on a computer:
#
#   make RACK_DIR=/path/to/Rack-SDK install
#
# RACK_DIR is a Rack 2 SDK. Without one, nova/tools/rack-sdk-from-repo.sh
# assembles an equivalent from the Rack sources in this repository.

RACK_DIR ?= ../../../Rack-SDK

# The firmware is written for 32-bit ARM GCC 4.x. TEST is its own desktop
# configuration; the prelude supplies the headers old toolchains pulled in
# by themselves. See BrickRack's CMakeLists.txt, which these flags follow.
FLAGS += -Ieurorack -Isrc/br -Isrc
FLAGS += -DTEST -include src/br/core/mi_prelude.h
FLAGS += -fno-strict-aliasing
# The firmware trips most of -Wall -Wextra.
FLAGS += -Wno-unused-local-typedef -Wno-unused-variable -Wno-unused-function \
	-Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-private-field \
	-Wno-deprecated-declarations -Wno-unused-but-set-variable -Wno-parentheses
EXTRA_CXXFLAGS += -std=c++17

SOURCES += $(wildcard src/*.cpp)
SOURCES += src/br/core/module.cc src/br/core/model.cc
SOURCES += $(wildcard src/br/modules/*.cc)

SOURCES += eurorack/stmlib/dsp/units.cc
SOURCES += eurorack/stmlib/dsp/atan.cc
SOURCES += eurorack/stmlib/utils/random.cc

# Plaits
SOURCES += $(wildcard eurorack/plaits/dsp/*.cc)
SOURCES += $(wildcard eurorack/plaits/dsp/*/*.cc)
SOURCES += eurorack/plaits/resources.cc

# Beads
SOURCES += eurorack/beads/dsp/delay_clock.cc
SOURCES += eurorack/beads/dsp/delay_controller.cc
SOURCES += eurorack/beads/dsp/mu_law.cc
SOURCES += eurorack/beads/dsp/processor.cc
SOURCES += eurorack/beads/dsp/synth.cc
SOURCES += eurorack/beads/dsp/trigger_generator.cc
SOURCES += eurorack/beads/resources.cc

# Peaks
SOURCES += eurorack/peaks/drums/bass_drum.cc
SOURCES += eurorack/peaks/drums/fm_drum.cc
SOURCES += eurorack/peaks/drums/high_hat.cc
SOURCES += eurorack/peaks/drums/snare_drum.cc
SOURCES += eurorack/peaks/modulations/lfo.cc
SOURCES += eurorack/peaks/modulations/multistage_envelope.cc
SOURCES += eurorack/peaks/number_station/number_station.cc
SOURCES += eurorack/peaks/pulse_processor/pulse_randomizer.cc
SOURCES += eurorack/peaks/pulse_processor/pulse_shaper.cc
SOURCES += eurorack/peaks/processors.cc
SOURCES += eurorack/peaks/resources.cc

# Edges: its tables (eurorack/edges/resources.cc) are #included by the wrapper.

DISTRIBUTABLES += res
DISTRIBUTABLES += LICENSE.md THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.brickrack.txt

include $(RACK_DIR)/plugin.mk
