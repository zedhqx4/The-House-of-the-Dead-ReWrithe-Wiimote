// Native Wii Remote input for ReWrithe. Protocol and game-facing state are platform independent;
// each operating system supplies only the Bluetooth/HID transport.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hotd::game {
struct GunState;
class Runtime;
class Pause;
struct PauseInput;

constexpr int kWiimotePlayers = 4;

namespace wii {
constexpr int kPlayers = kWiimotePlayers;
// Button bits as they appear in the high/low button bytes of a Wii Remote input report.
// Keep these masks in protocol form so the decoder, menu navigation and remapping share one definition.
constexpr uint16_t Two=0x0001, One=0x0002, B=0x0004, A=0x0008, Minus=0x0010,
                   Home=0x0080, Left=0x0100, Right=0x0200, Down=0x0400, Up=0x0800, Plus=0x1000;
// Buttons offered by the remapping UI. D-pad directions remain reserved for menu navigation.
constexpr std::array<uint16_t, 7> Bindable{B,A,Plus,Home,One,Two,Minus};
const char* ButtonName(uint16_t button);

// Normalized infrared-camera coordinates. DecodeReport flips X to match the game's screen-space convention.
struct AimPoint { double x=0, y=0; };
struct AimCalibration {
    // 3x3 projective transform (homography), stored row-major and normalized with h[8] == 1.
    std::array<double,9> h{1,0,0,0,1,0,0,0,1};
    bool calibrated=false;
    bool Apply(AimPoint raw, AimPoint& mapped) const;
    bool Fit(const std::array<AimPoint,4>& samples);
};
struct Profile {
    // Action order is part of the settings-file format: Fire, Reload, Confirm, Start / Pause.
    // Duplicate physical buttons are rejected both when loading and when assigning from the menu.
    std::array<uint16_t,4> buttons{B,A,One,Plus};
    AimCalibration calibration;
    int smoothing=1;  // 0 off, 1 light, 2 strong
};
bool AssignButton(Profile& profile, int action, uint16_t button);
struct Settings {
    // Support is opt-in; continuous controls whether discovery keeps running after a manual scan window.
    // Rumble is independent so users can keep IR/input while disabling the Wii Remote's binary motor.
    bool enabled=false, continuous=false, rumble=true;
    std::array<Profile,kPlayers> player;
    bool Load(const std::string& file);
    bool Save(const std::string& file) const;
};
struct Remote {
    // connected means the HID session is alive; reserved means this identity still owns the player slot
    // and can reclaim it after a transient disconnect. They intentionally are not equivalent.
    bool connected=false, reserved=false, tracking=false;
    // buttons is the current held mask; pressed contains rising edges since the previous backend poll.
    uint16_t buttons=0, pressed=0;
    int battery=-1, dots=0;
    AimPoint raw{0.5,0.5};
};
struct Snapshot {
    std::array<Remote,kPlayers> player;
    bool scanning=false;
    std::string message="Wii Remote support is off";
};
// Decode one HID input report and update held buttons, press edges, battery and IR state.
bool DecodeReport(const uint8_t* bytes, size_t size, Remote& remote);
// Build the Wii Remote memory-write output report used to configure the IR camera.
std::array<uint8_t,22> WriteRegister(uint32_t address, const uint8_t* bytes, size_t size);

// OS-specific transport. Implementations own their worker/device threads and return a snapshot;
// callers never read HID descriptors or touch platform handles from the game thread.
class Backend {
public:
    virtual ~Backend()=default;
    virtual void Enable(bool enabled, bool continuous)=0;
    virtual void Scan()=0;
    virtual void Disconnect(int player)=0;
    virtual void SwapPlayers()=0;
    virtual void SetRumble(int player, bool enabled)=0;
    virtual Snapshot Poll()=0;
};
std::unique_ptr<Backend> CreateBackend();
} // namespace wii

// Game-facing adapter: turns backend reports into ReWrithe gun/pause/menu input and game-event rumble.
// It is deliberately platform-neutral; only the Backend implementation knows about Bluetooth/HID APIs.
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

    // Four-point IR calibration is started from Options > Controls > Wii Remotes.
    // The host draws the targets on the current menu and calls CaptureCalibration every frame.
    bool StartCalibration(int player);
    void CaptureCalibration();
    void CancelCalibration();
    bool Calibrating() const { return calibrationPlayer_ >= 0; }
    bool BeginBinding(int player, int action);
    void CaptureBinding();
    void CancelBinding();
    bool BindingActive(int player, int action) const;
    std::string BindingText(int player, int action) const;
    bool MenuPointer(float& x, float& y, int* player = nullptr);
    bool bindingChanged = false;
    // While calibrating/remapping, consume that remote's controls so setup presses cannot leak into gameplay.
    // The *_MenuBlockedPlayer_ state also waits for the last held button to be released before restoring menu input.
    bool MenuInputSuppressed() const { return Calibrating() || calibrationMenuBlockedPlayer_ >= 0 ||
        bindingPlayer_ >= 0 || bindingMenuBlockedPlayer_ >= 0; }
    bool CalibrationTarget(float& x, float& y) const;
    std::vector<std::string> CalibrationLines() const;
    bool calibrated = false; // main saves wiimotes.ini after a profile completes calibration
    void Apply(Runtime& runtime, GunState guns[], Pause& pause, PauseInput& pauseInput, int width, int height);
    const wii::Snapshot& Status() const { return snapshot_; }

private:
    std::unique_ptr<wii::Backend> backend_;
    wii::Snapshot snapshot_;
    // Per-player history used for disconnect/pause handling, aim smoothing and event-based rumble.
    std::array<bool,kWiimotePlayers> lost_{};
    std::array<bool,kWiimotePlayers> seen_{};
    std::array<wii::AimPoint,kWiimotePlayers> smoothedAim_{};
    std::array<bool,kWiimotePlayers> hasAim_{};
    std::array<wii::AimPoint,kWiimotePlayers> menuAim_{};
    std::array<bool,kWiimotePlayers> hasMenuAim_{};
    std::array<std::chrono::steady_clock::time_point,kWiimotePlayers> rumbleUntil_{};
    std::array<bool,kWiimotePlayers> rumbleActive_{};
    std::array<int,kWiimotePlayers> rumbleLives_{}, rumbleScore_{};
    std::array<uint32_t,kWiimotePlayers> rumbleReloads_{};
    std::array<bool,kWiimotePlayers> rumbleSeen_{};
    void PulseRumble(int player, int durationMs);
    int bindingPlayer_ = -1, bindingAction_ = -1, bindingMessagePlayer_ = -1, bindingMessageAction_ = -1;
    int bindingMenuBlockedPlayer_ = -1;
    uint16_t bindingMenuBlockedButton_ = 0;
    std::string bindingMessage_;
    int calibrationPlayer_ = -1, calibrationStep_ = 0, calibrationMenuBlockedPlayer_ = -1;
    std::array<wii::AimPoint,4> calibrationSamples_{};
    std::array<bool,kWiimotePlayers> calibrationConsumed_{};
    bool calibrationFailed_ = false;
};
} // namespace hotd::game
