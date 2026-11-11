#include "game/wiimote.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static void Check(bool ok,const char* why) {
    if(!ok) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
using namespace hotd::game::wii;

static std::array<uint8_t,22> Report(uint16_t buttons,int x=500,int y=380,bool track=true) {
    std::array<uint8_t,22> r{};
    r[0]=0x33; r[1]=uint8_t(buttons>>8); r[2]=uint8_t(buttons);
    for(int i=6;i<18;++i) r[i]=0xff;
    if(track) for(int i=0;i<2;++i) {
        const int xx=x+(i?70:-70);
        r[6+i*3]=uint8_t(xx); r[7+i*3]=uint8_t(y);
        r[8+i*3]=uint8_t(((xx>>8)<<4)|((y>>8)<<6)|3);
    }
    return r;
}

int main() {
    Remote remote;
    auto b=Report(B);
    Check(DecodeReport(b.data(),b.size(),remote),"decode valid extended IR report");
    Check(remote.tracking && remote.dots==2,"detect both IR points");
    Check((remote.buttons&B)!=0 && (remote.pressed&B)!=0,"new button press edge");
    Check(remote.raw.x>0.45 && remote.raw.x<0.55,"IR horizontal centre is normalized");
    remote.pressed=0;
    Check(DecodeReport(b.data(),b.size(),remote),"decode held-button report");
    Check((remote.pressed&B)==0,"held button does not repeat press edge");
    auto released=Report(0);
    Check(DecodeReport(released.data(),released.size(),remote),"decode released report");
    auto pressed=Report(B);
    Check(DecodeReport(pressed.data(),pressed.size(),remote),"decode second press");
    Check((remote.pressed&B)!=0,"second press produces a new edge");

    uint8_t malformed[4]{0x33,0,0,0};
    Check(!DecodeReport(malformed,sizeof(malformed),remote),"reject truncated report");

    AimCalibration calibration;
    const std::array<AimPoint,4> samples{{{.25,.20},{.75,.24},{.78,.77},{.22,.73}}};
    Check(calibration.Fit(samples),"fit four-point aim calibration");
    const std::array<AimPoint,4> targets{{{.15,.15},{.85,.15},{.85,.85},{.15,.85}}};
    for(int i=0;i<4;++i) {
        AimPoint mapped;
        Check(calibration.Apply(samples[i],mapped),"apply calibration homography");
        Check(std::abs(mapped.x-targets[i].x)<1e-6 && std::abs(mapped.y-targets[i].y)<1e-6,
              "calibration maps each sample to its target");
    }

    Settings settings;
    settings.enabled=true;
    settings.continuous=true;
    settings.rumble=false;
    settings.player[0].buttons={A,B,One,Minus};
    Check(AssignButton(settings.player[0],2,Two),"assign a unique remapped action button");
    Check(!AssignButton(settings.player[0],0,Two),"reject a duplicate action binding");
    Check(std::string(ButtonName(Plus))=="+","name Wii Remote buttons for the setup menu");
    settings.player[2].buttons={One,Two,Plus,Home};
    settings.player[3].smoothing=2;
    const std::string path="wiimote-test.ini";
    Check(settings.Save(path),"save four-player settings");
    Settings loaded;
    Check(loaded.Load(path),"load four-player settings");
    Check(loaded.enabled && loaded.continuous,"restore support settings");
    Check(!loaded.rumble,"restore rumble-off preference");
    Check(loaded.player[0].buttons==settings.player[0].buttons,"restore remapped P1 action bindings");
    Check(loaded.player[2].buttons==settings.player[2].buttons,"restore P3 bindings");
    Check(loaded.player[3].smoothing==2,"restore P4 settings");
    std::remove(path.c_str());
    std::puts("Wiimote protocol / calibration / four-player settings: OK");
    return 0;
}
