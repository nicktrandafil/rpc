#include "rpc/mpsc.h"

#include <catch2/catch_all.hpp>

using namespace rpc;

TEST_CASE("construct and send one value", "[mpsc]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        tx.send(5);
        auto const x = co_await rx.recv();
        REQUIRE(x == 5);
        ++counter;
        co_return;
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("construct and send many values, conditional variable isn't really involved",
          "[mpsc]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        executor.spawn([tx = std::move(tx)]() -> Task<void> {
            for (int i = 0; i < 10; ++i) {
                tx.send(i);
            }
            co_return;
        }());

        for (int i = 0; i < 10; ++i) {
            auto const x = co_await rx.recv();
            REQUIRE(x == i);
            ++counter;
        }

        co_return;
    }());
    REQUIRE(counter == 10);
}
