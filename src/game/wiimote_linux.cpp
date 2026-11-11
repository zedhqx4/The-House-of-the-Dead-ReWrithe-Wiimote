// Linux native Bluetooth HID transport. BlueZ/kernel perform Bluetooth pairing; ReWrithe
// opens the connected Nintendo HID interface directly through hidraw (never as a mouse).
#ifdef __linux__
#include "game/wiimote.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <initializer_list>
#include <utility>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

namespace hotd::game::wii {
namespace {
using Clock=std::chrono::steady_clock;

bool IsRemote(int fd) {
    struct hidraw_devinfo info{};
    if(ioctl(fd,HIDIOCGRAWINFO,&info)<0) return false;
    return info.vendor==0x057e && (info.product==0x0306 || info.product==0x0330);
}
std::string UniqueId(int fd,const std::string& fallback) {
    char value[256]{};
    if(ioctl(fd,HIDIOCGRAWUNIQ(sizeof(value)),value)>=0 && value[0]) return value;
    return fallback;
}
std::vector<std::pair<std::string,std::string>> HidrawDevices() {
    std::vector<std::pair<std::string,std::string>> out;
    std::error_code ec;
    for(const auto& entry:std::filesystem::directory_iterator("/dev",ec)) {
        const auto name=entry.path().filename().string();
        if(name.rfind("hidraw",0)!=0) continue;
        const auto path=entry.path().string();
        const int fd=open(path.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
        if(fd<0) continue;
        if(IsRemote(fd)) out.emplace_back(path,UniqueId(fd,path));
        close(fd);
    }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    return out;
}
class Device {
public:
    std::string path,key;
    std::atomic<bool> stop{false},alive{false};
    std::atomic<int> player{0};
    explicit Device(std::string p,std::string id,int slot)
        :path(std::move(p)),key(std::move(id)),player(slot){}
    ~Device(){stop=true;if(thread.joinable())thread.join();}
    bool Open() {
        fd=open(path.c_str(),O_RDWR|O_NONBLOCK|O_CLOEXEC);
        if(fd<0 || !IsRemote(fd)) return false;
        const uint8_t request[]{0x15,0};
        if(!Write(request,sizeof(request))) return false;
        uint8_t report[64]{};
        pollfd pfd{fd,POLLIN,0};
        if(::poll(&pfd,1,700)>0) {
            const ssize_t n=read(fd,report,sizeof(report));
            if(n>0) DecodeReport(report,size_t(n),state);
        }
        state.connected=state.reserved=true;
        state.pressed=0;
        alive=true;
        thread=std::thread([this]{Run();});
        return true;
    }
    Remote Poll() {
        std::lock_guard lock(mutex);
        Remote r=state;
        r.connected=alive;
        r.reserved=true;
        state.pressed=0;
        return r;
    }
private:
    int fd=-1;
    std::thread thread;
    std::mutex mutex;
    Remote state;
    bool Write(const uint8_t* bytes,size_t size) {
        if(fd<0 || size>64) return false;
        uint8_t report[64]{};
        std::copy(bytes,bytes+size,report);
        const ssize_t n=write(fd,report,size);
        return n==ssize_t(size);
    }
    bool Send(std::initializer_list<uint8_t> bytes) {
        return Write(bytes.begin(),bytes.size());
    }
    bool InitIR() {
        const uint8_t start=8,mode=3;
        const uint8_t sensitivity1[]{2,0,0,0x71,1,0,0xaa,0,0x64},sensitivity2[]{0x63,3};
        if(!Send({0x13,4}) || !Send({0x1a,4})) return false;
        for(auto cmd:{WriteRegister(0xb00030,&start,1),WriteRegister(0xb00000,sensitivity1,9),
                      WriteRegister(0xb0001a,sensitivity2,2),WriteRegister(0xb00033,&mode,1),
                      WriteRegister(0xb00030,&start,1)}) {
            if(stop || !Write(cmd.data(),cmd.size())) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        return Send({0x12,4,0x33});
    }
    void Run() {
        if(!InitIR()) {alive=false;return;}
        auto last=Clock::now(),statusAt=last;
        int led=-1;
        while(!stop) {
            if(led!=player) {
                led=player;
                if(!Send({0x11,uint8_t(0x10<<led)})) break;
            }
            pollfd pfd{fd,POLLIN,0};
            const int ready=::poll(&pfd,1,10);
            if(ready<0 && errno!=EINTR) break;
            if(ready>0 && (pfd.revents&POLLIN)) {
                uint8_t report[64]{};
                const ssize_t n=read(fd,report,sizeof(report));
                if(n>0) {
                    bool valid=false;
                    {std::lock_guard lock(mutex);valid=DecodeReport(report,size_t(n),state);}
                    if(valid) last=Clock::now();
                    if(report[0]==0x20 && !Send({0x12,4,0x33})) break;
                } else if(n<0 && errno!=EAGAIN && errno!=EINTR) break;
            }
            const auto now=Clock::now();
            if(now-last>std::chrono::milliseconds(300)) {
                std::lock_guard lock(mutex);
                state.tracking=false;
                state.buttons=state.pressed=0;
            }
            if(now-last>std::chrono::seconds(5)) break;
            if(now-statusAt>std::chrono::seconds(10)) {
                if(!Send({0x15,0})) break;
                statusAt=now;
            }
        }
        Send({0x11,0});
        alive=false;
    }
};

class NativeBackend final : public Backend {
public:
    NativeBackend():worker([this]{Run();}){}
    ~NativeBackend() override {
        stop=true;cv.notify_all();
        if(worker.joinable()) worker.join();
        devices={};
    }
    void Enable(bool on,bool continuous) override {
        {std::lock_guard lock(mutex);enabled=on;continuous_=continuous;if(!on)clear=true;}
        cv.notify_all();
    }
    void Scan() override {
        {std::lock_guard lock(mutex);requested=true;blocked.clear();}
        cv.notify_all();
    }
    void Disconnect(int p) override {
        if(p<0 || p>=kPlayers) return;
        {std::lock_guard lock(mutex);disconnect[p]=true;}
        cv.notify_all();
    }
    void SwapPlayers() override {
        std::lock_guard lock(mutex);
        std::swap(devices[0],devices[1]);std::swap(keys[0],keys[1]);
        for(int i=0;i<kPlayers;++i) if(devices[i]) devices[i]->player=i;
    }
    Snapshot Poll() override {
        std::lock_guard lock(mutex);
        Snapshot s;s.scanning=scanning;s.message=message;
        for(int i=0;i<kPlayers;++i) {
            if(devices[i]) s.player[i]=devices[i]->Poll();
            s.player[i].reserved=!keys[i].empty();
        }
        return s;
    }
private:
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable cv;
    std::array<std::shared_ptr<Device>,kPlayers> devices;
    std::array<std::string,kPlayers> keys;
    std::array<bool,kPlayers> disconnect{};
    std::set<std::string> blocked;
    bool enabled=false,continuous_=false,requested=false,clear=false,scanning=false;
    std::string message="Pair a Wii Remote in Bluetooth settings, then scan here";
    std::thread worker;
    void Run() {
        auto deadline=Clock::now();
        while(!stop) {
            std::array<std::shared_ptr<Device>,kPlayers> retired;
            bool scan=false;
            {
                std::unique_lock lock(mutex);
                cv.wait_for(lock,std::chrono::milliseconds(300),[&]{
                    return stop || requested || clear ||
                        std::any_of(disconnect.begin(),disconnect.end(),[](bool v){return v;});
                });
                if(stop) break;
                for(int i=0;i<kPlayers;++i) if(clear || disconnect[i]) {
                    if(disconnect[i] && !keys[i].empty()) blocked.insert(keys[i]);
                    retired[i]=std::move(devices[i]);
                    keys[i].clear();
                    disconnect[i]=false;
                }
                clear=false;
                if(requested) {deadline=Clock::now()+std::chrono::seconds(20);requested=false;}
                scan=enabled && (continuous_ || Clock::now()<deadline);
                scanning=scan;
                message=scan?"Searching for connected Nintendo Wii Remote HID devices":
                    enabled?"Pair remotes in Bluetooth settings, then scan again":"Wii Remote support is off";
            }
            retired={};
            if(!scan) continue;
            for(const auto& [path,key]:HidrawDevices()) {
                int slot=-1;
                {
                    std::lock_guard lock(mutex);
                    if(!enabled || blocked.count(key)) continue;
                    for(int i=0;i<kPlayers;++i)
                        if(keys[i]==key && (!devices[i] || !devices[i]->alive)) {slot=i;break;}
                    if(slot<0 && std::find(keys.begin(),keys.end(),key)!=keys.end()) continue;
                    if(slot<0) for(int i=0;i<kPlayers;++i) if(keys[i].empty()) {slot=i;break;}
                }
                if(slot<0) continue;
                auto candidate=std::make_shared<Device>(path,key,slot);
                if(!candidate->Open()) continue;
                std::shared_ptr<Device> previous;
                {
                    std::lock_guard lock(mutex);
                    if(enabled && !blocked.count(key) && (keys[slot].empty() || keys[slot]==key)) {
                        previous=std::move(devices[slot]);
                        devices[slot]=candidate;
                        keys[slot]=key;
                    }
                }
            }
            std::unique_lock lock(mutex);
            cv.wait_for(lock,std::chrono::milliseconds(750),[&]{
                return stop || requested || clear ||
                    std::any_of(disconnect.begin(),disconnect.end(),[](bool v){return v;});
            });
        }
    }
};
} // namespace
std::unique_ptr<Backend> CreateBackend(){return std::make_unique<NativeBackend>();}
} // namespace hotd::game::wii
#endif
