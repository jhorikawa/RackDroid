// Runs a module made from the chat before RackDroid does, in a process of its
// own that may die. GPL-3.0-or-later.
//
//     modtest check MODULE.so             every way it can be used, looking for mistakes
//     modtest speed MODULE.so [PROBES]    how fast it is, what comes out of it, and
//                                         two minutes of it, as fast as it goes
//
// "check" is built with the address and undefined behaviour sanitizers, as is
// the module it is given (nova/bridge/nova_build.py). Answers are lines of JSON;
// {"at": ...} says what is about to be tried, for when it does not come back.

#include <rack.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace rack;


/* ---- What the module may do to this device: nothing ---- */

static bool sandbox() {
	struct rlimit none = {0, 0};
	struct rlimit cpu = {60, 60};
	setrlimit(RLIMIT_FSIZE, &none);
	setrlimit(RLIMIT_CORE, &none);
	setrlimit(RLIMIT_CPU, &cpu);
#define DENY(nr) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM)
	struct sock_filter code[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
		DENY(__NR_execve), DENY(__NR_execveat), DENY(__NR_socket), DENY(__NR_connect), DENY(__NR_bind),
		DENY(__NR_unlinkat), DENY(__NR_renameat), DENY(__NR_renameat2), DENY(__NR_mkdirat), DENY(__NR_linkat),
		DENY(__NR_symlinkat), DENY(__NR_truncate), DENY(__NR_fchmodat), DENY(__NR_fchownat), DENY(__NR_ptrace),
		DENY(__NR_process_vm_writev), DENY(__NR_memfd_create), DENY(__NR_io_uring_setup), DENY(__NR_openat2),
		DENY(__NR_clone3), DENY(__NR_kill),
		// Files: reading only.
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_openat, 0, 4),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[2])),
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, O_WRONLY | O_RDWR | O_CREAT | O_TRUNC, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		// Threads, but no processes.
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 3),
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
		BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, 0x10000 /* CLONE_THREAD */, 1, 0),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {(unsigned short) (sizeof code / sizeof code[0]), code};
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return false;
	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}


/* ---- Memory asked for in process() ---- */

static bool g_counting = false;
static long g_allocations = 0;

#if !__has_feature(address_sanitizer)
void* operator new(size_t n) {
	if (g_counting)
		g_allocations++;
	void* p = malloc(n ? n : 1);
	if (!p)
		throw std::bad_alloc();
	return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }
#endif


/* ---- */

