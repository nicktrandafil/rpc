#pragma once

#include "log.h"

#include <exception>
#include <utility>

namespace alonite {

template <class Fn>
struct ScopeExit : Fn {
    ~ScopeExit() {
        try {
            (*this)();
        } catch (...) {
            alonite_log_error("exception durring scope exit");
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
                alonite_log_error("exception during scope fail");
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

} // namespace alonite

#define ALONITE_CONCATENATE_IMPL(s1, s2) s1##s2

#define ALONITE_CONCATENATE(s1, s2) ALONITE_CONCATENATE_IMPL(s1, s2)

#define ALONITE_UNIQUE_IDENTIFIER ALONITE_CONCATENATE(UNIQUE_IDENTIFIER_, __LINE__)

#define ALONITE_SCOPE_EXIT auto const ALONITE_UNIQUE_IDENTIFIER = alonite::MakeScopeExit{}->*[&]

#define ALONITE_SCOPE_FAIL auto const ALONITE_UNIQUE_IDENTIFIER = alonite::MakeScopeFail{}->*[&]
