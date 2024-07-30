#pragma once

#include "contract.h"
#include "scope_exit.h"

#include <any>
#include <coroutine>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stack>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// todo: std::function with non trivial function object allocates, try to avoid this

#define alonite_print(...)                                                               \
    std::format_to(std::ostreambuf_iterator{std::cout}, __VA_ARGS__)

#define feature(x, msg) x

namespace alonite {

constexpr inline unsigned long long operator""_KB(unsigned long long const x) {
    return 1024L * x;
}

class TaskStack;

struct Executor {
    static unsigned next_id() noexcept {
        static unsigned id = 0; // todo: atomic
        return ++id;
    }

    virtual void spawn(std::function<void()>&& task) = 0;
    virtual void spawn(std::function<void()>&& task, std::chrono::milliseconds after) = 0;
    virtual bool remove_guard(TaskStack* x) noexcept = 0;
    virtual void add_guard(std::shared_ptr<TaskStack>&& x) noexcept(false) = 0;
    virtual void add_guard_group(
            unsigned id,
            std::vector<std::shared_ptr<TaskStack>> x) noexcept(false) = 0;
    virtual bool remove_guard_group(unsigned id) noexcept = 0;
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

    TaskStack() = default;

    explicit TaskStack(ErasedFrame&& x) noexcept(false)
            : frames{std::vector{std::move(x)}} {
    }

    TaskStack(TaskStack const&) = delete;

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

    constexpr static size_t soo_len = 24;

    template <class T>
    T take_result() noexcept(false) {
        alonite_assert(result_index != Result::none, Invariant{});

        ALONITE_SCOPE_EXIT {
            result_index = Result::none;
        };

        switch (result_index) {
        case Result::none:
            return static_cast<T>(alonite_unreachable(Invariant{}));
        case Result::exception:
            std::rethrow_exception(result_exception);
        case Result::value:
            if constexpr (std::is_void_v<T>) {
                return;
            } else {
                static_assert(std::is_nothrow_destructible_v<T>);

                ALONITE_SCOPE_EXIT {
                    destroy_value(result_value);
                    destroy_value = nullptr;
                };

                if constexpr (sizeof(T) <= soo_len) {
                    return std::move(
                            *std::launder(reinterpret_cast<T*>(&result_value.storage)));
                } else {
                    return std::move(*static_cast<T*>(result_value.ptr));
                }
            }
        }

        return static_cast<T>(alonite_unreachable(Invariant{}));
    }

    template <class T>
    void put_result(T&& val) noexcept(noexcept(std::forward<T>(val))) {
        using U = std::decay_t<T>;
        alonite_assert(result_index == Result::none, Invariant{});

        if constexpr (sizeof(T) <= soo_len) {
            new (&result_value.storage) U(std::forward<T>(val));
        } else {
            result_value.ptr = new T{std::forward<T>(val)};
        }

        destroy_value = +[](ResultType& result_value) {
            if constexpr (sizeof(T) <= soo_len) {
                std::launder(reinterpret_cast<U*>(&result_value.storage))->~U();
            } else {
                delete static_cast<U*>(result_value.ptr);
            }
        };

        result_index = Result::value;
    }

    void put_result() noexcept {
        alonite_assert(result_index == Result::none, Invariant{});
        result_index = Result::value;
    }

    void put_result(std::exception_ptr ptr) noexcept {
        alonite_assert(result_index == Result::none, Invariant{});
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
            destroy_value(result_value);
        }
    }

    unsigned tasks_to_complete = 0;
    unsigned tasks_completed = 0;

    unsigned completed_task_index = 0;

    std::shared_ptr<TaskStack> guard;

private:
    enum class Result {
        none,
        exception,
        value,
    } result_index = Result::none;
    std::exception_ptr result_exception;
    union ResultType {
        alignas(std::max_align_t) char storage[soo_len];
        void* ptr;
    } result_value;
    void (*destroy_value)(ResultType&) = nullptr;
    std::stack<ErasedFrame, std::vector<ErasedFrame>>
            frames; // todo: allocator optimization
};

inline thread_local Executor* current_executor = nullptr;

inline void schedule(std::shared_ptr<TaskStack>&& stack) {
    stack->erased_top().ex->spawn(
            [feature(stack = std::move(stack),
                     "Have both abort signal and ready result set in JoinHandle "
                     "in case of a race condition")] {
                stack->erased_top().co.resume();
            });
}

