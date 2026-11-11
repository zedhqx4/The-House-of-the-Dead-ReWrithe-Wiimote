#include "game/wiimote.h"
#include "game/pause.h"
#include "game/runtime.h"
#include <algorithm>
#include <cmath>

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
void Wiimotes::Apply(Runtime& rt,GunState guns[],Pause& pause,PauseInput& pin,int width,int height) {
    if(!settings.enabled) return;
    const bool paused=pause.paused();
    const int mode=gameMode(rt);
    for(int p=0;p<kWiimotePlayers;++p) {
        auto& remote=snapshot_.player[p];
        if(!remote.connected || !remote.reserved) {
            if(!lost_[p] && mode==4 && playerState(rt,gamePlayer[p])==5 && !paused)
                pause.focus(false);
            lost_[p]=true;
            remote.pressed=0;
            continue;
        }
        lost_[p]=false;
        const auto& profile=settings.player[p];
        wii::AimPoint aim=remote.raw;
        if(profile.calibration.calibrated)
            profile.calibration.Apply(remote.raw,aim);
        if(remote.tracking) {
            const float margin=float(height)*0.05f;
            guns[p].x=std::clamp(float(aim.x)*float(width),-margin,float(width)+margin);
            guns[p].y=std::clamp(float(aim.y)*float(height),-margin,float(height)+margin);
            guns[p].pointer=false;
        }

        const uint16_t down=remote.buttons;
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
        guns[p].held=(down&profile.buttons[0])!=0;
        remote.pressed=0;
    }
}

} // namespace hotd::game
