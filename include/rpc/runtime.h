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

struct Executor {
    virtual void spawn(std::function<void()>&& task) = 0; // todo: better function
    virtual void spawn(std::function<void()>&& task,
                       std::chrono::milliseconds after) = 0; // todo: better function
    virtual void increment_work() = 0;
    virtual void decrement_work() = 0;
    virtual ~Executor() = default;
};

class TaskStack {
public:
    template <class T>
    struct Frame {
        std::coroutine_handle<T> co;
        Executor* ex;
    };

    struct ErasedFrame {
        std::coroutine_handle<> co;
        Executor* ex;
    };

    template <class T>
    void push(Frame<T> frame) noexcept(false) {
        frames.push(ErasedFrame{
                .co = frame.co,
                .ex = frame.ex,
        });
    }

    template <class T>
    Frame<T> pop() noexcept {
        Frame<T> ret{
                .co = std::coroutine_handle<T>::from_address(frames.top().co.address()),
                .ex = frames.top().ex};
        frames.pop();
        return ret;
    }

    template </*todo: PromiceConcept*/ class T>
    Frame<T> top() const noexcept {
        return Frame{
                .co = std::coroutine_handle<T>::from_address(frames.top().co.address()),
                .ex = frames.top().ex};
    }

    ErasedFrame erased_top() const noexcept {
        return ErasedFrame{.co = frames.top().co, .ex = frames.top().ex};
    }

    template <class T>
    T take_result() noexcept(false) {
        static_assert(std::is_nothrow_move_constructible_v<T>);
        rpc_assert(result_index != Result::none, Invariant{});
        switch (result_index) {
        case Result::none:
            return rpc_unreachable(Invariant{});
        case Result::exception:
            std::rethrow_exception(result_exception);
        case Result::value:
            result_index = Result::none;
            return std::move(*std::launder(reinterpret_cast<T*>(&result_value)));
        }
    }

private:
    enum class Result {
        none,
        exception,
        value,
    } result_index;
    std::exception_ptr result_exception;
    std::aligned_storage<10_KB, alignof(std::max_align_t)> result_value;

    std::stack<ErasedFrame, std::vector<ErasedFrame>>
            frames; // todo: allocator optimizaiton
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

        // todo: All the operations performed in `await_suspend` can be performed here,
        // but then the caller coroutine would resume before the current coroutine is
        // destroyed. We cannot destroy the current coroutine here because it has not yet
        // been suspended. If we move the contents of `await_suspend` here, we would
        // sacrifice the guaranteed destruction order.
        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                bool await_ready() const noexcept {
                    return true;
                }

                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<promise_type> co) noexcept {
                    auto stack = co.promise().stack.lock();
                    rpc_assert(stack, Invariant{});
                    TaskStack::Frame<promise_type> const frame =
                            stack->template pop<promise_type>();

                    RPC_SCOPE_EXIT {
                        frame.co.destroy();
                    };

                    return frame.ex == stack->erased_top().ex
                                 // immediately resume
                                 ? stack->template top<promise_type>().co

                                 // schedule
                                 : [stack = std::move(stack)] {
                                       stack->erased_top().ex->spawn(
                                               [stack = std::move(stack)] {
                                                   stack->erased_top().co.resume();
                                               });
                                       return std::noop_coroutine();
                                   }();
                }

                void await_resume() noexcept {
                }
            };

            return FinalAwaiter{};
        }

        template <class U>
        void return_value(U&& val) noexcept(noexcept(T(std::forward<U>(val)))) {
            auto const lock = stack.lock();
            rpc_assert(lock, Invariant{});
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

    T get_result() && noexcept(false) {
        RPC_SCOPE_EXIT {
            stack.reset();
        };
        return stack->take_result<T>();
    }

    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

    std::shared_ptr<TaskStack> stack;
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
