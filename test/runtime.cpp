#include <rpc/runtime.h>

#include <catch2/catch_all.hpp>

using namespace rpc;
using namespace std::chrono_literals;

TEST_CASE("Await for result", "[ThisThreadExecutor::spawn(task)]") {
    ThisThreadExecutor executor;
    executor.block_on([&]() -> Task<void> {
        auto x = co_await executor.spawn([]() -> Task<int> {
            co_return 1 + 1;
        }());
        REQUIRE(x == 2);
        co_return;
    }());
}

TEST_CASE("Discard the handle", "[ThisThreadExecutor::spawn(task)]") {
    ThisThreadExecutor executor;
    int effect = 0;
    executor.block_on([&]() -> Task<void> {
        executor.spawn([](int* effect) -> Task<void> {
            *effect = 1;
            co_return;
        }(&effect));
        co_return;
    }());
    REQUIRE(effect == 1);
}

TEST_CASE("Abort", "[ThisThreadExecutor::spawn(task)]") {
    ThisThreadExecutor executor;
    int effect = 0;
    executor.block_on([&]() -> Task<void> {
        auto h = executor.spawn([](int* effect) -> Task<void> {
            co_await Sleep{2ms};
            *effect = 1;
            co_return;
        }(&effect));
        co_await Sleep{1ms};
        h.abort();
        co_await h;
        co_return;
    }());
    REQUIRE(effect == 1);
}
