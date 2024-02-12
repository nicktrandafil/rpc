#include "rpc/mpsc.h"

#include <rpc/scope_exit.h>

#include <catch2/catch_all.hpp>

using namespace rpc;
using std::chrono_literals::operator""ms;

TEST_CASE("construct and send one value", "[mpsc]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    auto const executor = ThisThreadExecutor::construct();
    int counter = 0;
    executor->block_on([&]() -> Task<void> {
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
    auto const executor = ThisThreadExecutor::construct();
    int counter = 0;
    executor->block_on([&]() -> Task<void> {
        executor->spawn([tx = std::move(tx)]() -> Task<void> {
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

TEST_CASE("construct and send many values, conditional variable is involved", "[mpsc]") {
    auto [tx, rx] = mpsc::unbound_channel<int>();
    auto const executor = ThisThreadExecutor::construct();
    int counter = 0;
    auto const start = std::chrono::steady_clock::now();
    executor->block_on([&]() -> Task<void> {
        auto h = executor->spawn([](auto tx) -> Task<void> {
            for (int i = 0; i < 10; ++i) {
                co_await Sleep{5ms};
                tx.send(i);
            }
        }(std::move(tx)));

        for (int i = 0; i < 10; ++i) {
            auto const x = co_await rx.recv();
            REQUIRE(x == i);
            ++counter;
        }

        co_return;
    }());
    REQUIRE(counter == 10);
    auto const dur = std::chrono::steady_clock::now() - start;
    REQUIRE(50ms <= dur);
    REQUIRE(dur <= 65ms);
}
