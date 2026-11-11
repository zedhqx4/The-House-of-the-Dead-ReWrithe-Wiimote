#include "game/wiimote.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hotd::game::wii {

bool AimCalibration::Apply(AimPoint p, AimPoint& out) const {
    const double z=h[6]*p.x+h[7]*p.y+h[8];
    if (!std::isfinite(z) || std::abs(z)<1e-8) return false;
    out={(h[0]*p.x+h[1]*p.y+h[2])/z,(h[3]*p.x+h[4]*p.y+h[5])/z};
    return std::isfinite(out.x) && std::isfinite(out.y);
}

bool AimCalibration::Fit(const std::array<AimPoint,4>& s) {
    const std::array<AimPoint,4> target{{{.15,.15},{.85,.15},{.85,.85},{.15,.85}}};
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
                for(int i=0;i<4;++i) for(int j=0;j<i;++j) if(binding[i]==binding[j]) valid=false;
                if(valid) profile.buttons=binding;
            }
            if(key==prefix+"calibration") {
                AimCalibration c;
                bool valid=true;
                for(double& x:c.h) if(!(v>>x) || !std::isfinite(x) || std::abs(x)>1000) valid=false;
                const double det=c.h[0]*(c.h[4]*c.h[8]-c.h[5]*c.h[7])
                    -c.h[1]*(c.h[3]*c.h[8]-c.h[5]*c.h[6])
                    +c.h[2]*(c.h[3]*c.h[7]-c.h[4]*c.h[6]);
                if(valid && std::abs(det)>1e-8) { c.calibrated=true; profile.calibration=c; }
            }
        }
    }
    *this=loaded;
    return true;
}

bool Settings::Save(const std::string& file) const {
    std::ofstream f(file,std::ios::trunc);
    if(!f) return false;
    f<<"enabled="<<enabled<<"\ncontinuous="<<continuous<<'\n'<<std::setprecision(12);
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

bool DecodeReport(const uint8_t* b,size_t n,Remote& r) {
    if(!b || n<3) return false;
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
    const uint16_t buttons=static_cast<uint16_t>(((b[1]<<8)|b[2])&0x1f9f);
    r.pressed|=buttons & ~r.buttons;
    r.buttons=buttons;
    if(b[0]==0x20) r.battery=std::clamp(int(b[6])*100/0xc0,0,100);
    if(b[0]==0x33) {
        std::array<AimPoint,4> dots{};
        int count=0;
        for(int i=0;i<4;++i) {
            const auto* d=b+6+i*3;
            const int x=d[0]+((d[2]&0x30)<<4);
            const int y=d[1]+((d[2]&0xc0)<<2);
            if(x<1023 && y<768) dots[count++]={double(x),double(y)};
        }
        r.dots=count;
        r.tracking=count>=2;
        if(r.tracking) {
            int left=0,right=1;
            double best=-1;
            for(int i=0;i<count;++i) for(int j=i+1;j<count;++j) {
                const double dx=dots[i].x-dots[j].x,dy=dots[i].y-dots[j].y;
                const double d=dx*dx+dy*dy;
                if(d>best) { best=d; left=i; right=j; }
            }
            if(best<16) { r.tracking=false; return true; }
            r.raw={1.0-(dots[left].x+dots[right].x)/(2*1023.0),
                   (dots[left].y+dots[right].y)/(2*767.0)};
        }
    }
    return true;
}

std::array<uint8_t,22> WriteRegister(uint32_t addr,const uint8_t* data,size_t n) {
    std::array<uint8_t,22> b{};
    if(n>16 || !n || !data) return b;
    b[0]=0x16; b[1]=0x04; b[2]=uint8_t(addr>>16); b[3]=uint8_t(addr>>8);
    b[4]=uint8_t(addr); b[5]=uint8_t(n);
    std::copy(data,data+n,b.begin()+6);
    return b;
}

} // namespace hotd::game::wii
