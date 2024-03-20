#pragma once

#include "contract.h"
#include "scope_exit.h"

#include <sys/epoll.h>

#include <coroutine>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stack>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// hypothesis:
// * a coroutine can have at most two references to it

#define rpc_print(...) std::format_to(std::ostreambuf_iterator{std::cout}, __VA_ARGS__)

namespace rpc {

constexpr inline auto operator""_KB(unsigned long long const x) -> unsigned long long {
    return 1024L * x;
}

class TaskStack {
public:
    template <class T>
    void push(std::coroutine_handle<T> co) noexcept(false) {
        frames.push(Frame{
                .co = co,
        });
    }

    template <class T>
    T take_result() const noexcept(false) {
        rpc_assert(state != Result::none, Invariant{});
        switch (state) {
        case Result::none:
            return rpc_unreachable(Invariant{});
        case Result::exception:
            std::rethrow_exception(exception);
        case Result::value:
            
        }
    }

private:
    struct Frame {
        std::coroutine_handle<> co;
    };

    enum class Result {
        none,
        exception,
        value,
    } state;
    std::exception_ptr exception;
    std::aligned_storage<10_KB, alignof(std::max_align_t)> result;

    std::stack<Frame, std::vector<Frame>> frames; // todo: allocator optimizaiton
};

template <class T = void>
class Task {
public:
    Task() = delete;

    Task(Task const&) = default;
    Task& operator=(Task const&) = default;

    Task(Task&& rhs) = default;
    Task& operator=(Task&& rhs) = default;

    struct promise_type {
        promise_type() {}

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        struct Awaiter {
            // todo: if we have continuation and it is in the same executor, we can avoid
            // scheduling it
            bool await_ready() const noexcept {
                return false;
            }

            // todo: if you ever decide to respect coroutines' associated executor,
            // watch this
            template <class U>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<U>) noexcept {
                if (!c) {
                    return std::noop_coroutine();
                }
                return c.get();
            }

            void await_resume() noexcept {
            }
        };

        auto final_suspend() noexcept {
            // todo: Optimize to not keep shared ref if we are not going to continue the
            // caller right away scheduling it to its not executor
            return Awaiter{this->continuation.lock()};
        }

        template <class U>
        void return_value(U&& val) noexcept {
            static_assert(noexcept(noexcept(T(std::forward<U>(val)))));
            result.template emplace<1>(std::forward<U>(val));
        }

        void unhandled_exception() {
            result.template emplace<2>(std::current_exception());
        }

        std::variant<std::monostate, T, std::exception_ptr> result;

        std::weak_ptr<TaskStack> stack; // todo: implement own weak ptr
    };

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    auto await_suspend(std::coroutine_handle<U> caller) noexcept {
        auto const stack = caller.stack.lock();
        assert(stack);
        this->co->promise().stack = stack;
        return this->co.get();
    }

    T await_resume() {
        return std::move(*this).get_result();
    }

    T get_result() && {
        rpc_assert(co->promise().result.index() != 0, Invariant{});

        RPC_SCOPE_EXIT {
            co.reset();
        };

        if (co->promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(co->promise().result)));
        }

        return get<1>(std::move(co->promise().result));
    }

    void resume() {
        rpc_assert(co, Invariant{});
        co->resume();
    }

    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

    std::shared_ptr<TaskStack> stack;
};

template <>
class Task<void> {
public:
    Task() = delete;

    Task(Task const&) = default;
    Task& operator=(Task const&) = default;

    Task(Task&& rhs) = default;
    Task& operator=(Task&& rhs) = default;

    struct promise_type : public InsideCoRefRecord {
        promise_type()
                : InsideCoRefRecord(
                        std::coroutine_handle<promise_type>::from_promise(*this)
                                .address()) {
        }

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        struct Awaiter {
            ErasedCoSharedRef c;

            bool await_ready() const noexcept {
                return false;
            }

            // todo: if you ever decide to respect coroutines' associated executor,
            // watch this
            template <class T>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<T>) noexcept {
                if (!c) {
                    return std::noop_coroutine();
                }
                return c.get();
            }

