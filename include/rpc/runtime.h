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
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// hypothesis:
// * a coroutine can have at most two references to it

#define rpc_print(...) std::format_to(std::ostreambuf_iterator{std::cout}, __VA_ARGS__)

namespace rpc {

class OutsideCoRefRecord {
public:
    explicit OutsideCoRefRecord(void* ptr) noexcept
            : ptr{ptr} {
    }

    void inc_refs() noexcept {
        ++refs;
    }

    size_t dec_refs() noexcept {
        return --refs;
    }

    size_t get_refs() const noexcept {
        return refs;
    }

    void* get_ptr() const noexcept {
        return ptr;
    }

    void reset_ptr() noexcept {
        ptr = nullptr;
    }

private:
    void* ptr = nullptr;
    size_t refs = 0;
};

class InsideCoRefRecord {
public:
    explicit InsideCoRefRecord(void* co) noexcept(false)
            : ptr{new OutsideCoRefRecord{co}} {
    }

    void inc_refs() noexcept {
        ++refs;
    }

    size_t dec_refs() noexcept {
        return --refs;
    }

    OutsideCoRefRecord* get_outside_ptr() noexcept {
        return ptr;
    }

    size_t get_refs() const noexcept {
        return refs;
    }

private:
    OutsideCoRefRecord* ptr;
    size_t refs = 0;
};

template <class T>
class CoSharedRef {
public:
    explicit CoSharedRef(std::coroutine_handle<T> co) noexcept
            : co{co} {
        if (co) {
            co.promise().inc_refs();
        }
    }

    ~CoSharedRef() noexcept {
        if (co && co.promise().dec_refs() == 0) {
            if (co.promise().get_outside_ptr()->get_refs() == 0) {
                delete co.promise().get_outside_ptr();
            } else {
                co.promise().get_outside_ptr()->reset_ptr();
            }
            co.destroy();
        }
    }

    CoSharedRef(CoSharedRef const& rhs) noexcept
            : co{rhs.co} {
        if (co) {
            co.promise().inc_refs();
        }
    }

    CoSharedRef& operator=(CoSharedRef const& rhs) noexcept {
        CoSharedRef<T> tmp{rhs};
        *this = std::move(tmp);
        return *this;
    }

    CoSharedRef(CoSharedRef&& rhs) noexcept
            : co{rhs.co} {
        rhs.co = nullptr;
    }

    CoSharedRef& operator=(CoSharedRef&& rhs) noexcept {
        this->~CoSharedRef();
        new (this) CoSharedRef(std::move(rhs));
        return *this;
    }

    auto get() const noexcept {
        return co;
    }

    std::coroutine_handle<T>* operator->() noexcept {
        return &co;
    }

    std::coroutine_handle<T> const* operator->() const noexcept {
        return &co;
    }

    explicit operator bool() const noexcept {
        return !!co;
    }

    void reset() noexcept {
        CoSharedRef tmp{nullptr};
        std::swap(*this, tmp);
    }

private:
    std::coroutine_handle<T> co;
};

class ErasedCoSharedRef {
public:
    ErasedCoSharedRef() = default;

    template <class T>
    explicit ErasedCoSharedRef(std::coroutine_handle<T> co) noexcept
            : co{co}
            , inc_refs{+[](void* co) {
                std::coroutine_handle<T>::from_address(co).promise().inc_refs();
            }}
            , dec_refs{+[](void* co) {
                return std::coroutine_handle<T>::from_address(co).promise().dec_refs();
            }}
            , destroy{+[](void* co) {
                std::coroutine_handle<T>::from_address(co).destroy();
            }}
            , get_outside_refs{+[](void* co) {
                return std::coroutine_handle<T>::from_address(co)
                        .promise()
                        .get_outside_ptr()
                        ->get_refs();
            }}
            , destroy_outside_record{+[](void* co) {
                delete std::coroutine_handle<T>::from_address(co)
                        .promise()
                        .get_outside_ptr();
            }}
            , reset_outside_ptr{+[](void* co) {
                std::coroutine_handle<T>::from_address(co)
                        .promise()
                        .get_outside_ptr()
                        ->reset_ptr();
            }} {
        if (co) {
            co.promise().inc_refs();
        }
    }

    ~ErasedCoSharedRef() noexcept {
        if (co && dec_refs(co.address()) == 0) {
            if (get_outside_refs(co.address()) == 0) {
                destroy_outside_record(co.address());
            } else {
                reset_outside_ptr(co.address());
            }
            destroy(co.address());
        }
    }