/// \post will not move if the `stack` is not scheduled
inline bool schedule_on_other_ex(std::shared_ptr<TaskStack>&& stack) {
    alonite_assert(stack, Invariant{});
    return stack->erased_top().ex != current_executor
        && (schedule(std::move(stack)), true);
}

inline std::coroutine_handle<> resume(Executor* current_executor,
                                      std::shared_ptr<TaskStack>&& stack) noexcept {
    return current_executor == stack->erased_top().ex
                 // immediately resume
                 ? stack->erased_top().co
                 // schedule
                 : [stack = std::move(stack)]() mutable -> std::coroutine_handle<> {
        schedule(std::move(stack));
        return std::noop_coroutine();
    }();
}

struct FinalAwaiter {
    std::shared_ptr<TaskStack> stack;

    bool await_ready() noexcept {
        return schedule_on_other_ex(std::move(stack));
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> co) noexcept {
        ALONITE_SCOPE_EXIT {
            co.destroy();
        };
        alonite_assert(stack, Invariant{});
        return stack->erased_top().co;
    }

    void await_resume() noexcept {
    }
};

struct BasePromise {
    std::weak_ptr<TaskStack> stack; // todo: implement own weak ptr

    void unhandled_exception() {
        auto const stack = this->stack.lock();
        alonite_assert(stack, Invariant{});
        stack->put_result(std::current_exception());
    }
};

template <class PromiseT>
struct ReturnValue {
    template <class U>
    void return_value(U&& val) noexcept(noexcept(std::decay_t<U>(std::forward<U>(val)))) {
        using T = typename PromiseT::ValueType;
        static_assert(std::is_convertible_v<U, T>,
                      "return expression should be convertible to return type");
        auto const stack = static_cast<PromiseT*>(this)->stack.lock();
        alonite_assert(stack, Invariant{});
        stack->template put_result<T>(std::forward<U>(val));
    }
};

template <class PromiseT>
struct ReturnVoid {
    void return_void() noexcept {
        static_assert(std::is_void_v<typename PromiseT::ValueType>);
        auto const stack = static_cast<PromiseT*>(this)->stack.lock();
        alonite_assert(stack, Invariant{});
        stack->put_result();
    }
};

template <class PromiseT, class CoroT>
struct CreateStack {
    CoroT get_return_object() noexcept(false) {
        auto const self = static_cast<PromiseT*>(this);
        auto const co = std::coroutine_handle<PromiseT>::from_promise(*self);
        CoroT ret{std::make_shared<TaskStack>(
                          TaskStack::ErasedFrame{.co = co, .ex = current_executor}),
                  co};
        self->stack = ret.stack;
        return ret;
    }
};

template <class T>
class Task {
public:
    Task() = delete; // todo:? maybe allow default construction, this would allow using
                     // tasks in initializer lists

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    Task(Task&& rhs) = default;
    Task& operator=(Task&& rhs) = default;

    struct promise_type
            : BasePromise
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType = T;

        promise_type() = default;

        promise_type(promise_type const&) = delete;
        promise_type& operator=(promise_type const&) = delete;
        promise_type(promise_type&&) = delete;
        promise_type& operator=(promise_type&&) = delete;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            auto stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            return FinalAwaiter{std::move(stack)};
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
        alonite_assert(stack, Invariant{});
        alonite_assert(co, Invariant{});
        stack->push(TaskStack::ErasedFrame{
                .co = co, .ex = current_executor}); // Current coroutine is suspended, so
                                                    // it is safe to modify the stack
        co.promise().stack = stack;
        return co;
    }

    T await_resume() noexcept(false) {
        return std::move(*this).get_result();
    }

    T get_result() && noexcept(false) {
        auto stack = this->stack.lock();
        alonite_assert(stack, Invariant{});
        return stack->template take_result<T>();
    }

    Task(std::coroutine_handle<promise_type> co) noexcept
            : co{co} {
    }

    std::coroutine_handle<promise_type> co;
    std::weak_ptr<TaskStack> stack;
};

class ConditionalVariable {
public:
    ConditionalVariable() = default;

    ConditionalVariable(ConditionalVariable const&) = delete;
    ConditionalVariable& operator=(ConditionalVariable const&) = delete;

    ConditionalVariable(ConditionalVariable&&) = delete;
    ConditionalVariable& operator=(ConditionalVariable&&) = delete;

