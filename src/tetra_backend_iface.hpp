// tetra_backend_iface.hpp
//
// Runtime-selectable TETRA backend. The server is one binary that decides
// per session which signal chain to run from the client's "protocol" hint
// (see session.cpp): the FM-discriminator + DSD path for the analog-FM
// digital modes, or the π/4-DQPSK modem + a TETRA subprocess backend for
// TETRA. Because a TETRA backend is chosen at run time, not build time, the
// two concrete backends (osmo TetraProcess, tetra-kit TetraKitProcess) are
// reached through this small polymorphic interface instead of the old
// compile-time type alias (tetra_backend_selector.hpp, now removed).
//
// Both concrete backends already expose the identical surface --
// start(cfg, on_event, on_audio) / write_bits / stop / running, emitting the
// shared DsdEvent -- differing only in their config struct. ITetraBackend
// erases that config difference: each adapter owns a default-constructed
// config of its backend's type and forwards start()'s callbacks into the
// concrete start(). Neither TetraProcess nor TetraKitProcess is modified, so
// their existing tests keep exercising them directly.

#pragma once

#include "dsd_backend_types.hpp"
#include "tetra_process.hpp"
#include "tetra_kit_process.hpp"

#include <functional>
#include <memory>
#include <cstddef>
#include <cstdint>

namespace dsdsrv {

// The TETRA backends, chosen at run time from the client's protocol hint:
//   Osmo     -> sq5bpf osmo-tetra tetra-rx  (protocol "tetra")
//   Tetrakit -> tetra-kit decoder            (protocol "tetrakit")
enum class TetraBackendKind { Osmo, Tetrakit };

// Backend-agnostic handle the session holds. The callback signatures match
// both concrete backends' start() exactly.
class ITetraBackend {
public:
    using EventCallback = std::function<void(const DsdEvent&)>;
    using AudioCallback = std::function<void(const int16_t* pcm, std::size_t n)>;

    virtual ~ITetraBackend() = default;

    virtual bool start(EventCallback on_event, AudioCallback on_audio) = 0;
    virtual bool write_bits(const unsigned char* bits, std::size_t n) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;
};

// Adapter wrapping a concrete backend (TetraProcess / TetraKitProcess). It
// owns a default-constructed config of that backend's type -- the session set
// no per-field options before, and doesn't now -- and hands it to the
// concrete start() along with the callbacks.
template <class Proc, class Cfg>
class TetraBackendAdapter final : public ITetraBackend {
public:
    bool start(EventCallback on_event, AudioCallback on_audio) override {
        return proc_.start(cfg_, std::move(on_event), std::move(on_audio));
    }
    bool write_bits(const unsigned char* bits, std::size_t n) override {
        return proc_.write_bits(bits, n);
    }
    void stop() override { proc_.stop(); }
    bool running() const override { return proc_.running(); }

private:
    Proc proc_;
    Cfg cfg_;
};

inline std::unique_ptr<ITetraBackend> make_tetra_backend(TetraBackendKind kind) {
    switch (kind) {
        case TetraBackendKind::Tetrakit:
            return std::make_unique<
                TetraBackendAdapter<TetraKitProcess, TetraKitProcessConfig>>();
        case TetraBackendKind::Osmo:
        default:
            return std::make_unique<
                TetraBackendAdapter<TetraProcess, TetraProcessConfig>>();
    }
}

} // namespace dsdsrv
