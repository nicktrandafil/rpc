#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <iostream>

class Arguments {};

class Prototype {};

class Transport {};

class Rpc {
public:
    void set_transport(Transport&&) {
    }

    void declare_method(std::string&& name, Prototype&&) {
    }

    void call_method(std::string&& name, Arguments&&) {
    }
};

TEST_CASE("") {
    Rpc rpc;
    rpc.set_transport(Transport{});
    rpc.declare_method("hello", Prototype{});
    rpc.call_method("hello", Arguments{});
}