    /// \note Can be called from synchronous code
    void notify() noexcept {
        if (auto c = this->continuation.lock()) {
            if (c->erased_top().ex == current_executor) {
                c->erased_top().co.resume();
            } else {
                c->erased_top().ex->spawn([c]() mutable {
                    c->erased_top().co.resume();
                });
            }
        }
    }

    auto wait() {
        return Awaiter{this};
    }

private:
    struct Awaiter {
        bool await_ready() const noexcept {
            return false;
        }

        void await_resume() const noexcept {
        }

        template <class U>
        void await_suspend(std::coroutine_handle<U> continuation) noexcept {
            current_executor->increment_work();
            cv->continuation = continuation.promise().stack;
        }

        ~Awaiter() noexcept {
            current_executor->decrement_work();
        }

        ConditionalVariable* cv;
    };

    std::weak_ptr<TaskStack> continuation;
};

struct Error : std::exception {};

template <class T = void>
struct Void {
    using WrapperT = T;
    static T take(TaskStack& stack) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return stack.template take_result<T>();
    }
};

template <>
struct Void<void> {
    using WrapperT = Void<void>;
    static Void take(TaskStack& stack) noexcept {
        stack.template take_result<void>();
        return Void{};
    }
    bool operator==(Void const&) const = default;
};

class Canceled : public Error {
public:
    Canceled() = default;

    template <class T>
    explicit Canceled(/*not forwarding*/ T&& t) noexcept(false)
            : result{std::move(t)} {
    }

    bool was_already_completed() const noexcept {
        return result.has_value();
    }

    template <class T>
    T get_result() const noexcept(false) {
        return std::any_cast<T>(result);
    }

    char const* what() const noexcept override {
        return "canceled";
    }

private:
    std::any result;
};

class TimedOut : public Error {
public:
    TimedOut() = default;

    char const* what() const noexcept override {
        return "timed out";
    }
};

