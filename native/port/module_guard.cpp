/* See module_guard.hpp.
 *
 * A guarded call leaves a jump buffer on a list of its thread; the handlers of
 * the fatal signals jump back to it. Loops are found by a thread that looks at
 * how often each thread has entered and left, and signals one that has been
 * inside the same call for too long.
 *
 * Everything the handlers touch is allocated beforehand. Rack's threads come
 * and go (Engine > Threads), so the per-thread records are a fixed table, given
 * back when a thread ends.
 */
#include <csignal>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <exception>
#include <mutex>

#include <cerrno>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <android/log.h>

#include "module_guard.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.guard", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "rackdroid.guard", __VA_ARGS__)


namespace rackdroid {


namespace {

const int SIGNALS[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT};
const int NUM_SIGNALS = sizeof(SIGNALS) / sizeof(SIGNALS[0]);
/** Not used by Android's runtime, unlike SIGUSR1. */
const int SIG_HANG = SIGUSR2;

const int MAX_THREADS = 64;
const int MAX_CODE = 64;
const int MAX_REPORTS = 16;
const int MAX_DEPTH = 48;
const int WATCH_MS = 150;
/** Looks at the same call before it counts as hung: about 1.5 s. A thread that
is merely kept waiting by the system, at any priority, is not away that long,
and when the whole app is frozen the watching stops too. */
const int HUNG_LOOKS = 10;
/** Drawing holds up the screen, and a loop of lines fills the memory. */
const int HUNG_LOOKS_DRAWING = 3;

struct Frame {
	sigjmp_buf jump;
	Frame* below;
};

struct Thread {
	std::atomic<int> tid{0};
	std::atomic<Frame*> top{nullptr};
	/** Goes up with every call entered and left. */
	std::atomic<uint32_t> count{0};
	/** The count at which the watcher gave up on it. */
	std::atomic<uint32_t> hungAt{0};
	std::atomic<bool> hung{false};
	/** What the innermost call is for: a GuardWhere. */
	std::atomic<int> where{0};
	sigset_t mask;
	/** What the handler found. */
	int signal = 0;
	uintptr_t pc = 0;
	uintptr_t fp = 0;
	uintptr_t address = 0;
	/** The watcher's own. */
	uint32_t seen = 0;
	int looks = 0;
};

struct Code {
	std::atomic<uintptr_t> base{0};
	uintptr_t end = 0;
	char tag[64];
};

struct Report {
	/** 0 free, 1 being written, 2 to be read. */
	std::atomic<int> state{0};
	const void* owner;
	int where;
	char what[160];
	int code;
	uintptr_t offset;
};

Thread g_threads[MAX_THREADS];
Code g_code[MAX_CODE];
std::atomic<int> g_numCode{0};
Report g_reports[MAX_REPORTS];
pthread_key_t g_key;
std::atomic<bool> g_installed{false};
struct sigaction g_before[NUM_SIGNALS];
char g_marker[512];


void threadEnded(void* p) {
	Thread* t = (Thread*) p;
	t->top.store(nullptr);
	t->tid.store(0);
}


Thread* thisThread() {
	Thread* t = (Thread*) pthread_getspecific(g_key);
	if (t)
		return t;
	int tid = gettid();
	for (Thread& c : g_threads) {
		int none = 0;
		if (!c.tid.compare_exchange_strong(none, tid))
			continue;
		c.top.store(nullptr);
		c.hung.store(false);
		c.looks = 0;
		pthread_sigmask(SIG_SETMASK, NULL, &c.mask);
		pthread_setspecific(g_key, &c);
		return &c;
	}
	return nullptr;
}


int codeAt(uintptr_t pc) {
	int n = g_numCode.load(std::memory_order_acquire);
	for (int i = 0; i < n; i++) {
		if (pc >= g_code[i].base.load(std::memory_order_relaxed) && pc < g_code[i].end)
			return i;
	}
	return -1;
}


/** Reads memory that may not be there. */
bool peek(uintptr_t address, void* to, size_t n) {
	struct iovec local = {to, n};
	struct iovec remote = {(void*) address, n};
	return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == (ssize_t) n;
}


/** The first place on the stack that is in such a library, or -1. `found`, if
given, gets a bit for each library met. */
int codeOnStack(uintptr_t pc, uintptr_t fp, uintptr_t& offset, uint64_t* found = nullptr) {
	int first = -1;
	for (int i = 0; i < MAX_DEPTH; i++) {
		int c = codeAt(pc);
		if (c >= 0) {
			if (first < 0) {
				first = c;
				offset = pc - g_code[c].base.load(std::memory_order_relaxed);
				// A return address is one past the call.
				if (i > 0)
					offset -= 4;
			}
			if (!found)
				return first;
			*found |= (uint64_t) 1 << c;
		}
		uintptr_t next[2];
		if (!fp || (fp & 7) || !peek(fp, next, sizeof next) || next[0] <= fp)
			break;
		fp = next[0];
		// The top bits hold the pointer authentication code, where there is one.
		pc = next[1] & 0x0000ffffffffffffull;
	}
	return first;
}


void writeReport(const void* owner, int where, const char* what, int code, uintptr_t offset) {
	for (Report& r : g_reports) {
		int none = 0;
		if (!r.state.compare_exchange_strong(none, 1))
			continue;
		r.owner = owner;
		r.where = where;
		snprintf(r.what, sizeof r.what, "%s", what);
		r.code = code;
		r.offset = offset;
		r.state.store(2, std::memory_order_release);
		return;
	}
}


/** The app is about to die, and not in a guarded call. */
void writeMarker(uintptr_t pc, uintptr_t fp) {
	uintptr_t offset = 0;
	uint64_t found = 0;
	int first = codeOnStack(pc, fp, offset, &found);
	if (first < 0 || !g_marker[0])
		return;
	int fd = open(g_marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	char line[128];
	int n = snprintf(line, sizeof line, "%s 0x%lx\n", g_code[first].tag, (unsigned long) offset);
	(void) !write(fd, line, n);
	for (int i = 0; i < MAX_CODE; i++) {
		if (i == first || !(found >> i & 1))
			continue;
		n = snprintf(line, sizeof line, "%s\n", g_code[i].tag);
		(void) !write(fd, line, n);
	}
	close(fd);
}


void onSignal(int signal, siginfo_t* info, void* context) {
	int error = errno;
	ucontext_t* uc = (ucontext_t*) context;
#if defined(__aarch64__)
	uintptr_t pc = uc->uc_mcontext.pc;
	uintptr_t fp = uc->uc_mcontext.regs[29];
#elif defined(__x86_64__)
	uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
	uintptr_t fp = uc->uc_mcontext.gregs[REG_RBP];
#else
	uintptr_t pc = 0, fp = 0;
	(void) uc;
#endif
	Thread* t = (Thread*) pthread_getspecific(g_key);
	Frame* frame = t ? t->top.load(std::memory_order_relaxed) : nullptr;
	if (signal == SIG_HANG) {
		// It may have left the call in the meantime.
		if (!frame || !t->hung.load() || t->hungAt.load() != t->count.load()) {
			errno = error;
			return;
		}
	}
	if (frame) {
		t->signal = signal;
		t->pc = pc;
		t->fp = fp;
		t->address = (uintptr_t) info->si_addr;
		siglongjmp(frame->jump, 1);
	}
	writeMarker(pc, fp);
	for (int i = 0; i < NUM_SIGNALS; i++) {
		if (SIGNALS[i] != signal)
			continue;
		struct sigaction& before = g_before[i];
		if (before.sa_flags & SA_SIGINFO) {
			before.sa_sigaction(signal, info, context);
		}
		else if (before.sa_handler == SIG_DFL || before.sa_handler == SIG_IGN) {
			// Happens again on return, and then for good.
			sigaction(signal, &before, NULL);
			if (signal == SIGABRT)
				raise(signal);
		}
		else {
			before.sa_handler(signal);
		}
	}
	errno = error;
}


void* watch(void*) {
	pthread_setname_np(pthread_self(), "module-guard");
	while (true) {
		usleep(WATCH_MS * 1000);
		for (Thread& t : g_threads) {
			int tid = t.tid.load();
			if (!tid)
				continue;
			uint32_t count = t.count.load(std::memory_order_relaxed);
			if (!t.top.load(std::memory_order_relaxed) || count != t.seen) {
				t.seen = count;
				t.looks = 0;
				continue;
			}
			if (++t.looks < (t.where.load() == GUARD_DRAW ? HUNG_LOOKS_DRAWING : HUNG_LOOKS))
				continue;
			t.looks = 0;
			t.hungAt.store(count);
			t.hung.store(true);
			LOGE("thread %d has been in a module for too long", tid);
			syscall(SYS_tgkill, getpid(), tid, SIG_HANG);
		}
	}
	return NULL;
}


const char* signalText(int signal) {
	switch (signal) {
		case SIGSEGV:
		case SIGBUS: return "a memory access that is not allowed";
		case SIGFPE: return "an arithmetic error";
		case SIGILL:
		case SIGTRAP: return "a check failed: an index out of range, or something as impossible";
		case SIGABRT: return "it was aborted: a check failed, or an exception nobody caught";
		case SIG_HANG: return "it did not come back: a loop without an end";
	}
	return "a crash";
}

} // namespace


void guardInstall(const std::string& marker) {
	static std::once_flag once;
	std::call_once(once, [&] {
		snprintf(g_marker, sizeof g_marker, "%s", marker.c_str());
		pthread_key_create(&g_key, threadEnded);
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = onSignal;
		// On the thread's alternate stack (bionic gives every thread one), for
		// a stack that has run over; not blocked, for a handler that jumps away.
		sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
		sigemptyset(&sa.sa_mask);
		for (int i = 0; i < NUM_SIGNALS; i++)
			sigaction(SIGNALS[i], &sa, &g_before[i]);
		sigaction(SIG_HANG, &sa, NULL);
		pthread_t thread;
		pthread_create(&thread, NULL, watch, NULL);
		pthread_detach(thread);
		g_installed.store(true, std::memory_order_release);
		LOGI("installed");
	});
}


void guardAddCode(const void* address, const std::string& tag) {
	struct Found {
		uintptr_t address;
		uintptr_t base;
		uintptr_t end;
	} f = {(uintptr_t) address, 0, 0};
	dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* p) -> int {
		Found* f = (Found*) p;
		bool inside = false;
		uintptr_t end = 0;
		for (int i = 0; i < info->dlpi_phnum; i++) {
			const ElfW(Phdr)& ph = info->dlpi_phdr[i];
			if (ph.p_type != PT_LOAD)
				continue;
			uintptr_t from = info->dlpi_addr + ph.p_vaddr;
			uintptr_t to = from + ph.p_memsz;
			end = to > end ? to : end;
			inside = inside || (f->address >= from && f->address < to);
		}
		if (!inside)
			return 0;
		f->base = info->dlpi_addr;
		f->end = end;
		return 1;
	}, &f);
	static std::mutex mutex;
	std::lock_guard<std::mutex> lock(mutex);
	int n = g_numCode.load();
	if (!f.end || n >= MAX_CODE)
		return;
	snprintf(g_code[n].tag, sizeof g_code[n].tag, "%s", tag.c_str());
	g_code[n].end = f.end;
	g_code[n].base.store(f.base);
	g_numCode.store(n + 1, std::memory_order_release);
}


