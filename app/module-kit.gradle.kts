// What a module is compiled with on the device (nova/kit, nova/bridge/nova_build.py):
// Rack's headers and those they include, the kit's own, and the test. Only the
// sideload build carries it, as it alone has the chat.
//
// Some of the headers are made when CMake configures the native build, so this
// comes after that; the newest configuration is the one this APK is built from.

import java.security.MessageDigest
import java.util.Properties
import java.util.zip.ZipFile

val rack = rootProject.file("third_party/Rack")

fun newestCxx(): File? = file(".cxx/RelWithDebInfo").listFiles()
	?.map { File(it, "arm64-v8a") }
	?.filter { File(it, "_deps/jansson-build/include/jansson.h").isFile }
	?.maxByOrNull { File(it, "CMakeCache.txt").lastModified() }

fun ndkInclude(): File {
	val properties = Properties()
	val local = rootProject.file("local.properties")
	if (local.isFile)
		local.inputStream().use { properties.load(it) }
	val sdk = properties.getProperty("sdk.dir") ?: System.getenv("ANDROID_HOME") ?: System.getenv("ANDROID_SDK_ROOT") ?: ""
	val prebuilt = File(sdk, "ndk/27.2.12479018/toolchains/llvm/prebuilt")
	return File(prebuilt.listFiles()?.firstOrNull { it.isDirectory } ?: prebuilt, "sysroot/usr/include")
}

val packModuleKit = tasks.register<Zip>("packModuleKit") {
	mustRunAfter(tasks.matching { it.name.startsWith("configureCMakeRelWithDebInfo") })
	dependsOn(tasks.matching { it.name == "configureCMakeRelWithDebInfo[arm64-v8a]" })
	// The same headers are the same bytes: the device tells by them whether a
	// module has to be compiled again.
	isPreserveFileTimestamps = false
	isReproducibleFileOrder = true
	into("include/rack") { from(File(rack, "include")) { include("**/*.hpp", "**/*.h") } }
	into("include/nanovg") { from(File(rack, "dep/nanovg/src")) { include("*.h") } }
	into("include/nanosvg") { from(File(rack, "dep/nanosvg/src")) { include("*.h") } }
	into("include/glfw") { from(File(rack, "dep/glfw/include")) { include("GLFW/glfw3.h") } }
	into("include/simde") { from(File(rack, "dep/simde")) { include("simde/*.h", "simde/x86/mmx.h", "simde/x86/*sse*.h") } }
	into("include/pffft") { from(File(rack, "dep/pffft")) { include("pffft.h") } }
	into("include/speex") { from(File(rack, "dep/speexdsp/include")) { include("speex/*.h") } }
	into("include/compat") { from(rootProject.file("native/compat")) { include("GL/*.h") } }
	into("include/made") {
		from(provider { newestCxx()?.let { listOf(File(it, "dep_include"), File(it, "blendish_touch"), File(it, "_deps/jansson-build/include")) } ?: emptyList<File>() })
	}
	into("include/gl") { from(provider { ndkInclude() }) { include("GLES2/*.h", "GLES3/*.h", "EGL/*.h", "KHR/*.h") } }
	from(rootProject.file("nova/kit")) { include("nova_kit.hpp", "nova_kit_impl.hpp", "__assertion_handler", "test.cpp", "guide.md") }
	archiveFileName.set("module-kit.zip")
	destinationDirectory.set(layout.buildDirectory.dir("generated/kit-assets"))
	// Modules made with other headers of Rack must be made again (MadeModules.kt).
	val abiFile = layout.buildDirectory.file("generated/kit-assets/module-kit.abi")
	outputs.file(abiFile)
	doLast {
		val lines = ZipFile(archiveFile.get().asFile).use { zip ->
			zip.entries().asSequence().filter { !it.isDirectory && it.name.startsWith("include/") }
				.map { "${it.name} ${String.format("%08x", it.crc)}" }.sorted().toList()
		}
		val sha = MessageDigest.getInstance("SHA-256").digest(lines.joinToString("\n").toByteArray())
		abiFile.get().asFile.writeText(sha.take(8).joinToString("") { String.format("%02x", it) })
	}
}

tasks.matching { it.name.startsWith("mergeSideload") && it.name.endsWith("Assets") }.configureEach { dependsOn(packModuleKit) }
tasks.matching { it.name.startsWith("lint") || it.name.startsWith("generateSideload") }.configureEach { mustRunAfter(packModuleKit) }