static double now() {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void say(json_t* j) {
	char* s = json_dumps(j, JSON_COMPACT | JSON_REAL_PRECISION(7));
	printf("%s\n", s ? s : "{}");
	fflush(stdout);
	free(s);
	json_decref(j);
}

static std::string g_at;

/** A call that does not come back. The sanitizer says where it is. */
static void onAlarm(int) {
#if __has_feature(address_sanitizer)
	(void) !write(2, "Alarm clock\n", 12);
	raise(SIGTRAP);
#else
	raise(SIGTRAP);
#endif
}

#if !__has_feature(address_sanitizer)
/** The sanitizer says where; without it, the places on the stack that are in the
module, for nova_build.py to look up. */
static void onFault(int signal, siginfo_t*, void* context) {
	ucontext_t* uc = (ucontext_t*) context;
	uintptr_t pc = uc->uc_mcontext.pc;
	uintptr_t* fp = (uintptr_t*) uc->uc_mcontext.regs[29];
	printf("fault %d", signal);
	for (int i = 0; i < 24 && pc; i++) {
		Dl_info info;
		if (dladdr((void*) pc, &info) && info.dli_fname && strstr(info.dli_fname, "release.so"))
			printf(" (release.so+0x%lx)", (unsigned long) (pc - (uintptr_t) info.dli_fbase - (i ? 4 : 0)));
		if (!fp || ((uintptr_t) fp & 7) || (uintptr_t) fp < (uintptr_t) &info)
			break;
		pc = fp[1];
		uintptr_t* next = (uintptr_t*) fp[0];
		if (next <= fp)
			break;
		fp = next;
	}
	printf("\n");
	fflush(stdout);
	_exit(4);
}
#endif

static void at(const std::string& what) {
	alarm(4);
	g_at = what;
	say(json_pack("{s:s}", "at", what.c_str()));
}

[[noreturn]] static void fail(const std::string& what) {
	say(json_pack("{s:s, s:s}", "fail", what.c_str(), "at", g_at.c_str()));
	_exit(3);
}

static uint32_t g_seed = 1;

static float noise() {
	g_seed = g_seed * 1664525u + 1013904223u;
	return (float) (g_seed >> 8) / (float) 0xffffff * 2.f - 1.f;
}


struct Lib {
	plugin::Plugin plugin;
	plugin::Model* model = nullptr;
	const char* (*describe)(engine::Module*) = nullptr;
	void (*draw)(engine::Module*, NVGcontext*, float, float) = nullptr;
	int (*stopped)(engine::Module*) = nullptr;
};

static Lib g_lib;

static void load(const char* path) {
	at("loading it");
	void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!handle)
		fail(dlerror());
	void (*init)(plugin::Plugin*) = (void (*)(plugin::Plugin*)) dlsym(handle, "init");
	g_lib.describe = (const char* (*)(engine::Module*)) dlsym(handle, "nova_describe");
	g_lib.draw = (void (*)(engine::Module*, NVGcontext*, float, float)) dlsym(handle, "nova_draw");
	g_lib.stopped = (int (*)(engine::Module*)) dlsym(handle, "nova_stopped");
	if (!init || !g_lib.describe || !g_lib.draw || !g_lib.stopped)
		fail("it is not a module of the kit");
	g_lib.plugin.slug = "NovaMade";
	g_lib.plugin.version = "2.0.0";
	init(&g_lib.plugin);
	if (g_lib.plugin.models.size() != 1)
		fail("it must hold one module");
	g_lib.model = g_lib.plugin.models.front();
}

static engine::Module* make() {
	at("making one (the constructor)");
	engine::Module* m = g_lib.model->createModule();
	m->id = 1;
	engine::Module::AddEvent e;
	m->onAdd(e);
	return m;
}


struct Rig {
	engine::Module* m;
	float rate = 48000.f;
	int64_t frame = 0;

	void setRate(float r) {
		rate = r;
		engine::Module::SampleRateChangeEvent e;
		e.sampleRate = r;
		e.sampleTime = 1.f / r;
		m->onSampleRateChange(e);
	}
	void connect(int inChannels, int outChannels) {
		for (engine::Input& i : m->inputs) {
			i.channels = inChannels;
			for (float& v : i.voltages)
				v = 0.f;
		}
		for (engine::Output& o : m->outputs) {
			o.channels = outChannels;
			for (float& v : o.voltages)
				v = 0.f;
		}
	}
	void step() {
		engine::Module::ProcessArgs a;
		a.sampleRate = rate;
		a.sampleTime = 1.f / rate;
		a.frame = frame++;
		m->process(a);
	}
	void defaults() {
		for (engine::ParamQuantity* pq : m->paramQuantities)
			m->params[pq->paramId].setValue(pq->defaultValue);
	}
	void setParam(int i, float part) {
		engine::ParamQuantity* pq = m->paramQuantities[i];
		float v = pq->minValue + (pq->maxValue - pq->minValue) * part;
		m->params[i].setValue(pq->snapEnabled ? std::round(v) : v);
	}
};


static void checkOutputs(Rig& r) {
	for (size_t o = 0; o < r.m->outputs.size(); o++) {
		engine::Output& out = r.m->outputs[o];
		if (out.channels > 16)
			fail(string::f("output %d was given %d channels", (int) o, (int) out.channels));
		for (int c = 0; c < out.channels; c++) {
			if (!std::isfinite(out.voltages[c]))
				fail(string::f("output %d \"%s\" is %f", (int) o, r.m->outputInfos[o]->name.c_str(), out.voltages[c]));
		}
	}
}


static float g_peak = 0.f;

