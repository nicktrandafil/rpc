#pragma once

#include "contract.h"

#include <format>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace rpc {

struct ProtocolError : std::runtime_error {
    using runtime_error::runtime_error;
};

class Argument {
public:
private:
};

class ReturnValue {};

template <class T>
struct Type {};

class Prototype {
public:
    template <class T>
    Prototype(Type<T>)
            : c{new Model<T>{}} {
    }

    /// \throw std::logic_error
    template <class T>
    void validate_argument() const noexcept(false) {
        c->validate_argument(typeid(T));
    }

    /// \throw ProtocolError
    void validate(ReturnValue const&) const noexcept(false) {
        rpc_todo();
    }

private:
    struct Concept {
        virtual ~Concept() = default;
        /// \throw std::logic_error
        virtual void validate_argument(std::type_info const&) noexcept(false) = 0;
    };

    template <class T>
    struct Model;

    template <class R, class T>
    struct Model<R(T)> : Concept {
        void validate_argument(std::type_info const& actual) noexcept(false) override {
            if (auto const& expected = typeid(T); expected != actual) {
                throw std::logic_error(std::format(
                        "expected {}, got {}", expected.name(), actual.name()));
            }
        }
    };

    std::unique_ptr<Concept> c;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual void write(std::vector<std::byte> const&) const noexcept = 0;

    virtual void read(std::vector<std::byte>&) const noexcept = 0;

private:
};

class ChannelTransport : public Transport {
public:
    void write(std::vector<std::byte> const&) const noexcept override {
        rpc_todo();
    }

    void read(std::vector<std::byte>&) const noexcept override {
        rpc_todo();
    }
};

class Call {};

class Rpc {
public:
    explicit Rpc(std::unique_ptr<Transport>&& t)
            : transport{std::move(t)} {
    }

    /// \throw std::logic_error if method already defined
    template <class T>
    void declare_method(std::string&& name) noexcept(false) {
        auto proto = Prototype(Type<T>{});
        if (!remote_methods.emplace(std::move(name), std::move(proto)).second) {
            throw std::logic_error("method already defined");
        }
    }

    template <class T>
    ReturnValue call_method(std::string_view name, T&&) {
        auto const it = remote_methods.find(name);
        if (it == remote_methods.end()) {
            throw std::logic_error(std::format("method {} not defined", name));
        }

        it->second.validate_argument<std::remove_reference_t<T>>();

        return rpc_todo();
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
    std::unordered_map<std::string, Prototype, TransparentHash, std::equal_to<>>
            remote_methods;
    std::vector<Call> calls;
};

} // namespace rpc
