#include "game/wiimote.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hotd::game::wii {

// Human-readable names are used by the remapping page; the masks themselves stay in Wii report format.
const char* ButtonName(uint16_t button) {
    switch(button) {
        case B: return "B";
        case A: return "A";
        case Plus: return "+";
        case Home: return "HOME";
        case One: return "1";
        case Two: return "2";
        case Minus: return "-";
        default: return "Unknown";
    }
}

// Keep each action-to-button mapping one-to-one. Invalid action indices and buttons not offered by the UI
// are rejected rather than silently changing a different action or persisting an unsupported control.
bool AssignButton(Profile& profile,int action,uint16_t button) {
    if(action<0 || action>=int(profile.buttons.size()) ||
       std::find(Bindable.begin(),Bindable.end(),button)==Bindable.end()) return false;
    for(int i=0;i<int(profile.buttons.size());++i)
        if(i!=action && profile.buttons[i]==button) return false;
    profile.buttons[size_t(action)]=button;
    return true;
}

// Apply a projective transform: divide the transformed X/Y numerators by the third homogeneous coordinate.
// A near-zero denominator is a point at infinity and must not be allowed into cursor coordinates.
bool AimCalibration::Apply(AimPoint p, AimPoint& out) const {
    const double z=h[6]*p.x+h[7]*p.y+h[8];
    if (!std::isfinite(z) || std::abs(z)<1e-8) return false;
    out={(h[0]*p.x+h[1]*p.y+h[2])/z,(h[3]*p.x+h[4]*p.y+h[5])/z};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

// Fit the eight free coefficients of a 3x3 homography (h[8] is fixed to 1) from four source/target pairs.
// The caller supplies samples in target order: top-left, top-right, bottom-right, bottom-left.
bool AimCalibration::Fit(const std::array<AimPoint,4>& s) {
    const std::array<AimPoint,4> target{{{.15,.15},{.85,.15},{.85,.85},{.15,.85}}};
    // Reject crossed, collinear or nearly collapsed sample quads before solving; they cannot define a
    // stable screen mapping and would otherwise produce extreme coefficients or a flipped cursor.
    double sign=0;
    for(int i=0;i<4;++i) {
        const auto a=s[i],b=s[(i+1)%4],c=s[(i+2)%4];
        const double cross=(b.x-a.x)*(c.y-b.y)-(b.y-a.y)*(c.x-b.x);
        if(!std::isfinite(cross) || std::abs(cross)<.002 || (sign && cross*sign<0)) return false;
        sign=cross;
    }
    double m[8][9]{};
    for(int i=0;i<4;++i) {
        const double x=s[i].x,y=s[i].y,u=target[i].x,v=target[i].y;
        const double a[9]{x,y,1,0,0,0,-u*x,-u*y,u};
        const double b[9]{0,0,0,x,y,1,-v*x,-v*y,v};
        std::copy(a,a+9,m[i*2]); std::copy(b,b+9,m[i*2+1]);
    }
    // Solve the 8x8 linear system with Gauss-Jordan elimination and partial pivoting. Choosing the
    // largest available pivot reduces numerical error when the user's four samples are unevenly spaced.
    for(int col=0;col<8;++col) {
        int pivot=col;
        for(int r=col+1;r<8;++r) if(std::abs(m[r][col])>std::abs(m[pivot][col])) pivot=r;
        if(std::abs(m[pivot][col])<1e-9) return false;
        for(int j=0;j<9;++j) std::swap(m[col][j],m[pivot][j]);
        const double v=m[col][col];
        for(int j=col;j<9;++j) m[col][j]/=v;
        for(int r=0;r<8;++r) if(r!=col) {
            const double f=m[r][col];
            for(int j=col;j<9;++j) m[r][j]-=f*m[col][j];
        }
    }
    AimCalibration next;
    for(int i=0;i<8;++i) {
        next.h[i]=m[i][8];
        if(!std::isfinite(next.h[i]) || std::abs(next.h[i])>1000) return false;
    }
    next.calibrated=true;
    *this=next;
    return true;
}

// Parse into a temporary Settings object and publish it only after the file has been read. Bad individual
// values keep their defaults; invalid button sets or singular calibration matrices are ignored.
bool Settings::Load(const std::string& file) {
    std::ifstream f(file);
    if(!f) return false;
    Settings loaded;
    for(std::string line;std::getline(f,line);) {
        const auto pos=line.find('=');
        if(pos==std::string::npos) continue;
        const std::string key=line.substr(0,pos);
        std::istringstream v(line.substr(pos+1));
        int number=0;
        if(key=="enabled" && v>>number) loaded.enabled=number==1;
        else if(key=="continuous" && v>>number) loaded.continuous=number==1;
        else if(key=="rumble" && v>>number) loaded.rumble=number!=0;
        for(int p=0;p<kPlayers;++p) {
            const auto prefix="p"+std::to_string(p+1)+".";
            auto& profile=loaded.player[p];
            if(key==prefix+"smoothing" && v>>number) profile.smoothing=std::clamp(number,0,2);
            if(key==prefix+"buttons") {
                auto binding=profile.buttons;
                bool valid=true;
                for(auto& b:binding) {
                    if(!(v>>number) || std::find(Bindable.begin(),Bindable.end(),number)==Bindable.end()) valid=false;
                    else b=static_cast<uint16_t>(number);
                }
                // A partially parsed or duplicated mapping is discarded as a whole; never save a half-remap.
                for(int i=0;i<4;++i) for(int j=0;j<i;++j) if(binding[i]==binding[j]) valid=false;
                if(valid) profile.buttons=binding;
            }
            if(key==prefix+"calibration") {
                AimCalibration c;
                bool valid=true;
                for(double& x:c.h) if(!(v>>x) || !std::isfinite(x) || std::abs(x)>1000) valid=false;
                // Finite coefficients alone are not enough: a singular matrix has no usable inverse mapping.
                const double det=c.h[0]*(c.h[4]*c.h[8]-c.h[5]*c.h[7])
                    -c.h[1]*(c.h[3]*c.h[8]-c.h[5]*c.h[6])
                    +c.h[2]*(c.h[3]*c.h[7]-c.h[4]*c.h[6]);
                if(valid && std::abs(det)>1e-8) { c.calibrated=true; profile.calibration=c; }
            }
        }
    }
    // Migrate the previous built-in defaults without disturbing custom player mappings.
    for(auto& profile:loaded.player)
        if(profile.buttons==std::array<uint16_t,4>{B,A,Plus,Home})
            profile.buttons={B,A,One,Plus};
    *this=loaded;
    return true;
}

// Store one global policy plus independent P1-P4 profiles. The action order matches Profile::buttons;
// calibration is omitted until a player has completed a valid four-point fit.
bool Settings::Save(const std::string& file) const {
    std::ofstream f(file,std::ios::trunc);
    if(!f) return false;
    f<<"enabled="<<enabled<<"\ncontinuous="<<continuous<<"\nrumble="<<rumble<<'\n'<<std::setprecision(12);
    for(int p=0;p<kPlayers;++p) {
        const auto prefix="p"+std::to_string(p+1)+".";
        const auto& profile=player[p];
        f<<prefix<<"smoothing="<<profile.smoothing<<'\n'<<prefix<<"buttons=";
        for(auto b:profile.buttons) f<<b<<' ';
        f<<'\n';
        if(profile.calibration.calibrated) {
            f<<prefix<<"calibration=";
            for(auto x:profile.calibration.h) f<<x<<' ';
            f<<'\n';
        }
    }
    f.flush();
    return bool(f);
}

// HID reports have different fixed layouts. Validate the report ID and its minimum length before reading
// any offsets, so a short disconnect/malformed packet cannot expose uninitialized bytes as controller input.
bool DecodeReport(const uint8_t* b,size_t n,Remote& r) {
    if(!b || n<3) return false;
    // Supported input report IDs and their minimum byte counts:
    // 0x20=status/battery, 0x21=memory-read data, 0x22=acknowledgement,
    // 0x30=buttons, 0x31=buttons+accelerometer, 0x33=buttons+accelerometer+extended IR.
    size_t required=0;
    switch(b[0]) {
        case 0x20: required=7; break;
        case 0x21: required=22; break;
        case 0x22: required=5; break;
        case 0x30: required=3; break;
        case 0x31: required=6; break;
        case 0x33: required=18; break;
        default: return false;
    }
    if(n<required) return false;
    // Button bytes are big-endian. 'pressed' accumulates only 0->1 transitions; held buttons stay in buttons.
    // The first two payload bytes are the button word (high byte first); 0x1f9f keeps defined controls
    // and strips reserved bits before the state is exposed to menu/game code.
    const uint16_t buttons=static_cast<uint16_t>(((b[1]<<8)|b[2])&0x1f9f);
    r.pressed|=buttons & ~r.buttons;
    r.buttons=buttons;
    if(b[0]==0x20) r.battery=std::clamp(int(b[6])*100/0xc0,0,100);
    if(b[0]==0x33) {
        // Report 0x33 carries the extended IR payload beginning at byte 6. Each dot packs 10-bit X/Y
        // across three bytes; 0x3ff/0x2ff are invalid sentinels for the camera's 1024x768 coordinate space.
        std::array<AimPoint,4> dots{};
        int count=0;
        for(int i=0;i<4;++i) {
            const auto* d=b+6+i*3;
            // Each IR dot uses two low bytes plus two high bits packed into the third byte:
            // bits 4-5 extend X to 10 bits; bits 6-7 extend Y to 10 bits.
            const int x=d[0]+((d[2]&0x30)<<4);
            const int y=d[1]+((d[2]&0xc0)<<2);
            if(x<1023 && y<768) dots[count++]={double(x),double(y)};
        }
        r.dots=count;
        r.tracking=count>=2;
        if(r.tracking) {
            // The sensor can report up to four blobs. Use the most widely separated pair as the two
            // sensor-bar emitters; very close points are not a reliable direction/center measurement.
            int left=0,right=1;
            double best=-1;
            for(int i=0;i<count;++i) for(int j=i+1;j<count;++j) {
                const double dx=dots[i].x-dots[j].x,dy=dots[i].y-dots[j].y;
                const double d=dx*dx+dy*dy;
                if(d>best) { best=d; left=i; right=j; }
            }
            if(best<16) { r.tracking=false; return true; }
            // Normalize to [0,1]. X is inverted because the remote's camera axis is opposite screen X.
            r.raw={1.0-(dots[left].x+dots[right].x)/(2*1023.0),
                   (dots[left].y+dots[right].y)/(2*767.0)};
        }
    }
    return true;
}

// Wii Remote memory writes carry a three-byte address and at most 16 payload bytes in a 22-byte report.
// Returning a zeroed report for invalid input lets callers reject bad writes without an out-of-bounds copy.
std::array<uint8_t,22> WriteRegister(uint32_t addr,const uint8_t* data,size_t n) {
    std::array<uint8_t,22> b{};
    if(n>16 || !n || !data) return b;
    // 0x16 is the memory-write command and 0x04 selects the Wii Remote register space; the address
    // follows as three big-endian bytes, then the payload length and payload.
    b[0]=0x16; b[1]=0x04; b[2]=uint8_t(addr>>16); b[3]=uint8_t(addr>>8);
    b[4]=uint8_t(addr); b[5]=uint8_t(n);
    std::copy(data,data+n,b.begin()+6);
    return b;
}

} // namespace hotd::game::wii
