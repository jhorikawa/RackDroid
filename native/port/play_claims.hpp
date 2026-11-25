#pragma once
#include <string>
#include <vector>

namespace rack {
namespace engine {
struct Module;
}
}

namespace rackdroid {

/** Which of the controller the patch plays with. In play mode (R3) those are the
patch's; everything else goes on operating the rack. A control is in use if a
cable hangs on its jack -- of Nova Play (nova/plugin/src/play.cpp) or of a MIDI-CC
to CV that listens to the controller -- or if a slot of Nova Play uses it.

Controls go by the numbers of the CCs that gamepad_midi.cpp sends. Render thread. */

const int PLAY_CCS = 84;

/** Looks again, every quarter of a second or at once. */
void playClaimsStep(bool now = false);

bool playClaimed(int cc);

/** What plays, in words for the screen: "R stick, L2, A". */
std::string playClaimsText();

/** For the touch surfaces: XY pad 1, drums 2, faders 4. */
int playTouchWanted();

/** Which CC each of the 16 outputs of a MIDI-CC to CV module carries (-1: none),
if it listens to the controller; else empty. */
std::vector<int> padCellsOf(rack::engine::Module* module);

} // namespace rackdroid
