#include "host.hpp"

#include "envelope.hpp"

namespace villen {

Host::Host(net::WsServer& ws, std::vector<std::unique_ptr<IEngineFactory>> engines)
    : ws_(ws), engines_(std::move(engines)) {
    // Boot at the launcher (none active): inbound messages only get an
    // engine-announce until the operator starts something.
    installCallbacks();
}

Host::~Host() {
    // Drop callbacks before the Host (and the Room/engine they capture) dies, so a
    // late poll can't dispatch into freed state.
    ws_.setCallbacks({});
}

const char* Host::activeName() const {
    return active_ ? engines_[activeIndex_]->name() : "";
}

void Host::startEngine(std::size_t index) {
    if (index >= engines_.size()) return;
    stopEngine();  // guarantee a clean slate before constructing the next

    active_ = engines_[index]->create();          // ctor acquires the engine's resources
    if (!active_) return;                          // creation failed: stay at the launcher
    room_ = std::make_unique<Room>(ws_, *active_, active_->seats());
    active_->attach(*room_);                       // admin actions can now reach transport
    activeIndex_ = index;

    installCallbacks();
    announceAll();  // existing clients learn the new engine and load its view
}

void Host::stopEngine() {
    if (!active_) return;
    activeIndex_ = static_cast<std::size_t>(-1);
    active_->detach();  // null the engine's Room* before the Room dies, so the
                        // engine destructor can't use-after-free it
    room_.reset();      // membership first (no more dispatch targets)
    active_.reset();    // dtor releases the engine's resources (admin-shell §4)

    installCallbacks();
    announceAll();  // tell clients we're back at the launcher (engine: null)
}

void Host::tick(std::uint64_t nowMs) {
    if (active_ && room_) active_->onTick(*room_, nowMs);
}

void Host::announce(ConnId id) { ws_.send(id, envelope::engineAnnounce(activeName())); }

void Host::announceAll() {
    // No broadcast-with-id helper; the announce is identical for everyone, so a
    // single broadcast frame suffices.
    ws_.broadcast(envelope::engineAnnounce(activeName()));
}

void Host::installCallbacks() {
    if (active_ && room_) {
        Room* room = room_.get();
        ws_.setCallbacks({
            [this, room](ConnId id) {
                announce(id);     // which engine, first
                room->onOpen(id);
            },
            [room](ConnId id, std::string_view msg) { room->onMessage(id, msg); },
            [room](ConnId id, std::string_view bytes) { room->onBinary(id, bytes); },
            [room](ConnId id) { room->onClose(id); },
        });
    } else {
        // Launcher: no engine. Announce "none" on connect; ignore everything else
        // (a client just waits for the operator to start an engine).
        ws_.setCallbacks({
            [this](ConnId id) { announce(id); },
            [](ConnId, std::string_view) {},
            [](ConnId, std::string_view) {},
            [](ConnId) {},
        });
    }
}

}  // namespace villen
