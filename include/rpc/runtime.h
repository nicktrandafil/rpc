#pragma once

#include "contract.h"

#include <sys/epoll.h>

#include <coroutine>
#include <cstring>
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

    T get_result() {
        if (coroutine.promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(coroutine.promise().result)));
        }
        return get<1>(std::move(coroutine.promise().result));
    }

    void resume() {
        RPC_ASSERT(coroutine, Invariant{});
        coroutine.resume();
    }

private:
    Task(std::coroutine_handle<promise_type> coroutine) noexcept
            : coroutine(coroutine) {
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

    void get_result() {
        if (coroutine.promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(coroutine.promise().result)));
        }
        RPC_ASSERT(coroutine.promise().result.index() == 1, Invariant{});
    }

    void resume() {
        RPC_ASSERT(coroutine, Invariant{});
        coroutine.resume();
    }

private:
    Task(std::coroutine_handle<promise_type> coroutine) noexcept
            : coroutine(coroutine) {
    }

private:
    std::coroutine_handle<promise_type> coroutine;
};

struct Executor : public std::enable_shared_from_this<Executor> {
    virtual void spawn(std::function<void()>&& task) = 0;
    virtual void increment_work() = 0;
    virtual void decrement_work() = 0;
    virtual ~Executor() = default;
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
        task.resume();

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

class IOContext final : public std::enable_shared_from_this<IOContext> {
public:
    IOContext(std::shared_ptr<Executor> executor)
            : executor(executor)
            , epollfd(0) {
        epollfd = epoll_create1(0);
        if (epollfd == -1) {
            throw std::runtime_error(std::strerror(errno));
        }
    }

    ~IOContext() noexcept {
        auto const res = close(epollfd);
        RPC_ASSERT(res != -1, Invariant{});
    }

    IOContext(IOContext const&) = delete;
    IOContext& operator=(IOContext const&) = delete;
    IOContext(IOContext&&) = delete;
    IOContext& operator=(IOContext&&) = delete;

    Task<void> readable(int fd) {
        struct ReadableSignal {
            ReadableSignal(int fd, IOContext* io)
                    : fd{fd}
                    , io{io} {
            }

            bool await_ready() noexcept {
                return false;
            }
            void await_suspend(std::coroutine_handle<> handle) {
                {
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLONESHOT;
                    ev.data.fd = fd;
                    ev.data.ptr = handle.address();
                    epoll_ctl(io->epollfd, EPOLL_CTL_MOD, fd, &ev);
                    io->poll();
                }
            }
            void await_resume() noexcept {
            }

            int fd;
            IOContext* io;
        };

        co_await ReadableSignal{fd, this};
    }

    void check_in(int fd) noexcept {
        epoll_event ev{};
        ev.data.fd = fd;
        auto const res = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
        RPC_ASSERT(res == 0, UnlikelyAbort{});
    }

    void check_out(int fd) noexcept {
        auto const res = epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, nullptr);
        RPC_ASSERT(res == 0, UnlikelyAbort{});
    }

private:
    void notify_readable(std::coroutine_handle<> coro) {
        executor->spawn([coro] { coro.resume(); });
    }

    void poll() {
        auto option_n = epoll_wait(epollfd, events.data(), 1, 0);
        RPC_ASSERT(0 <= option_n, UnlikelyAbort{});
        if (option_n == 0) {
            executor->spawn([self = shared_from_this()] { self->poll(); });
        } else {
            for (size_t i = 0, n = static_cast<size_t>(option_n); i < n; ++i) {
                if (events[i].events & EPOLLIN) {
                    auto const handle =
                            std::coroutine_handle<>::from_address(events[i].data.ptr);
                    notify_readable(handle);
                }
            }
        }
    }

private:
    static constexpr size_t r = 10;
    std::shared_ptr<Executor> executor;
    int epollfd;
    std::array<epoll_event, 1> events;
};

} // namespace rpc
