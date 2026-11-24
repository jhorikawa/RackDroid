package org.rackdroid

import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager

/**
 * Starts the program that answers the chat (nova/bridge/nova_bridge.py), in
 * Termux, where Claude Code lives: installed there and signed in to by whoever
 * owns the device. This app neither contains it nor sees its sign-in; it hands
 * Termux a small script and the number to call back on.
 *
 * Termux lets another app do that if its owner has said so twice: with the
 * permission RUN_COMMAND, and with allow-external-apps = true in
 * ~/.termux/termux.properties.
 */
class TermuxBridge(private val activity: Activity, private val server: PatchServer) {
	companion object {
		private const val TERMUX = "com.termux"
		private const val PERMISSION = "com.termux.permission.RUN_COMMAND"
		private const val HOME = "/data/data/com.termux/files/home"
		private const val BASH = "/data/data/com.termux/files/usr/bin/bash"
		/** Seconds without this app after which the bridge leaves as well. */
		private const val PATIENCE = 120
		const val REQUEST = 0x4e56

		/** Standard input is the link on one line, then the program. */
		private val SCRIPT = """
			umask 077
			mkdir -p "${'$'}HOME/nova-chat" && cd "${'$'}HOME/nova-chat" || exit 1
			IFS= read -r link && printf '%s\n' "${'$'}link" > chat-link.json
			cat > nova_bridge.py.new && mv nova_bridge.py.new nova_bridge.py
			: > bridge.log
			exec proot-distro login debian --bind "${'$'}HOME/nova-chat:/nova-chat" -- \
				env NOVA_CHAT_LINK=/nova-chat/chat-link.json NOVA_CHAT_TIMEOUT=$PATIENCE \
				python3 /nova-chat/nova_bridge.py run >> bridge.log 2>&1
		""".trimIndent()

		const val SETUP = "Claude Code did not answer. In Termux:\n" +
			"1. allow-external-apps = true in ~/.termux/termux.properties, then restart Termux\n" +
			"2. Debian with Claude Code in it (proot-distro)\n" +
			"3. Sign in: claude auth login\n" +
			"4. For making modules: pkg install clang llvm\n" +
			"What went wrong is in ~/nova-chat/bridge.log."
	}

	enum class Outcome { STARTED, NO_TERMUX, ASKED, FAILED }

	private val installed: Boolean
		get() = runCatching { activity.packageManager.getPackageInfo(TERMUX, 0) }.isSuccess

	fun start(): Outcome {
		if (!installed)
			return Outcome.NO_TERMUX
		if (activity.checkSelfPermission(PERMISSION) != PackageManager.PERMISSION_GRANTED) {
			activity.requestPermissions(arrayOf(PERMISSION), REQUEST)
			return Outcome.ASKED
		}
		return try {
			val program = activity.assets.open("nova_bridge.py").bufferedReader().use { it.readText() }
			activity.startForegroundService(Intent("com.termux.RUN_COMMAND")
				.setClassName(TERMUX, "com.termux.app.RunCommandService")
				.putExtra("com.termux.RUN_COMMAND_PATH", BASH)
				.putExtra("com.termux.RUN_COMMAND_ARGUMENTS", arrayOf("-c", SCRIPT))
				.putExtra("com.termux.RUN_COMMAND_STDIN", server.linkJson() + "\n" + program)
				.putExtra("com.termux.RUN_COMMAND_WORKDIR", HOME)
				.putExtra("com.termux.RUN_COMMAND_BACKGROUND", true)
				.putExtra("com.termux.RUN_COMMAND_COMMAND_LABEL", "RackDroid chat"))
			Outcome.STARTED
		} catch (t: Throwable) {
			android.util.Log.w("rackdroid.link", "cannot start the bridge", t)
			Outcome.FAILED
		}
	}
}
