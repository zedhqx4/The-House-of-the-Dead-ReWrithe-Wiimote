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
    std::swap(rumbleUntil_[0],rumbleUntil_[1]);
    std::swap(rumbleActive_[0],rumbleActive_[1]);
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
    const auto now=std::chrono::steady_clock::now();
    for(int p=0;p<kWiimotePlayers;++p) {
        const bool want=settings.enabled && settings.rumble && snapshot_.player[p].connected &&
                        now<rumbleUntil_[p];
        if(!want) rumbleUntil_[p]={};
        if(want!=rumbleActive_[p]) {
            backend_->SetRumble(p,want);
            rumbleActive_[p]=want;
        }
    }
}
void Wiimotes::ClearEdges() {
    for(auto& remote:snapshot_.player) remote.pressed=0;
}
void Wiimotes::PulseRumble(int player,int durationMs) {
    if(player<0 || player>=kWiimotePlayers || durationMs<=0 || !settings.enabled || !settings.rumble)
        return;
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::clamp(durationMs,1,250));
    if(until>rumbleUntil_[player]) rumbleUntil_[player]=until;
}
bool Wiimotes::StartCalibration(int player) {
    if(player<0 || player>=kWiimotePlayers || !settings.enabled) return false;
    const auto& remote=snapshot_.player[player];
    if(!remote.connected || !remote.reserved) return false;
    calibrationPlayer_=player;
    calibrationStep_=0;
    calibrationFailed_=false;
    calibrationConsumed_.fill(false);
    if(!(remote.buttons&wii::B)) calibrationMenuBlockedPlayer_=-1;
    snapshot_.player[player].pressed=0;
    return true;
}
void Wiimotes::CancelCalibration() {
    if(calibrationPlayer_>=0) calibrationMenuBlockedPlayer_=calibrationPlayer_;
    calibrationPlayer_=-1;
    calibrationStep_=0;
}
void Wiimotes::CaptureCalibration() {
    calibrationConsumed_.fill(false);
    if(calibrationMenuBlockedPlayer_>=0) {
        const auto& blocked=snapshot_.player[calibrationMenuBlockedPlayer_];
        if(!blocked.connected || !(blocked.buttons&wii::B))
            calibrationMenuBlockedPlayer_=-1;
    }
    if(calibrationPlayer_<0) return;
    auto& remote=snapshot_.player[calibrationPlayer_];
    if(!remote.connected || !remote.reserved) {
        CancelCalibration();
        return;
    }
    // Consume this player's controls even on the frame that completes calibration.
    calibrationConsumed_[calibrationPlayer_]=true;
    if(remote.pressed&wii::B) {
        CancelCalibration();
        remote.pressed=0;
        return; // B backs out of calibration.
    }
    if((remote.pressed&wii::A) && remote.tracking) {
        calibrationSamples_[calibrationStep_]=remote.raw;
        if(++calibrationStep_==4) {
            auto& calibration=settings.player[calibrationPlayer_].calibration;
            if(calibration.Fit(calibrationSamples_)) {
                calibrated=true;
                calibrationFailed_=false;
                calibrationMenuBlockedPlayer_=calibrationPlayer_;
                calibrationPlayer_=-1;
                calibrationStep_=0;
            } else {
                calibrationStep_=0;
                calibrationFailed_=true;
            }
        }
    }
    remote.pressed=0;
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
        "Press A when both IR dots are visible",
        "B or Esc: cancel"
    };
    if(calibrationFailed_) lines.push_back("Calibration geometry invalid; repeat the four points");
    return lines;
}

bool Wiimotes::BeginBinding(int player,int action) {
    if(player<0 || player>=kWiimotePlayers || action<0 || action>=4 || !settings.enabled) return false;
    const auto& remote=snapshot_.player[player];
    if(!remote.connected || !remote.reserved) return false;
    bindingPlayer_=player;
    bindingAction_=action;
    bindingMessagePlayer_=player;
    bindingMessageAction_=action;
    bindingMessage_.clear();
    bindingChanged=false;
    snapshot_.player[player].pressed=0;
    return true;
}

