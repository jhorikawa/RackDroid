#pragma once
#include <cstdint>

namespace rackdroid {

/** Pause: the engine stands still and nothing sounds. Not a mute -- sequencers,
envelopes and delay lines stop where they are and go on from there.

The engine is driven from the audio callback, so holding it is a matter of the
callback handing back silence without asking for any frames. */

/** Render thread. */
void transportSetPaused(bool paused);
void transportToggle();
/** Any thread. */
bool transportPaused();

/** Render thread, every loop whether or not there is a surface: carries out
what the UI thread asked for, and plays on when a patch has been opened --
which is when `patchEpoch` (audioPortEpoch()) moves. */
void transportStep(int patchEpoch);

/** Adds the "PAUSED" sign to the scene. Call once, after the patch is
launched. */
void installTransport();

/** Audio thread: the fade is over and this block is to be silence, with the
engine left alone. */
bool transportHeld();

/** Audio thread, on each block the engine did produce: fades it out on the way
into a pause and in on the way out. */
void transportFade(float* output, int channels, int32_t frames, int32_t sampleRate);

} // namespace rackdroid
