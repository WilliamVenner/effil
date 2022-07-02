#include "shutdown.h"

extern "C"
#ifdef _WIN32
 __declspec(dllexport)
#endif
void effil_shutdown() {
	effil::SHUTDOWN.store(true, std::memory_order_release);

	while (effil::Shutdown::activeThreads() > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

namespace effil {

extern std::atomic<bool> SHUTDOWN = false;
extern std::atomic<size_t> ACTIVE_THREAD_COUNT = 0;

}