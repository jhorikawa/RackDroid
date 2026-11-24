package org.rackdroid

import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.Normalizer

/** What the module pickers know about a module, for searching. */
interface ModuleText {
	val key: String
	val name: String
	val brand: String
	val plugin: String
	val tags: List<String>
	val description: String
}

/**
 * Finding a module by what it does.
 *
 * A module's name often gives no hint of that -- the one that draws a wave is
 * called "Scope" -- and the fifteen categories of the pickers leave most of
 * Rack's fifty-seven tags out. So there are three things to search by here:
 * the tags the plugin's maker chose, in other words for them (Japanese ones
 * included), and tags of one's own.
 *
 * A tag of one's own belongs to the model, not to a module in a patch: it is
 * kept beside the app's patches in module-tags.json, and patches do not change.
 */
object ModuleTags {
	/** @param ja its Japanese name. @param words what else it may be looked for by. */
	class BuiltIn(val name: String, val ja: String, words: String) {
		internal val folded = fold("$name $ja $words")
		/** The English ones, as they would stand in a module's name. */
		internal val inNames = fold("$name $words").split(' ').filter { w -> w.length > 2 && w.all { it in 'a'..'z' } }
	}

	/** Rack's own tags (tag.cpp), in other words. */
	val builtIns: List<BuiltIn> = listOf(
		BuiltIn("Arpeggiator", "アルペジエーター", "arp アルペジオ 分散和音"),
		BuiltIn("Attenuator", "アッテネーター", "attenuverter level 減衰 音量 レベル 反転"),
		BuiltIn("Blank", "ブランク", "panel 空白 飾り"),
		BuiltIn("Chorus", "コーラス", "揺れ"),
		BuiltIn("Clock generator", "クロック", "clock tempo bpm テンポ 拍 同期 ジェネレーター"),
		BuiltIn("Clock modulator", "クロック変調", "clock divider multiplier swing 分周 逓倍 クロック・モジュレーター"),
		BuiltIn("Compressor", "コンプレッサー", "dynamics 圧縮"),
		BuiltIn("Controller", "コントローラー", "knob slider fader 操作 つまみ"),
		BuiltIn("Delay", "ディレイ", "echo エコー 遅延 やまびこ"),
		BuiltIn("Digital", "デジタル", ""),
		BuiltIn("Distortion", "ディストーション", "overdrive fuzz saturation 歪み ひずみ"),
		BuiltIn("Drum", "ドラム", "drums percussion kick snare hihat 打楽器 パーカッション リズム"),
		BuiltIn("Dual", "デュアル", "2系統"),
		BuiltIn("Dynamics", "ダイナミクス", "音量 強弱"),
		BuiltIn("Effect", "エフェクト", "fx effects エフェクター 効果"),
		BuiltIn("Envelope follower", "エンベロープ・フォロワー", "追従 音量検出"),
		BuiltIn("Envelope generator", "エンベロープ", "adsr eg envelope 包絡 立ち上がり ジェネレーター"),
		BuiltIn("Equalizer", "イコライザー", "eq 音質"),
		BuiltIn("Expander", "エクスパンダー", "拡張"),
		BuiltIn("External", "外部", "外部機器 接続"),
		BuiltIn("Filter", "フィルター", "vcf lowpass highpass bandpass cutoff ローパス ハイパス バンドパス 濾過"),
		BuiltIn("Flanger", "フランジャー", ""),
		BuiltIn("Function generator", "ファンクション・ジェネレーター", "function 関数 slope"),
		BuiltIn("Granular", "グラニュラー", "grain texture 粒 グレイン"),
		BuiltIn("Hardware clone", "ハードウェア・クローン", "hardware 実機 移植"),
		BuiltIn("Limiter", "リミッター", ""),
		BuiltIn("Logic", "ロジック", "and or xor boolean 論理"),
		BuiltIn("Low-frequency oscillator", "LFO", "低周波 揺らし モジュレーション modulation"),
		BuiltIn("Low-pass gate", "ローパス・ゲート", "lpg"),
		BuiltIn("MIDI", "MIDI", "keyboard 鍵盤 キーボード"),
		BuiltIn("Mixer", "ミキサー", "mix sum 混ぜる 足し合わせ 合成"),
		BuiltIn("Multiple", "マルチプル", "mult split copy 分岐 複製"),
		BuiltIn("Noise", "ノイズ", "white pink 雑音"),
		BuiltIn("Oscillator", "オシレーター", "vco sound source 発振 音源"),
		BuiltIn("Panning", "パンニング", "pan stereo 定位 左右 ステレオ"),
		BuiltIn("Phaser", "フェイザー", ""),
		BuiltIn("Physical modeling", "物理モデリング", "physical string 弦 フィジカル"),
		BuiltIn("Polyphonic", "ポリフォニック", "poly 和音 多声"),
		BuiltIn("Quad", "クワッド", "4系統"),
		BuiltIn("Quantizer", "クオンタイザー", "scale note 音階 音程 補正 スケール"),
		BuiltIn("Random", "ランダム", "chance probability 乱数 確率 偶然"),
		BuiltIn("Recording", "レコーディング", "record 録音"),
		BuiltIn("Reverb", "リバーブ", "room hall 残響"),
		BuiltIn("Ring modulator", "リング・モジュレーター", "ring リング変調"),
		BuiltIn("Sample and hold", "サンプル&ホールド", "s&h sh track hold 保持"),
		BuiltIn("Sampler", "サンプラー", "sample playback 再生"),
		BuiltIn("Sequencer", "シーケンサー", "sequence step pattern ステップ 自動演奏 パターン"),
		BuiltIn("Slew limiter", "スルー・リミッター", "slew glide portamento lag なめらか ポルタメント"),
		BuiltIn("Speech", "スピーチ", "voice 声 しゃべる"),
		BuiltIn("Switch", "スイッチ", "selector router 切替 切り替え"),
		BuiltIn("Synth voice", "シンセ・ボイス", "voice 音源 シンセ"),
		BuiltIn("Tuner", "チューナー", "tuning pitch 調律 音程"),
		BuiltIn("Utility", "ユーティリティ", "tool 道具 便利"),
		BuiltIn("Visual", "可視化", "scope oscilloscope waveform meter spectrum analyzer display view " +
			"波形 波 表示 オシロスコープ スコープ メーター 見る 画面 ビジュアル スペクトラム アナライザー かしか はけい"),
		BuiltIn("Vocoder", "ボコーダー", "声"),
		BuiltIn("Voltage-controlled amplifier", "VCA", "amplifier amp 増幅 音量 アンプ"),
		BuiltIn("Waveshaper", "ウェーブシェイパー", "wavefolder fold shape 波形整形 折り返し"),
	)
	private val builtInByName = builtIns.associateBy { it.name }

