#pragma once

#include "log.h"

#include <exception>
#include <utility>

namespace rpc {

template <class Fn>
struct ScopeExit : Fn {
    ~ScopeExit() {
        try {
            (*this)();
        } catch (...) {
            RPC_LOG_ERROR("exception durring scope exit");
        }
    }
};

struct MakeScopeExit {
    template <class Fn>
    auto operator->*(Fn&& fn) const {
        return ScopeExit<Fn>{std::forward<Fn>(fn)};
    }
};

template <class Fn>
struct ScopeFail : Fn {
    ~ScopeFail() {
        if (std::current_exception()) {
            try {
                (*this)();
            } catch (...) {
                RPC_LOG_ERROR("exception during scope fail");
            }
        }
    }
};

struct MakeScopeFail {
    template <class Fn>
    auto operator->*(Fn&& fn) const {
        return ScopeFail<Fn>{std::forward<Fn>(fn)};
    }
};

} // namespace rpc

#define RPC_CONCATENATE_IMPL(s1, s2) s1##s2

#define RPC_CONCATENATE(s1, s2) RPC_CONCATENATE_IMPL(s1, s2)

#define RPC_UNIQUE_IDENTIFIER RPC_CONCATENATE(UNIQUE_IDENTIFIER_, __LINE__)

#define RPC_SCOPE_EXIT auto const RPC_UNIQUE_IDENTIFIER = rpc::MakeScopeExit{}->*[&]

#define RPC_SCOPE_FAIL auto const RPC_UNIQUE_IDENTIFIER = rpc::MakeScopeFail{}->*[&]