template <class T>
struct JoinHandle {
    struct promise_type
            : BasePromise
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType = T;

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
            alonite_assert(stack, Invariant{});
            auto const co = std::coroutine_handle<promise_type>::from_promise(*this);
            stack->push(TaskStack::ErasedFrame{.co = co, .ex = current_executor});
            return JoinHandle{co, stack};
        }

        std::suspend_never initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() const noexcept {
            struct Awaiter {
                std::optional<std::shared_ptr<TaskStack>> continuation;

                bool await_ready() noexcept {
                    return !continuation.has_value()
                        || /* we need to suspend only to continue on other executor*/
                           schedule_on_other_ex(std::move(*continuation));
                }

                // todo: reuse
                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<promise_type> co) noexcept {
                    ALONITE_SCOPE_EXIT {
                        co.destroy();
                    };
                    // todo:? maybe release
                    return (*continuation)->erased_top().co;
                }

                void await_resume() noexcept {
                }
            };

            auto const stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            feature(current_executor->remove_guard(stack.get()),
                    "Have both abort signal and ready result set in JoinHandle in case "
                    "of a race condition");

            if (!continuation.has_value()) {
                return Awaiter{};
            } else if (auto continuation = this->continuation->lock()) {
                return Awaiter{std::move(continuation)};
            } else {
                return Awaiter{};
            }
        }
    };

    bool await_ready() const noexcept {
        auto const stack = this->stack.lock();
        return !stack || stack->size() == 0;
    }

    template <class U>
    bool await_suspend(std::coroutine_handle<U> caller) const noexcept(false) {
        auto const our_stack = this->stack.lock();
        alonite_assert(our_stack, Invariant{});
        alonite_assert(co, Invariant{});
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

        if (!stack_guard) {
            throw Canceled{Void<T>::take(*stack)};
        }

        this->stack_guard.reset();

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

    ~JoinHandle() /*noexcept(false) todo: ensure noexcept*/ {
        if (stack_guard) {
            executor->add_guard(std::move(stack_guard));
        }
    }

    bool abort() {
        if (!await_ready()) {
            stack_guard.reset();
            return true;
        } else {
            return false;
        }
    }

    Executor* executor; // todo: get from the stack
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
                using ValueType [[maybe_unused]] = T;

                Stack get_return_object() noexcept(false) {
                    Stack ret{std::make_shared<TaskStack>(TaskStack::ErasedFrame{
                            .co = std::coroutine_handle<promise_type>::from_promise(
                                    *this),
                            .ex = current_executor})};
                    stack = ret.stack;
                    return ret;
                }

                std::suspend_never initial_suspend() const noexcept {
                    return {};
                }

                std::suspend_never final_suspend() const noexcept {
                    auto const stack = this->stack.lock();
                    alonite_assert(stack, Invariant{});
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

    void add_guard(std::shared_ptr<TaskStack>&& x) noexcept(false) override {
        spawned_tasks.emplace(std::move(x));
    }

    void add_guard_group(unsigned id, std::vector<std::shared_ptr<TaskStack>> x) noexcept(
            false) override {
        spawned_task_groups.emplace(id, std::move(x));
    }

    bool remove_guard(TaskStack* x) noexcept override {
        if (auto const it = spawned_tasks.find(x); it != spawned_tasks.end()) {
            spawned_tasks.erase(it);
            return true;
        } else {
            return false;
        }
    }

    bool remove_guard_group(unsigned id) noexcept override {
        return spawned_task_groups.erase(id) > 0;
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

    struct Hash {
        struct is_transparent;

        size_t operator()(std::shared_ptr<TaskStack> const& x) const noexcept {
            return std::hash<TaskStack*>{}(x.get());
        }

        size_t operator()(TaskStack* x) const noexcept {
            return std::hash<TaskStack*>{}(x);
        }
    };

    struct Equal {
        struct is_transparent;

        bool operator()(std::shared_ptr<TaskStack> const& lhs,
                        std::shared_ptr<TaskStack> const& rhs) const noexcept {
            return lhs.get() == rhs.get();
        }

        bool operator()(std::shared_ptr<TaskStack> const& lhs,
                        TaskStack* rhs) const noexcept {
            return lhs.get() == rhs;
        }

        bool operator()(TaskStack* lhs,
                        std::shared_ptr<TaskStack> const& rhs) const noexcept {
            return lhs == rhs.get();
        }

        bool operator()(TaskStack* lhs, TaskStack* rhs) const noexcept {
            return lhs == rhs;
        }
    };

    static constexpr size_t r = 10;
    std::vector<Work> tasks;
    std::vector<DelayedWork> delayed_tasks;
    std::unordered_set<std::shared_ptr<TaskStack>, Hash, Equal> spawned_tasks;
    std::unordered_map<unsigned, std::vector<std::shared_ptr<TaskStack>>>
            spawned_task_groups;
    size_t work{0};
};

class ThreadPoolExecutor : public Executor {
public:
private:
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
        alonite_assert(caller_stack, Invariant{});
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

template <class TaskT /*Coroutine Concept*/>
class Timeout {
public:
    explicit Timeout(std::chrono::milliseconds duration, TaskT&& task) noexcept
            : dur{duration}
            , task{std::move(task)} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) {
        using T = typename TaskT::promise_type::ValueType;

        struct Stack {
            struct promise_type
                    : BasePromise
                    , CreateStack<promise_type, Stack>
                    , std::conditional_t<std::is_void_v<T>,
                                         ReturnVoid<promise_type>,
                                         ReturnValue<promise_type>> {
                using ValueType [[maybe_unused]] = T;

                std::weak_ptr<TaskStack> continuation;

                std::suspend_always initial_suspend() const noexcept {
                    return {};
                }

                auto final_suspend() const noexcept {
                    struct Awaiter {
                        std::optional<std::shared_ptr<TaskStack>> continuation;

                        bool await_ready() noexcept {
                            return /* we need to suspend only to continue on other
                                      executor*/
                                    !continuation.has_value()
                                    || schedule_on_other_ex(std::move(*continuation));
                        }

                        // todo: reuse
                        std::coroutine_handle<> await_suspend(
                                std::coroutine_handle<promise_type> co) noexcept {
                            ALONITE_SCOPE_EXIT {
                                co.destroy();
                            };
                            // todo:? maybe release
                            return (*continuation)->erased_top().co;
                        }

                        void await_resume() noexcept {
                        }
                    };

                    auto stack = this->stack.lock();
                    alonite_assert(stack, Invariant{});
                    stack->pop();
                    alonite_assert(stack->size() == 0, Invariant{});

                    auto continuation = this->continuation.lock();
                    return (continuation && current_executor->remove_guard(stack.get()))
                                 ? Awaiter{std::move(
                                           (continuation->guard = std::move(stack),
                                            continuation))}
                                 : Awaiter{};
                }
            };

            std::shared_ptr<TaskStack> stack;
            std::coroutine_handle<promise_type>
                    co; // todo:? is available as first frame in the stack
        };

        auto task_wrapper = [](auto task) -> Stack {
            co_return co_await task;
        }(std::move(task));
        this->stack = task_wrapper.stack;
        task_wrapper.co.promise().continuation = caller.promise().stack;
        current_executor->add_guard(std::shared_ptr{task_wrapper.stack});
        current_executor->spawn(
                [stack = std::weak_ptr{std::move(task_wrapper.stack)},
                 continuation = caller.promise().stack,
                 executor = current_executor]() mutable {
                    if (auto const x = stack.lock();
                        x && executor->remove_guard(x.get())) {
                        cancel_and_continue(std::move(stack), std::move(continuation));
                    }
                },
                this->dur);

        return task_wrapper.co;
    }

    typename TaskT::promise_type::ValueType await_resume() noexcept(false) {
        auto const stack = this->stack.lock();
        if (!stack) {
            throw TimedOut{};
        }
        return stack->template take_result<typename TaskT::promise_type::ValueType>();
    }

private:
    static void cancel_and_continue(std::weak_ptr<TaskStack>&& stack,
                                    std::weak_ptr<TaskStack>&& continuation) {
        if (stack.lock()) {
            current_executor->spawn([stack = std::move(stack),
                                     continuation = std::move(continuation)]() mutable {
                cancel_and_continue(std::move(stack), std::move(continuation));
            });
        } else if (auto x = continuation.lock();
                   x
                   && !schedule_on_other_ex(
                           /*not moved if not scheduled*/ std::move(x))) {
            x->erased_top().co.resume();
        }
    }

private:
    std::chrono::milliseconds dur;
    TaskT task;
    std::weak_ptr<TaskStack> stack;
};

template <class T>
struct WhenAllStack {
    struct promise_type
            : BasePromise
            , CreateStack<promise_type, WhenAllStack>
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType [[maybe_unused]] = T;

        std::weak_ptr<TaskStack> continuation;

        std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                std::optional<std::shared_ptr<TaskStack>> continuation;

                bool await_ready() noexcept {
                    if (!continuation.has_value()
                        || ++(*continuation)->tasks_completed
                                   < (*continuation)->tasks_to_complete) {
                        return true;
                    }
                    return schedule_on_other_ex(std::move(*continuation));
                }

                // todo: reuse
                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<> co) noexcept {
                    ALONITE_SCOPE_EXIT {
                        co.destroy();
                    };
                    return (*continuation)->erased_top().co;
                }

                void await_resume() noexcept {
                }
            };

            auto const stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            alonite_assert(stack->size() == 0, Invariant{});

            auto continuation = this->continuation.lock();
            return continuation ? Awaiter{std::move(continuation)} : Awaiter{};
        }
    };

    std::shared_ptr<TaskStack> stack;
    std::coroutine_handle<promise_type>
            co; // todo:? is available as first frame in the stack
};