	fun builtIn(name: String): BuiltIn? = builtInByName[name]

	// ---- matching ----

	/** One spelling for what is the same word: full and half width, upper and
	 * lower case, hiragana and katakana, with and without the middle dot. */
	fun fold(s: String): String {
		val n = Normalizer.normalize(s, Normalizer.Form.NFKC).lowercase()
		val out = StringBuilder(n.length)
		for (c in n) {
			when {
				c == '・' -> {}
				c in 'ぁ'..'ゖ' -> out.append(c + 0x60)
				else -> out.append(c)
			}
		}
		return out.toString()
	}

	/** The words of a query; every one of them has to be found. */
	fun words(query: String): List<String> =
		fold(query).split(' ', '\t').filter { it.isNotEmpty() && it != "#" }.map { it.removePrefix("#") }

	/** How well a module answers a query: 0 not at all, more is better. The
	 * least of what its words score, so one word found only in passing does
	 * not ride on another. */
	fun score(m: ModuleText, words: List<String>): Int {
		if (words.isEmpty())
			return 1
		val name = fold(m.name)
		val origin = MutableOrigin.of(m.key, m.description)?.let { fold(it) }
		val own = of(m.key).map { fold(it) }
		val company = fold("${m.brand} ${m.plugin}")
		val description = fold(m.description)
		var least = Int.MAX_VALUE
		for (w in words) {
			val s = when {
				name == w || origin == w -> 9
				name.startsWith(w) || origin?.startsWith(w) == true -> 8
				own.any { it == w } -> 7
				name.contains(w) -> 6
				own.any { it.contains(w) } -> 5
				m.tags.any { it.contains(w, ignoreCase = true) || builtInByName[it]?.folded?.contains(w) == true } ->
					tagScore(m, name, w)
				company.contains(w) -> 3
				description.contains(w) -> 2
				else -> return 0
			}
			least = minOf(least, s)
		}
		return least
	}

