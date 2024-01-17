#pragma once

#include "contract.h"

#include <list>
#include <memory>

namespace rpc::mpsc {
namespace detail {

template <class T>
struct State {
    std::list<T> queue;
};

} // namespace detail

template <class T>
class Sender;

template <class T>
class Receiver {
public:
private:
    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> unbound_channel() noexcept(false);

    Receiver(std::shared_ptr<detail::State<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::shared_ptr<detail::State<T>> state;
};

template <class T>
class Sender {
public:
private:
    template <class U>
    friend std::pair<Sender<U>, Receiver<U>> unbound_channel() noexcept(false);

    Sender(std::shared_ptr<detail::State<T>> state) noexcept
            : state{std::move(state)} {
    }

    std::weak_ptr<detail::State<T>> state;
};

/// \throw std::bad_alloc
template <class T>
std::pair<Sender<T>, Receiver<T>> unbound_channel() noexcept(false) {
    auto state = std::make_shared<detail::State<T>>();
    return {Sender<T>{state}, Receiver<T>{std::move(state)}};
}

} // namespace rpc::mpsc
