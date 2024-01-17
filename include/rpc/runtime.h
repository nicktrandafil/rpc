#pragma once

#include "contract.h"

#include <coroutine>
#include <functional>
#include <memory>
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

struct Executor {
    virtual void spawn(std::function<void()>&& task) = 0;
    virtual ~Executor() = default;
};

class ThisThreadExecutor final
        : public Executor
        , public std::enable_shared_from_this<ThisThreadExecutor> {
public:
    ThisThreadExecutor() {
        tasks.reserve(r);
    }

    /// \throw std::bad_alloc
    template <class T>
    T block_on(Task<T>&& task) {
        task.launch();
        while (!tasks.empty()) {
            std::vector<std::function<void()>> tasks2;
            tasks2.reserve(r);
            std::swap(tasks, tasks2);
            for (auto& task : tasks2) {
                std::move(task)();
            }
        }
        return task.get_result();
    }

    void spawn(std::function<void()>&& task) override {
        tasks.push_back(std::move(task));
    }

private:
    static constexpr size_t r = 10;
    std::vector<std::function<void()>> tasks;
};

} // namespace rpc