bool guardNextFault(GuardFault& fault) {
	for (Report& r : g_reports) {
		if (r.state.load(std::memory_order_acquire) != 2)
			continue;
		fault.owner = r.owner;
		fault.where = r.where;
		fault.what = r.what;
		fault.tag = r.code >= 0 ? g_code[r.code].tag : "";
		fault.offset = r.offset;
		r.state.store(0);
		return true;
	}
	return false;
}


const char* guardWhereName(int where) {
	switch (where) {
		case GUARD_MAKE: return "as it was made";
		case GUARD_PROCESS: return "in process()";
		case GUARD_EVENT: return "in reset() or rateChanged()";
		case GUARD_STATE: return "in saving or loading what it keeps";
		case GUARD_DRAW: return "in draw()";
		case GUARD_WIDGET: return "in making its panel";
		case GUARD_REMOVE: return "as it was taken out (its destructor)";
	}
	return "somewhere";
}


} // namespace rackdroid


using namespace rackdroid;


extern "C" int nova_guard_call(void (*fn)(void*), void* arg, const void* owner, int where) {
	Thread* t = g_installed.load(std::memory_order_acquire) ? thisThread() : nullptr;
	if (!t) {
		fn(arg);
		return 0;
	}
	Frame frame;
	frame.below = t->top.load(std::memory_order_relaxed);
	if (sigsetjmp(frame.jump, 0) == 0) {
		t->count.store(t->count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		t->where.store(where, std::memory_order_relaxed);
		std::atomic_signal_fence(std::memory_order_seq_cst);
		t->top.store(&frame, std::memory_order_relaxed);
		char text[160];
		bool thrown = false;
		try {
			fn(arg);
		}
		catch (const std::exception& e) {
			thrown = true;
			snprintf(text, sizeof text, "an exception: %s", e.what());
		}
		catch (...) {
			thrown = true;
			snprintf(text, sizeof text, "an exception");
		}
		t->top.store(frame.below, std::memory_order_relaxed);
		std::atomic_signal_fence(std::memory_order_seq_cst);
		t->count.store(t->count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		if (thrown)
			writeReport(owner, where, text, -1, 0);
		return thrown ? 1 : 0;
	}
	// From the handler.
	t->top.store(frame.below, std::memory_order_relaxed);
	t->count.store(t->count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	t->hung.store(false);
	// abort() had blocked everything.
	pthread_sigmask(SIG_SETMASK, &t->mask, NULL);
	uintptr_t offset = 0;
	int code = codeOnStack(t->pc, t->fp, offset);
	writeReport(owner, where, signalText(t->signal), code, offset);
	return 1;
}


extern "C" void nova_guard_report(const void* owner, int where, const char* text) {
	writeReport(owner, where, text, -1, 0);
}
