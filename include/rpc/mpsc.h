#pragma once

#include "contract.h"
#include "runtime.h"

#include <list>
#include <memory>
#include <mutex>
#include <optional>

namespace rpc::mpsc {
namespace detail {

template <class T>
class UnboundState {
public:
    /// \throw std::bad_alloc
    void push(T value) noexcept(false) {
        std::scoped_lock lock{mutex};
        queue.push_back(std::move(value));
    }

    Task<std::optional<T>> pop() noexcept {
        std::scoped_lock lock{mutex};
        if (!queue.empty()) {
            auto ret = std::move(queue.front());
            queue.pop_front();
            co_return ret;
        }

        if (!has_producer) {
            co_return std::nullopt;
        }

        co_return todo();
    }

private:
    std::list<T> queue;
    std::mutex mutex;
    bool has_producer{true};
    std::optional<ConditionalVariable> cv;
};

} // namespace detail

template <class T>
class UnboundSender;

template <class T>
class UnboundReceiver {
public:
    UnboundReceiver(UnboundReceiver const&) = delete;
    UnboundReceiver operator=(UnboundReceiver const&) = delete;

    UnboundReceiver(UnboundReceiver&&) = default;
    UnboundReceiver operator=(UnboundReceiver&&) = delete;

    Task<std::optional<T>> recv() noexcept {
        co_return state->pop();
    }

private:
    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(
            false);

    UnboundReceiver(std::shared_ptr<detail::UnboundState<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::shared_ptr<detail::UnboundState<T>> state;
};

struct ClosedError : std::exception {
    const char* what() const noexcept override {
        return "closed";
    }
};

template <class T>
class UnboundSender {
public:
    UnboundSender(UnboundSender const&) = default;
    UnboundSender& operator=(UnboundSender const&) = default;

    UnboundSender(UnboundSender&&) = default;
    UnboundSender& operator=(UnboundSender&&) = default;

    /// \throw std::bad_alloc, ClosedError
    void send(T value) noexcept(false) {
        if (auto state = this->state.lock()) {
            state->push(std::move(value));
        } else {
            throw ClosedError{};
        }
    }

private:
    template <class U>
    friend std::pair<UnboundSender<U>, UnboundReceiver<U>> unbound_channel() noexcept(
            false);

    UnboundSender(std::shared_ptr<detail::UnboundState<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::weak_ptr<detail::UnboundState<T>> state;
};

/// \throw std::bad_alloc
template <class T>
std::pair<UnboundSender<T>, UnboundReceiver<T>> unbound_channel() noexcept(false) {
    auto state = std::make_shared<detail::UnboundState<T>>();
    return {UnboundSender<T>{state}, UnboundReceiver<T>{std::move(state)}};
}

} // namespace rpc::mpsc
