package org.rackdroid

import android.app.Activity
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * The patch, for a program on the same device that is not this app: the chat's
 * bridge (nova/bridge) asks for modules and cables here, and passes on what is
 * typed and answered.
 *
 * It listens on the loopback address only, and only while it has been switched
 * on. Any app on the device can reach that address, so each side proves to the
 * other that it knows the token before anything is said: neither sends the
 * token itself, so whoever else sits on the port learns nothing.
 *
 * Lines of JSON both ways. After the handshake a connection says what it is:
 *
 *   tools   asks for operations on the patch ({"op": ...}, patch_ops.cpp, and
 *           the two about the module list that are answered here)
 *   bridge  the same, and it is what answers the chat: it is sent
 *           {"chat": text}, {"stop": true}, {"new": true} and
 *           {"settings": ...}, and sends {"event": ...}
 *   user    somewhere the chat is typed: sends those, and is sent the events
 */
class PatchServer(
	private val activity: Activity,
	/** The module list as JSON (browser_native.cpp). */
	private val modelsJson: () -> String,
	private val requestBuild: () -> Unit,
	/** What the chat is to be answered by: {"model", "effort"}. */
	private val settings: () -> JSONObject,
) {
	companion object {
		private const val TAG = "rackdroid.link"
		private const val PORT = 38470
		private const val FILE = "chat-link.json"
		private const val MAX_CLIENTS = 8
		private const val MAX_LINE = 256 * 1024
		/** More models than this are not listed whole, only searched. */
		private const val MAX_CATALOG = 500
		private const val MAX_FOUND = 15
	}

	private class Model(override val key: String, override val name: String, override val brand: String,
		override val plugin: String, override val tags: List<String>, override val description: String) : ModuleText

	private inner class Client(val socket: Socket) {
		var role = ""
		private val out = socket.getOutputStream()

		fun send(o: JSONObject) {
			val bytes = (o.toString() + "\n").toByteArray()
			try {
				synchronized(out) {
					out.write(bytes)
					out.flush()
				}
			} catch (t: Throwable) {
				close()
			}
		}

		fun close() {
			runCatching { socket.close() }
		}
	}

	private val clients = CopyOnWriteArrayList<Client>()
	@Volatile private var server: ServerSocket? = null
	private var token = ByteArray(0)

	/** This run of the app: a conversation does not outlive it. */
	private val instance = random(8)

	val running: Boolean get() = server != null

	/** What the bridge says, and {"event": "bridge"} as it comes and goes. On
	 * some thread of this server's. */
	@Volatile var onEvent: ((JSONObject) -> Unit)? = null

	val bridgeConnected: Boolean get() = clients.any { it.role == "bridge" }

	/** {"chat": text}, {"stop": true}, {"new": true} or {"settings": ...}. False
	 * without a bridge. */
	fun toBridge(o: JSONObject): Boolean {
		val bridge = clients.firstOrNull { it.role == "bridge" } ?: return false
		Thread { bridge.send(o) }.start()
		return true
	}

	/** How to find and recognise this app, for the bridge. */
	fun linkJson(): String = JSONObject().put("port", PORT).put("token", String(token)).put("instance", instance).toString()

	/** Takes back the last answer's changes. Not on the UI thread: it waits for
	 * the render thread. Null when done, else what was wrong. */
	fun undo(): String? {
		val o = JSONObject(String(nativePatchOp("{\"op\":\"undo\"}".toByteArray())))
		return if (o.optBoolean("ok")) null else o.optString("error")
	}

	private fun random(n: Int) = ByteArray(n).also { SecureRandom().nextBytes(it) }.joinToString("") { "%02x".format(it) }

	/** Returns false if this build may not listen (no INTERNET permission: the
	 * Play build) or the port is taken. */
	fun start(): Boolean {
		if (running)
			return true
		return try {
			token = loadToken()
			val s = ServerSocket()
			s.reuseAddress = true
			s.bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), PORT))
			server = s
			runCatching { requestBuild() }
			Thread({ accept(s) }, "patch-server").start()
			log("listening on $PORT")
			true
		} catch (t: Throwable) {
			log("cannot listen: $t")
			false
		}
	}

	fun stop() {
		val s = server ?: return
		server = null
		runCatching { s.close() }
		clients.forEach { it.close() }
		log("stopped")
	}

	/** What the bridge needs to find and recognise this app. */
	private fun loadToken(): ByteArray {
		val f = File(activity.getExternalFilesDir(null), FILE)
		runCatching {
			val o = JSONObject(f.readText())
			if (o.getInt("port") == PORT && o.getString("token").length == 64)
				return o.getString("token").toByteArray()
		}
		val hex = random(32)
		f.writeText(JSONObject().put("port", PORT).put("token", hex).toString())
		return hex.toByteArray()
	}

	private fun accept(s: ServerSocket) {
		while (server === s) {
			val socket = try {
				s.accept()
			} catch (t: Throwable) {
				break
			}
			if (clients.size >= MAX_CLIENTS) {
				runCatching { socket.close() }
				continue
			}
			val client = Client(socket)
			clients.add(client)
			Thread({ serve(client) }, "patch-client").start()
		}
	}

	private fun proof(who: String, a: String, b: String): String {
		val mac = Mac.getInstance("HmacSHA256")
		mac.init(SecretKeySpec(token, "HmacSHA256"))
		return mac.doFinal("$who:$a:$b".toByteArray()).joinToString("") { "%02x".format(it) }
	}

	private fun serve(client: Client) {
		try {
			client.socket.tcpNoDelay = true
			client.socket.soTimeout = 5000
			val reader = BufferedReader(InputStreamReader(client.socket.getInputStream(), Charsets.UTF_8))
			val nonce = random(16)
			client.send(JSONObject().put("hello", "rackdroid").put("nonce", nonce))
			val hello = JSONObject(readLine(reader) ?: return)
			val theirs = hello.getString("nonce")
			val expected = proof("client", nonce, theirs)
			if (!MessageDigest.isEqual(expected.toByteArray(), hello.getString("proof").toByteArray())) {
				log("a connection that does not know the token")
				return
			}
			val role = hello.getString("role")
			// One that an earlier run of the app started, still looking for it:
			// it may be older than this app, and this run starts its own.
			if (role == "bridge" && hello.optString("instance", instance) != instance) {
				client.send(JSONObject().put("proof", proof("server", nonce, theirs)).put("replaced", true))
				return
			}
			client.role = role
			client.send(JSONObject().put("proof", proof("server", nonce, theirs)).put("instance", instance)
				.put("settings", settings()))
			client.socket.soTimeout = 0
			log("connected: ${client.role}")
			// The newest one answers. The other is told, or it would come back
			// and the two would take turns.
			if (client.role == "bridge")
				clients.filter { it !== client && it.role == "bridge" }.forEach {
					it.send(JSONObject().put("replaced", true))
					it.close()
				}
			while (true) {
				val line = readLine(reader) ?: break
				if (line.isBlank())
					continue
				handle(client, JSONObject(line))
			}
		} catch (t: Throwable) {
			if (running)
				log("${client.role.ifEmpty { "a connection" }} dropped: $t")
		} finally {
			client.close()
			clients.remove(client)
			if (client.role == "bridge" && running && !bridgeConnected)
				toUsers(JSONObject().put("event", "bridge").put("connected", false))
		}
	}

	private fun readLine(reader: BufferedReader): String? {
		val sb = StringBuilder()
		while (true) {
			val c = reader.read()
			if (c < 0)
				return if (sb.isEmpty()) null else sb.toString()
			if (c == '\n'.code)
				return sb.toString()
			if (sb.length >= MAX_LINE)
				throw IllegalStateException("line too long")
			sb.append(c.toChar())
		}
	}

	private fun handle(client: Client, o: JSONObject) {
		when {
			o.has("op") && client.role in setOf("tools", "bridge") -> {
				val answer = JSONObject(operate(o))
				if (o.has("id"))
					answer.put("id", o.get("id"))
				client.send(answer)
			}
			o.has("event") && client.role == "bridge" -> toUsers(o)
			(o.has("chat") || o.has("stop") || o.has("new")) && client.role == "user" -> {
				val bridge = clients.firstOrNull { it.role == "bridge" }
				if (bridge == null)
					client.send(JSONObject().put("event", "error").put("text", "The bridge is not running."))
				else
					bridge.send(o)
			}
		}
	}

	private fun toUsers(o: JSONObject) {
		clients.filter { it.role == "user" }.forEach { it.send(o) }
		onEvent?.invoke(o)
	}

	/* ---- Operations ---- */

	private fun operate(o: JSONObject): String = when (o.getString("op")) {
		"find" -> onUi { answer(find(o.optString("query"))) }
		"catalog" -> onUi { answer(catalog()) }
		else -> String(nativePatchOp(o.toString().toByteArray()))
	}

	private fun answer(text: String) = JSONObject().put("ok", true).put("text", text).toString()

	/** The module list and its tags belong to the UI thread. */
	private fun onUi(what: () -> String): String {
		val task = FutureTask(what)
		activity.runOnUiThread(task)
		return try {
			task.get(5, TimeUnit.SECONDS)
		} catch (t: Throwable) {
			JSONObject().put("ok", false).put("error", "The module list cannot be read now.").toString()
		}
	}

	private fun models(): List<Model> {
		ModuleTags.refresh(activity.getExternalFilesDir(null))
		val arr = JSONArray(modelsJson())
		return (0 until arr.length()).map { i ->
			val o = arr.getJSONObject(i)
			val tagsJ = o.optJSONArray("tags") ?: JSONArray()
			Model(o.getString("key"), o.optString("name"), o.optString("brand"), o.optString("plugin"),
				(0 until tagsJ.length()).map { tagsJ.getString(it) }, o.optString("description"))
		}
	}

	private fun line(m: Model, withDescription: Boolean): String {
		val sb = StringBuilder("${m.key} \"${m.name}\" [${m.tags.joinToString(", ")}]")
		MutableOrigin.of(m.key, m.description)?.let { sb.append(" = Mutable Instruments $it") }
		val own = ModuleTags.of(m.key)
		if (own.isNotEmpty())
			sb.append(" (the user's tags: ${own.joinToString(", ")})")
		if (withDescription && m.description.isNotBlank())
			sb.append(" - ${m.description.trim().replace(Regex("\\s+"), " ")}")
		return sb.toString()
	}

	private fun find(query: String): String {
		val found = ModuleTags.search(models(), query)
		if (found.isEmpty())
			return "No installed module matches \"$query\"."
		return found.take(MAX_FOUND).joinToString("\n") { line(it, true) } +
			(if (found.size > MAX_FOUND) "\n... and ${found.size - MAX_FOUND} more; ask for something narrower." else "")
	}

	private fun catalog(): String {
		val all = models().filter { "Blank" !in it.tags }
		if (all.isEmpty())
			return ""
		if (all.size > MAX_CATALOG)
			return "${all.size} modules are installed, too many to list: search for them."
		return all.joinToString("\n") { line(it, false) }
	}

	private fun log(text: String) {
		android.util.Log.i(TAG, text)
	}

	private external fun nativePatchOp(request: ByteArray): ByteArray
}