static void run(Rig& r, const std::string& what, int frames, double seconds, std::function<float(int, int, int64_t)> input,
		bool turn = false) {
	at(string::f("process(), %s, at %g Hz", what.c_str(), r.rate));
	double until = now() + seconds;
	for (int f = 0; f < frames; f++) {
		if (input) {
			for (size_t i = 0; i < r.m->inputs.size(); i++) {
				engine::Input& in = r.m->inputs[i];
				for (int c = 0; c < in.channels; c++)
					in.voltages[c] = input((int) i, c, r.frame);
			}
		}
		if (turn && f % 64 == 0) {
			for (size_t p = 0; p < r.m->params.size(); p++)
				r.setParam((int) p, noise() * 0.5f + 0.5f);
		}
		r.step();
		// The guard's clean-up hides it; what the module itself set is wanted.
		if (g_lib.stopped(r.m))
			fail("an output was not a number (NaN or infinite) for a tenth of a second");
		checkOutputs(r);
		for (engine::Output& o : r.m->outputs) {
			for (int c = 0; c < o.channels; c++)
				g_peak = std::fmax(g_peak, std::fabs(o.voltages[c]));
		}
		if (f % 64 == 63 && now() > until && f > 512)
			return;
	}
}


/* ---- A NanoVG that draws nowhere ---- */

static NVGcontext* nowhere() {
	static NVGparams p;
	memset(&p, 0, sizeof p);
	p.renderCreate = [](void*, void*) { return 1; };
	p.renderCreateTexture = [](void*, int, int, int, int, const unsigned char*) { return 1; };
	p.renderDeleteTexture = [](void*, int) { return 1; };
	p.renderUpdateTexture = [](void*, int, int, int, int, int, const unsigned char*) { return 1; };
	p.renderGetTextureSize = [](void*, int, int* w, int* h) { *w = *h = 16; return 1; };
	p.renderViewport = [](void*, float, float, float) {};
	p.renderCancel = [](void*) {};
	p.renderFlush = [](void*) {};
	p.renderFill = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, const float*, const NVGpath*, int) {};
	p.renderStroke = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, float, float, const NVGpath*, int) {};
	p.renderTriangles = [](void*, NVGpaint*, NVGcompositeOperationState, NVGscissor*, const NVGvertex*, int, float) {};
	p.renderDelete = [](void*) {};
	return nvgCreateInternal(&p, NULL);
}

static void draw(engine::Module* m, NVGcontext* vg) {
	for (int i = 0; i < 4; i++) {
		nvgBeginFrame(vg, 300, 400, 1.f);
		nvgSave(vg);
		g_lib.draw(m, vg, 60.f + 60.f * i, 40.f + 20.f * i);
		nvgRestore(vg);
		nvgEndFrame(vg);
	}
}


/* ---- check ---- */

