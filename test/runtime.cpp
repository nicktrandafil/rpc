#include <rpc/runtime.h>

#include <catch2/catch_all.hpp>

using namespace rpc;
using namespace std::chrono_literals;

TEST_CASE("value result", "[ThisThreadExecutor::block_on(Task<T>)]") {
    ThisThreadExecutor executor;
    auto const x = executor.block_on([&]() -> Task<int> {
        co_return 1 + 1;
    }());
    REQUIRE(x == 2);
}

TEST_CASE("exception result", "[ThisThreadExecutor::block_on(Task<T>)]") {
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

TEST_CASE("void result", "[ThisThreadExecutor::block_on(Task<T>)]") {
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

TEST_CASE("destruction order should be natural",
          "[ThisThreadExecutor::block_on(Task<T>)]") {
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

TEST_CASE("await for result", "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    executor.block_on([&]() -> Task<void> {
        auto x = co_await executor.spawn([]() -> Task<int> {
            co_return 1 + 1;
        }());
        REQUIRE(x == 2);
        co_return;
    }());
}

TEST_CASE("ignore result", "[Sleep]") {
    ThisThreadExecutor executor;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        co_await Sleep{5ms};
        co_return;
    }());
    auto const end = std::chrono::steady_clock::now();
    REQUIRE(5ms <= end - start);
    REQUIRE(end - start < 10ms);
}

TEST_CASE("ignore result", "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    executor.block_on([&]() -> Task<void> {
        executor.spawn([&]() -> Task<int> {
            run = true;
            co_return 1 + 1;
        }());
        co_return;
    }());
    REQUIRE(run);
}

TEST_CASE("use some sleep to actually enter the block_on loop",
          "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    executor.block_on([&]() -> Task<void> {
        executor.spawn([](bool& run) -> Task<int> {
            co_await Sleep{5ms};
            run = true;
            co_return 1 + 1;
        }(run));
        co_return;
    }());
    REQUIRE(run);
}

TEST_CASE("abort", "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    bool exception = false;
    executor.block_on([&]() -> Task<void> {
        auto handle = executor.spawn([](bool& run) -> Task<void> {
            co_await Sleep{5ms};
            run = true;
            co_return;
        }(run));

        co_await Sleep{1ms};
        REQUIRE(handle.abort());

        try {
            co_await handle;
        } catch (Canceled const&) {
            exception = true;
        }

        co_return;
    }());
    REQUIRE(!run);
    REQUIRE(exception);
}

TEST_CASE("abort already ready task does nothing",
          "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    bool exception = false;
    executor.block_on([&]() -> Task<void> {
        auto handle = executor.spawn([](bool& run) -> Task<void> {
            run = true;
            co_return;
        }(run));

        co_await Sleep{1ms};
        REQUIRE(!handle.abort());

        try {
            co_await handle;
        } catch (Canceled const&) {
            exception = true;
        }

        co_return;
    }());
    REQUIRE(run);
    REQUIRE(!exception);
}

TEST_CASE("the task was already completed by the abort time",
          "[ThisThreadExecutor::spawn(Task<T>)]") {
    ThisThreadExecutor executor;
    bool run = false;
    bool exception = false;
    executor.block_on([&]() -> Task<void> {
        // todo: avoid this when multi-threaded executor is supported
        JoinHandle<void>* hack = nullptr;
        JoinHandle<void> handle =
                executor.spawn([](bool& run, auto const& hack) -> Task<void> {
                    run = true;

                    co_await Sleep{2ms};
                    rpc_assert(hack, Invariant{});

                    // at this point, we are practically complete
                    REQUIRE(hack->abort());

                    co_return;
                }(run, hack));
        hack = &handle;

        co_await Sleep{1ms};

        try {
            co_await handle;
        } catch (Canceled const& x) {
            REQUIRE(x.was_already_completed());
            exception = true;
        }

        co_return;
    }());
    REQUIRE(run);
    REQUIRE(exception);
}

TEST_CASE("", "[ConditionalVariable]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        ConditionalVariable cv;

        executor.spawn([](int* counter, ConditionalVariable* cv) -> Task<void> {
            auto const start = std::chrono::steady_clock::now();

            co_await cv->wait();

            auto const elapsed = std::chrono::steady_clock::now() - start;

            *counter += 2;

            REQUIRE(5ms < elapsed);
#ifdef NDEBUG
            REQUIRE(elapsed < 6ms);
#endif

            co_return;
        }(&counter, &cv));

        co_await Sleep{5ms};
        cv.notify();
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("the coroutine is on time", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const x = co_await Timeout{2ms, []() -> Task<int> {
                                            co_await Sleep{1ms};
                                            co_return 1;
                                        }()};
        ++counter;
        REQUIRE(x == 1);
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("the coroutine is late", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        try {
            co_await Timeout{1ms, []() -> Task<int> {
                                 co_await Sleep{2ms};
                                 co_return 1;
                             }()};
            counter = 1;
        } catch (TimedOut const&) {
            counter = 2;
        }
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("void task, the coroutine is on time", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        co_await Timeout{2ms, []() -> Task<void> {
                             co_await Sleep{1ms};
                             co_return;
                         }()};
        ++counter;
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("void task, the coroutine is late", "[Timeout]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        try {
            co_await Timeout{1ms, []() -> Task<void> {
                                 co_await Sleep{2ms};
                                 co_return;
                             }()};
            counter = 1;
        } catch (TimedOut const&) {
            counter = 2;
        }
    }());
    REQUIRE(counter == 2);
}

TEST_CASE("one void task", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<void> {
            x += 1;
            co_return;
        }(counter)};
        counter += 2;
        REQUIRE(tmp == std::tuple{Void<>{}});
    }());
    REQUIRE(counter == 3);
}

