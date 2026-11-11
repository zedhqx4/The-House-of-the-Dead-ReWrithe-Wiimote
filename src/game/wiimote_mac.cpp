// macOS native Bluetooth HID transport via IOKit. Pair the Wii Remote once in macOS Bluetooth
// settings; ReWrithe opens its HID interface directly and reads IR reports (no mouse conversion).
#ifdef __APPLE__
#include "game/wiimote.h"
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hotd::game::wii {
namespace {
using Clock=std::chrono::steady_clock;

int NumberProperty(IOHIDDeviceRef device,CFStringRef key) {
    CFTypeRef value=IOHIDDeviceGetProperty(device,key);
    if(!value || CFGetTypeID(value)!=CFNumberGetTypeID()) return -1;
    int number=-1;
    CFNumberGetValue((CFNumberRef)value,kCFNumberIntType,&number);
    return number;
}
std::string StringProperty(IOHIDDeviceRef device,CFStringRef key) {
    CFTypeRef value=IOHIDDeviceGetProperty(device,key);
    if(!value || CFGetTypeID(value)!=CFStringGetTypeID()) return {};
    char text[256]{};
    if(!CFStringGetCString((CFStringRef)value,text,sizeof(text),kCFStringEncodingUTF8)) return {};
    return text;
}
std::string DeviceId(IOHIDDeviceRef device) {
    const auto serial=StringProperty(device,CFSTR(kIOHIDSerialNumberKey));
    if(!serial.empty()) return serial;
    const int location=NumberProperty(device,CFSTR(kIOHIDLocationIDKey));
    if(location>0) return std::to_string(location);
    return std::to_string(CFHash(device));
}
bool IsRemote(IOHIDDeviceRef device) {
    const int vendor=NumberProperty(device,CFSTR(kIOHIDVendorIDKey));
    const int product=NumberProperty(device,CFSTR(kIOHIDProductIDKey));
    return vendor==0x057e && (product==0x0306 || product==0x0330);
}
class Device {
public:
    std::string key;
    std::atomic<bool> stop{false},alive{false};
    std::atomic<int> player{0};
    explicit Device(IOHIDDeviceRef d,int slot):player(slot),device(d) {
        if(device) CFRetain(device);
        key=DeviceId(device);
    }
    ~Device() {
        stop=true;
        if(thread.joinable()) thread.join();
        if(device) CFRelease(device);
    }
    bool Open() {
        if(!device || !IsRemote(device)) return false;
        if(IOHIDDeviceOpen(device,kIOHIDOptionsTypeNone)!=kIOReturnSuccess) return false;
        alive=true;
        thread=std::thread([this]{Run();});
        return true;
    }
    Remote Poll() {
        std::lock_guard lock(mutex);
        Remote r=state;r.connected=alive;r.reserved=true;
        state.pressed=0;
        return r;
    }
    void SetRumble(bool enabled) { rumbleRequested.store(enabled,std::memory_order_relaxed); }
private:
    IOHIDDeviceRef device=nullptr;
    std::thread thread;
    std::mutex mutex;
    Remote state;
    std::atomic<bool> rumbleRequested{false};
    std::array<uint8_t,64> reportBuffer{};
    std::atomic<int64_t> lastReportNs{0};
    static void Input(void* context,IOReturn result,void*,IOHIDReportType,uint32_t reportId,uint8_t* data,CFIndex length) {
        if(result!=kIOReturnSuccess || !context || length<0 || length>63) return;
        auto* self=static_cast<Device*>(context);
        std::array<uint8_t,64> packet{};
        size_t size=0;
        if(length>0 && data[0]==uint8_t(reportId)) {
            std::copy(data,data+length,packet.begin());
            size=size_t(length);
        } else {
            packet[0]=uint8_t(reportId);
            if(length) std::copy(data,data+length,packet.begin()+1);
            size=size_t(length)+1;
        }
        {
            std::lock_guard lock(self->mutex);
            DecodeReport(packet.data(),size,self->state);
        }
        self->lastReportNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count(),std::memory_order_relaxed);
    }
    bool Send(const uint8_t* bytes,size_t length) {
        if(!device || !bytes || !length || length>64) return false;
        // IOKit requires the report ID both as the argument and as byte zero in the report buffer.
        return IOHIDDeviceSetReport(device,kIOHIDReportTypeOutput,bytes[0],
            bytes,CFIndex(length))==kIOReturnSuccess;
    }
    bool Send(std::initializer_list<uint8_t> bytes) {
        return Send(bytes.begin(),bytes.size());
    }
    bool InitIR() {
        const uint8_t start=8,mode=3;
        const uint8_t sensitivity1[]{2,0,0,0x71,1,0,0xaa,0,0x64},sensitivity2[]{0x63,3};
        if(!Send({0x13,4}) || !Send({0x1a,4})) return false;
        for(auto cmd:{WriteRegister(0xb00030,&start,1),WriteRegister(0xb00000,sensitivity1,9),
                      WriteRegister(0xb0001a,sensitivity2,2),WriteRegister(0xb00033,&mode,1),
                      WriteRegister(0xb00030,&start,1)}) {
            if(stop || !Send(cmd.data(),cmd.size())) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
        }
        return Send({0x12,4,0x33});
    }
    void Run() {
        IOHIDDeviceRegisterInputReportCallback(device,reportBuffer.data(),reportBuffer.size(),Input,this);
        IOHIDDeviceScheduleWithRunLoop(device,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
        if(!InitIR()) {
            alive=false;
            IOHIDDeviceUnscheduleFromRunLoop(device,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
            IOHIDDeviceClose(device,kIOHIDOptionsTypeNone);
            return;
        }
        auto last=Clock::now(),statusAt=last;
        lastReportNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            last.time_since_epoch()).count(),std::memory_order_relaxed);
        int led=-1;bool rumbling=false;
        while(!stop) {
            if(led!=player) {
                led=player;
                if(!Send({0x11,uint8_t(0x10<<led)})) break;
            }
            const bool wantRumble=rumbleRequested.load(std::memory_order_relaxed);
            if(wantRumble!=rumbling) {
                if(!Send({0x10,uint8_t(wantRumble?1:0)})) break;
                rumbling=wantRumble;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,true);
            const auto now=Clock::now();
            const auto reported=Clock::time_point(std::chrono::nanoseconds(
                lastReportNs.load(std::memory_order_relaxed)));
            if(reported>last) last=reported;
            if(now-last>std::chrono::seconds(5)) break;
            if(now-statusAt>std::chrono::seconds(10)) {
                if(!Send({0x15,0})) break;
                statusAt=now;
            }
            if(now-last>std::chrono::milliseconds(300)) {
                std::lock_guard lock(mutex);
                state.tracking=false;
                state.buttons=state.pressed=0;
            }
        }
        if(rumbling) Send({0x10,0});
        Send({0x11,0});
        IOHIDDeviceUnscheduleFromRunLoop(device,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
        IOHIDDeviceClose(device,kIOHIDOptionsTypeNone);
        alive=false;
    }
};

std::vector<IOHIDDeviceRef> HidDevices() {
    std::vector<IOHIDDeviceRef> out;
    IOHIDManagerRef manager=IOHIDManagerCreate(kCFAllocatorDefault,kIOHIDOptionsTypeNone);
    if(!manager) return out;
    const int vendor=0x057e;
    CFNumberRef v=CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&vendor);
    const void* keys[]{CFSTR(kIOHIDVendorIDKey)};
    const void* values[]{v};
    CFDictionaryRef matching=CFDictionaryCreate(kCFAllocatorDefault,keys,values,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager,matching);
    if(IOHIDManagerOpen(manager,kIOHIDOptionsTypeNone)==kIOReturnSuccess) {
        CFSetRef set=IOHIDManagerCopyDevices(manager);
        if(set) {
            const CFIndex count=CFSetGetCount(set);
            std::vector<const void*> items(static_cast<size_t>(count));
            CFSetGetValues(set,items.data());
            for(const void* value:items) {
                auto device=(IOHIDDeviceRef)value;
                if(IsRemote(device)) {CFRetain(device);out.push_back(device);}
            }
            CFRelease(set);
        }
        IOHIDManagerClose(manager,kIOHIDOptionsTypeNone);
    }
    if(matching) CFRelease(matching);
    if(v) CFRelease(v);
    CFRelease(manager);
    return out;
}

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
    void SetRumble(int p,bool enabled) override {
        if(p<0 || p>=kPlayers) return;
        std::shared_ptr<Device> device;
        {std::lock_guard lock(mutex);device=devices[p];}
        if(device) device->SetRumble(enabled);
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
                        std::any_of(disconnect.begin(),disconnect.end(),[](bool x){return x;});
                });
                if(stop) break;
                for(int i=0;i<kPlayers;++i) if(clear || disconnect[i]) {
                    if(disconnect[i] && !keys[i].empty()) blocked.insert(keys[i]);
                    retired[i]=std::move(devices[i]);keys[i].clear();disconnect[i]=false;
                }
                clear=false;
                if(requested) {deadline=Clock::now()+std::chrono::seconds(20);requested=false;}
                scan=enabled && (continuous_ || Clock::now()<deadline);
                scanning=scan;
                message=scan?"Looking for paired Nintendo Wii Remote HID devices":
                    enabled?"Pair remotes in Bluetooth settings, then scan again":"Wii Remote support is off";
            }
            retired={};
            if(!scan) continue;
            auto found=HidDevices();
            for(auto device:found) {
                const std::string id=DeviceId(device);
                int slot=-1;
                {
                    std::lock_guard lock(mutex);
                    if(!enabled || blocked.count(id)) {CFRelease(device);continue;}
                    for(int i=0;i<kPlayers;++i)
                        if(keys[i]==id && (!devices[i] || !devices[i]->alive)) {slot=i;break;}
                    if(slot<0 && std::find(keys.begin(),keys.end(),id)!=keys.end()) {CFRelease(device);continue;}
                    if(slot<0) for(int i=0;i<kPlayers;++i) if(keys[i].empty()) {slot=i;break;}
                }
                if(slot<0) {CFRelease(device);continue;}
                auto candidate=std::make_shared<Device>(device,slot);
                CFRelease(device);
                if(!candidate->Open()) continue;
                std::shared_ptr<Device> previous;
                {
                    std::lock_guard lock(mutex);
                    if(enabled && !blocked.count(id) && (keys[slot].empty() || keys[slot]==id)) {
                        previous=std::move(devices[slot]);devices[slot]=candidate;keys[slot]=id;
                    }
                }
            }
            std::unique_lock lock(mutex);
            cv.wait_for(lock,std::chrono::milliseconds(750),[&]{
                return stop || requested || clear ||
                    std::any_of(disconnect.begin(),disconnect.end(),[](bool x){return x;});
            });
        }
    }
};
} // namespace
std::unique_ptr<Backend> CreateBackend(){return std::make_unique<NativeBackend>();}
} // namespace hotd::game::wii
#endif
