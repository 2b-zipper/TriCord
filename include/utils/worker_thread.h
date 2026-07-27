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

	void start(std::function<void()> fn, int priorityDelta, size_t stackSize = 32 * 1024);

	bool joinable() const { return thread != nullptr; }
	void join();

  private:
	Thread thread = nullptr;
};

} // namespace Utils

#endif // WORKER_THREAD_H
