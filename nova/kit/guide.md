A module is one C++17 class. Everything is included already; there are no #include lines. Example, complete:

struct Folder : nova::Module {
	enum Params { FOLD, SYM, CV_AMOUNT, PARAMS };
	enum Inputs { IN, CV, INPUTS };
	enum Outputs { OUT, OUTPUTS };
	enum Lights { LIGHTS };
	int shape = 0;
	std::array<float, 128> trace{};
	size_t at = 0;

	Folder() {
		config(PARAMS, INPUTS, OUTPUTS, LIGHTS);
		configParam(FOLD, 0.f, 1.f, 0.3f, "Fold", "%", 0.f, 100.f);
		configParam(SYM, -1.f, 1.f, 0.f, "Symmetry");
		configParam(CV_AMOUNT, -1.f, 1.f, 0.f, "Fold CV amount");
		configInput(IN, "Audio");
		configInput(CV, "Fold CV");
		configOutput(OUT, "Audio");
		configBypass(IN, OUT);
		menuChoice("shape", "Shape", {"Sine", "Triangle"}, &shape);
		panel.title = "FOLDER";
		panel.row({big(FOLD)});
		panel.display(60);
		panel.row({knob(SYM), trim(CV_AMOUNT, "CV amt")});
		panel.row({in(IN), in(CV), out(OUT)});
	}

	void process(const ProcessArgs& args) override {
		float cv = params[CV_AMOUNT].getValue() * inputs[CV].getVoltage() / 10.f;
		float fold = clamp(params[FOLD].getValue() + cv, 0.f, 1.f);
		int n = std::max(1, inputs[IN].getChannels());
		for (int c = 0; c < n; c++) {
			float x = inputs[IN].getVoltage(c) / 5.f * (1.f + 7.f * fold) + params[SYM].getValue();
			outputs[OUT].setVoltage(5.f * std::sin(x * float(M_PI) / 2.f), c);
		}
		outputs[OUT].setChannels(n);
		if (args.frame % 32 == 0) {
			at = (at + 1) % trace.size();
			trace[at] = outputs[OUT].getVoltage();
		}
	}

	void draw(NVGcontext* vg, Vec size) override {
		nvgBeginPath(vg);
		for (size_t i = 0; i < trace.size(); i++) {
			float x = size.x * i / trace.size();
			float y = size.y * (0.5f - trace[(at + 1 + i) % trace.size()] / 14.f);
			if (i) nvgLineTo(vg, x, y); else nvgMoveTo(vg, x, y);
		}
		nvgStrokeColor(vg, panel.accent);
		nvgStrokeWidth(vg, 1.5f);
		nvgStroke(vg);
	}
};

The class is named as the module's id: a capital letter, then letters and digits. Helper structs may stand before it.

A module may override only: process(args), reset() (Initialize), rateChanged(), draw(vg, size). Not onReset, dataToJson and so on.

Rack's own: config, configParam(id, min, max, default, name, unit, displayBase, displayMultiplier, displayOffset) (displayBase 2 or 10: exponential; for a pitch knob in octaves: configParam(F, -4.f, 4.f, 0.f, "Frequency", " Hz", 2, dsp::FREQ_C4)), configSwitch(id, 0.f, 2.f, 0.f, "Mode", {"A", "B", "C"}), configButton(id, name), configInput, configOutput, configLight(id, name), configBypass(in, out), getParamQuantity(id)->snapEnabled = true. params[i].getValue()/setValue(v), inputs[i].getVoltage(c), getVoltageSum(), getPolyVoltage(c), getNormalVoltage(v, c), getChannels(), isConnected(), outputs[i].setVoltage(v, c), setChannels(n), lights[i].setBrightness(0..1), setBrightnessSmooth(v, args.sampleTime). args.sampleRate, args.sampleTime, args.frame. clamp, crossfade, rescale, dsp::SchmittTrigger (process(v, 0.1f, 1.f) is true once as it rises), dsp::BooleanTrigger (buttons), dsp::PulseGenerator (trigger(1e-3f); process(sampleTime)), dsp::ClockDivider, dsp::TRCFilter<float>, dsp::TBiquadFilter<float>, dsp::ExponentialSlewLimiter, dsp::exp2_taylor5(x), dsp::FREQ_C4, random::uniform(), random::normal(), simd::float_4 for many voices.

The kit's: 
- keep("key", &x): x (float, int, bool, std::string, std::array or std::vector of those; the length never changes) is kept in the patch, and put back to its present value by Initialize. After loading, an int kept like that can be anything: clamp it before it is an index.
- menuChoice("key", label, {names}, &int), menuSwitch("key", label, &bool), menuSlider("key", label, &float, min, max, unit): rows of the module's menu, kept too.
- Delay d; d.resize(samples) (in the constructor and rateChanged), d.push(v), d.read(samplesAgo) (between samples too), d.clear().
- Filter f; f.process(x, hz, resonance 0..1, args.sampleTime); then f.low, f.band, f.high.
- Smooth s; s.process(target, seconds, args.sampleTime).
- sampleRate: right in the constructor, rateChanged() and process().

Panel: rows from the top down, of big(p) knob(p) small(p) trim(p) in(i) out(o) light(l) gap(), each with an optional short label as second argument (else the configured name; labels have room for about 8 letters, so give short ones). knob() of a configSwitch is a switch (2 or 3 positions), of a configButton a button. display(height) is the room for draw(). 332 px of height in all: a row of big knobs takes 60, knobs 44, jacks 39, trims 33; so at most 7 or 8 rows. Four to six in a row is fine. The width follows; panel.width = HP makes it wider. panel.color, panel.accent = nvgRGB(r, g, b). What the panel leaves out is put below.

It must be right at any sample rate, with anything in any input, unconnected inputs, every position of every knob. Outputs about +-5 V for audio, 0..10 V for envelopes and gates, 1 V/octave with 0 V = C4. A division by what can be 0, a log or sqrt of what can be negative, a phase reduced with while (use x -= std::floor(x)) and feedback above 1 without a limit are the usual mistakes. The state of a filter or a feedback loop must not grow without end: clamp it, or use std::tanh.

process() runs 48000 times a second on a handheld: no vectors, strings or containers that grow in it; std::sin, exp and pow are fine a few times per frame, not per voice and partial: work slow things out every 16 frames (args.frame % 16 == 0) and smooth them. draw() is called 60 times a second; only lines, shapes and nvgText.

Buffers are std::array or std::vector, indexed with [] - every index is checked, and an index out of range stops the module. No raw pointers, C arrays, new, malloc, casts of pointers, threads, files, network, printing, APP, preprocessor lines.

A changed module is exchanged in the patch at once, with its cables and knobs by their numbers: keep the order of the enums, and add new ones at the end.

probes (make_module, edit_module): up to 6 runs of the finished module, to see that it does what it should. Each is {"in": {"Audio": "sine 220", "V/oct": 1.0, "Gate": "gate 2", "CV": "noise"}, "set": {"Fold": 0.8}, "seconds": 0.5}; a sine is +-5 V, a gate 0/10 V at that many Hz. They answer with each output's range, mean, last value and frequency. Use them: pitch at 0 V and 1 V, an envelope after a gate, the gain of a filter below and above its cutoff.
