// Native Wii Remote input for ReWrithe. Protocol and game-facing state are platform independent;
// each operating system supplies only the Bluetooth/HID transport.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hotd::game {
struct GunState;
class Runtime;
class Pause;
struct PauseInput;

constexpr int kWiimotePlayers = 4;

namespace wii {
constexpr int kPlayers = kWiimotePlayers;
constexpr uint16_t Two=0x0001, One=0x0002, B=0x0004, A=0x0008, Minus=0x0010,
                   Home=0x0080, Left=0x0100, Right=0x0200, Down=0x0400, Up=0x0800, Plus=0x1000;
constexpr std::array<uint16_t, 7> Bindable{B,A,Plus,Home,One,Two,Minus};

struct AimPoint { double x=0, y=0; };
struct AimCalibration {
    std::array<double,9> h{1,0,0,0,1,0,0,0,1};
    bool calibrated=false;
    bool Apply(AimPoint raw, AimPoint& mapped) const;
    bool Fit(const std::array<AimPoint,4>& samples);
};
struct Profile {
    // Fire, reload, confirm, pause / start. Duplicate bindings are rejected on load.
    std::array<uint16_t,4> buttons{B,A,Plus,Home};
    AimCalibration calibration;
    int smoothing=1;  // 0 off, 1 light, 2 strong
};
struct Settings {
    bool enabled=false, continuous=false;
    std::array<Profile,kPlayers> player;
    bool Load(const std::string& file);
    bool Save(const std::string& file) const;
};
struct Remote {
    bool connected=false, reserved=false, tracking=false;
    uint16_t buttons=0, pressed=0;
    int battery=-1, dots=0;
    AimPoint raw{0.5,0.5};
};
struct Snapshot {
    std::array<Remote,kPlayers> player;
    bool scanning=false;
    std::string message="Wii Remote support is off";
};
bool DecodeReport(const uint8_t* bytes, size_t size, Remote& remote);
std::array<uint8_t,22> WriteRegister(uint32_t address, const uint8_t* bytes, size_t size);

class Backend {
public:
    virtual ~Backend()=default;
    virtual void Enable(bool enabled, bool continuous)=0;
    virtual void Scan()=0;
    virtual void Disconnect(int player)=0;
    virtual void SwapPlayers()=0;
    virtual Snapshot Poll()=0;
};
std::unique_ptr<Backend> CreateBackend();
} // namespace wii

class Wiimotes {
public:
    Wiimotes();
    ~Wiimotes();
    Wiimotes(const Wiimotes&)=delete;
    Wiimotes& operator=(const Wiimotes&)=delete;

    wii::Settings settings;
    int gamePlayer[kWiimotePlayers] = {0,1,2,3};
    bool lostPause = true;
    std::function<void(int)> onJoin;

    bool Load(const std::string& file);
    bool Save(const std::string& file) const;
    void Configure();
    void Scan();
    void Disconnect(int player);
    void SwapPlayers();
    void Refresh();
    void ClearEdges();
    bool fireHeld(int player) const;
    void Apply(Runtime& runtime, GunState guns[], Pause& pause, PauseInput& pauseInput, int width, int height);
    const wii::Snapshot& Status() const { return snapshot_; }

private:
    std::unique_ptr<wii::Backend> backend_;
    wii::Snapshot snapshot_;
    std::array<bool,kWiimotePlayers> lost_{};
    std::array<bool,kWiimotePlayers> seen_{};
    std::array<wii::AimPoint,kWiimotePlayers> smoothedAim_{};
    std::array<bool,kWiimotePlayers> hasAim_{};
};
} // namespace hotd::game
