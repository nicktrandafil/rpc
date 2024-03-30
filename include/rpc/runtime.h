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

constexpr inline unsigned long long operator""_KB(unsigned long long const x) {
    return 1024L * x;
}

class TaskStack;

struct Executor {
    virtual void spawn(std::function<void()>&& task) = 0;
    virtual void spawn(std::function<void()>&& task, std::chrono::milliseconds after) = 0;
    virtual void cancel(TaskStack* x) noexcept = 0;
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

    void push(ErasedFrame frame) noexcept(false) {
        frames.push(frame);
    }

    ErasedFrame pop() noexcept {
        ErasedFrame ret = frames.top();
        frames.pop();
        return ret;
    }

    template </*todo: PromiseConcept*/ class T>
    Frame<T> top() const noexcept {
        return Frame<T>{
                .co = std::coroutine_handle<T>::from_address(frames.top().co.address()),
                .ex = frames.top().ex};
    }

    ErasedFrame erased_top() const noexcept {
        return frames.top();
    }

    template <class T>
    T take_result() noexcept(false) {
        rpc_assert(result_index != Result::none, Invariant{});

        RPC_SCOPE_EXIT {
            result_index = Result::none;
        };

        switch (result_index) {
        case Result::none:
            return static_cast<T>(rpc_unreachable(Invariant{}));
        case Result::exception:
            std::rethrow_exception(result_exception);
        case Result::value:
            if constexpr (std::is_void_v<T>) {
                return;
            } else {
                static_assert(std::is_nothrow_move_constructible_v<T>);
                static_assert(std::is_nothrow_destructible_v<T>);

                RPC_SCOPE_EXIT {
                    std::launder(reinterpret_cast<T*>(&result_value))->~T();
                };

                return std::move(*std::launder(reinterpret_cast<T*>(&result_value)));
            }
        }

        return static_cast<T>(rpc_unreachable(Invariant{}));
    }

    template <class T>
    void put_result(T&& val) noexcept(noexcept(std::forward<T>(val))) {
        static_assert(sizeof(val) <= sizeof(result_value));
        rpc_assert(result_index == Result::none, Invariant{});
        new (&result_value) T(std::forward<T>(val));
        destroy_value = +[](void* x) {
            static_cast<T*>(x)->~T();
        };
        result_index = Result::value;
    }

    void put_result() noexcept {
        rpc_assert(result_index == Result::none, Invariant{});
        result_index = Result::value;
    }

    void put_result(std::exception_ptr ptr) noexcept {
        rpc_assert(result_index == Result::none, Invariant{});
        result_exception = ptr;
        result_index = Result::exception;
    }

    size_t size() const noexcept {
        return frames.size();
    }

    ~TaskStack() noexcept {
        while (!frames.empty()) {
            frames.top().co.destroy();
            frames.pop();
        }

        if (result_index == Result::value && destroy_value != nullptr) {
            destroy_value(&result_value);
        }
    }

private:
    enum class Result {
        none,
        exception,
        value,
    } result_index;
    std::exception_ptr result_exception;
    std::aligned_storage_t<10_KB, alignof(std::max_align_t)> result_value;
    void (*destroy_value)(void*) = nullptr;

    std::stack<ErasedFrame, std::vector<ErasedFrame>>
            frames; // todo: allocator optimization
};

inline thread_local Executor* current_executor = nullptr;

inline std::coroutine_handle<> resume(Executor* current_executor,
                                      std::shared_ptr<TaskStack>&& stack) noexcept {
    return current_executor == stack->erased_top().ex
                 // immediately resume
                 ? stack->erased_top().co

                 // schedule
                 : [stack = std::move(stack)]() -> std::coroutine_handle<> {
        stack->erased_top().ex->spawn(
                [stack = std::move(stack) /*todo: maybe to keep weak pointer?*/] {
                    stack->erased_top().co.resume();
                });
        return std::noop_coroutine();
    }();
}

template </*PromiseConcept*/ class T>
struct FinalAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<T> co) noexcept {
        auto stack = co.promise().stack.lock();
        rpc_assert(stack, Invariant{});
        auto const frame = stack->pop();
        RPC_SCOPE_EXIT {
            frame.co.destroy();
        };
        return resume(frame.ex, std::move(stack));
    }

    void await_resume() noexcept {
    }
};

struct BasePromise {
    std::weak_ptr<TaskStack> stack; // todo: implement own weak ptr

