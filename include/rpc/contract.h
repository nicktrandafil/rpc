#pragma once

#include <cstdlib>

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

}