	/** Among the modules with a tag, those whose name says the same come
	 * first ("波形" finds Scope before everything else tagged Visual), and
	 * blank panels, decorated ones as they are, come last. */
	private fun tagScore(m: ModuleText, name: String, w: String): Int {
		if ("Blank" in m.tags && builtInByName["Blank"]?.folded?.contains(w) != true)
			return 1
		val named = m.tags.any { t ->
			val b = builtInByName[t]
			b != null && b.folded.contains(w) && b.inNames.any { name.contains(it) }
		}
		return if (named) 5 else 4
	}

	/** Those that answer the query: the best first, and of equals the stock
	 * modules and then by name. */
	fun <T : ModuleText> search(all: List<T>, query: String): List<T> {
		val w = words(query)
		return all.map { it to score(it, w) }.filter { it.second > 0 }
			.sortedWith(compareBy({ -it.second }, { it.first.brand != "VCV" }, { it.first.name.lowercase() },
				{ it.first.brand.lowercase() }))
			.map { it.first }
	}

	// ---- tags of one's own ----

	private const val FILE = "module-tags.json"
	private var file: File? = null
	private var readAt = 0L
	/** Model key to its tags, in the order they were given. */
	private val own = LinkedHashMap<String, MutableList<String>>()

	/** Reads the tags, if the file is not the one read last: it can be put
	 * there from a computer (novasync tags). Call before showing a picker. */
	fun refresh(dir: File?) {
		val f = File(dir ?: return, FILE)
		val stamp = if (f.exists()) f.lastModified() else 0L
		if (file == f && stamp == readAt)
			return
		file = f
		readAt = stamp
		own.clear()
		if (stamp == 0L)
			return
		try {
			val tags = JSONObject(f.readText()).getJSONObject("tags")
			for (key in tags.keys()) {
				val list = tags.getJSONArray(key)
				for (i in 0 until list.length())
					add(key, list.getString(i))
			}
		} catch (t: Throwable) {
			// Not written over: whoever edited it may want to put it right.
			runCatching { f.copyTo(File(dir, "$FILE.unreadable"), overwrite = true) }
			android.util.Log.w("rackdroid.tags", "unreadable $f", t)
			own.clear()
		}
	}

	fun of(key: String): List<String> = own[key].orEmpty()

	/** Every tag in use, by name, with how many models have it. */
	fun inUse(): List<Pair<String, Int>> =
		own.values.flatten().groupingBy { it }.eachCount().toList().sortedBy { fold(it.first) }

	/** A tag as it is kept: no spaces at the ends, no #, and spelled as it
	 * already is elsewhere if it only differs in case or width. */
	fun normalize(text: String): String {
		val t = text.trim().removePrefix("#").trim().replace(Regex("\\s+"), " ")
		val f = fold(t)
		return inUse().firstOrNull { fold(it.first) == f }?.first ?: t
	}

	fun set(key: String, tag: String, on: Boolean) {
		if (tag.isEmpty())
			return
		if (on) add(key, tag)
		else {
			own[key]?.remove(tag)
			if (own[key]?.isEmpty() == true)
				own.remove(key)
		}
		save()
	}

	private fun add(key: String, tag: String) {
		val list = own.getOrPut(key) { ArrayList() }
		if (tag.isNotEmpty() && tag !in list)
			list.add(tag)
	}

	private fun save() {
		val f = file ?: return
		runCatching {
			val tags = JSONObject()
			for ((key, list) in own)
				tags.put(key, JSONArray(list))
			val text = JSONObject().put("version", 1).put("tags", tags).toString(1)
			f.parentFile?.mkdirs()
			val tmp = File(f.parentFile, "$FILE.tmp")
			tmp.writeText(text)
			if (!tmp.renameTo(f)) {
				f.writeText(text)
				tmp.delete()
			}
			readAt = f.lastModified()
		}.onFailure { android.util.Log.w("rackdroid.tags", "could not save $f", it) }
	}
}
