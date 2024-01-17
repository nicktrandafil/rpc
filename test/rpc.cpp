#include "rpc/rpc.h"

#include <catch2/catch_all.hpp>

using namespace rpc;

TEST_CASE("") {
    Rpc rpc{std::make_unique<ChannelTransport>()};
    rpc.declare_method<void(int)>("hello");
    rpc.call_method("hello", 5);
}