static void check() {
	engine::Module* m = make();
	at("reading its knobs, jacks and panel");
	say(json_pack("{s:s}", "describe", g_lib.describe(m)));
	if (m->inputs.size() > 64 || m->outputs.size() > 64 || m->params.size() > 128)
		fail("that is too many knobs or jacks");
	Rig r;
	r.m = m;
	NVGcontext* vg = nowhere();

	auto sine = [&](int i, int c, int64_t f) { return 5.f * std::sin(2.f * float(M_PI) * (220.f + 37.f * i + 5.f * c) * f / r.rate); };
	auto wild = [&](int, int, int64_t) { return 12.f * noise(); };
	auto gates = [&](int i, int, int64_t f) { return (f / (200 + 60 * i)) % 2 ? 10.f : 0.f; };
	auto steps = [&](int i, int, int64_t f) { return std::round(noise() * 60.f) / 12.f * ((f / 500 + i) % 2); };

	for (float rate : {44100.f, 96000.f, 12000.f}) {
		at(string::f("rateChanged() to %g Hz", rate));
		r.setRate(rate);
		r.defaults();
		r.connect(0, 0);
		run(r, "with nothing connected", 4000, 0.15, nullptr);
		r.connect(0, 1);
		run(r, "with only the outputs connected", 4000, 0.15, nullptr);
		r.connect(1, 1);
		run(r, "with sines in every input", 8000, 0.25, sine);
		run(r, "with gates in every input", 8000, 0.25, gates);
		run(r, "with steps of notes in every input", 4000, 0.15, steps);
		run(r, "with +12 V in every input", 2000, 0.1, [](int, int, int64_t) { return 12.f; });
		run(r, "with -12 V in every input", 2000, 0.1, [](int, int, int64_t) { return -12.f; });
		run(r, "with noise of 12 V in every input", 6000, 0.2, wild);
		run(r, "with noise in, and the knobs turned all the time", 8000, 0.3, wild, true);
		r.connect(16, 1);
		run(r, "with 16 channels in every input", 4000, 0.3, wild);
		run(r, "with 16 channels in, and the knobs turned all the time", 4000, 0.3, wild, true);
		r.connect(1, 1);
		for (size_t p = 0; p < m->params.size(); p++) {
			for (float part : {0.f, 1.f}) {
				r.defaults();
				r.setParam((int) p, part);
				run(r, string::f("with \"%s\" at its %s", m->paramQuantities[p]->name.c_str(), part > 0.f ? "maximum" : "minimum"),
					1200, 0.06, sine);
			}
		}
		for (float part : {0.f, 1.f}) {
			for (size_t p = 0; p < m->params.size(); p++)
				r.setParam((int) p, part);
			run(r, part > 0.f ? "with every knob at its maximum" : "with every knob at its minimum", 4000, 0.2, sine);
		}
		at("draw()");
		draw(m, vg);
	}

	at("saving and loading what it keeps");
	json_t* data = m->dataToJson();
	m->dataFromJson(data);
	json_t* odd = json_object();
	const char* key;
	json_t* value;
	json_object_foreach(data, key, value) {
		json_object_set_new(odd, key, json_is_array(value) ? json_array() : json_integer(99999));
	}
	at("loading values out of their range for what it keeps");
	m->dataFromJson(odd);
	run(r, "after that", 2000, 0.1, sine);
	draw(m, vg);
	m->dataFromJson(data);
	json_decref(odd);

	at("reset()");
	engine::Module::ResetEvent reset;
	m->onReset(reset);
	run(r, "after Initialize", 2000, 0.1, sine);
	at("Randomize");
	engine::Module::RandomizeEvent random;
	m->onRandomize(random);
	run(r, "after Randomize", 2000, 0.1, wild);

	// A second one beside it, and the first one gone.
	engine::Module* m2 = make();
	Rig r2;
	r2.m = m2;
	r2.setRate(48000.f);
	r2.connect(1, 1);
	run(r2, "a second one beside it", 2000, 0.1, sine);
	at("taking it out (the destructor)");
	delete m;
	run(r2, "the second one, after that", 2000, 0.1, sine);
	at("taking it out (the destructor)");
	delete m2;
	say(json_pack("{s:{s:f}}", "ok", "peak", g_peak));
}


/* ---- speed, and what comes out ---- */

static int findName(json_t* key, const char* text, const std::vector<std::string>& names) {
	(void) key;
	char* end;
	long n = strtol(text, &end, 10);
	for (size_t i = 0; i < names.size(); i++) {
		if (string::lowercase(names[i]) == string::lowercase(text))
			return (int) i;
	}
	return *text && !*end && n >= 0 && n < (long) names.size() ? (int) n : -1;
}

