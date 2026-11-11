#include "game/wiimote.h"
namespace hotd::game::wii {
namespace {
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
