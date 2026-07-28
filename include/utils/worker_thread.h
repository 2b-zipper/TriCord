#ifndef WORKER_THREAD_H
#define WORKER_THREAD_H

#include <3ds.h>
#include <cstddef>
#include <functional>

namespace Utils {

// std::thread inherits its creator's priority, and the 3DS kernel does not
// time-slice equal priorities, so CPU-bound work there stalls rendering.
class WorkerThread {
  public:
	WorkerThread() = default;
	WorkerThread(const WorkerThread &) = delete;
	WorkerThread &operator=(const WorkerThread &) = delete;
	WorkerThread(WorkerThread &&other) noexcept;
	WorkerThread &operator=(WorkerThread &&other) noexcept;
	~WorkerThread();

	// preferExtraCore is New 3DS only; the Old 3DS second core belongs to the
	// system services this app leans on.
	void start(std::function<void()> fn, int priorityDelta, size_t stackSize = 32 * 1024, bool preferExtraCore = false);

	bool joinable() const { return thread != nullptr; }
	void join();

	static bool extraCoreAvailable();
	static int extraCoreThreads();

  private:
	Thread thread = nullptr;
	bool onExtraCore = false;
};

} // namespace Utils

#endif // WORKER_THREAD_H
