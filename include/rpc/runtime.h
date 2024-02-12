#pragma once

#include "contract.h"

#include <sys/epoll.h>

#include <coroutine>
#include <cstdio>
#include <cstring>
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

namespace rpc {

class CoWeakRefRecord {
public:
    explicit CoWeakRefRecord(void* ptr) noexcept
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

class CoRefRecord {
public:
    explicit CoRefRecord(void* co) noexcept(false)
            : weak{new CoWeakRefRecord{co}} {
    }

    void inc_refs() noexcept {
        ++refs;
    }

    size_t dec_refs() noexcept {
        return --refs;
    }

    CoWeakRefRecord* get_weak() noexcept {
        return weak;
    }

private:
    CoWeakRefRecord* weak;
    size_t refs = 0;
};

template <class T>
class CoRef {
public:
    explicit CoRef(std::coroutine_handle<T> co) noexcept
            : co{co} {
        if (co) {
            co.promise().inc_refs();
        }
    }

    ~CoRef() noexcept {
        if (co && co.promise().dec_refs() == 0) {
            if (co.promise().get_weak()->get_refs() == 0) {
                delete co.promise().get_weak();
            } else {
                co.promise().get_weak()->reset_ptr();
            }
            co.destroy();
        }
    }

    CoRef(CoRef const& rhs) noexcept
            : co{rhs.co} {
        if (co) {
            co.promise().inc_refs();
        }
    }

    CoRef& operator=(CoRef const& rhs) noexcept {
        CoRef<T> tmp{rhs};
        *this = std::move(tmp);
        return *this;
    }

    CoRef(CoRef&& rhs) noexcept
            : co{rhs.co} {
        rhs.co = nullptr;
    }

    CoRef& operator=(CoRef&& rhs) noexcept {
        this->~CoRef();
        new (this) CoRef(std::move(rhs));
        return *this;
    }

    auto get() const noexcept {
        return co;
    }

    std::coroutine_handle<T>* operator->() noexcept {
        return &co;
    }

    explicit operator bool() const noexcept {
        return !!co;
    }

private:
    std::coroutine_handle<T> co;
};

class ErasedCoRef {
public:
    ErasedCoRef() = default;

    template <class T>
    explicit ErasedCoRef(std::coroutine_handle<T> co) noexcept
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
            , get_weak_refs{+[](void* co) {
                return std::coroutine_handle<T>::from_address(co)
                        .promise()
                        .get_weak()
                        ->get_refs();
            }}
            , destroy_weak_record{+[](void* co) {
                delete std::coroutine_handle<T>::from_address(co).promise().get_weak();
            }}
            , reset_weak_ptr{+[](void* co) {
                std::coroutine_handle<T>::from_address(co)
                        .promise()
                        .get_weak()
                        ->reset_ptr();
            }} {
        if (co) {
            co.promise().inc_refs();
        }
    }

    ~ErasedCoRef() noexcept {
        if (co && dec_refs(co.address()) == 0) {
            if (get_weak_refs(co.address()) == 0) {
                destroy_weak_record(co.address());
            } else {
                reset_weak_ptr(co.address());
            }
            destroy(co.address());
        }
    }

    ErasedCoRef(ErasedCoRef const& rhs) noexcept
            : co{rhs.co}
            , inc_refs{rhs.inc_refs}
            , dec_refs{rhs.dec_refs}
            , destroy{rhs.destroy}
            , get_weak_refs{rhs.get_weak_refs}
            , destroy_weak_record{rhs.destroy_weak_record} {
        if (co) {
            inc_refs(co.address());
        }
    }

    ErasedCoRef& operator=(ErasedCoRef const& rhs) {
        ErasedCoRef tmp{rhs};
        *this = std::move(tmp);
        return *this;
    }

    ErasedCoRef(ErasedCoRef&& rhs) noexcept
            : co{rhs.co}
            , inc_refs{rhs.inc_refs}
            , dec_refs{rhs.dec_refs}
            , destroy{rhs.destroy}
            , get_weak_refs{rhs.get_weak_refs}
            , destroy_weak_record{rhs.destroy_weak_record} {
        rhs.co = nullptr;
    }