template <class... TaskT /*Coroutine Concept*/>
class WhenAll {
public:
    static_assert(sizeof...(TaskT) > 0);

    explicit WhenAll(TaskT&&... tasks) noexcept
            : tasks{std::move(tasks)...} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) {
        auto const continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = sizeof...(TaskT);
            x->tasks_completed = 0;
        }

        // create stacks
        auto task_wrappers = std::apply(
                []<typename... X>(X&&... task) {
                    return std::tuple{
                            [](auto task)
                                    -> WhenAllStack<typename X::promise_type::ValueType> {
                                co_return co_await task;
                            }(std::move(task))...};
                },
                std::move(tasks));

        // set continuation
        this->stacks = std::apply(
                [continuation](auto&... stack_wrapper) {
                    return std::array{
                            (stack_wrapper.co.promise().continuation = continuation,
                             stack_wrapper.stack)...};
                },
                task_wrappers);

        return std::apply(
                [](auto const& first, auto&&... other) {
                    // schedule
                    [](...) {
                    }((schedule(std::move(other.stack)), 0)...);

                    // resume immediately
                    return first.co;
                },
                std::move(task_wrappers));
    }

    std::tuple<typename Void<typename TaskT::promise_type::ValueType>::WrapperT...>
    await_resume() noexcept(false) {
        return [this]<size_t... Is>(std::index_sequence<Is...>) {
            return std::tuple{
                    Void<typename std::tuple_element_t<Is, std::tuple<TaskT...>>::
                                 promise_type::ValueType>::take(*stacks[Is])...};
        }(std::make_index_sequence<sizeof...(TaskT)>());
    }

