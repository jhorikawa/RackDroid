#!/usr/bin/env bash
# Builds and runs the offline check of the wrapped modules (render.cc).
#   nova/plugin/test/run.sh [dir-for-wav-files]
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=build/test
mkdir -p "$OUT"
SRC=(test/render.cc src/br/core/module.cc src/br/core/model.cc src/br/modules/*.cc
	eurorack/stmlib/dsp/units.cc eurorack/stmlib/dsp/atan.cc eurorack/stmlib/utils/random.cc
	eurorack/plaits/dsp/*.cc eurorack/plaits/dsp/*/*.cc eurorack/plaits/resources.cc
	eurorack/beads/dsp/*.cc eurorack/beads/resources.cc
	eurorack/peaks/drums/*.cc eurorack/peaks/modulations/*.cc eurorack/peaks/number_station/*.cc
	eurorack/peaks/pulse_processor/*.cc eurorack/peaks/processors.cc eurorack/peaks/resources.cc)
# The flags of the plugin itself (Makefile), minus Rack.
${CXX:-c++} -std=c++17 -O2 -funsafe-math-optimizations -fno-strict-aliasing -w \
	-DTEST -include src/br/core/mi_prelude.h -Ieurorack -Isrc/br \
	"${SRC[@]}" -o "$OUT/render"
[ $# -gt 0 ] && mkdir -p "$1"
"$OUT/render" "$@"
