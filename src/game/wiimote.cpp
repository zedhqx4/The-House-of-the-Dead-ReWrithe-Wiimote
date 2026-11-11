#include "game/wiimote.h"
#include "game/pause.h"
#include "game/pads.h"
#include "game/runtime.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace hotd::game {
namespace {
using G = A::G;
int playerState(const Runtime& rt,int p) {
    return int32_t(rt.mem.u32(Runtime::player_rec(p)));
}
int gameMode(const Runtime& rt) {
    return rt.mem.at<int16_t>(G::game_mode);
}
}

Wiimotes::Wiimotes() : backend_(wii::CreateBackend()) {}
Wiimotes::~Wiimotes() = default;

bool Wiimotes::Load(const std::string& file) {
    const bool ok=settings.Load(file);
    Configure();
    if(settings.enabled && backend_) backend_->Scan();
    return ok;
}
bool Wiimotes::Save(const std::string& file) const {
    return settings.Save(file);
}
void Wiimotes::Configure() {
    if(backend_) backend_->Enable(settings.enabled,settings.continuous);
}
void Wiimotes::Scan() {
    if(!backend_) return;
    settings.enabled=true;
    Configure();
    backend_->Scan();
}
void Wiimotes::Disconnect(int player) {
    if(backend_ && player>=0 && player<kWiimotePlayers) backend_->Disconnect(player);
}
void Wiimotes::SwapPlayers() {
    if(!backend_) return;
    backend_->SwapPlayers();
    std::swap(settings.player[0],settings.player[1]);
    std::swap(gamePlayer[0],gamePlayer[1]);
    std::swap(snapshot_.player[0],snapshot_.player[1]);
    std::swap(lost_[0],lost_[1]);
    std::swap(seen_[0],seen_[1]);
    std::swap(smoothedAim_[0],smoothedAim_[1]);
    std::swap(hasAim_[0],hasAim_[1]);
}
void Wiimotes::Refresh() {
    if(!backend_) return;
    const auto fresh=backend_->Poll();
    snapshot_.scanning=fresh.scanning;
    snapshot_.message=fresh.message;
    for(int p=0;p<kWiimotePlayers;++p) {
        const auto pending=snapshot_.player[p].pressed;
        snapshot_.player[p]=fresh.player[p];
        snapshot_.player[p].pressed|=pending;
        if(!fresh.player[p].connected) snapshot_.player[p].pressed=0;
    }
}
void Wiimotes::ClearEdges() {
    for(auto& remote:snapshot_.player) remote.pressed=0;
}
bool Wiimotes::StartCalibration() {
    calibrationPlayer_ = -1;
    calibrationStep_ = 0;
    calibrationFailed_ = false;
    for(int p=0;p<kWiimotePlayers;++p) {
        const auto& remote=snapshot_.player[p];
        if(remote.connected && remote.reserved) { calibrationPlayer_=p; break; }
    }
    return calibrationPlayer_ >= 0;
}
bool Wiimotes::CalibrationTarget(float& x,float& y) const {
    if(calibrationPlayer_ < 0) return false;
    static constexpr float targets[4][2]={{.15f,.15f},{.85f,.15f},{.85f,.85f},{.15f,.85f}};
    x=targets[calibrationStep_][0];
    y=targets[calibrationStep_][1];
    return true;
}
std::vector<std::string> Wiimotes::CalibrationLines() const {
    if(calibrationPlayer_ < 0) return {};
    static const char* names[4]={"top left","top right","bottom right","bottom left"};
    std::vector<std::string> lines={
        "WII REMOTE CALIBRATION",
        "",
        "Player "+std::to_string(calibrationPlayer_+1)+": point at the "+names[calibrationStep_]+" target",
        "Press B when both IR dots are visible"
    };
    if(calibrationFailed_) lines.push_back("Calibration geometry invalid; repeat the four points");
    lines.push_back("Esc: cancel");
    return lines;
}

bool Wiimotes::fireHeld(int player) const {
    if(player<0 || player>=kWiimotePlayers || !settings.enabled) return false;
    const auto& remote=snapshot_.player[player];
    return remote.connected && (remote.buttons&settings.player[player].buttons[0])!=0;
}
void Wiimotes::Apply(Runtime& rt,GunState guns[],Pause& pause,PauseInput& pin,int width,int height) {
    if(!settings.enabled) return;
    const bool paused=pause.paused();
    const int mode=gameMode(rt);
    for(int p=0;p<kWiimotePlayers;++p) {
        auto& remote=snapshot_.player[p];
        if(!remote.connected || !remote.reserved) {
            if(p==calibrationPlayer_) CancelCalibration();
            if(lostPause && seen_[p] && !lost_[p] && mode==4 && playerState(rt,gamePlayer[p])==5 && !paused)
                pause.focus(false);
            lost_[p]=true;
            remote.pressed=0;
            continue;
        }
        lost_[p]=false;
        seen_[p]=true;
        auto& profile=settings.player[p];
        if(p==calibrationPlayer_) {
            if((remote.pressed & wii::B) && remote.tracking) {
                calibrationSamples_[calibrationStep_]=remote.raw;
                if(++calibrationStep_==4) {
                    if(profile.calibration.Fit(calibrationSamples_)) {
                        calibrated=true;
                        calibrationFailed_=false;
                        calibrationPlayer_=-1;
                    } else {
                        calibrationStep_=0;
                        calibrationFailed_=true;
                    }
                }
            }
            remote.pressed=0;
            continue; // calibration presses must never become in-game shots / reloads
        }
        wii::AimPoint aim=remote.raw;
        if(profile.calibration.calibrated)
            profile.calibration.Apply(remote.raw,aim);
        if(remote.tracking) {
            const float alpha=profile.smoothing==0?1.0f:profile.smoothing==1?0.65f:0.35f;
            if(!hasAim_[p]) {
                smoothedAim_[p]=aim;
                hasAim_[p]=true;
            } else {
                smoothedAim_[p].x+=alpha*(aim.x-smoothedAim_[p].x);
                smoothedAim_[p].y+=alpha*(aim.y-smoothedAim_[p].y);
            }
            const float margin=float(height)*0.05f;
            guns[p].x=std::clamp(float(smoothedAim_[p].x)*float(width),-margin,float(width)+margin);
            guns[p].y=std::clamp(float(smoothedAim_[p].y)*float(height),-margin,float(height)+margin);
            guns[p].pointer=false;
        } else {
            hasAim_[p]=false;
        }

        const uint16_t edge=remote.pressed;
        if(paused) {
            pin.up=pin.up || ((edge& wii::Up)!=0);
            pin.down=pin.down || ((edge& wii::Down)!=0);
            pin.activate=pin.activate || ((edge&profile.buttons[2])!=0) || ((edge&profile.buttons[0])!=0);
            pin.toggle=pin.toggle || ((edge&profile.buttons[3])!=0) || ((edge&wii::Minus)!=0);
        } else {
            if(edge&profile.buttons[0]) guns[p].trigger=true;
            if(edge&profile.buttons[1]) guns[p].reload=true;
            if(edge&profile.buttons[3]) {
                const int gp=gamePlayer[p];
                if(mode==4 && playerState(rt,gp)==5) pin.toggle=true;
                else if(onJoin) onJoin(p);
                else rt.player_join(gp);
            }
        }
        // Retain the held state for game modes which support hold-to-skip / automatic fire.
        remote.pressed=0;
    }
}

} // namespace hotd::game