private:
    std::tuple<TaskT...> tasks;
    std::array<std::shared_ptr<TaskStack>, sizeof...(TaskT)> stacks;
};

template <class TaskT /*Coroutine Concept*/>
class WhenAllDyn {
public:
    explicit WhenAllDyn(std::vector<TaskT>&& tasks) noexcept
            : tasks{std::move(tasks)} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) noexcept(
            false) {
        auto const continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = this->tasks.size();
            x->tasks_completed = 0;
        }

        stacks.resize(tasks.size());
        std::transform(
                std::make_move_iterator(begin(tasks)),
                std::make_move_iterator(end(tasks)),
                begin(stacks),
                [continuation](TaskT&& task) {
                    auto ret = [](auto task)
                            -> WhenAllStack<typename TaskT::promise_type::ValueType> {
                        co_return co_await task;
                    }(std::move(task));
                    ret.co.promise().continuation = continuation;
                    return std::move(ret.stack);
                });

        for (unsigned i = 1; i < stacks.size(); ++i) {
            schedule(std::shared_ptr(stacks[i]));
        }

        return stacks[0]->erased_top().co;
    }

    std::vector<typename Void<typename TaskT::promise_type::ValueType>::WrapperT>
    await_resume() noexcept(false) {
        std::vector<typename Void<typename TaskT::promise_type::ValueType>::WrapperT> ret(
                stacks.size());
        std::transform(begin(stacks), end(stacks), begin(ret), [](auto const& stack) {
            return Void<typename TaskT::promise_type::ValueType>::take(*stack);
        });
        return ret;
    }

private:
    std::vector<TaskT> tasks;
    std::vector<std::shared_ptr<TaskStack>> stacks;
};

template <class T>
struct WhenAnyStack {
    struct promise_type
            : BasePromise
            , CreateStack<promise_type, WhenAnyStack>
            , std::conditional_t<std::is_void_v<T>,
                                 ReturnVoid<promise_type>,
                                 ReturnValue<promise_type>> {
        using ValueType [[maybe_unused]] = T;

        std::weak_ptr<TaskStack> continuation;
        unsigned index;
        unsigned id;

        std::suspend_always initial_suspend() const noexcept {
            return {};
        }

        auto final_suspend() noexcept {
            struct Awaiter {
                std::optional<std::shared_ptr<TaskStack>> continuation;
                unsigned index{};

                bool await_ready() noexcept {
                    if (continuation.has_value()) {
                        (*continuation)->completed_task_index = index;
                        return schedule_on_other_ex(std::move(*continuation));
                    }
                    return true;
                }

                // todo: reuse
                std::coroutine_handle<> await_suspend(
                        std::coroutine_handle<> co) noexcept {
                    ALONITE_SCOPE_EXIT {
                        co.destroy();
                    };
                    // todo:? maybe release
                    return (*continuation)->erased_top().co;
                }

                void await_resume() noexcept {
                }
            };

            auto stack = this->stack.lock();
            alonite_assert(stack, Invariant{});
            stack->pop();
            alonite_assert(stack->size() == 0, Invariant{});

            auto continuation = this->continuation.lock();

            return (continuation && current_executor->remove_guard_group(id))
                         ? Awaiter{std::move((continuation->guard = std::move(stack),
                                              continuation)),
                                   index}
                         : Awaiter{};
        }
    };

    std::shared_ptr<TaskStack> stack;
    std::coroutine_handle<promise_type>
            co; // todo:? is available as first frame in the stack
};

template <class... TaskT /*Coroutine Concept*/>
class WhenAny {
public:
    static_assert(sizeof...(TaskT) > 0);

