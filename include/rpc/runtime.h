#pragma once

#include "contract.h"

#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rpc {

template <class T = void>
class Task {
public:
    Task() = delete;

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& rhs) noexcept
            : coroutine(rhs.coroutine) {
        rhs.coroutine = nullptr;
    }

    Task& operator=(Task&& rhs) noexcept {
        this->~Task();
        new (this) Task(std::move(rhs));
        return *this;
    }

    ~Task() noexcept {
        if (coroutine) {
            coroutine.destroy();
        }
    }

    struct promise_type {
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                promise_type* me;
                bool await_ready() const noexcept {
                    return false;
                }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
                    if (!me->continuation) {
                        return std::noop_coroutine();
                    }

                    if (me->canceled.test(std::memory_order_acquire)) {
                        me->continuation.destroy();
                        return std::noop_coroutine();
                    }

                    return me->continuation;
                }
                void await_resume() noexcept {
                }
            };
            return Awaiter{this};
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
        std::coroutine_handle<> continuation;
        std::atomic_flag canceled = ATOMIC_FLAG_INIT;
    };

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> coroutine) noexcept {
        this->coroutine.promise().continuation = coroutine;
        return this->coroutine;
    }

    T await_resume() {
        return get_result();
    }

private:
    friend struct Executor;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
            : coroutine(coroutine) {
    }

    void launch() {
        RPC_ASSERT(coroutine, Invariant{});
        coroutine.resume();
    }

    T get_result() {
        if (coroutine.promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(coroutine.promise().result)));
        }
        return get<1>(std::move(coroutine.promise().result));
    }

private:
    std::coroutine_handle<promise_type> coroutine;
};

template <>
class Task<void> {
public:
    Task() = delete;

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& rhs) noexcept
            : coroutine(rhs.coroutine) {
        rhs.coroutine = nullptr;
    }

    Task& operator=(Task&& rhs) noexcept {
        this->~Task();
        new (this) Task(std::move(rhs));
        return *this;
    }

    ~Task() noexcept {
        if (coroutine) {
            coroutine.destroy();
        }
    }

    struct promise_type {
        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                promise_type* me;
                bool await_ready() const noexcept {
                    return false;
                }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
                    if (me->continuation) {
                        return me->continuation;
                    } else {
                        return std::noop_coroutine();
                    }
                }
                void await_resume() noexcept {
                }
            };
            return Awaiter{this};
        }

        void return_void() noexcept {
            result.emplace<1>(Void{});
        }

        void unhandled_exception() {
            result.emplace<2>(std::current_exception());
        }

        struct Void {};

        std::variant<std::monostate, Void, std::exception_ptr> result;
        std::coroutine_handle<> continuation;
    };

    bool await_ready() const noexcept {
        return false;
    }

    auto await_suspend(std::coroutine_handle<> coroutine) noexcept {
        this->coroutine.promise().continuation = coroutine;
        return this->coroutine;
    }

    void await_resume() {
        get_result();
    }

private:
    friend struct Executor;

    template <class>
    friend class JoinHandle;

    Task(std::coroutine_handle<promise_type> coroutine) noexcept
            : coroutine(coroutine) {
    }

    void launch() {
        RPC_ASSERT(coroutine, Invariant{});
        coroutine.resume();
    }

    void get_result() {
        if (coroutine.promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(coroutine.promise().result)));
        }
        RPC_ASSERT(coroutine.promise().result.index() == 1, Invariant{});
    }

private:
    std::coroutine_handle<promise_type> coroutine;
};

struct Executor : public std::enable_shared_from_this<Executor> {
    virtual void spawn(std::function<void()>&& task) = 0;
    virtual void increment_work() = 0;
    virtual void decrement_work() = 0;
    virtual ~Executor() = default;

protected:
    template <class T>
    void launch(Task<T>& task) const noexcept {
        task.launch();
    }

    template <class T>
    decltype(auto) get_result(T&& task) const noexcept {
        return std::forward<T>(task).get_result();
    }
};

inline thread_local std::shared_ptr<Executor> current_executor;

class ConditionalVariable {
public:
    ConditionalVariable() = default;

    ConditionalVariable(ConditionalVariable const&) = delete;
    ConditionalVariable& operator=(ConditionalVariable const&) = delete;

    ConditionalVariable(ConditionalVariable&&) = delete;
    ConditionalVariable& operator=(ConditionalVariable&&) = delete;

    void notify() noexcept {
        if (auto const executor = this->executor.lock()) {
            auto h = this->continuation;
            this->continuation = nullptr;
            executor->spawn([h] {
                if (h) {
                    h.resume();
                }
            });
            this->executor.reset();
        }
    }

    auto wait(std::unique_lock<std::mutex>& lock) noexcept {
        struct Awaiter {
            bool await_ready() const noexcept {
                return false;
            }

            void await_resume() const noexcept {
                lock->lock();
                current_executor->decrement_work();
            }

            void await_suspend(std::coroutine_handle<> c) noexcept {
                RPC_ASSERT(current_executor, Invariant{});
                current_executor->increment_work();
                outer->executor = current_executor;
                outer->continuation = c;
            }

            ConditionalVariable* outer;
            std::unique_lock<std::mutex>* lock;
        };

        return Awaiter{this, &lock};
    }

private:
    std::coroutine_handle<> continuation;
    std::weak_ptr<Executor> executor;
};

template <class T>
class JoinHandle {
public:
    explicit JoinHandle(Task<T>&& task)
            : task{std::move(task)} {
    }

    void abort() const noexcept {
        task.coroutine.promise().test_and_set(std::memory_order_release);
    }

    Task<T> wait() const noexcept {
        co_return co_await task;
    }

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
    T block_on(Task<T>&& task) {
        launch(task);

        while (true) {
            // work
            while (!tasks.empty()) {
                std::vector<std::function<void()>> tasks2;
                tasks2.reserve(r);
                std::swap(tasks, tasks2);
                for (auto& task : tasks2) {
                    std::move(task)();
                }
            }

            if (work == 0) {
                break;
            }

            std::this_thread::yield();
        }

        return get_result(std::move(task));
    }

    template <class T>
    JoinHandle<T> spawn(Task<T>&& task) {
        launch(task);
        return JoinHandle<T>{std::move(task)};
    }

private:
    template <class T>
    friend class Task;

    void spawn(std::function<void()>&& task) override {
        tasks.push_back(std::move(task));
    }

    void increment_work() override {
        ++work;
    }

    void decrement_work() override {
        --work;
    }

    static constexpr size_t r = 10;
    std::vector<std::function<void()>> tasks;
    size_t work{0};
};

} // namespace rpc