            void await_resume() noexcept {
            }
        };

        auto final_suspend() noexcept {
            // todo: Optimize to not keep shared ref if we are not going to continue the
            // caller right away scheduling it to its not executor
            return Awaiter{this->continuation.lock()};
        }

        void return_void() noexcept {
            result.emplace<1>(Void{});
        }

        void unhandled_exception() {
            result.emplace<2>(std::current_exception());
        }

        struct Void {};

        std::variant<std::monostate, Void, std::exception_ptr> result;
        ErasedCoWeakRef continuation;

        std::weak_ptr<TaskStack> stack;
    };

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    auto await_suspend(std::coroutine_handle<U> caller) noexcept {
        this->co->promise().continuation = ErasedCoWeakRef{caller};
        return this->co.get();
    }

    void await_resume() {
        std::move(*this).get_result();
    }

    void get_result() && {
        rpc_assert(co->promise().result.index() != 0, Invariant{});

        RPC_SCOPE_EXIT {
            co.reset();
        };

        if (co->promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(co->promise().result)));
        }

        rpc_assert(co->promise().result.index() == 1, Invariant{});
    }

    void resume() {
        rpc_assert(co, Invariant{});
        co->resume();
    }

    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

    CoSharedRef<promise_type> co;
};

struct Executor {
    virtual void spawn(std::function<void()>&& task) = 0;
    virtual void spawn(std::function<void()>&& task, std::chrono::milliseconds after) = 0;
    virtual void increment_work() = 0;
    virtual void decrement_work() = 0;
    virtual ~Executor() = default;
};

inline thread_local Executor* current_executor = nullptr;

class ConditionalVariable {
public:
    ConditionalVariable() = default;

    ConditionalVariable(ConditionalVariable const&) = delete;
    ConditionalVariable& operator=(ConditionalVariable const&) = delete;

    ConditionalVariable(ConditionalVariable&&) = delete;
    ConditionalVariable& operator=(ConditionalVariable&&) = delete;

    void notify() noexcept {
        if (auto c = this->continuation.lock()) {
            rpc_assert(executor, Invariant{});
            executor->spawn([c]() mutable {
                if (c) {
                    c->resume();
                }
            });
        }
    }

    auto wait(std::unique_lock<std::mutex>& lock) noexcept {
        return Awaiter{this, &lock};
    }

private:
    struct Awaiter {
        bool await_ready() const noexcept {
            return false;
        }

        void await_resume() const noexcept {
        }

        template <class U>
        void await_suspend(std::coroutine_handle<U> c) noexcept {
            rpc_assert(current_executor, Invariant{});
            current_executor->increment_work();
            cv->executor = current_executor;
            cv->continuation = ErasedCoWeakRef{c};
            lock->unlock();
        }

        ~Awaiter() {
            rpc_assert(current_executor == cv->executor, Invariant{});
            current_executor->decrement_work();
            cv->continuation = ErasedCoWeakRef{};
            cv->executor = nullptr;
            lock->lock();
        }

        ConditionalVariable* cv;
        std::unique_lock<std::mutex>* lock;
    };

    ErasedCoWeakRef continuation;
    Executor* executor = nullptr;
};

template <class T>
class JoinHandle {
public:
    explicit JoinHandle(Task<T>&& task)
            : task{std::move(task)} {
    }

    ~JoinHandle() {
        if (!task.co || task.co->done()) {
            return;
        }

        auto co = task.co;

        auto pin = [](auto t) -> CoGuard {
            auto foo = std::move(t);
            co_return;
        }(std::move(task));

        co->promise().continuation = ErasedCoWeakRef{pin.co};
    }

    JoinHandle(JoinHandle const&) = delete;
    JoinHandle& operator=(JoinHandle const&) = delete;
    JoinHandle(JoinHandle&&) = default;
    JoinHandle& operator=(JoinHandle&&) = default;

    void abort() const noexcept {
        rpc_todo();
    }

    bool await_ready() const noexcept {
        return task.co->done();
    }