    explicit WhenAny(TaskT&&... tasks) noexcept
            : tasks{std::move(tasks)...} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) {
        continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = x->completed_task_index = sizeof...(TaskT);
        }

        // create stacks
        auto task_wrappers = std::apply(
                []<typename... X>(X&&... task) {
                    return std::tuple{
                            [](auto task)
                                    -> WhenAnyStack<typename X::promise_type::ValueType> {
                                co_return co_await task;
                            }(std::move(task))...};
                },
                std::move(tasks));

        auto const id = current_executor->next_id();
        auto arr_vec = std::apply(
                [this, id](auto&... stack_wrapper) {
                    unsigned i = 0;
                    return std::pair{std::array{(stack_wrapper.co.promise().continuation =
                                                         continuation,
                                                 stack_wrapper.co.promise().index = i++,
                                                 stack_wrapper.co.promise().id = id,
                                                 std::weak_ptr{stack_wrapper.stack})...},
                                     std::vector{stack_wrapper.stack...}};
                },
                task_wrappers);

        this->stacks = std::move(arr_vec.first);
        alonite_assert(stacks[0].lock(), Invariant{});
        current_executor->add_guard_group(id, std::move(arr_vec.second));

        return std::apply(
                [](auto const& first, auto&&... other) {
                    [](...) {
                    }((schedule(std::move(other.stack)), 0)...);
                    return first.co;
                },
                std::move(task_wrappers));
    }

    using ReturnType = std::variant<
            typename Void<typename TaskT::promise_type::ValueType>::WrapperT...>;

    ReturnType await_resume() noexcept(false) {
        auto const continuation = this->continuation.lock();
        alonite_assert(continuation, Invariant{});

        std::optional<ReturnType> ret;
        [&]<auto... Is>(std::index_sequence<Is...>) {
            [](...) {
            }(continuation->completed_task_index == Is
              && (alonite_assert(stacks[Is].lock(), Invariant{}),
                  ret = ReturnType{std::in_place_index<Is>,
                                   Void<typename std::tuple_element_t<
                                           Is,
                                           std::tuple<TaskT...>>::promise_type::
                                                ValueType>::take(*stacks[Is].lock())},
                  true)...);
        }(std::make_index_sequence<sizeof...(TaskT)>());

        return std::move(*ret);
    }

private:
    std::tuple<TaskT...> tasks;
    std::weak_ptr<TaskStack> continuation;
    std::array<std::weak_ptr<TaskStack>, sizeof...(TaskT)> stacks;
};

template <class TaskT /*Coroutine Concept*/>
class WhenAnyDyn {
public:
    explicit WhenAnyDyn(std::vector<TaskT>&& tasks) noexcept
            : tasks{std::move(tasks)} {
    }

    bool await_ready() noexcept {
        return false;
    }

    template <class U>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<U> caller) {
        continuation = caller.promise().stack;

        if (auto const x = continuation.lock()) {
            x->tasks_to_complete = x->completed_task_index = this->tasks.size();
        }

        auto const id = current_executor->next_id();
        std::vector<std::shared_ptr<TaskStack>> stacks(tasks.size());
        std::transform(
                std::make_move_iterator(begin(tasks)),
                std::make_move_iterator(end(tasks)),
                begin(stacks),
                [this, i = size_t(0), id](TaskT&& task) mutable {
                    auto ret = [](auto task)
                            -> WhenAnyStack<typename TaskT::promise_type::ValueType> {
                        co_return co_await task;
                    }(std::move(task));
                    ret.co.promise().continuation = continuation;
                    ret.co.promise().index = i++;
                    ret.co.promise().id = id;
                    return std::move(ret.stack);
                });

        this->stacks.resize(stacks.size());
        std::transform(
                begin(stacks), end(stacks), begin(this->stacks), [](auto const& x) {
                    return std::weak_ptr{x};
                });

        current_executor->add_guard_group(id, stacks);

        for (unsigned i = 1; i < stacks.size(); ++i) {
            schedule(std::shared_ptr(stacks[i]));
        }

        return stacks[0]->erased_top().co;
    }

    using ReturnType = typename Void<typename TaskT::promise_type::ValueType>::WrapperT;

    ReturnType await_resume() noexcept(false) {
        auto const continuation = this->continuation.lock();
        alonite_assert(continuation, Invariant{});
        return Void<typename TaskT::promise_type::ValueType>::take(
                *stacks[continuation->completed_task_index].lock());
    }

private:
    std::vector<TaskT> tasks;
    std::weak_ptr<TaskStack> continuation;
    std::vector<std::weak_ptr<TaskStack>> stacks;
};

template <class T>
inline JoinHandle<T> spawn(Task<T>&& task) noexcept(false) {
    auto stack = std::make_shared<TaskStack>();
    auto t = [&](Task<T> task, std::weak_ptr<TaskStack>) -> JoinHandle<T> {
        co_return co_await task;
    }(std::move(task), stack);
    return t;
}

} // namespace alonite