    ErasedCoRef& operator=(ErasedCoRef&& rhs) {
        this->~ErasedCoRef();
        new (this) ErasedCoRef(std::move(rhs));
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

    using GetWeakRefs = size_t (*)(void*);
    using DestroyWeakRef = void (*)(void*);
    using ResetWeakPtr = void (*)(void*);

    std::coroutine_handle<> co = nullptr;
    IncRef inc_refs;
    DecRef dec_refs;
    Destroy destroy;
    GetWeakRefs get_weak_refs;
    DestroyWeakRef destroy_weak_record;
    ResetWeakPtr reset_weak_ptr;
};

class ErasedCoWeakRef {
public:
    template <class T>
    explicit ErasedCoWeakRef(std::coroutine_handle<T> co) noexcept
            : lock_impl{+[](void* co) {
                return ErasedCoRef{std::coroutine_handle<T>::from_address(co)};
            }} {
        if (co) {
            rec = co.promise().get_weak();
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

    ErasedCoRef lock() const noexcept {
        if (rec && rec->get_ptr()) {
            return lock_impl(rec->get_ptr());
        }
        return {};
    }

private:
    CoWeakRefRecord* rec = nullptr;
    using Lock = ErasedCoRef (*)(void*);
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

    struct promise_type : public CoRefRecord {
        promise_type()
                : CoRefRecord(std::coroutine_handle<promise_type>::from_promise(*this)
                                      .address()) {
        }

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
                    return me->continuation.get();
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
        ErasedCoRef continuation;
    };

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    auto await_suspend(std::coroutine_handle<U> waiting) noexcept {
        this->co->promise().continuation = ErasedCoRef{waiting};
        return this->co.get();
    }

    T await_resume() {
        return get_result();
    }

    T get_result() {
        if (co->promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(co->promise().result)));
        }
        return get<1>(std::move(co->promise().result));
    }

    void resume() {
        RPC_ASSERT(co, Invariant{});
        co->resume();
    }

private:
    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

private:
    CoRef<promise_type> co;
};

template <>
class Task<void> {
public:
    Task() = delete;

    Task(Task const&) = default;
    Task& operator=(Task const&) = default;

    Task(Task&& rhs) = default;
    Task& operator=(Task&& rhs) = default;

    struct promise_type : public CoRefRecord {
        promise_type()
                : CoRefRecord(std::coroutine_handle<promise_type>::from_promise(*this)
                                      .address()) {
        }

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
                        return me->continuation.get();
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
        ErasedCoRef continuation;
    };

    bool await_ready() const noexcept {
        return false;
    }

    template <class U>
    auto await_suspend(std::coroutine_handle<U> waiting) noexcept {
        this->co->promise().continuation = ErasedCoRef{waiting};
        return this->co.get();
    }

    void await_resume() {
        get_result();
    }

    void get_result() {
        if (co->promise().result.index() == 2) {
            std::rethrow_exception(get<2>(std::move(co->promise().result)));
        }
        RPC_ASSERT(co->promise().result.index() == 1, Invariant{});
    }

    void resume() {
        RPC_ASSERT(co, Invariant{});
        co->resume();
    }

private:
    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

private:
    CoRef<promise_type> co;
};

struct Executor : public std::enable_shared_from_this<Executor> {
    virtual void spawn(std::function<void()>&& task) = 0;
    virtual void spawn(std::function<void()>&& task, std::chrono::milliseconds after) = 0;
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
            executor->spawn([c = this->continuation]() mutable {
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
            RPC_ASSERT(current_executor, Invariant{});
            current_executor->increment_work();
            cv->executor = current_executor;
            cv->continuation = ErasedCoRef{c};
            lock->unlock();
        }

        ~Awaiter() {
            RPC_ASSERT(current_executor == cv->executor.lock(), Invariant{});
            current_executor->decrement_work();
            cv->continuation = ErasedCoRef{};
            cv->executor.reset();
            lock->lock();
        }

        ConditionalVariable* cv;
        std::unique_lock<std::mutex>* lock;
    };

    ErasedCoRef continuation;
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
    ThisThreadExecutor() {
        tasks.reserve(r);
    }

public:
    static std::shared_ptr<ThisThreadExecutor> construct() {
        return std::shared_ptr<ThisThreadExecutor>(new ThisThreadExecutor{});
    }

    /// \throw std::bad_alloc
    template <class T>
    T block_on(Task<T>&& task) {
        current_executor = shared_from_this();
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
                auto const it =
                        std::partition(delayed_tasks.begin(),
                                       delayed_tasks.end(),
                                       [now](auto const& p) { return now < p.second; });

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
    void await_suspend(std::coroutine_handle<T> waiting) {
        if (auto const executor = this->executor.lock()) {
            executor->spawn(
                    [waiting = ErasedCoWeakRef{waiting}]() mutable {
                        if (auto r = waiting.lock()) {
                            r->resume();
                        }
                    },
                    dur);
        }
    }
    void await_resume() noexcept {
    }

private:
    std::chrono::milliseconds dur;
    std::weak_ptr<Executor> executor;
};

} // namespace rpc
