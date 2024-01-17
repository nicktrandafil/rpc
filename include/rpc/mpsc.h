#pragma once

#include "contract.h"

#include <list>
#include <memory>

namespace rpc::mpsc {

template <class T>
class Sender {};

template <class T>
class Receiver {
public:
private:
    struct State {
        std::list<T> queue;
    };

    std::shared_ptr<State> state;
};

template <class T>
std::pair<Sender<T>, Receiver<T>> unbound_channel() {
    todo();
}

} // namespace rpc::mpsc