    void unhandled_exception() {
        auto const stack = this->stack.lock();
        rpc_assert(stack, Invariant{});
        stack->put_result(std::current_exception());
    }
};

template <class In>
struct ReturnValue {
    template <class U>
    void return_value(U&& val) noexcept(noexcept(std::decay_t<U>(std::forward<U>(val)))) {
        auto const stack = static_cast<In*>(this)->stack.lock();
        rpc_assert(stack, Invariant{});
        stack->put_result(std::forward<U>(val));
    }
};

template <class In>
struct ReturnVoid {
    void return_void() noexcept {
        auto const stack = static_cast<In*>(this)->stack.lock();
        rpc_assert(stack, Invariant{});
        stack->put_result();
    }
};

template <class T>
class Task {
public:
    Task() = delete;

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& rhs) = default;
    Task& operator=(Task&& rhs) = default;

    struct promise_type
            : BasePromise
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        promise_type() = default;

        promise_type(promise_type const&) = delete;
        promise_type& operator=(promise_type const&) = delete;
        promise_type(promise_type&&) = delete;
        promise_type& operator=(promise_type&&) = delete;

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
            return FinalAwaiter<promise_type>{};
        }
    };

    bool await_ready() const noexcept {
        return false;
    }

    // This is the point where a new coroutine attaches to a existing stack of coroutines
    template <class U>
    std::coroutine_handle<promise_type> await_suspend(
            std::coroutine_handle<U> caller) noexcept(false) {
        this->stack = caller.promise().stack;
        auto const stack = this->stack.lock();
        rpc_assert(stack, Invariant{});
        rpc_assert(co, Invariant{});
        stack->push(TaskStack::ErasedFrame{.co = co, .ex = current_executor});
        co.promise().stack = stack;
        return co;
    }

    T await_resume() noexcept(false) {
        return std::move(*this).get_result();
    }

    T get_result() && noexcept(false) {
        auto stack = this->stack.lock();
        rpc_assert(stack, Invariant{});
        return stack->template take_result<T>();
    }

    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

    std::coroutine_handle<promise_type> co;
    std::weak_ptr<TaskStack> stack;
};

/*class ConditionalVariable {
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
};*/

class Error : std::exception {};

class Canceled : Error {
    char const* what() const noexcept override {
        return "canceled";
    }
};

template <class T>
struct JoinHandle {
    struct promise_type
            : BasePromise
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        std::optional<std::weak_ptr<TaskStack>> continuation;

#if defined(__clang__)
        promise_type(Task<T> const&, std::weak_ptr<TaskStack> x)
                : BasePromise{std::move(x)} {
        }
#else
        template <class U>
        promise_type(U const&, Task<T> const&, std::weak_ptr<TaskStack> x)
                : BasePromise{std::move(x)} {
        }
#endif

        promise_type(promise_type const&) = delete;
        promise_type& operator=(promise_type const&) = delete;
        promise_type(promise_type&&) = delete;
        promise_type& operator=(promise_type&&) = delete;

        JoinHandle get_return_object() noexcept(false) {
            auto const stack = this->stack.lock();
            rpc_assert(stack, Invariant{});
            auto const co = std::coroutine_handle<promise_type>::from_promise(*this);
            stack->push(TaskStack::ErasedFrame{.co = co, .ex = current_executor});
            return JoinHandle{co, stack};
        }

        std::suspend_never initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() const noexcept {
            struct Awaiter {
                std::weak_ptr<TaskStack> stack;
                std::optional<std::weak_ptr<TaskStack>> continuation;

                bool await_ready() const noexcept {
                    return !continuation.has_value();
                }

                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<promise_type>) noexcept {
                    auto const our_stack = this->stack.lock();
                    if (!our_stack) {
                        return std::noop_coroutine();
                    }

                    auto const frame = our_stack->erased_top();
                    RPC_SCOPE_EXIT {
                        frame.co.destroy();
                    };

                    auto continuation_stack = this->continuation.value().lock();
                    if (!continuation_stack) {
                        return std::noop_coroutine();
                    }

                    return resume(frame.ex, std::move(continuation_stack));
                }

                void await_resume() noexcept {
                    if (auto const our_stack = this->stack.lock()) {
                        our_stack->pop();
                        rpc_assert(our_stack->size() == 0, Invariant{});
                    }
                }
            };

            auto const stack = this->stack.lock();
            rpc_assert(stack, Invariant{});
            current_executor->cancel(stack.get());