static void probe(json_t* spec) {
	engine::Module* m = make();
	at("a probe");
	Rig r;
	r.m = m;
	r.setRate(48000.f);
	r.connect(0, 1);
	std::vector<std::string> inputs, outputs, params;
	for (engine::PortInfo* i : m->inputInfos) inputs.push_back(i->name);
	for (engine::PortInfo* o : m->outputInfos) outputs.push_back(o->name);
	for (engine::ParamQuantity* p : m->paramQuantities) params.push_back(p->name);

	struct Source { int kind = -1; float value = 0.f; };
	std::vector<Source> sources(m->inputs.size());
	json_t* out = json_object();
	const char* key;
	json_t* value;
	json_object_foreach(json_object_get(spec, "in"), key, value) {
		int i = findName(NULL, key, inputs);
		if (i < 0) {
			json_object_set_new(out, "error", json_string(string::f("there is no input \"%s\"", key).c_str()));
			continue;
		}
		m->inputs[i].channels = 1;
		if (json_is_number(value)) {
			sources[i].kind = 0;
			sources[i].value = json_number_value(value);
		}
		else if (json_is_string(value)) {
			std::string s = json_string_value(value);
			size_t space = s.find(' ');
			std::string word = s.substr(0, space);
			sources[i].value = space == std::string::npos ? (word == "gate" ? 2.f : 440.f) : atof(s.c_str() + space);
			sources[i].kind = word == "sine" ? 1 : word == "gate" ? 2 : word == "noise" ? 3 : -1;
			if (sources[i].kind < 0)
				json_object_set_new(out, "error", json_string(string::f("\"%s\" is not a number, \"sine HZ\", \"gate HZ\" or \"noise\"", s.c_str()).c_str()));
		}
	}
	json_object_foreach(json_object_get(spec, "set"), key, value) {
		int p = findName(NULL, key, params);
		if (p < 0 || !json_is_number(value))
			json_object_set_new(out, "error", json_string(string::f("there is no param \"%s\", or not a number for it", key).c_str()));
		else
			m->params[p].setValue(json_number_value(value));
	}
	float seconds = json_is_number(json_object_get(spec, "seconds")) ? json_number_value(json_object_get(spec, "seconds")) : 0.5f;
	seconds = math::clamp(seconds, 0.02f, 10.f);
	int frames = (int) (seconds * r.rate);
	int nOut = (int) m->outputs.size();
	std::vector<float> lo(nOut, INFINITY), hi(nOut, -INFINITY);
	std::vector<double> sum(nOut, 0.0);
	std::vector<std::vector<float>> trace(nOut);
	for (int f = 0; f < frames; f++) {
		for (size_t i = 0; i < sources.size(); i++) {
			float t = (float) (f / (double) r.rate);
			float v = 0.f;
			if (sources[i].kind == 0) v = sources[i].value;
			else if (sources[i].kind == 1) v = 5.f * (float) std::sin(2.0 * M_PI * sources[i].value * f / r.rate);
			else if (sources[i].kind == 2) v = std::fmod(t * sources[i].value, 1.f) < 0.5f ? 10.f : 0.f;
			else if (sources[i].kind == 3) v = 5.f * noise();
			if (m->inputs[i].channels)
				m->inputs[i].voltages[0] = v;
		}
		r.step();
		for (int o = 0; o < nOut; o++) {
			float v = m->outputs[o].voltages[0];
			lo[o] = std::fmin(lo[o], v);
			hi[o] = std::fmax(hi[o], v);
			sum[o] += v;
			trace[o].push_back(v);
		}
	}
	json_t* outs = json_object();
	for (int o = 0; o < nOut; o++) {
		float mean = (float) (sum[o] / frames);
		float band = (hi[o] - lo[o]) * 0.1f;
		int crossings = 0;
		int first = -1, last = -1;
		bool below = false;
		for (int f = 0; f < frames; f++) {
			if (trace[o][f] < mean - band) below = true;
			else if (below && trace[o][f] > mean + band) {
				below = false;
				crossings++;
				if (first < 0) first = f;
				last = f;
			}
		}
		json_t* j = json_pack("{s:f, s:f, s:f, s:i}", "min", lo[o], "max", hi[o], "mean", mean, "channels", (int) m->outputs[o].channels);
		if (crossings > 2 && hi[o] - lo[o] > 0.01f)
			json_object_set_new(j, "hz", json_real((crossings - 1) * r.rate / (double) (last - first)));
		if (frames > 0)
			json_object_set_new(j, "end", json_real(trace[o][frames - 1]));
		json_object_set_new(outs, outputs[o].c_str(), j);
	}
	json_object_set_new(out, "outputs", outs);
	say(json_pack("{s:o}", "probe", out));
	delete m;
}


