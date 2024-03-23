#include <rpc/runtime.h>

#include <catch2/catch_all.hpp>

using namespace rpc;
using namespace std::chrono_literals;

TEST_CASE("value", "[ThisThreadExecutor::block_on(task)]") {
    ThisThreadExecutor executor;
    auto const x = executor.block_on([&]() -> Task<int> {
        co_return 1 + 1;
    }());
    REQUIRE(x == 2);
}

TEST_CASE("exception", "[ThisThreadExecutor::block_on(task)]") {
    ThisThreadExecutor executor;
    auto t = true;
    REQUIRE_THROWS_AS((executor.block_on([&]() -> Task<int> {
                          if (t) {
                              throw 1;
                          }
                          co_return 1 + 1;
                      }())),
                      int);
}

TEST_CASE("void", "[ThisThreadExecutor::block_on(task)]") {
    ThisThreadExecutor executor;
    bool executed = false;
    executor.block_on([&]() -> Task<void> {
        RPC_SCOPE_EXIT {
            executed = true;
        };
        co_return;
    }());
    REQUIRE(executed);
}

TEST_CASE("Common destruction order", "[ThisThreadExecutor::block_on(task)]") {
    ThisThreadExecutor executor;

    int acc = 0;

    struct Add {
        [[maybe_unused]] ~Add() noexcept {
            acc *= 2;
            acc += x;
        }

        int& acc;
        int x;
    };

    executor.block_on([&](std::shared_ptr<Add>) -> Task<void> {
        co_await [&](std::shared_ptr<Add>) -> Task<void> {
            co_return;
        }(std::shared_ptr<Add>(new Add{acc, 2}));
    }(std::shared_ptr<Add>(new Add{acc, 1})));

    REQUIRE(acc == 5);
}

// TEST_CASE("Await for result", "[ThisThreadExecutor::spawn(task)]") {
//     ThisThreadExecutor executor;
//     executor.block_on([&]() -> Task<void> {
//         auto x = co_await executor.spawn([]() -> Task<int> {
//             co_return 1 + 1;
//         }());
//         REQUIRE(x == 2);
//         co_return;
//     }());
// }

// TEST_CASE("Discard the handle", "[ThisThreadExecutor::spawn(task)]") {
//     ThisThreadExecutor executor;
//     int effect = 0;
//     executor.block_on([&]() -> Task<void> {
//         executor.spawn([](int* effect) -> Task<void> {
//             *effect = 1;
//             co_return;
//         }(&effect));
//         co_return;
//     }());
//     REQUIRE(effect == 1);
// }

// TEST_CASE("Abort", "[ThisThreadExecutor::spawn(task)]") {
//     ThisThreadExecutor executor;
//     int effect = 0;
//     executor.block_on([&]() -> Task<void> {
//         auto h = executor.spawn([](int* effect) -> Task<void> {
//             co_await Sleep{2ms};
//             *effect = 1;
//             co_return;
//         }(&effect));
//         co_await Sleep{1ms};
//         h.abort();
//         co_await h;
//         co_return;
//     }());
//     REQUIRE(effect == 1);
// }
