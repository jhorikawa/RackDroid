#pragma once

#include <string>

namespace rackdroid {

/** Puts the patches folder where a computer can reach it.

`userDir`/patches is in the app's private storage, which nothing outside the
app can read: a patch could only leave through the share sheet, one at a time.
This makes it a link to `externalDir`/Patches -- the app's own folder on shared
storage, readable and writable over adb with no permission to ask for -- and
moves any patches already there across. Every path the app uses stays the
same, so nothing else needs to know.

Call once at startup, after userDir exists and before anything reads the
patches. Leaves things as they were if shared storage is not there. */
void sharePatchesFolder(const std::string& userDir, const std::string& externalDir);

} // namespace rackdroid
