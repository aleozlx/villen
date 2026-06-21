// Villen — the game-framework contract (DESIGN-game-framework §4).
//
// The defining principle: **Villen never constructs a gameplay message.** The
// host hands an engine a Room handle and notifies it of membership events; the
// engine emits its own protocol bytes through that handle and Villen relays them
// verbatim, never reading the payload. So no game concept (chess, snake, …)
// appears inside Villen itself — this header names none.
//
// Two layers (framework §1): Villen runs *engines*; an engine runs a family of
// *games* (variants). `ChessEngine` is the host-facing runtime; "regular vs fairy
// chess" is its content, not a separate host module. Concrete engines first; the
// abstract bases (a `TickEngine`, …) are extracted only once ≥2 engines share
// infra — so this interface is deliberately small.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace villen {

// A connection identity (matches net::WsServer::ConnId). 0 is "no connection".
using ConnId = std::uint64_t;
// An index into the engine's seat roster. kNoSeat is a spectator / unseated conn.
using SeatId = int;
inline constexpr SeatId kNoSeat = -1;

// The transport seam (framework §5.3): snapshots/lifecycle go Reliable; a future
// unreliable channel (WebRTC) can carry high-rate state. Today's WebSocket is
// always reliable, so this is recorded but not yet acted on.
enum class Delivery { Reliable, Unreliable };

// An engine declares the seats it wants, by name; Villen assigns and holds them.
// chess -> {"white","black"}; a four-player game names four. Order is the
// auto-assign order (framework §5.2).
struct SeatRoster {
    std::vector<std::string> names;
};

class Room;  // the membership + transport handle (room.hpp)

// What every engine implements. Villen owns the envelope (who, which seat, the
// join/leave/reconnect lifecycle); the engine owns the payload (moves, state).
class IEngine {
 public:
    virtual ~IEngine() = default;  // teardown through the base pointer on switch

    // Declares the seat shape once, at construction; Villen builds the roster.
    virtual SeatRoster seats() const = 0;

    // Membership notifications. seat == kNoSeat means a spectator. onJoin is the
    // engine's cue to push its current state to the joiner.
    virtual void onJoin(Room&, ConnId, SeatId) = 0;
    virtual void onLeave(Room&, ConnId, SeatId) = 0;

    // An already-seated, opaque payload — the engine's own protocol. Villen has
    // not parsed it (beyond ruling out the join/seat envelope).
    virtual void onMessage(Room&, ConnId, SeatId, std::string_view payload) = 0;

    // Real-time engines advance the world here; turn-based engines don't override
    // it (framework §5.1). `nowMs` is a monotonic millisecond clock.
    virtual void onTick(Room&, std::uint64_t /*nowMs*/) {}

    // Optional admin surfaces (admin-shell §8). statusLine is a generic one-line
    // roster status; drawAdmin draws *only* the engine's own panel body (no
    // chrome) and is a no-op headless. Both may use room() for state/actions.
    virtual std::string statusLine() const { return {}; }
    virtual void drawAdmin() {}

    // Admin "new game".
    virtual void reset() {}

    // Villen calls attach() right after building the Room, so admin-triggered
    // actions (reset, a drawAdmin button) can reach the transport without the
    // network passing a Room& in (those paths aren't network events). It calls
    // detach() at teardown — before destroying the Room — so the engine's
    // destructor can't use-after-free a Room that has outlived this pointer.
    void attach(Room& r) { room_ = &r; }
    void detach() { room_ = nullptr; }

 protected:
    Room* room_ = nullptr;  // valid between attach() and detach() (the active
                            // engine's lifetime); null otherwise — guard before use
};

// One IEngine instance per active room; the launcher picks among factories
// (admin-shell §2). Single-room is just "one factory, one instance".
class IEngineFactory {
 public:
    virtual ~IEngineFactory() = default;
    virtual std::unique_ptr<IEngine> create() = 0;
    virtual const char* name() const = 0;       // shown in the launcher
    virtual const char* clientDir() const = 0;  // engine's client subdir; "" = host default
};

}  // namespace villen