TEST_CASE("one int task", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<int> {
            x += 1;
            co_return 2;
        }(counter)};
        counter += 2;
        REQUIRE(tmp == std::tuple{2});
    }());
    REQUIRE(counter == 3);
}

TEST_CASE("one int task and one void", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter1 = 0;
    int counter2 = 0;
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<int> {
                                              x += 1;
                                              co_return 2;
                                          }(counter1),
                                          [](auto& x) -> Task<void> {
                                              x += 1;
                                              co_return;
                                          }(counter2)};
        counter1 += 2;
        REQUIRE(tmp == std::tuple{2, Void<>{}});
    }());
    REQUIRE(counter1 == 3);
    REQUIRE(counter2 == 1);
}

TEST_CASE("one int task and one void with sleeps", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter1 = 0;
    int counter2 = 0;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        auto const tmp = co_await WhenAll{[](auto& x) -> Task<int> {
                                              co_await Sleep{1ms};
                                              x += 1;
                                              co_return 2;
                                          }(counter1),
                                          [](auto& x) -> Task<void> {
                                              co_await Sleep{2ms};
                                              x += 1;
                                              co_return;
                                          }(counter2)};
        counter1 += 2;
        REQUIRE(tmp == std::tuple{2, Void<>{}});
    }());
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(counter1 == 3);
    REQUIRE(counter2 == 1);
    REQUIRE(2ms < elapsed);
    REQUIRE(elapsed < 3ms);
}

TEST_CASE("check tasks execute simultaneously", "[WhenAll]") {
    ThisThreadExecutor executor;
    int counter = 2;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        co_await WhenAll{[](auto& x) -> Task<void> {
                             co_await Sleep{2ms};
                             x *= 2;
                             co_await Sleep{2ms};
                             x *= 4;
                         }(counter),
                         [](auto& x) -> Task<void> {
                             co_await Sleep{2ms};
                             x *= 3;
                             co_await Sleep{2ms};
                             x *= 5;
                         }(counter)};
    }());
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(counter == 2 * (2 * 3) * (4 * 5));
    REQUIRE(4ms < elapsed);
#ifdef NDEBUG
    REQUIRE(elapsed < 5ms);
#endif
}

TEST_CASE("two voids with sleeps", "[WhenAllDyn]") {
    ThisThreadExecutor executor;
    int counter1 = 0;
    int counter2 = 0;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<void>> tasks;

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{1ms};
            x += 1;
            co_return;
        }(counter1));

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{2ms};
            x += 1;
            co_return;
        }(counter2));

        auto const tmp = co_await WhenAllDyn{std::move(tasks)};

        counter1 += 2;
        REQUIRE(tmp == std::vector{Void<>{}, Void<>{}});
    }());
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(counter1 == 3);
    REQUIRE(counter2 == 1);
    REQUIRE(2ms < elapsed);
    REQUIRE(elapsed < 3ms);
}

TEST_CASE("an int", "[WhenAllDyn]") {
    ThisThreadExecutor executor;
    int counter = 0;
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<int>> tasks;

        tasks.push_back([]() -> Task<int> {
            co_return 2;
        }());

        auto const tmp = co_await WhenAllDyn{std::move(tasks)};

        counter += 1;
        REQUIRE(tmp == std::vector{2});
    }());
    REQUIRE(counter == 1);
}

TEST_CASE("check tasks execute simultaneously", "[WhenAllDyn]") {
    ThisThreadExecutor executor;
    int counter = 2;
    auto const start = std::chrono::steady_clock::now();
    executor.block_on([&]() -> Task<void> {
        std::vector<Task<void>> tasks;

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{2ms};
            x *= 2;
            co_await Sleep{2ms};
            x *= 4;
        }(counter));

        tasks.push_back([](auto& x) -> Task<void> {
            co_await Sleep{2ms};
            x *= 3;
            co_await Sleep{2ms};
            x *= 5;
        }(counter));

        co_await WhenAllDyn{std::move(tasks)};
    }());
    auto const elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(counter == 2 * (2 * 3) * (4 * 5));
    REQUIRE(4ms < elapsed);
#ifdef NDEBUG
    REQUIRE(elapsed < 5ms);
#endif
}
