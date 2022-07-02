#pragma once

#include <sol.hpp>

#include <thread>
#include <chrono>

extern "C"
#ifdef _WIN32
 __declspec(dllexport)
#endif
void effil_shutdown();

namespace effil {

extern std::atomic<bool> SHUTDOWN;
extern std::atomic<size_t> ACTIVE_THREAD_COUNT;

class Shutdown {
public:
	static int Shutdown::gc(lua_State* L) {
		effil_shutdown();
		return 0;
	}

	static void Shutdown::register_cookie(lua_State* L) {
		static std::atomic<bool> registered = false;

		// Ensure that the cookie is only registered once (ergo, in the main thread)
		if (registered.exchange(true, std::memory_order_acq_rel))
			return;

		lua_newuserdata(L, sizeof(Shutdown));

		lua_createtable(L, 0, 2);

		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");

		lua_pushcfunction(L, Shutdown::gc);
		lua_setfield(L, -2, "__gc");

		lua_setmetatable(L, -2);

		lua_setfield(L, LUA_REGISTRYINDEX, "effil_shutdown_cookie");
	}

	static bool requested() {
		return SHUTDOWN.load(std::memory_order_relaxed);
	}

	static size_t activeThreads() {
		return ACTIVE_THREAD_COUNT.load(std::memory_order_relaxed);
	}

	static void threadStart() {
		ACTIVE_THREAD_COUNT.fetch_add(1, std::memory_order_release);
	}

	static void threadFinish() {
		ACTIVE_THREAD_COUNT.fetch_sub(1, std::memory_order_release);
	}
};

}