            return Awaiter{stack, continuation};
        }
    };

    bool await_ready() const noexcept {
        auto const stack = this->stack.lock();
        return !stack || stack->size() == 0;
    }

    template <class U>
    bool await_suspend(std::coroutine_handle<U> caller) const noexcept(false) {
        auto const our_stack = this->stack.lock();
        rpc_assert(our_stack, Invariant{});
        rpc_assert(co, Invariant{});
        if (await_ready()) {
            return false;
        } else {
            co.promise().continuation = caller.promise().stack;
            return true;
        }
    }

    T await_resume() noexcept(false) {
        return std::move(*this).get_result();
    }

    T get_result() && noexcept(false) {
        auto const stack = this->stack.lock();
        if (!stack) {
            throw Canceled{};
        }

        rpc_assert(stack, Invariant{});
        return stack->template take_result<T>();
    }

    JoinHandle(std::coroutine_handle<promise_type> co,
               std::shared_ptr<TaskStack> stack) noexcept
            : co{co}
            , stack{stack}
            , stack_guard{std::move(stack)} {
        executor = current_executor;
    }

    JoinHandle(JoinHandle const&) = delete;
    JoinHandle& operator=(JoinHandle const&) = delete;

    JoinHandle(JoinHandle&&) = default;
    JoinHandle& operator=(JoinHandle&&) = default;

    void abort() {
        if (!await_ready()) {
            stack_guard.reset();
            executor->cancel(stack.lock().get());
        }
    }

    Executor* executor;
    std::coroutine_handle<promise_type> co;

    std::weak_ptr<TaskStack> stack;
    std::shared_ptr<TaskStack> stack_guard;
};

class ThisThreadExecutor final : public Executor {
public:
    ThisThreadExecutor() {
        tasks.reserve(r);
    }

    template <class T>
    T block_on(Task<T> task) noexcept(false) {
        current_executor = this;

        struct Stack {
            std::shared_ptr<TaskStack> stack;

            struct promise_type
                    : BasePromise
                    , std::conditional_t<std::is_void_v<T>,
                                         ReturnVoid<promise_type>,
                                         ReturnValue<promise_type>> {
                Stack get_return_object() noexcept(false) {
                    Stack ret{std::make_shared<TaskStack>()};
                    stack = ret.stack;
                    ret.stack->push(TaskStack::ErasedFrame{
                            .co = std::coroutine_handle<promise_type>::from_promise(
                                    *this),
                            .ex = current_executor});
                    return ret;
                }

                std::suspend_never initial_suspend() const noexcept {
                    return {};
                }

                std::suspend_never final_suspend() const noexcept {
                    auto const stack = this->stack.lock();
                    rpc_assert(stack, Invariant{});
                    stack->pop();
                    return {};
                }
            };
        };

        auto t = [&](auto task) -> Stack {
            co_return co_await task;
        }(std::move(task));

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

        return t.stack->template take_result<T>();
    }

    template <class T>
    JoinHandle<T> spawn(Task<T>&& task) {
        current_executor = this;
        spawned_tasks.emplace_back(std::make_shared<TaskStack>());
        auto t = [&](Task<T> task, std::weak_ptr<TaskStack>) -> JoinHandle<T> {
            co_return co_await task;
        }(std::move(task), spawned_tasks.back());
        return t;
    }

    void cancel(TaskStack* x) noexcept override {
        auto const it = std::find_if(
                spawned_tasks.begin(), spawned_tasks.end(), [x](auto const& y) {
                    return y.get() == x;
                });
        rpc_assert(it != spawned_tasks.end(), Invariant{});
        spawned_tasks.erase(it);
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
    std::vector<std::shared_ptr<TaskStack>> spawned_tasks;
    size_t work{0};
};

class Sleep {
public:
    explicit Sleep(std::chrono::milliseconds dur) noexcept
            : dur{dur} {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template <class T>
    void await_suspend(std::coroutine_handle<T> caller) {
        auto caller_stack = caller.promise().stack.lock();
        rpc_assert(caller_stack, Invariant{});
        caller_stack->erased_top().ex->spawn(
                [caller_stack = std::weak_ptr{caller_stack}]() mutable {
                    if (auto const r = caller_stack.lock()) {
                        r->erased_top().co.resume();
                    }
                },
                dur);
    }

    void await_resume() noexcept {
    }

private:
    std::chrono::milliseconds dur;
};

} // namespace rpc