    ErasedCoSharedRef(ErasedCoSharedRef const& rhs) noexcept
            : co{rhs.co}
            , inc_refs{rhs.inc_refs}
            , dec_refs{rhs.dec_refs}
            , destroy{rhs.destroy}
            , get_outside_refs{rhs.get_outside_refs}
            , destroy_outside_record{rhs.destroy_outside_record} {
        if (co) {
            inc_refs(co.address());
        }
    }

    ErasedCoSharedRef& operator=(ErasedCoSharedRef const& rhs) {
        ErasedCoSharedRef tmp{rhs};
        *this = std::move(tmp);
        return *this;
    }

    ErasedCoSharedRef(ErasedCoSharedRef&& rhs) noexcept
            : co{rhs.co}
            , inc_refs{rhs.inc_refs}
            , dec_refs{rhs.dec_refs}
            , destroy{rhs.destroy}
            , get_outside_refs{rhs.get_outside_refs}
            , destroy_outside_record{rhs.destroy_outside_record} {
        rhs.co = nullptr;
    }

    ErasedCoSharedRef& operator=(ErasedCoSharedRef&& rhs) {
        this->~ErasedCoSharedRef();
        new (this) ErasedCoSharedRef(std::move(rhs));
        return *this;
    }

    std::coroutine_handle<> get() const noexcept {
        return co;
    }

    std::coroutine_handle<>* operator->() noexcept {
        return &co;
    }

    explicit operator bool() const noexcept {
        return !!get();
    }

private:
    using IncRef = void (*)(void*);
    using DecRef = size_t (*)(void*);
    using Destroy = void (*)(void*);

    using GetOutsideRefs = size_t (*)(void*);
    using DestroyOutsideRecord = void (*)(void*);
    using ResetOutsidePtr = void (*)(void*);

    std::coroutine_handle<> co = nullptr;
    IncRef inc_refs;
    DecRef dec_refs;
    Destroy destroy;
    GetOutsideRefs get_outside_refs;
    DestroyOutsideRecord destroy_outside_record;
    ResetOutsidePtr reset_outside_ptr;
};

class ErasedCoWeakRef {
public:
    ErasedCoWeakRef() = default;

    template <class T>
    explicit ErasedCoWeakRef(std::coroutine_handle<T> co) noexcept
            : lock_impl{+[](void* co) {
                return ErasedCoSharedRef{std::coroutine_handle<T>::from_address(co)};
            }} {
        if (co) {
            rec = co.promise().get_outside_ptr();
            rec->inc_refs();
        }
    }

    ~ErasedCoWeakRef() noexcept {
        if (rec && rec->dec_refs() == 0 && !rec->get_ptr()) {
            delete rec;
        }
    }

    ErasedCoWeakRef(ErasedCoWeakRef const& rhs) noexcept
            : rec{rhs.rec}
            , lock_impl{rhs.lock_impl} {
        if (rec) {
            rec->inc_refs();
        }
    }

    ErasedCoWeakRef& operator=(ErasedCoWeakRef const& rhs) noexcept {
        ErasedCoWeakRef tmp{rhs};
        *this = std::move(tmp);
        return *this;
    }

    ErasedCoWeakRef(ErasedCoWeakRef&& rhs) noexcept
            : rec{rhs.rec}
            , lock_impl{rhs.lock_impl} {
        rhs.rec = nullptr;
    }

    ErasedCoWeakRef& operator=(ErasedCoWeakRef&& rhs) noexcept {
        this->~ErasedCoWeakRef();
        new (this) ErasedCoWeakRef(std::move(rhs));
        return *this;
    }

    ErasedCoSharedRef lock() const noexcept {
        if (rec && rec->get_ptr()) {
            return lock_impl(rec->get_ptr());
        }
        return {};
    }

private:
    OutsideCoRefRecord* rec = nullptr;
    using Lock = ErasedCoSharedRef (*)(void*);
    Lock lock_impl;
};

template <class T = void>
class Task {
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
        ErasedCoWeakRef continuation;
    };

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    auto await_suspend(std::coroutine_handle<U> caller) noexcept {
        this->co->promise().continuation = ErasedCoWeakRef{caller};
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

    CoSharedRef<promise_type> co;
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

        auto pin = [](auto co) -> CoGuard {
            std::ignore = co;
            co_return;
        }(task.co);

        task.co->promise().continuation = ErasedCoWeakRef{pin.co};
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
    // Live until final_suspend
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
