#include "utils/worker_thread.h"
#include "log.h"

namespace Utils {

namespace {

void trampoline(void *arg) {
	auto *fn = static_cast<std::function<void()> *>(arg);
	(*fn)();
	delete fn;
}

s32 workerPriority(int delta) {
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	prio += delta;
	if (prio > 0x3F) {
		prio = 0x3F;
	}
	if (prio < 0x18) {
		prio = 0x18;
	}
	return prio;
}

} // namespace

WorkerThread::WorkerThread(WorkerThread &&other) noexcept : thread(other.thread) { other.thread = nullptr; }

WorkerThread &WorkerThread::operator=(WorkerThread &&other) noexcept {
	if (this != &other) {
		join();
		thread = other.thread;
		other.thread = nullptr;
	}
	return *this;
}

WorkerThread::~WorkerThread() { join(); }

void WorkerThread::start(std::function<void()> fn, int priorityDelta, size_t stackSize) {
	join();

	auto *heapFn = new std::function<void()>(std::move(fn));
	s32 prio = workerPriority(priorityDelta);

	thread = threadCreate(trampoline, heapFn, stackSize, prio, -2, false);
	if (!thread) {
		delete heapFn;
		Logger::log("[Thread] threadCreate failed (prio 0x%lx, stack %u)", (unsigned long)prio, (unsigned)stackSize);
	}
}

void WorkerThread::join() {
	if (thread) {
		threadJoin(thread, U64_MAX);
		threadFree(thread);
		thread = nullptr;
	}
}

} // namespace Utils
