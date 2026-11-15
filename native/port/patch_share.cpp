#include <filesystem>
#include <system_error>

#include <sys/stat.h>

#include <android/log.h>

#include <system.hpp>

#include "patch_share.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.patches", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "rackdroid.patches", __VA_ARGS__)

namespace fs = std::filesystem;

namespace rackdroid {


/** Lets the group read and write what is in the shared folder. On this part of
shared storage the file modes are real, and the app's files are born private to
it (rw-------): adb, which gets in through the folder's group, could list a
patch but not read it. */
static void openToGroup(const fs::path& dir) {
	std::error_code ec;
	const fs::perms rw = fs::perms::owner_read | fs::perms::owner_write
		| fs::perms::group_read | fs::perms::group_write;
	for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
		if (entry.is_regular_file(ec))
			fs::permissions(entry.path(), rw, fs::perm_options::replace, ec);
	}
}


void sharePatchesFolder(const std::string& userDir, const std::string& externalDir) {
	if (externalDir.empty())
		return;
	// Files the app creates from here on are readable by their group too.
	// Its private storage is closed off by its own directory, not by this.
	::umask(0007);
	fs::path link = fs::path(userDir) / "patches";
	fs::path shared = fs::path(externalDir) / "Patches";
	std::error_code ec;

	fs::create_directories(shared, ec);
	if (ec || !fs::is_directory(shared, ec)) {
		LOGW("shared storage is not available (%s); patches stay private", shared.c_str());
		return;
	}

	if (fs::is_symlink(link, ec)) {
		if (fs::read_symlink(link, ec) == shared) {
			openToGroup(shared);
			return;
		}
		// Points somewhere else (the app was moved, or reinstalled under
		// another id): the folder it should point to is the one above.
		fs::remove(link, ec);
	}
	else if (fs::is_directory(link, ec)) {
		// Patches saved before this existed: across they go. One already
		// over there by the same name is the newer copy of the two only if
		// someone put it there, so it is left alone.
		int moved = 0;
		for (const fs::directory_entry& entry : fs::directory_iterator(link, ec)) {
			fs::path target = shared / entry.path().filename();
			if (fs::exists(target, ec))
				continue;
			fs::copy(entry.path(), target, fs::copy_options::recursive, ec);
			if (!ec)
				moved++;
		}
		fs::remove_all(link, ec);
		if (ec) {
			LOGW("could not clear %s (%s); patches stay private", link.c_str(), ec.message().c_str());
			return;
		}
		LOGI("moved %d patches to %s", moved, shared.c_str());
	}

	fs::create_directory_symlink(shared, link, ec);
	if (ec) {
		// Without the link the app would have no patches folder at all.
		LOGW("could not link %s -> %s (%s)", link.c_str(), shared.c_str(), ec.message().c_str());
		fs::create_directories(link, ec);
		return;
	}
	openToGroup(shared);
	LOGI("patches are at %s", shared.c_str());
}


} // namespace rackdroid
