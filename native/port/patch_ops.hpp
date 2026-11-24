#pragma once
#include <string>

namespace rackdroid {

/** Reading and changing the open patch on behalf of something that is not a
hand: the chat (PatchServer.kt passes its requests here).

One request is one JSON object, {"op": ..., ...}; the answer is
{"ok": true, "text": ...} or {"ok": false, "error": ...}. Both texts are written
to be read by a language model. The operations are listed in patch_ops.cpp.

Everything asked for between two "turn" requests is one step of Undo. */

/** Any thread but the render thread: waits for the render thread to carry the
request out. */
std::string patchOpCall(const std::string& request);

/** Render thread, every frame, before Window::step(). */
void patchOpsStep();

} // namespace rackdroid