static double nanoseconds(int channels, int frames) {
	engine::Module* m = make();
	at("measuring how fast it is");
	Rig r;
	r.m = m;
	r.setRate(48000.f);
	r.connect(channels, 1);
	auto step = [&]() {
		for (engine::Input& i : m->inputs) {
			for (int c = 0; c < channels; c++)
				i.voltages[c] = 5.f * noise();
		}
		r.step();
	};
	double from = now();
	for (int f = 0; f < 4800 && now() - from < 1.0; f++)
		step();
	if (channels == 1) {
		g_counting = true;
		for (int f = 0; f < 2000; f++) {
			step();
			if (f % 64 == 63 && now() - from > 2.0)
				break;
		}
		g_counting = false;
	}
	// The best of some stretches: the rest is the device doing something else.
	double best = 1e9;
	from = now();
	for (int k = 0; k < 12 && now() - from < 1.5; k++) {
		double t = now();
		int n = frames / 12;
		for (int f = 0; f < n; f++)
			step();
		best = std::fmin(best, (now() - t) / n * 1e9);
	}
	delete m;
	return best;
}

/** What only comes after a while: a buffer that is full, a phase that has grown
too large, a level that creeps up. */
static void soak() {
	engine::Module* m = make();
	at("process(), two minutes of it without a break");
	Rig r;
	r.m = m;
	r.setRate(48000.f);
	r.connect(1, 1);
	double from = now();
	int64_t frame = 0;
	const int64_t FRAMES = 48000 * 120;
	for (; frame < FRAMES; frame++) {
		for (size_t i = 0; i < m->inputs.size(); i++) {
			int64_t f = frame + 4001 * (int64_t) i;
			// Audio for a while, gates for a while.
			m->inputs[i].voltages[0] = (f / 96000) % 2 ? ((f / 6000) % 2 ? 10.f : 0.f) : 5.f * noise();
		}
		if (frame % 240000 == 0) {
			for (size_t p = 0; p < m->params.size(); p++)
				r.setParam((int) p, frame ? noise() * 0.5f + 0.5f : 0.5f);
		}
		r.step();
		if (frame % 4800 == 0) {
			if (g_lib.stopped(m))
				fail(string::f("an output was not a number (NaN or infinite) after %d s", (int) (frame / 48000)));
			alarm(4);
			if (now() - from > 2.0)
				break;
		}
	}
	say(json_pack("{s:{s:i}}", "soak", "seconds", (int) (frame / 48000)));
	delete m;
}

static void speed(const char* probes) {
	double mono = nanoseconds(1, 24000);
	long allocations = g_allocations;
	double poly = nanoseconds(16, 6000);
	say(json_pack("{s:{s:f, s:f, s:i}}", "speed", "ns", mono, "ns16", poly, "allocations", (int) allocations));
	soak();
	if (probes) {
		json_error_t error;
		json_t* list = json_load_file(probes, 0, &error);
		size_t i;
		json_t* spec;
		json_array_foreach(list, i, spec) {
			probe(spec);
		}
		json_decref(list);
	}
	say(json_pack("{s:{}}", "ok"));
}


int main(int argc, char** argv) {
	if (argc < 3)
		return 2;
	std::string mode = argv[1];
	signal(SIGALRM, onAlarm);
#if !__has_feature(address_sanitizer)
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = onFault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT})
		sigaction(s, &sa, NULL);
#endif
	// Before anything of the module's is in the process.
	contextSet(new Context);
	random::init();
	APP->engine = new engine::Engine;
	say(json_pack("{s:b}", "sandbox", (int) sandbox()));
	try {
		load(argv[2]);
		if (mode == "check")
			check();
		else
			speed(argc > 3 ? argv[3] : NULL);
	}
	catch (std::exception& e) {
		fail(string::f("an exception: %s", e.what()));
	}
	catch (...) {
		fail("an exception");
	}
	// No destructors: the engine's are not the question.
	_exit(0);
}
