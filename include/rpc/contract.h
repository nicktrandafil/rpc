#pragma once

#include <cstdlib>
#include <format>
#include <source_location>
#include <stdexcept>

namespace rpc {

struct Todo {
    [[noreturn]] Todo(std::source_location const& l) {
#ifdef RPC_ABORT_ON_TODO
        static_cast<void>(l);
        std::abort();
#else
        throw std::runtime_error(std::format("todo at {}:{}:{} ({})",
                                             l.file_name(),
                                             l.line(),
                                             l.column(),
                                             l.function_name()));
#endif
    }

    template <class T>
    [[noreturn]] operator T&() const noexcept {
        std::abort();
    }
};

#define rpc_todo()                                                                       \
    rpc::Todo {                                                                          \
        std::source_location::current()                                                  \
    }

struct Invariant {
    [[noreturn]] void failed(std::source_location const& l) {
#ifdef RPC_ABORT_ON_INVARIANT_VIOLATION
        static_cast<void>(l);
        std::abort();
#else
        throw std::runtime_error(std::format("invariant failed at {}:{}:{} ({})",
                                             l.file_name(),
                                             l.line(),
                                             l.column(),
                                             l.function_name()));
#endif
    }
};

struct UnlikelyAbort {
    [[noreturn]] void failed(std::source_location const&) {
        std::abort();
    }
};

#define rpc_assert(expr, module)                                                         \
    if (!(expr)) {                                                                       \
        module.failed(std::source_location::current());                                  \
    }                                                                                    \
    static_assert(true)

} // namespace rpc
