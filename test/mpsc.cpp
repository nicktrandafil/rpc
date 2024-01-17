#include "rpc/mpsc.h"

#include <catch2/catch_all.hpp>

using namespace rpc;

TEST_CASE("channel") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
}
