#include "threading.h"

#include "stored-object.h"
#include "notifier.h"
#include "spin-mutex.h"
#include "utils.h"

#include <thread>
#include <sstream>

namespace effil {

using Status = ThreadHandle::Status;
using Command = ThreadHandle::Command;

namespace {

const sol::optional<std::chrono::milliseconds> NO_TIMEOUT;

// Thread specific pointer to current thread
static thread_local ThreadHandle* thisThreadHandle = nullptr;

// Doesn't inherit std::exception
// to prevent from catching this exception third party lua C++ libs
class LuaHookStopException {};

bool isFinishStatus(Status stat) {
    return stat == Status::Canceled || stat == Status::Completed || stat == Status::Failed;
}

std::string statusToString(Status status) {
    switch (status) {
        case Status::Running:
            return "running";
        case Status::Paused:
            return "paused";
        case Status::Canceled:
            return "canceled";
        case Status::Completed:
            return "completed";
        case Status::Failed:
            return "failed";
    }
    assert(false);
    return "unknown";
}

int luaErrorHandler(lua_State* state) {
    luaL_traceback(state, state, nullptr, 1);
    const auto stacktrace = sol::stack::pop<std::string>(state);
    thisThreadHandle->result().emplace_back(createStoredObject(stacktrace));
    return 1;
}

const lua_CFunction luaErrorHandlerPtr = luaErrorHandler;

void luaHook(lua_State*, lua_Debug*) {
    assert(thisThreadHandle);
    switch (thisThreadHandle->command()) {
        case Command::Run:
            break;
        case Command::Cancel:
            thisThreadHandle->changeStatus(Status::Canceled);
            throw LuaHookStopException();
        case Command::Pause: {
            thisThreadHandle->changeStatus(Status::Paused);
            Command cmd;
            do {
                cmd = thisThreadHandle->waitForCommandChange(NO_TIMEOUT);
            } while(cmd != Command::Run && cmd != Command::Cancel);
            if (cmd == Command::Run) {
                thisThreadHandle->changeStatus(Status::Running);
            } else {
                thisThreadHandle->changeStatus(Status::Canceled);
                throw LuaHookStopException();
            }
            break;
        }
    }
}

} // namespace

namespace this_thread {

ScopedSetInterruptable::ScopedSetInterruptable(IInterruptable* notifier) {
    if (thisThreadHandle) {
        thisThreadHandle->setNotifier(notifier);
    }
}

ScopedSetInterruptable::~ScopedSetInterruptable() {
    if (thisThreadHandle) {
        thisThreadHandle->setNotifier(nullptr);
    }
}

void interruptionPoint() {
    if (thisThreadHandle && thisThreadHandle->command() == Command::Cancel)
    {
        thisThreadHandle->changeStatus(Status::Canceled);
        throw LuaHookStopException();
    }
}

std::string threadId() {
    std::stringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

void yield() {
    luaHook(nullptr, nullptr);
    std::this_thread::yield();
}

void sleep(const sol::stack_object& duration, const sol::stack_object& metric) {
    if (duration.valid()) {
        REQUIRE(duration.get_type() == sol::type::number)
                << "bad argument #1 to 'effil.sleep' (number expected, got "
                << luaTypename(duration) << ")";

        if (metric.valid())
        {
            REQUIRE(metric.get_type() == sol::type::string)
                    << "bad argument #2 to 'effil.sleep' (string expected, got "
                    << luaTypename(metric) << ")";
        }
        try {
            Notifier notifier;
            notifier.waitFor(fromLuaTime(duration.as<int>(),
                                         metric.as<sol::optional<std::string>>()));
        } RETHROW_WITH_PREFIX("effil.sleep");
    }
    else {
        yield();
    }
}

} // namespace this_thread

ThreadHandle::ThreadHandle()
        : status_(Status::Running)
        , command_(Command::Run)
        , currNotifier_(nullptr)
        , lua_(std::make_unique<sol::state>())
{
    luaL_openlibs(*lua_);
}

void ThreadHandle::putCommand(Command cmd) {
    std::unique_lock<std::mutex> lock(stateLock_);
    if (isFinishStatus(status_))
        return;

    command_ = cmd;
    statusNotifier_.reset();
    commandNotifier_.notify();
}

void ThreadHandle::changeStatus(Status stat) {
    std::unique_lock<std::mutex> lock(stateLock_);
    status_ = stat;
    commandNotifier_.reset();
    statusNotifier_.notify();
    if (isFinishStatus(stat))
        completionNotifier_.notify();
}

void Thread::runThread(Thread thread,
               Function function,
               effil::StoredArray arguments) {
    thisThreadHandle = thread.ctx_.get();
    assert(thisThreadHandle != nullptr);

    try {
        {
            ScopeGuard reportComplete([thread, &arguments](){
                // Let's destroy accociated state
                // to release all resources as soon as possible
                arguments.clear();
                thread.ctx_->destroyLua();
            });
            sol::protected_function userFuncObj = function.loadFunction(thread.ctx_->lua());

            #if LUA_VERSION_NUM > 501

            sol::stack::push(thread.ctx_->lua(), luaErrorHandlerPtr);
            userFuncObj.error_handler = sol::reference(thread.ctx_->lua());
            sol::stack::pop_n(thread.ctx_->lua(), 1);

            #endif // LUA_VERSION NUM > 501

            sol::protected_function_result result = userFuncObj(std::move(arguments));
            if (!result.valid()) {
                if (thread.ctx_->status() == Status::Canceled)
                    return;

                sol::error err = result;
                std::string what = err.what();
                throw std::runtime_error(what);
            }

            sol::variadic_args args(thread.ctx_->lua(), -lua_gettop(thread.ctx_->lua()));
            for (const auto& iter : args) {
                StoredObject store = createStoredObject(iter.get<sol::object>());
                if (store->gcHandle() != nullptr)
                {
                    thread.ctx_->addReference(store->gcHandle());
                    store->releaseStrongReference();
                }
                thread.ctx_->result().emplace_back(std::move(store));
            }
        }
        thread.ctx_->changeStatus(Status::Completed);
    } catch (const LuaHookStopException&) {
        thread.ctx_->changeStatus(Status::Canceled);
    } catch (const std::exception& err) {
        DEBUG("thread") << "Failed with msg: " << err.what() << std::endl;
        auto& returns = thread.ctx_->result();
        returns.insert(returns.begin(),
                { createStoredObject("failed"),
                  createStoredObject(err.what()) });
        thread.ctx_->changeStatus(Status::Failed);
    }
}

void Thread::initialize(
    const std::string& path,
    const std::string& cpath,
    int step,
    const sol::function& function,
    const sol::variadic_args& variadicArgs)
{

    sol::optional<Function> functionObj;
    try {
        functionObj = GC::instance().create<Function>(function);
    } RETHROW_WITH_PREFIX("effil.thread");

    ctx_->lua()["package"]["path"] = path;
    ctx_->lua()["package"]["cpath"] = cpath;
    try {
        luaopen_effil(ctx_->lua());
        sol::stack::pop<sol::object>(ctx_->lua());
    } RETHROW_WITH_PREFIX("effil.thread");

    if (step != 0)
        lua_sethook(ctx_->lua(), luaHook, LUA_MASKCOUNT, step);

    effil::StoredArray arguments;
    try {
        for (const auto& arg : variadicArgs) {
            const auto& storedObj = createStoredObject(arg.get<sol::object>());
            ctx_->addReference(storedObj->gcHandle());
            storedObj->releaseStrongReference();
            arguments.emplace_back(storedObj);
        }
    } RETHROW_WITH_PREFIX("effil.thread");

    std::thread thr(&Thread::runThread,
                    *this,
                    functionObj.value(),
                    std::move(arguments));
    thr.detach();
}

void Thread::exportAPI(sol::state_view& lua) {
    sol::usertype<Thread> type(
            "new", sol::no_constructor,
            "get", &Thread::get,
            "wait", &Thread::wait,
            "cancel", &Thread::cancel,
            "pause", &Thread::pause,
            "resume", &Thread::resume,
            "status", &Thread::status);

    sol::stack::push(lua, type);
    sol::stack::pop<sol::object>(lua);
}

StoredArray Thread::status(const sol::this_state& lua) {
    const auto stat = ctx_->status();
    if (stat == Status::Failed) {
        assert(!ctx_->result().empty());
        return ctx_->result();
    } else {
        const sol::object luaStatus = sol::make_object(lua, statusToString(stat));
        return StoredArray({createStoredObject(luaStatus)});
    }
}

sol::optional<std::chrono::milliseconds> toOptionalTime(const sol::optional<int>& duration,
                                                        const sol::optional<std::string>& period) {
    if (duration)
        return fromLuaTime(*duration, period);
    else
        return sol::optional<std::chrono::milliseconds>();
}

StoredArray Thread::wait(const sol::this_state& lua,
                                                 const sol::optional<int>& duration,
                                                 const sol::optional<std::string>& period) {
    ctx_->waitForCompletion(toOptionalTime(duration, period));
    return status(lua);
}

StoredArray Thread::get(const sol::optional<int>& duration,
                       const sol::optional<std::string>& period) {
    if (ctx_->waitForCompletion(toOptionalTime(duration, period)) && ctx_->status() == Status::Completed)
        return ctx_->result();
    else
        return StoredArray();
}

bool Thread::cancel(const sol::this_state&,
                    const sol::optional<int>& duration,
                    const sol::optional<std::string>& period) {
    ctx_->putCommand(Command::Cancel);
    ctx_->interrupt();
    Status status = ctx_->waitForStatusChange(toOptionalTime(duration, period));
    return isFinishStatus(status);
}

bool Thread::pause(const sol::this_state&,
                   const sol::optional<int>& duration,
                   const sol::optional<std::string>& period) {
    ctx_->putCommand(Command::Pause);
    Status status = ctx_->waitForStatusChange(toOptionalTime(duration, period));
    return status == Status::Paused;
}

void Thread::resume() {
    ctx_->putCommand(Command::Run);
}

} // effil