void Wiimotes::CaptureBinding() {
    if(bindingMenuBlockedPlayer_>=0) {
        const auto& blocked=snapshot_.player[bindingMenuBlockedPlayer_];
        if(!blocked.connected || !(blocked.buttons&bindingMenuBlockedButton_)) {
            bindingMenuBlockedPlayer_=-1;
            bindingMenuBlockedButton_=0;
        }
    }
    if(bindingPlayer_<0) return;
    auto& remote=snapshot_.player[bindingPlayer_];
    if(!remote.connected || !remote.reserved) {
        bindingMessage_="Remote disconnected";
        bindingPlayer_=bindingAction_=-1;
        remote.pressed=0;
        return;
    }
    for(const uint16_t button:wii::Bindable) {
        if(!(remote.pressed&button)) continue;
        auto& profile=settings.player[bindingPlayer_];
        if(!wii::AssignButton(profile,bindingAction_,button)) {
            static const char* const names[]={"Fire","Reload","Confirm","Pause"};
            int other=-1;
            for(int i=0;i<4;++i)
                if(i!=bindingAction_ && profile.buttons[i]==button) { other=i; break; }
            bindingMessage_=other>=0 ? std::string("Already used by ")+names[other] :
                                       std::string("Button unavailable");
            remote.pressed=0;
            return;
        }
        bindingMessage_.clear();
        bindingMenuBlockedPlayer_=bindingPlayer_;
        bindingMenuBlockedButton_=button;
        bindingPlayer_=bindingAction_=-1;
        bindingChanged=true;
        remote.pressed=0;
        return;
    }
}

void Wiimotes::CancelBinding() {
    if(bindingPlayer_>=0 && bindingPlayer_<kWiimotePlayers)
        snapshot_.player[bindingPlayer_].pressed=0;
    bindingPlayer_=bindingAction_=-1;
    bindingMessage_.clear();
    bindingMessagePlayer_=bindingMessageAction_=-1;
}

bool Wiimotes::BindingActive(int player,int action) const {
    return bindingPlayer_==player && bindingAction_==action;
}

std::string Wiimotes::BindingText(int player,int action) const {
    if(BindingActive(player,action))
        return bindingMessage_.empty() ? std::string("Press a button...") : bindingMessage_;
    if(bindingMessagePlayer_==player && bindingMessageAction_==action && !bindingMessage_.empty())
        return bindingMessage_;
    if(player<0 || player>=kWiimotePlayers || action<0 || action>=4) return "Unknown";
    return wii::ButtonName(settings.player[player].buttons[action]);
}

bool Wiimotes::MenuPointer(float& x,float& y,int* player) {
    if(!settings.enabled || MenuInputSuppressed()) {
        hasMenuAim_.fill(false);
        return false;
    }
    for(int p=0;p<kWiimotePlayers;++p) {
        const auto& remote=snapshot_.player[p];
        if(!remote.connected || !remote.reserved || !remote.tracking) {
            hasMenuAim_[p]=false;
            continue;
        }
        wii::AimPoint aim=remote.raw;
        const auto& profile=settings.player[p];
        if(profile.calibration.calibrated)
            profile.calibration.Apply(remote.raw,aim);
        const float alpha=profile.smoothing==0?1.0f:profile.smoothing==1?0.65f:0.35f;
        if(!hasMenuAim_[p]) {
            menuAim_[p]=aim;
            hasMenuAim_[p]=true;
        } else {
            menuAim_[p].x+=alpha*(aim.x-menuAim_[p].x);
            menuAim_[p].y+=alpha*(aim.y-menuAim_[p].y);
        }
        x=float(std::clamp(menuAim_[p].x,0.0,1.0));
        y=float(std::clamp(menuAim_[p].y,0.0,1.0));
        if(player) *player=p;
        return true; // The frontend draws its own hand cursor; no SDL / OS mouse is generated.
    }
    return false;
}

bool Wiimotes::fireHeld(int player) const {
    if(player<0 || player>=kWiimotePlayers || !settings.enabled) return false;
    if(player==calibrationPlayer_ || player==calibrationMenuBlockedPlayer_ || calibrationConsumed_[player]) return false;
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
        if(p==calibrationPlayer_ || p==calibrationMenuBlockedPlayer_ || calibrationConsumed_[p]) {
            remote.pressed=0;
            continue; // calibration input must never become in-game shots / reloads
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
            if(edge&profile.buttons[0]) {
                guns[p].trigger=true;
                PulseRumble(p,90);
            }
            if(edge&profile.buttons[1]) {
                guns[p].reload=true;
                PulseRumble(p,130);
            }
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
