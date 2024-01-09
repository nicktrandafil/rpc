#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <coroutine>
#include <doctest/doctest.h>
#include <format>
#include <functional>
#include <iostream>
#include <memory>
#include <unordered_map>

namespace rpc {

struct Todo {
    [[noreturn]] Todo() {
        std::abort();
    }

    template <class T>
    [[noreturn]] operator T&() const noexcept {
        std::abort();
    }
};

[[noreturn]] inline Todo todo() noexcept {
    std::abort();
}

struct Invariant {
    [[noreturn]] void failed() {
        std::abort();
    }
};

#define RPC_ASSERT(expr, module)                                                         \
    if (!(expr)) {                                                                       \
        module.failed();                                                                 \
    }                                                                                    \
    static_assert(true)

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

struct ProtocolError : std::runtime_error {
    using runtime_error::runtime_error;
};

class Argument {
public:
private:
};

class ReturnValue {};

template <class T>
struct Type {};

class Prototype {
public:
    template <class T>
    Prototype(Type<T>)
            : c{new Model<T>{}} {
    }

    /// \throw std::logic_error
    template <class T>
    void validate_argument() const noexcept(false) {
        todo();
    }

    /// \throw ProtocolError
    void validate(ReturnValue const&) const noexcept(false) {
        todo();
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual void validate_arugment() noexcept(false) = 0;
    };

    template <class T>
    struct Model;

    template <class R, class T>
    struct Model<R(T)> : Concept {
        void validate_arugment() noexcept(false) override {
        }
    };

    std::unique_ptr<Concept> c;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual void write(std::vector<std::byte> const&) const noexcept = 0;

    virtual void read(std::vector<std::byte>&) const noexcept = 0;

private:
};

class ChannelTransport : public Transport {
public:
    void write(std::vector<std::byte> const&) const noexcept override {
        todo();
    }

    void read(std::vector<std::byte>&) const noexcept override {
        todo();
    }
};

class Call {};

class Rpc {
public:
    explicit Rpc(std::unique_ptr<Transport>&& t)
            : transport{std::move(t)} {
    }

    /// \throw std::logic_error if method already defined
    template <class T>
    void declare_method(std::string&& name) noexcept(false) {
        auto proto = Prototype(Type<T>{});
        if (!remote_methods.emplace(std::move(name), std::move(proto)).second) {
            throw std::logic_error("method already defined");
        }
    }

    template <class T>
    ReturnValue call_method(std::string_view name, T&&) {
        auto const it = remote_methods.find(name);
        if (it == remote_methods.end()) {
            throw std::logic_error(std::format("method {} not defined", name));
        }

        it->second.validate_argument<std::remove_reference_t<T>>();

        todo();
    }

private:
    struct TransparentHash {
        using is_transparent = void;
        template <class T>
        size_t operator()(T const& x) const noexcept {
            return std::hash<T>{}(x);
        }
    };

    std::unique_ptr<Transport> transport;
    std::unordered_map<std::string, Prototype, TransparentHash, std::equal_to<>>
            remote_methods;
    std::vector<Call> calls;
};

TEST_CASE("") {
    Rpc rpc{std::make_unique<ChannelTransport>()};
    rpc.declare_method<void(int)>("hello");
    rpc.call_method("hello", 5);
}

} // namespace rpc
