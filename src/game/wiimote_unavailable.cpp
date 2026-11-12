#include "game/wiimote.h"
namespace hotd::game::wii {
namespace {
// Compile-time fallback for platforms without a native HID transport. All methods are safe no-ops so the
// game-facing Wiimotes adapter can remain in the common build without platform-specific #ifdefs.
class UnavailableBackend final : public Backend {
public:
    void Enable(bool,bool) override {}
    void Scan() override {}
    void Disconnect(int) override {}
    void SwapPlayers() override {}
    void SetRumble(int,bool) override {}
    Snapshot Poll() override {
        Snapshot s;
        s.message="Native Wii Remote support is not available on this platform";
        return s;
    }
};
}
std::unique_ptr<Backend> CreateBackend(){return std::make_unique<UnavailableBackend>();}
} // namespace hotd::game::wii
