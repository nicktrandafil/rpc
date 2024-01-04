#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <iostream>
#include <memory>
#include <unordered_map>

class Arguments {};

class Prototype {};

class Transport {
public:
    virtual ~Transport() = default;

    virtual void write(std::vector<std::byte> const&) const noexcept = 0;

    virtual void read(std::vector<std::byte>&) const noexcept = 0;

private:
};

class Rpc {
public:
    void set_transport(std::unique_ptr<Transport>&& transport) {
    }

    void declare_method(std::string&& method_name, Prototype&& proto) {
        if (!remote_methods.emplace(std::move(method_name), std::move(proto)).second) {
            throw std::logic_error("method already defined");
        }
    }

    void call_method(std::string_view method_name, Arguments&&) {
        auto const it = remote_methods.find(method_name);
    }

private:
    struct TransparentHash {
        using is_transparent = void;
        template <class T>
        size_t operator()(T const& x) const noexcept {
            return std::hash<T>{}(x);
        }
    };

    std::unique_ptr<Transport> transport;
    std::unordered_map<std::string, Prototype, TransparentHash, std::less<>>
            remote_methods;
};

TEST_CASE("") {
    Rpc rpc;
    rpc.set_transport(std::make_unique<Transport>());
    rpc.declare_method("hello", Prototype{});
    rpc.call_method("hello", Arguments{});
}