    template <class U>
    auto await_suspend(std::coroutine_handle<U> caller) noexcept {
        task.co->promise().continuation = ErasedCoWeakRef(caller);
    }

    auto await_resume() noexcept {
        return task.await_resume();
    }

private:
    struct CoGuard {
        struct promise_type : public InsideCoRefRecord {
            promise_type()
                    : InsideCoRefRecord(
                            std::coroutine_handle<promise_type>::from_promise(*this)
                                    .address()) {
                inc_refs();
            }

            CoGuard get_return_object() {
                return CoGuard{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept {
                return {};
            }

            std::suspend_always final_suspend() noexcept {
                dec_refs();
                return {};
            }

            void return_void() noexcept {
            }

            void unhandled_exception() noexcept {
            }
        };

        std::coroutine_handle<promise_type> co;
    };

private:
    Task<T> task;
};

class ThisThreadExecutor final : public Executor {
public:
    ThisThreadExecutor() {
        tasks.reserve(r);
    }

    /// \throw std::bad_alloc
    template <class T>
    T block_on(Task<T> task) {
        current_executor = this;
        task.resume();
        while (true) {
            using std::chrono_literals::operator""ms;
            using std::chrono::steady_clock;

            do {
                std::vector<Work> tasks2;
                tasks2.reserve(r);
                std::swap(tasks, tasks2);
                for (auto& task : tasks2) {
                    std::move(task)();
                }

                auto const now = steady_clock::now();
                auto const it = std::partition(
                        delayed_tasks.begin(), delayed_tasks.end(), [now](auto const& p) {
                            return now < p.second;
                        });

                std::vector<DelayedWork> delayed_tasks2{
                        std::make_move_iterator(it),
                        std::make_move_iterator(delayed_tasks.end())};
                delayed_tasks.erase(it, delayed_tasks.end());

                for (auto& task : delayed_tasks2) {
                    std::move(task).first();
                }
            } while (!tasks.empty());

            if (delayed_tasks.empty() && work == 0) {
                break;
            }

            auto const now = steady_clock::now() + 10ms;
            std::this_thread::yield();

            auto const soon_work = std::min_element(delayed_tasks.begin(),
                                                    delayed_tasks.end(),
                                                    [](auto const& lhs, auto const& rhs) {
                                                        return lhs.second < rhs.second;
                                                    });

            auto const soon = soon_work != delayed_tasks.end()
                                    ? std::min(now, soon_work->second)
                                    : now;

            std::this_thread::sleep_until(soon);
        }
        return std::move(task).get_result();
    }

    template <class T>
    JoinHandle<T> spawn(Task<T>&& task) {
        task.resume();
        return JoinHandle<T>{std::move(task)};
    }

private:
    void spawn(std::function<void()>&& task) override {
        tasks.push_back(std::move(task));
    }

    void spawn(std::function<void()>&& task, std::chrono::milliseconds after) override {
        delayed_tasks.emplace_back(std::move(task),
                                   std::chrono::steady_clock::now() + after);
    }

    void increment_work() override {
        ++work;
    }

    void decrement_work() override {
        --work;
    }

    using Work = std::function<void()>;
    using DelayedWork =
            std::pair<std::function<void()>, std::chrono::steady_clock::time_point>;

    static constexpr size_t r = 10;
    std::vector<Work> tasks;
    std::vector<DelayedWork> delayed_tasks;
    size_t work{0};
};

class Sleep {
public:
    explicit Sleep(std::chrono::milliseconds dur) noexcept
            : dur{dur}
            , executor{current_executor} {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template <class T>
    void await_suspend(std::coroutine_handle<T> caller) {
        rpc_assert(executor, Invariant{});
        executor->spawn(
                [caller = ErasedCoWeakRef{caller}]() mutable {
                    if (auto r = caller.lock()) {
                        r->resume();
                    }
                },
                dur);
    }

    void await_resume() noexcept {
    }

private:
    std::chrono::milliseconds dur;
    Executor* executor;
};

} // namespace rpc
