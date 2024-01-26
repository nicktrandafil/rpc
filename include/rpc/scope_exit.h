#pragma once

#include "log.h"

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
