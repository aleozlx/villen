// Villen — ChessEngine: the first engine, on the IEngine contract.
//
// Everything chess-specific that used to leak into the host (the Position, the
// two named seats, the `state`/`proposeMove` wire format, turn derivation) lives
// here. Villen sees only an IEngine and the opaque bytes this engine emits
// (DESIGN-game-framework §7). The engine is the single authority on legality;
// Room is the single authority on seats.
//
// One IEngine *engine* can run a family of *games* (framework §1): ChessEngine is
// where regular and (later) fairy chess will both live — the host stays the same.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "engine.hpp"
#include "villen/chess/position.hpp"

namespace villen {

class ChessEngine : public IEngine {
 public:
    SeatRoster seats() const override;  // {"white","black"}
    void onJoin(Room&, ConnId, SeatId) override;
    void onLeave(Room&, ConnId, SeatId) override;
    void onMessage(Room&, ConnId, SeatId, std::string_view) override;
    std::string statusLine() const override;
    void drawAdmin() override;
    void reset() override;

 private:
    chess::Position position_ = chess::Position::initial();

    void broadcastState(Room&);
};

class ChessFactory : public IEngineFactory {
 public:
    std::unique_ptr<IEngine> create() override { return std::make_unique<ChessEngine>(); }
    const char* name() const override { return "chess"; }
    // "" = serve the host's configured client dir (today's single client/).
    // Per-engine client subdirs are wired with multi-engine client routing.
    const char* clientDir() const override { return ""; }
};

}  // namespace villen
