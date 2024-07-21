#include <alonite/runtime.h>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

static unsigned global = 0;

static alonite::Task<void> when_all_10k() {
    std::vector<alonite::JoinHandle<void>> tasks;
    tasks.reserve(10'000);
    for (unsigned i = 0; i < 10'000; ++i) {
        tasks.push_back(alonite::spawn([](unsigned i) -> alonite::Task<void> {
            co_await alonite::Sleep{1ms};
            global += i;
            co_return;
        }(i)));
    }
    co_await alonite::WhenAllDyn{std::move(tasks)};
}

static boost::asio::awaitable<void> spawn_10k() {
    for (unsigned i = 0; i < 10'000; ++i) {
        boost::asio::co_spawn(
                co_await boost::asio::this_coro::executor,
                [i]() -> boost::asio::awaitable<void> {
                    co_await boost::asio::steady_timer{
                            co_await boost::asio::this_coro::executor, 1ms}
                            .async_wait(boost::asio::use_awaitable);
                    global += i;
                },
                boost::asio::detached);
    }
}

static alonite::Task<void> my_co_main3() {
    for (unsigned i = 0; i < 10'000; ++i) {
        alonite::spawn([](unsigned i) -> alonite::Task<void> {
            co_await alonite::Sleep{1ms};
            global += i;
            co_return;
        }(i));
    }
    co_return;
}

int main() {
    {
        std::cout << "my coro - when all 10k with\n";
        alonite::ThisThreadExecutor exec;

        auto const start = std::chrono::steady_clock::now();
        exec.block_on(when_all_10k());
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    global = 0;

    {
        std::cout << "boost coro - spawn 10k\n";
        boost::asio::io_context io;
        boost::asio::co_spawn(io, spawn_10k(), boost::asio::detached);

        auto const start = std::chrono::steady_clock::now();
        io.run();
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }

    std::cout << "------------------\n";

    global = 0;

    {
        std::cout << "my coro - spawn 10k\n";
        alonite::ThisThreadExecutor exec;

        auto const start = std::chrono::steady_clock::now();
        exec.block_on(my_co_main3());
        auto const end = std::chrono::steady_clock::now();

        std::cout << "result: " << global << "\n";
        std::cout << "elapsed: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                  << "\n";
    }
}
