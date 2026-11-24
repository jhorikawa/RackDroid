package org.rackdroid

import android.app.Activity
import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import org.json.JSONArray
import org.json.JSONObject

/**
 * The chat: a panel along the right edge, with the rack to the left of it so
 * that the patch can be watched, and played, as it changes. A finger reaches
 * both. The controller is the rack's, with everything it does there, but while
 * something is typed or read here:
 *
 *   rack      the panel's window takes no keys, and RackDroid's own gets them
 *   typing    Android's keyboard, with its Japanese and its microphone; or, if
 *             that is chosen, one for the D-pad (PadKeyboard)
 *   reading   D-pad up and down scroll, Y stops the answer or takes back the
 *             last one, X chooses the model and the effort, A goes to typing
 *
 * START held goes from the rack to the chat and back; sending goes back as well,
 * and so does B when there is nothing else for it to leave. The sticks, the
 * triggers, L3 and R3 are the rack's throughout, as is SELECT.
 *
 * What is being answered goes on with the panel put away. The conversation lasts
 * as long as the app does.
 */
class ChatPanel(
	private val activity: Activity,
	private val server: PatchServer,
	private val bridge: TermuxBridge,
	private val settings: ChatSettings,
	private val store: ChatStore,
	private val menu: GamepadMenu,
	private val togglePause: () -> Unit,
	/** A command for gamepad_nav.cpp. */
	private val command: (Int) -> Unit,
	/** The conversation that is gone on with, for whoever connects. */
	private val sessionIs: (String) -> Unit,
) {
	private companion object {
		const val WIDTH_DP = 300
		/** How long Claude Code may take to come up. */
		const val START_MS = 25000L
		const val MAX_MESSAGES = 60
		/** As in gamepad_nav.cpp. */
		const val HOLD_MS = 400L
		const val HAT_THRESHOLD = 0.5f
	}

	private enum class Who { USER, CLAUDE, NOTE }

	/** @param kept what was done to the patch, as it was kept (ChatStore) */
	private class Message(val who: Who, var text: String, private val kept: String = "") {
		/** What was done to the patch, as it is done. */
		val added = ArrayList<String>()
		var removed = 0
		var cables = 0
		var unplugged = 0
		var knobs = 0
		var choices = 0
		var actions = 0
		var moved = 0
		val files = ArrayList<String>()
		/** Modules made or changed, and what was called with which. */
		val made = ArrayList<String>()
		val making = HashMap<String, String>()
		var refused = 0
		var listened = false
		var undid = false
		var view: TextView? = null

		fun work(): String {
			if (kept.isNotEmpty())
				return kept
			val parts = ArrayList<String>()
			if (refused > 0) parts.add("$refused attempt${if (refused > 1) "s" else ""} refused")
			parts.addAll(made)
			if (added.isNotEmpty()) parts.add(added.joinToString(" ") { "+$it" })
			if (removed > 0) parts.add("−$removed module${if (removed > 1) "s" else ""}")
			if (cables > 0) parts.add("$cables cable${if (cables > 1) "s" else ""}")
			if (unplugged > 0) parts.add("$unplugged unplugged")
			if (knobs > 0) parts.add("$knobs knob${if (knobs > 1) "s" else ""}")
			if (choices > 0) parts.add("$choices setting${if (choices > 1) "s" else ""}")
			if (moved > 0) parts.add("$moved moved")
			if (actions > 0) parts.add("$actions action${if (actions > 1) "s" else ""}")
			parts.addAll(files)
			if (undid) parts.add("undo")
			if (listened) parts.add("measured")
			return parts.joinToString("  ·  ")
		}
	}

	private val ui = Handler(Looper.getMainLooper())
	private val messages = ArrayList<Message>()
	private var answer: Message? = null
	private var askedAt = 0L
	private var stopping = false
	/** Typed before there was anyone to answer it. */
	private var unsent: String? = null
	private var startedAt = 0L
	private var draft = ""
	/** What it is at, if it is something that takes long, and since when. */
	private var doing: String? = null
	private var doingSince = 0L

	private var dialog: Dialog? = null
	private lateinit var list: LinearLayout
	private lateinit var scroll: ScrollView
	private lateinit var status: TextView
	private lateinit var input: EditText
	private lateinit var send: ImageButton
	private lateinit var keyboard: PadKeyboard
	private lateinit var help: TextView
	private lateinit var edge: View
	/** The controller is the chat's, not the rack's. */
	private var mine = false
	private var typing = true
	private var imeShown = false
	/** The keyboard is being put away to read, not to go back to the rack. */
	private var stays = false

	private val ink get() = AppTheme.current.textPrimary
	private val faint get() = AppTheme.current.textSecondary
	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()

	val showing get() = dialog?.isShowing == true
	private val working get() = answer != null

	init {
		server.onEvent = { o -> ui.post { onEvent(o) } }
		recall()
	}

	/** The store's current chat, as it was left. */
	private fun recall() {
		messages.clear()
		for (l in store.current.lines.takeLast(MAX_MESSAGES))
			messages.add(Message(if (l.who == ChatStore.USER) Who.USER else Who.CLAUDE, l.text, l.work))
		sessionIs(store.current.session)
	}

	fun toggle() {
		if (showing) close() else open()
	}

	/** START has been held at the rack. */
	fun summon() {
		if (!showing) open()
		else if (mine) toRack()
		else toChat()
	}

	/** Termux's permission was answered. */
	fun permissionAnswered(granted: Boolean) {
		if (granted)
			connect()
		else
			fail("Without that permission RackDroid cannot start Claude Code in Termux.")
	}

	/* ---- Getting someone to answer ---- */

	private fun connect() {
		if (!server.start()) {
			fail("This build cannot open the chat link.")
			return
		}
		if (server.bridgeConnected) {
			startedAt = 0L
			refreshStatus()
			return
		}
		when (bridge.start()) {
			TermuxBridge.Outcome.STARTED -> {
				val mine = SystemClock.uptimeMillis()
				startedAt = mine
				refreshStatus()
				ui.postDelayed({
					if (startedAt == mine && !server.bridgeConnected) {
						startedAt = 0L
						fail(TermuxBridge.SETUP)
					}
				}, START_MS)
			}
			TermuxBridge.Outcome.NO_TERMUX -> fail("The chat is answered by Claude Code in Termux, and Termux is not installed.")
			TermuxBridge.Outcome.ASKED -> {}
			TermuxBridge.Outcome.FAILED -> fail(TermuxBridge.SETUP)
		}
	}

	/** There is nobody to answer, and what has been asked is not left waiting. */
	private fun fail(text: String) {
		unsent = null
		finish(text)
		refreshStatus()
	}

	private fun onEvent(o: JSONObject) {
		when (o.optString("event")) {
			"bridge" -> {
				startedAt = 0L
				if (o.optBoolean("connected"))
					unsent?.let {
						unsent = null
						server.toBridge(JSONObject().put("chat", it))
					}
				else if (working)
					finish("Claude Code went away.")
			}
			"text" -> answer?.let {
				it.text += o.optString("text")
				show(it)
			}
			"doing" -> when (o.optString("name")) {
				"make_module" -> now("Writing a module")
				"edit_module" -> now("Changing a module")
				// What it asks for along with that does not come first.
				else -> if (answer?.making.isNullOrEmpty()) now(null)
			}
			"tool" -> answer?.let {
				val name = o.optString("name")
				val args = o.optJSONObject("args") ?: JSONObject()
				if (name == "make_module" || name == "edit_module") {
					it.making[o.optString("id")] = (if (name == "make_module") "made " else "changed ") + args.optString("id")
					now("Compiling and testing ${args.optString("id")}")
				}
				count(it, name, args)
				show(it)
			}
			"tool_done" -> answer?.let {
				it.making.remove(o.optString("id"))?.let { what ->
					if (o.optBoolean("ok")) it.made.add(what) else it.refused++
					now(if (o.optBoolean("ok")) null else "Putting it right")
					show(it)
				}
			}
			"done" -> {
				val m = answer
				if (m != null && o.optBoolean("stopped"))
					m.text = (m.text.trim() + "\nStopped.").trim()
				else if (m != null && !o.optBoolean("ok"))
					m.text = o.optString("text").ifBlank { "That did not work." }
				else if (m != null && m.text.isBlank())
					m.text = o.optString("text")
				finish(null)
			}
			"error" -> finish(o.optString("text"))
			"session" -> {
				store.setSession(o.optString("id"))
				sessionIs(store.current.session)
			}
			"forgot" -> note("Claude Code no longer has this conversation: it goes on without what was said so far.")
		}
		refreshStatus()
	}

	private fun now(what: String?) {
		doing = what
		doingSince = SystemClock.uptimeMillis()
	}

	private fun count(m: Message, tool: String, args: JSONObject) {
		fun size(key: String) = (args.optJSONArray(key) ?: JSONArray()).length()
		when (tool) {
			"add_modules" -> {
				val a = args.optJSONArray("models") ?: JSONArray()
				for (i in 0 until a.length())
					m.added.add(a.optString(i).substringAfter('/'))
			}
			"remove_modules" -> m.removed += size("modules")
			"connect" -> m.cables += size("cables")
			"disconnect" -> m.unplugged += size("cables")
			"set_knobs" -> m.knobs += size("knobs")
			"set_menu" -> m.choices += size("choices")
			"module_action" -> m.actions += size("modules")
			"move_modules" -> m.moved += size("moves")
			"save_patch" -> m.files.add("save")
			"open_patch" -> m.files.add("open")
			"play_controls" -> m.files.add("play controls")
			"revert_module" -> m.made.add("went back to version ${args.optInt("version")} of ${args.optString("id")}")
			"switch_module" -> m.made.add((if (args.optBoolean("on")) "started " else "stopped ") + args.optString("id"))
			"remove_made" -> m.made.add("removed ${args.optString("id")}")
			"play" -> m.files.add(if (args.optBoolean("on")) "play" else "pause")
			"listen" -> m.listened = true
			"undo" -> m.undid = true
		}
	}

	private fun finish(problem: String?) {
		val m = answer
		answer = null
		stopping = false
		doing = null
		if (m != null) {
			if (problem != null)
				m.text = (m.text.trim() + "\n" + problem).trim()
			store.add(ChatStore.CLAUDE, m.text.trim(), m.work())
			show(m)
		} else if (problem != null)
			note(problem)
		if (!showing && m != null)
			Toast.makeText(activity, m.text.take(140), Toast.LENGTH_LONG).show()
	}

	private fun ask() {
		val text = input.text.toString().trim()
		if (text.isEmpty() || working)
			return
		input.setText("")
		draft = ""
		append(Message(Who.USER, text))
		store.add(ChatStore.USER, text, "")
		val m = Message(Who.CLAUDE, "")
		answer = m
		askedAt = SystemClock.uptimeMillis()
		append(m)
		toRack()
		if (!server.toBridge(JSONObject().put("chat", text))) {
			unsent = text
			connect()
		}
		refreshStatus()
		ui.postDelayed(clock, 1000)
	}

	private val clock = object : Runnable {
		override fun run() {
			if (!working)
				return
			refreshStatus()
			ui.postDelayed(this, 1000)
		}
	}

	private fun stopAnswer() {
		if (!working || stopping)
			return
		stopping = true
		if (unsent != null) {
			unsent = null
			finish("Stopped.")
		} else
			server.toBridge(JSONObject().put("stop", true))
		refreshStatus()
	}

	private fun undoLast() {
		Thread {
			val problem = server.undo()
			ui.post { note(problem ?: "The last answer's changes are undone.") }
		}.start()
	}

	private fun showSettings() {
		stays = mine
		hideIme()
		val rows = listOf(
			PadRow("Chats…", note = "Those so far, to look up or go on with"),
			PadRow("Model: ${settings.modelName}", note = "What answers"),
			PadRow("Effort: ${settings.effort}", note = "How long it thinks about each answer"),
			PadRow("Keyboard: ${if (settings.padKeyboard) "D-pad" else "Android"}", note = "What you type with"),
			PadRow("Making modules: ${if (settings.making) "on" else "off"}", note = "Writing new ones, compiled and tested in Termux"))
		menu.choose("Chat", rows, 0) {
			when (it) {
				0 -> showChats()
				1 -> showModels()
				2 -> showEfforts()
				3 -> showKeyboards()
				else -> {
					settings.making = !settings.making
					server.toBridge(JSONObject().put("settings", settings.json()))
					note("From the next message: making modules is ${if (settings.making) "on" else "off"}.")
				}
			}
		}
	}

	private fun showChats(start: Int = 0) {
		val all = store.list()
		if (all.isEmpty()) {
			note("There is none yet.")
			return
		}
		val day = java.text.SimpleDateFormat("MMM d  HH:mm", java.util.Locale.US)
		menu.choose("Chats", all.map {
			menu.choiceRow(it.title.ifEmpty { "…" }.take(60), it.id == store.current.id,
				"${day.format(java.util.Date(it.at))}   ·   ${it.lines.size} messages")
		}, start, hint = "A  open      Y  delete      B  back", onY = { i ->
			menu.choose("Delete this chat?", listOf(menu.choiceRow("Keep it", false), menu.choiceRow("Delete it", false, all[i].title.take(60))), 0) {
				if (it == 1) {
					val open = all[i].id == store.current.id
					if (open && working)
						note("It is being answered.")
					else {
						store.delete(all[i].id)
						if (open)
							go(JSONObject().put("new", true))
					}
				}
				showChats(minOf(i, maxOf(0, store.list().size - 1)))
			}
		}) {
			if (working)
				note("First the answer that is being given.")
			else if (all[it].id != store.current.id && store.open(all[it].id))
				go(JSONObject().put("resume", store.current.session))
		}
	}

	/** To another chat: the panel shows it, and the bridge is told. */
	private fun go(bridge: JSONObject) {
		recall()
		if (::list.isInitialized && showing) {
			list.removeAllViews()
			messages.forEach { add(it) }
		}
		server.toBridge(bridge)
		greet()
		refreshStatus()
		toEnd()
	}

	private fun showKeyboards() {
		val pad = settings.padKeyboard
		menu.choose("Keyboard", listOf(
			menu.choiceRow("Android", !pad, "Touch, with Japanese and the microphone"),
			menu.choiceRow("D-pad", pad, "Letters and digits, without touching the screen")), if (pad) 1 else 0) {
			settings.padKeyboard = it == 1
			if (mine)
				go(typing)
		}
	}

	private fun showModels() {
		val all = ChatSettings.MODELS
		menu.choose("Model", all.map { menu.choiceRow(it.name, it.id == settings.model, it.note) },
			all.indexOfFirst { it.id == settings.model }) { set(all[it].id, settings.effort) }
	}

	private fun showEfforts() {
		val all = ChatSettings.EFFORTS
		menu.choose("Effort", all.map { menu.choiceRow(it.id, it.id == settings.effort, it.note.ifEmpty { null }) },
			all.indexOfFirst { it.id == settings.effort }) { set(settings.model, all[it].id) }
	}

	private fun set(model: String, effort: String) {
		if (model == settings.model && effort == settings.effort)
			return
		settings.set(model, effort)
		server.toBridge(JSONObject().put("settings", settings.json()))
		note("From the next message: ${settings.modelName}, effort ${settings.effort}.")
	}

	private fun newChat() {
		if (working)
			return
		store.startNew()
		go(JSONObject().put("new", true))
		toChat()
	}

	/* ---- The panel ---- */

	private fun bubble(m: Message): TextView = TextView(activity).apply {
		setTextColor(if (m.who == Who.USER) AppTheme.current.onAccent else if (m.who == Who.NOTE) faint else ink)
		setTextSize(TypedValue.COMPLEX_UNIT_SP, if (m.who == Who.NOTE) 12f else 14f)
		setLineSpacing(0f, 1.1f)
		setPadding(dp(10), dp(7), dp(10), dp(7))
		setTextIsSelectable(false)
		if (m.who != Who.NOTE)
			background = GradientDrawable().apply {
				cornerRadius = dp(12).toFloat()
				setColor(if (m.who == Who.USER) AppTheme.current.accent else AppTheme.current.surfaceInset)
			}
	}

	private fun add(m: Message) {
		val v = bubble(m)
		m.view = v
		list.addView(v, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
			gravity = if (m.who == Who.USER) Gravity.END else Gravity.START
			setMargins(if (m.who == Who.USER) dp(36) else 0, dp(4), 0, dp(4))
		})
		show(m)
	}

	private fun show(m: Message) {
		val v = m.view ?: return
		val work = m.work()
		val text = m.text.trim()
		v.text = when {
			work.isEmpty() && text.isEmpty() -> "…"
			work.isEmpty() -> text
			else -> android.text.SpannableStringBuilder().apply {
				append(work)
				setSpan(android.text.style.ForegroundColorSpan(AppTheme.current.accent), 0, work.length, 0)
				setSpan(android.text.style.RelativeSizeSpan(0.86f), 0, work.length, 0)
				if (text.isNotEmpty())
					append("\n").append(text)
			}
		}
		toEnd()
	}

	private fun toEnd() {
		if (::scroll.isInitialized)
			scroll.post { scroll.smoothScrollTo(0, list.height) }
	}

	private fun append(m: Message) {
		messages.add(m)
		while (messages.size > MAX_MESSAGES) {
			messages.removeAt(0)
			if (showing)
				list.removeViewAt(0)
		}
		if (showing)
			add(m)
	}

	private fun note(text: String) {
		append(Message(Who.NOTE, text))
		refreshStatus()
	}

	private fun greet() {
		if (messages.isEmpty())
			note("Say what the patch should do. Claude adds modules, patches cables and turns knobs" +
				(if (settings.making) ", and makes a module that is not there." else ".") + " Each answer is one step of Undo.")
	}

	private fun refreshStatus() {
		if (!showing)
			return
		status.text = when {
			stopping -> "Stopping…"
			working && unsent != null -> "Starting Claude Code…"
			working && doing != null -> "$doing… ${(SystemClock.uptimeMillis() - doingSince) / 1000} s"
			working -> "Working… ${(SystemClock.uptimeMillis() - askedAt) / 1000} s"
			server.bridgeConnected -> "${settings.modelName} · ${settings.effort} ▾"
			startedAt > 0L -> "Starting Claude Code…"
			else -> "Not connected"
		}
		send.setImageResource(if (working) R.drawable.ic_tb_close else R.drawable.ic_chat_send)
		send.contentDescription = if (working) "Stop" else "Send"
		help.text = when {
			!mine -> "The controller is at the rack\nhold START  to the chat"
			typing -> "A type   Y erase   START send   X read   B rack\nthe sticks are the rack's"
			working -> "↑↓ scroll   X settings   Y stop   B rack\nhold B  close: it goes on"
			else -> "A type   X settings   Y undo the answer   B rack\nhold B  close"
		}
		edge.setBackgroundColor(if (mine) AppTheme.current.accent else Color.TRANSPARENT)
	}

	/** The controller to the rack, the panel staying where it is. */
	private fun toRack() {
		mine = false
		typing = false
		stays = false
		stickAtRest()
		hideIme()
		dialog?.window?.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
		if (!showing)
			return
		showPadKeyboard()
		refreshStatus()
		toEnd()
	}

	/** The controller to the chat: to type, or to read if it is answering. */
	private fun toChat() = go(!working)

	private fun go(toTyping: Boolean) {
		typing = toTyping
		mine = true
		stays = false
		val w = dialog?.window ?: return
		val had = input.hasWindowFocus()
		w.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
		showPadKeyboard()
		if (!settings.padKeyboard) {
			if (!typing) {
				stays = true
				hideIme()
			} else if (had)
				showIme()
			else
				// Asked for before the window has the focus, it does not come.
				showImeSoon()
		}
		refreshStatus()
		toEnd()
	}

	private fun showImeSoon() {
		for (ms in longArrayOf(120, 350))
			input.postDelayed({ if (mine && typing && !imeShown && input.hasWindowFocus()) showIme() }, ms)
	}

	private fun showPadKeyboard() {
		keyboard.visibility = if (mine && typing && settings.padKeyboard && !imeShown) View.VISIBLE else View.GONE
	}

	private fun icon(res: Int, what: String, onClick: () -> Unit) = ImageButton(activity).apply {
		setImageResource(res)
		imageTintList = android.content.res.ColorStateList.valueOf(ink)
		scaleType = ImageView.ScaleType.FIT_CENTER
		contentDescription = what
		background = null
		isFocusable = false
		setPadding(dp(8), dp(8), dp(8), dp(8))
		setOnClickListener { onClick() }
	}

	private fun open() {
		val context: Context = activity
		list = LinearLayout(context).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(10), dp(4), dp(10), dp(4))
		}
		scroll = ScrollView(context).apply {
			isFocusable = false
			isFillViewport = false
			addView(list)
		}
		status = TextView(context).apply {
			setTextColor(faint)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
			setPadding(0, dp(8), 0, dp(8))
			setOnClickListener { showSettings() }
		}
		input = EditText(context).apply {
			setText(draft)
			setSelection(draft.length)
			hint = "Ask for a change"
			setTextColor(ink)
			setHintTextColor(AppTheme.current.textDisabled)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
			maxLines = 3
			background = GradientDrawable().apply {
				cornerRadius = dp(12).toFloat()
				setColor(AppTheme.current.surfaceInset)
			}
			setPadding(dp(12), dp(8), dp(12), dp(8))
			inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
				InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
			imeOptions = EditorInfo.IME_ACTION_SEND or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
				EditorInfo.IME_FLAG_NO_FULLSCREEN
			// A multi-line field that still sends with the keyboard's key.
			setRawInputType(InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES)
			setOnEditorActionListener { _, _, _ ->
				ask()
				true
			}
			setOnClickListener { go(true) }
			// A click does not come of the touch that takes the focus.
			setOnTouchListener { _, e ->
				if (e.action == MotionEvent.ACTION_UP && !mine)
					go(true)
				false
			}
		}
		send = ImageButton(context).apply {
			imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.onAccent)
			scaleType = ImageView.ScaleType.FIT_CENTER
			setPadding(dp(9), dp(9), dp(9), dp(9))
			isFocusable = false
			background = GradientDrawable().apply {
				shape = GradientDrawable.OVAL
				setColor(AppTheme.current.accent)
			}
			setOnClickListener { if (working) stopAnswer() else ask() }
		}
		keyboard = PadKeyboard(context, ink, "send", { type(it) }, { erase() }, { ask() })
		help = TextView(context).apply {
			setTextColor(faint)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
			gravity = Gravity.CENTER
			setPadding(0, dp(3), 0, dp(6))
		}
		val header = LinearLayout(context).apply {
			orientation = LinearLayout.HORIZONTAL
			gravity = Gravity.CENTER_VERTICAL
			setPadding(dp(14), dp(4), dp(4), 0)
			addView(TextView(context).apply {
				text = "Chat"
				setTextColor(ink)
				setTypeface(AppFont.get(context), Typeface.BOLD)
				setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
				setPadding(0, 0, dp(10), 0)
			})
			addView(status, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
			addView(icon(R.drawable.ic_tb_undo, "Undo the last answer") { undoLast() }, LinearLayout.LayoutParams(dp(38), dp(38)))
			addView(icon(R.drawable.ic_chat_new, "New chat") { newChat() }, LinearLayout.LayoutParams(dp(38), dp(38)))
			addView(icon(R.drawable.ic_tb_close, "Close") { close() }, LinearLayout.LayoutParams(dp(38), dp(38)))
		}
		edge = View(context)
		val column = LinearLayout(context).apply {
			orientation = LinearLayout.VERTICAL
			addView(header)
			addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
			addView(LinearLayout(context).apply {
				orientation = LinearLayout.HORIZONTAL
				gravity = Gravity.BOTTOM
				setPadding(dp(8), dp(4), dp(8), dp(4))
				addView(input, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
				addView(send, LinearLayout.LayoutParams(dp(40), dp(40)).apply { leftMargin = dp(6) })
			})
			addView(keyboard, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
				setMargins(dp(4), 0, dp(4), 0)
			})
			addView(help)
		}
		val root = LinearLayout(context).apply {
			orientation = LinearLayout.HORIZONTAL
			setBackgroundColor(AppTheme.withAlpha(AppTheme.current.surface, 96))
			// Lit while the controller is here.
			addView(edge, LinearLayout.LayoutParams(dp(3), ViewGroup.LayoutParams.MATCH_PARENT))
			addView(column, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
		}
		root.viewTreeObserver.addOnGlobalLayoutListener {
			val insets = ViewCompat.getRootWindowInsets(root)
			val shown = insets?.isVisible(WindowInsetsCompat.Type.ime()) == true
			if (showing)
				nativeChatInset(dp(WIDTH_DP), if (shown) insets!!.getInsets(WindowInsetsCompat.Type.ime()).bottom else 0)
			if (shown != imeShown) {
				imeShown = shown
				// Come up by a touch on the field: typing. Put away by its own key
				// or a swipe: done with it.
				if (!settings.padKeyboard && mine) {
					typing = shown
					if (!shown && !stays)
						toRack()
				}
				stays = false
				showPadKeyboard()
				refreshStatus()
				help.visibility = if (shown) View.GONE else View.VISIBLE
				toEnd()
			}
		}

		val d = object : Dialog(activity) {
			override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
				stick(ev)
				return super.dispatchGenericMotionEvent(ev)
			}

			/** The stick let go goes to whatever window has the focus by then. */
			override fun onWindowFocusChanged(hasFocus: Boolean) {
				super.onWindowFocusChanged(hasFocus)
				if (!hasFocus)
					stickAtRest()
			}
		}
		dialog = d
		d.requestWindowFeature(Window.FEATURE_NO_TITLE)
		d.setContentView(root)
		d.setCanceledOnTouchOutside(false)
		d.setOnKeyListener { _, code, event -> key(code, event) }
		d.setOnDismissListener {
			draft = input.text.toString()
			dialog = null
			imeShown = false
			mine = false
			stickAtRest()
			nativeChatInset(0, 0)
		}
		d.window?.apply {
			setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
			setGravity(Gravity.END or Gravity.TOP)
			clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
			// The rack beside it stays the finger's.
			addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN)
			setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE or
				WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN)
			@Suppress("DEPRECATION")
			decorView.systemUiVisibility = activity.window.decorView.systemUiVisibility
		}
		d.show()
		d.window?.setLayout(dp(WIDTH_DP), ViewGroup.LayoutParams.MATCH_PARENT)
		nativeChatInset(dp(WIDTH_DP), 0)
		messages.forEach { add(it) }
		greet()
		input.requestFocus()
		connect()
		toChat()
	}

	fun close() {
		hideIme()
		dialog?.dismiss()
	}

	private fun type(s: String) {
		val at = input.selectionStart.coerceAtLeast(0)
		input.text.replace(at, input.selectionEnd.coerceAtLeast(at), s)
	}

	private fun erase() {
		val at = input.selectionStart.coerceAtLeast(0)
		val end = input.selectionEnd.coerceAtLeast(at)
		if (end > at) input.text.delete(at, end)
		else if (at > 0) input.text.delete(Character.offsetByCodePoints(input.text, at, -1), at)
	}

	private fun showIme() {
		val w = dialog?.window ?: return
		input.requestFocus()
		androidx.core.view.WindowCompat.getInsetsController(w, input).show(WindowInsetsCompat.Type.ime())
	}

	private fun hideIme() {
		if (imeShown)
			(activity.getSystemService(Activity.INPUT_METHOD_SERVICE) as InputMethodManager)
				.hideSoftInputFromWindow(input.windowToken, 0)
	}

	private var hatX = 0f
	private var hatY = 0f

	/** The sticks and the triggers are the rack's, whatever has the keys. */
	private fun stick(ev: MotionEvent) {
		if (!ev.isFromSource(InputDevice.SOURCE_JOYSTICK) || ev.action != MotionEvent.ACTION_MOVE)
			return
		hatX = ev.getAxisValue(MotionEvent.AXIS_HAT_X)
		hatY = ev.getAxisValue(MotionEvent.AXIS_HAT_Y)
		val l2 = maxOf(ev.getAxisValue(MotionEvent.AXIS_BRAKE), ev.getAxisValue(MotionEvent.AXIS_LTRIGGER))
		val r2 = maxOf(ev.getAxisValue(MotionEvent.AXIS_GAS), ev.getAxisValue(MotionEvent.AXIS_RTRIGGER))
		nativeChatSticks(ev.getAxisValue(MotionEvent.AXIS_X), ev.getAxisValue(MotionEvent.AXIS_Y),
			ev.getAxisValue(MotionEvent.AXIS_Z), ev.getAxisValue(MotionEvent.AXIS_RZ), l2, r2)
	}

	private fun stickAtRest() {
		holds.values.forEach { ui.removeCallbacks(it) }
		hatX = 0f
		hatY = 0f
		nativeChatSticks(0f, 0f, 0f, 0f, 0f, 0f)
		nativeChatGate(false)
	}

	/** The D-pad keys that are held because the left stick is. */
	private val fromStick = HashSet<Int>()

	/** Android makes D-pad keys of a stick that nobody has taken, as it does of a
	 * D-pad that is an axis (the Nova's). Only the latter are meant. */
	private fun madeOfStick(code: Int, event: KeyEvent): Boolean {
		if (code !in KeyEvent.KEYCODE_DPAD_UP..KeyEvent.KEYCODE_DPAD_RIGHT || event.flags and KeyEvent.FLAG_FALLBACK == 0 ||
			!event.isFromSource(InputDevice.SOURCE_JOYSTICK))
			return false
		if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
			val dpad = when (code) {
				KeyEvent.KEYCODE_DPAD_UP -> hatY < -HAT_THRESHOLD
				KeyEvent.KEYCODE_DPAD_DOWN -> hatY > HAT_THRESHOLD
				KeyEvent.KEYCODE_DPAD_LEFT -> hatX < -HAT_THRESHOLD
				else -> hatX > HAT_THRESHOLD
			}
			if (dpad) fromStick.remove(code) else fromStick.add(code)
		}
		return if (event.action == KeyEvent.ACTION_UP) fromStick.remove(code) else code in fromStick
	}

	/** Buttons that do one thing let go and another held, and whether the one
	 * that is down now has been held. */
	private var heldLong = false
	private val holds = HashMap<Int, Runnable>()

	private fun tapOrHold(code: Int, first: Boolean, released: Boolean, tap: () -> Unit, hold: () -> Unit) {
		if (first) {
			heldLong = false
			val r = Runnable {
				heldLong = true
				hold()
			}
			holds[code] = r
			ui.postDelayed(r, HOLD_MS)
		} else if (released) {
			holds.remove(code)?.let { ui.removeCallbacks(it) }
			if (!heldLong)
				tap()
		}
	}

	private fun leave() {
		if (imeShown) {
			stays = true
			hideIme()
		} else
			toRack()
	}

	/** A release only counts if this window saw the press: see GamepadMenu. */
	private val pressedHere = HashSet<Int>()

	private fun key(code: Int, event: KeyEvent): Boolean {
		if (madeOfStick(code, event))
			return true
		val down = event.action == KeyEvent.ACTION_DOWN
		val first = down && event.repeatCount == 0
		val released = if (down) {
			if (first)
				pressedHere.add(code)
			false
		} else pressedHere.remove(code)
		val held = down && code in pressedHere
		// With Android's keyboard up, the D-pad moves in the text.
		if (imeShown && code in KeyEvent.KEYCODE_DPAD_UP..KeyEvent.KEYCODE_DPAD_CENTER)
			return false
		when (code) {
			KeyEvent.KEYCODE_DPAD_UP -> if (down) {
				if (typing && settings.padKeyboard) keyboard.move(0, -1) else scroll.smoothScrollBy(0, -dp(60))
			}
			KeyEvent.KEYCODE_DPAD_DOWN -> if (down) {
				if (typing && settings.padKeyboard) keyboard.move(0, 1) else scroll.smoothScrollBy(0, dp(60))
			}
			KeyEvent.KEYCODE_DPAD_LEFT -> if (down && typing) keyboard.move(-1, 0)
			KeyEvent.KEYCODE_DPAD_RIGHT -> if (down && typing) keyboard.move(1, 0)
			KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> {
				if (typing && settings.padKeyboard) {
					if (first) {
						if (imeShown) hideIme() else keyboard.press()
					}
				} else if (!imeShown && released && !working)
					go(true)
			}
			KeyEvent.KEYCODE_BUTTON_Y -> {
				if (typing) {
					if (held)
						erase()
				} else if (released) {
					if (working) stopAnswer() else undoLast()
				}
			}
			KeyEvent.KEYCODE_BUTTON_X -> if (released) {
				if (typing) go(false) else showSettings()
			}
			KeyEvent.KEYCODE_BUTTON_B -> tapOrHold(code, first, released, { leave() }, { close() })
			KeyEvent.KEYCODE_BUTTON_START -> tapOrHold(code, first, released,
				{ if (typing) ask() else if (!working) go(true) }, { toRack() })
			KeyEvent.KEYCODE_BUTTON_SELECT -> tapOrHold(code, first, released,
				{ togglePause() }, { command(GamepadMenu.CMD_CYCLE_ZOOM) })
			KeyEvent.KEYCODE_BUTTON_THUMBL -> if (first) nativeChatGate(true) else if (!down) nativeChatGate(false)
			KeyEvent.KEYCODE_BUTTON_THUMBR -> if (first) command(GamepadMenu.CMD_PLAY_MODE)
			// They come along with the triggers' axes, which are what is read.
			KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2 -> {}
			KeyEvent.KEYCODE_BUTTON_L1 -> if (down) scroll.smoothScrollBy(0, -scroll.height * 3 / 4)
			KeyEvent.KEYCODE_BUTTON_R1 -> if (down) scroll.smoothScrollBy(0, scroll.height * 3 / 4)
			KeyEvent.KEYCODE_BACK -> if (released) {
				if (imeShown) leave() else close()
			}
			else -> return false
		}
		return true
	}

	private external fun nativeChatInset(right: Int, bottom: Int)
	private external fun nativeChatSticks(lx: Float, ly: Float, rx: Float, ry: Float, l2: Float, r2: Float)
	private external fun nativeChatGate(down: Boolean)
}
