// Sound and music from the PC port's files (audio.h). UNVERIFIED (teaser): the cue table is read as the PC's
// sound_cue (0x412000) reads it; the mixing (voice count, volume curve, no panning) is our own.
#include "game/audio.h"

#include "assets/archive.h"
#include "assets/pc_files.h"
#ifdef HOTD_HAVE_ARCADE_SOUND
#include "audio/sound_board.h"
#else
namespace hotd::audio { class SoundBoard {}; }   // never made: complete for Audio's unique_ptr (a build without it)
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef HOTD_HAVE_STB_VORBIS
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#endif
#ifdef HOTD_HAVE_DR_MP3
#include "dr_mp3.h"
#endif
#if defined(HOTD_HAVE_STB_VORBIS) || defined(HOTD_HAVE_DR_MP3)
#define HOTD_HAVE_MUSIC
#endif

namespace hotd::game {

namespace {

constexpr int kRate = 44100;
// the voices' start / stop ramp (our own: the PC's DirectSound buffers start and stop at once, which clicks when a sample
// begins or ends away from its centre 128, e.g. GAMESNDS sample 13 = the reload / bony-hand click): 3 ms, also over a
// one-shot's last 3 ms
constexpr float kFade = kRate * 0.003f;
constexpr float kSfxGain = 0.5f;     // the PC effects' level (x the sample's volume and the effects setting)
// the PC sound's master level (effects and music; 2026-10-19): about the arcade board's loudness on the same Chapter 1
// run (autofire, 6000 frames, docs/teaser_status/audio_fixes.md), the soft limiter above 0.7 rarely reached
constexpr float kPcLevel = 0.66f;
constexpr uint32_t kCueTable = 0x5CBDA8;   // THOTD.EXE: 12 bytes per cue (type, name pointer, value)
constexpr int kCues = 269;                 // cue 269 is the next table ("Main Menu")

const std::pair<uint32_t, int> kCueMap[] = {
#include "game/cue_map.inc"
};

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    return pcfs::readFile(path, out);   // a file of the PC folder, or inside the PC zip
}

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// spanish (docs/spanish.md): what the Spanish release re-recorded, from comparing its files with the US ones (every
// O_*.RBH there is 16 bytes longer; these differ in most samples, the others only by a few re-encoded ones):
// O_VOC02-88 (not VOC04, nor VOC91-94: screams), the lines below, and GAMESNDS.RBH's sample 53 (cue 115)
const char* const kDubbedLines[] = {"GOO1_2", "HEL1_2", "HELP", "HELP2", "KILLM2", "NONONO", "NO_2", "NO_F3", "OHNO",
                                    "OHNO_2", "TELEPH", "THAN_2"};
const int kDubbedSamples[] = {53};
// the reload voice: PC cue 161 (GAMESNDS.RBH sample 36, silent on the Spanish CD) = arcade code 0xA01404, sent while
// an empty gun's trigger is pulled in the first 2 s (port/hud_state.cpp; after them 0xA01405, cue 162 VOC06: kept)
constexpr int kReloadCue = 161;

// the BODY chunks of a PIFF / RBHF file ("PIFF" size "RBHF", then chunks: 4-byte tag, little-endian size, data)
std::vector<std::pair<size_t, size_t>> rbhBodies(const std::vector<uint8_t>& d)
{
    std::vector<std::pair<size_t, size_t>> out;
    if (d.size() < 12 || std::memcmp(d.data(), "PIFF", 4) != 0 || std::memcmp(d.data() + 8, "RBHF", 4) != 0)
        return out;
    for (size_t i = 12; i + 8 <= d.size();) {
        const size_t n = le32(&d[i + 4]);
        if (i + 8 + n > d.size())
            break;
        if (std::memcmp(&d[i], "BODY", 4) == 0)
            out.push_back({i + 8, n});
        i += 8 + n;
    }
    return out;
}

// a file in the PC folder, whatever the case of its name (the PC files come from a CD: upper case, but copies vary)
// (spanish: mapped false for another folder than the checked PC game's: the Spanish voices' copy)
std::string findPath(const std::string& root, const std::string& rel, bool mapped = true)
{
    // the launcher's files check: the checked file (also when renamed / moved, or a patch's .orig copy)
    if (std::string m = mapped ? pcFileMapped(rel) : std::string(); !m.empty())
        return m;
    // a zipped PC install: the zip reader matches names whatever their case
    if (pcfs::isZip(root) || pcfs::inZip(root + "/x"))
        return root + "/" + rel;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = root;
    size_t s = 0;
    while (s <= rel.size()) {
        const size_t e = std::min(rel.find_first_of("/\\", s), rel.size());
        const std::string part = rel.substr(s, e - s);
        fs::path next = p / part;
        if (!fs::exists(next, ec)) {
            bool found = false;
            for (const auto& entry : fs::directory_iterator(p, ec)) {
                const std::string n = entry.path().filename().string();
                if (n.size() == part.size() && SDL_strcasecmp(n.c_str(), part.c_str()) == 0) {
                    next = entry.path();
                    found = true;
                    break;
                }
            }
            if (!found)
                return (fs::path(root) / rel).string();
        }
        p = next;
        s = e + 1;
    }
    return p.string();
}

} // namespace

// the music track being played: an .ogg (stb_vorbis) or an .mp3 (dr_mp3), whichever the build has; the whole file is
// read into memory first (a loose file or one inside a zip alike) and decoded from there
struct Audio::Music {
#ifdef HOTD_HAVE_MUSIC
    std::vector<uint8_t> data;
#ifdef HOTD_HAVE_STB_VORBIS
    stb_vorbis* v = nullptr;
#endif
#ifdef HOTD_HAVE_DR_MP3
    drmp3 mp3{};
    bool mp3Open = false;
#endif
    // a .wav (16-bit PCM: the setup's rips of a CD image's audio tracks): its samples in data, read directly
    const int16_t* pcm = nullptr;
    uint64_t pcmFrames = 0, pcmPos = 0;
    int channels = 2, rate = kRate;
    bool loop = true;
    uint64_t played = 0, total = 0;   // frames since the start (or the last loop) / in the file (0: not known)
    int loops = 0;
    std::vector<float> buf;    // decoded frames (stereo), at the track's rate
    size_t bufPos = 0;
    double frac = 1;
    float cur[2] = {};
    ~Music()
    {
#ifdef HOTD_HAVE_STB_VORBIS
        if (v) stb_vorbis_close(v);
#endif
#ifdef HOTD_HAVE_DR_MP3
        if (mp3Open) drmp3_uninit(&mp3);
#endif
    }
    // up to 2048 frames, stereo, into tmp (4096 floats); 0 at the end
    int decode(float* tmp)
    {
        if (pcm) {
            const int ch = std::max(1, channels);
            int n = 0;
            for (; n < 2048 && pcmPos < pcmFrames; ++n, ++pcmPos) {
                const int16_t* f = pcm + pcmPos * uint64_t(ch);
                tmp[n * 2] = float(f[0]) / 32768.0f;
                tmp[n * 2 + 1] = float(f[ch > 1 ? 1 : 0]) / 32768.0f;
            }
            return n;
        }
#ifdef HOTD_HAVE_STB_VORBIS
        if (v)
            return stb_vorbis_get_samples_float_interleaved(v, 2, tmp, 4096);
#endif
#ifdef HOTD_HAVE_DR_MP3
        if (mp3Open) {
            const int ch = std::max(1, int(mp3.channels));
            std::vector<float> raw(size_t(2048) * ch);
            const int n = int(drmp3_read_pcm_frames_f32(&mp3, 2048, raw.data()));
            for (int i = 0; i < n; ++i) {
                tmp[i * 2] = raw[size_t(i) * ch];
                tmp[i * 2 + 1] = raw[size_t(i) * ch + (ch > 1 ? 1 : 0)];
            }
            return n;
        }
#endif
        return 0;
    }
    void rewind()
    {
        pcmPos = 0;
#ifdef HOTD_HAVE_STB_VORBIS
        if (v) stb_vorbis_seek_start(v);
#endif
#ifdef HOTD_HAVE_DR_MP3
        if (mp3Open) drmp3_seek_to_pcm_frame(&mp3, 0);
#endif
    }
    // the next frame (stereo) at the track's rate into cur; false at the end
    bool next()
    {
        if (bufPos >= buf.size() / 2) {
            float tmp[4096];
            int n = decode(tmp);
            if (n == 0 && loop) {
                rewind();
                n = decode(tmp);
                played = 0;
                ++loops;
            }
            if (n == 0)
                return false;
            buf.assign(tmp, tmp + n * 2);
            bufPos = 0;
        }
        cur[0] = buf[bufPos * 2];
        cur[1] = buf[bufPos * 2 + 1];
        ++bufPos;
        ++played;
        return true;
    }
    // the Jukebox: to frame f (the next() after it reads from there)
    void seek(uint64_t f)
    {
        pcmPos = std::min(f, pcmFrames);
#ifdef HOTD_HAVE_STB_VORBIS
        if (v) stb_vorbis_seek(v, unsigned(f));
#endif
#ifdef HOTD_HAVE_DR_MP3
        if (mp3Open) drmp3_seek_to_pcm_frame(&mp3, f);
#endif
        buf.clear();
        bufPos = 0;
        played = f;
    }
#endif
};

Audio::Audio() = default;
Audio::~Audio() { shutdown(); }

bool Audio::startBoard(const std::vector<uint8_t>& program, const std::vector<uint8_t>& samples, std::string& err)
{
#ifdef HOTD_HAVE_ARCADE_SOUND
    std::lock_guard<std::recursive_mutex> g(lock_);
    std::lock_guard<std::mutex> bl(boardLock_);
    if (!board_)
        board_ = std::make_unique<audio::SoundBoard>();
    const bool ok = board_->load(program, samples, err);
    {
        std::lock_guard<std::mutex> rl(ringLock_);
        ringReset();
        boardReady_ = ok;
    }
    if (!ok) {
        boardOn_ = false;
        return false;
    }
    boardOn_ = true;
    boardHeld_ = false;
    return true;
#else
    (void)program;
    (void)samples;
    err = "the arcade sound board is not built in (CMake HOTD_ARCADE_SOUND)";
    return false;
#endif
}

void Audio::resetBoard()
{
#ifdef HOTD_HAVE_ARCADE_SOUND
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (boardOn_ && board_) {
        std::lock_guard<std::mutex> bl(boardLock_);
        board_->reset();   // the 68000 and the SCSP start over: no voice plays
        std::lock_guard<std::mutex> rl(ringLock_);
        ringReset();       // (audioload: what was made ahead is from before the reset)
    }
    boardHeld_ = false;
#endif
}

uint64_t Audio::boardSamples() const
{
#ifdef HOTD_HAVE_ARCADE_SOUND
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!boardOn_ || !board_)
        return 0;
    std::lock_guard<std::mutex> bl(boardLock_);
    std::lock_guard<std::mutex> rl(ringLock_);
    return board_->samplesRun() - (ringOn_ ? ringFill_ : 0);   // (audioload: the samples played, not those made ahead)
#else
    return 0;
#endif
}

void Audio::boardSkip(int frames)
{
#ifdef HOTD_HAVE_ARCADE_SOUND
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!boardOn_ || !board_ || frames <= 0)
        return;
    std::lock_guard<std::mutex> bl(boardLock_);
    {   // audioload: the codes still queued go in first (the Jukebox's song, then its seek)
        std::lock_guard<std::mutex> rl(ringLock_);
        for (const auto& c : cmds_)
            board_->command(c.second);
        cmds_.clear();
    }
    std::vector<float> scratch(size_t(std::min(frames, 4410)) * 2);
    while (frames > 0) {
        const int n = std::min(frames, 4410);
        std::fill(scratch.begin(), scratch.end(), 0.0f);
        board_->render(scratch.data(), n, 0.0f);
        frames -= n;
    }
    std::lock_guard<std::mutex> rl(ringLock_);
    ringReset();
#else
    (void)frames;
#endif
}

void Audio::stopBoard()
{
    std::lock_guard<std::recursive_mutex> g(lock_);
    boardOn_ = false;
}

// audioload: the board's ring (audio.h). The board thread makes board frames up to consumed_ + lead_ (+ want_, the WAV
// test's request) and puts the game's codes in at their stamped frame; the output (SDL's callback, the fake device or
// the WAV test) takes them from the ring
namespace {
constexpr size_t kRingFrames = 16384;   // 371 ms: room for any lead
constexpr int kBoardChunk = 256;        // board frames per turn (5.8 ms; boardLock_ is held that long)
constexpr int kResumeFrames = 441;      // after an underrun / a reset: the output starts again with 10 ms in the ring
constexpr int kFadeInFrames = 64;       // after an underrun: 1.5 ms fade in
void audioThreadBoost();                // Windows: MMCSS "Pro Audio" (end of the file)
}

void Audio::ringReset()
{
    ringRead_ = ringFill_ = 0;
    emuPos_ = consumed_ = 0;
    cmds_.clear();
    primed_ = ringTest_;   // (the WAV test never waits)
    underrun_ = false;
    underrunFrames_ = 0;
    fadeIn_ = 0;
    lastOut_[0] = lastOut_[1] = 0;
    ringSpace_.notify_all();
    ringData_.notify_all();
}

bool Audio::boardCommand(uint32_t code)
{
    std::lock_guard<std::mutex> rl(ringLock_);
    if (!ringOn_)
        return false;
    // the output's position + the lead: never a frame the board thread has made already (it stops there)
    cmds_.emplace_back(consumed_ + uint64_t(lead_), code);
    ringSpace_.notify_all();
    return true;
}

void Audio::boardLoop()
{
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    audioThreadBoost();
    std::vector<float> buf(size_t(kBoardChunk) * 2);
    std::vector<uint32_t> due;
    while (threadsRun_) {
        if (const uint64_t f = underrunLog_.exchange(0))
            note("audio: underrun " + std::to_string((f * 1000 + kRate / 2) / kRate) + " ms (" +
                 std::to_string(underruns_.load()) + " so far)");
        {
            std::unique_lock<std::mutex> rl(ringLock_);
            ringSpace_.wait(rl, [&] {
                return !threadsRun_ || underrunLog_.load() != 0 ||
                       (boardReady_ && emuPos_ < consumed_ + uint64_t(lead_ + want_) && ringFill_ < ringCap_);
            });
        }
        if (!threadsRun_)
            break;
        std::lock_guard<std::mutex> bl(boardLock_);
        std::unique_lock<std::mutex> rl(ringLock_);
        const uint64_t limit = consumed_ + uint64_t(lead_ + want_);
        if (!boardReady_ || !board_ || emuPos_ >= limit || ringFill_ >= ringCap_)
            continue;
        due.clear();
        while (!cmds_.empty() && cmds_.front().first <= emuPos_) {
            due.push_back(cmds_.front().second);
            cmds_.pop_front();
        }
        uint64_t end = std::min<uint64_t>({limit, emuPos_ + kBoardChunk, emuPos_ + (ringCap_ - ringFill_)});
        if (!cmds_.empty())
            end = std::min(end, cmds_.front().first);   // the next code goes in at its own frame
        const int n = int(end - emuPos_);
        rl.unlock();
#ifdef HOTD_HAVE_ARCADE_SOUND
        // The board thread is idle when HOTD_ARCADE_SOUND is off (boardReady_ can never become true).
        // The no-board stub intentionally has no command/render methods, so this guard is also required
        // at compile time: optional arcade audio must not break builds that disable the arcade sound board.
        for (uint32_t c : due)
            board_->command(c);
        std::fill(buf.begin(), buf.begin() + size_t(n) * 2, 0.0f);
        board_->render(buf.data(), n, 1.0f);   // (as mix() did: into zeros, gain 1)
#endif
        rl.lock();
        size_t w = (ringRead_ + ringFill_) % ringCap_;
        for (int i = 0; i < n; ++i, w = (w + 1) % ringCap_) {
            ring_[w * 2] = buf[size_t(i) * 2];
            ring_[w * 2 + 1] = buf[size_t(i) * 2 + 1];
        }
        ringFill_ += size_t(n);
        emuPos_ += uint64_t(n);
        ringData_.notify_all();
    }
}

void Audio::takeBoard(float* out, int frames)
{
    std::unique_lock<std::mutex> rl(ringLock_);
    if (ringTest_) {   // the WAV test: exactly these frames, made now (the same board frames as without the ring)
        want_ = frames;
        ringSpace_.notify_all();
        ringData_.wait(rl, [&] { return ringFill_ >= size_t(frames) || !threadsRun_ || !boardReady_; });
        want_ = 0;
    }
    if (!primed_ && ringFill_ >= size_t(std::min(kResumeFrames, std::max(lead_, 1)))) {
        primed_ = true;
        if (underrun_) {   // the end of an underrun: logged by the board thread, faded in
            underrunLog_ += underrunFrames_;
            underrun_ = false;
            fadeIn_ = kFadeInFrames;
            ringSpace_.notify_all();
        }
    }
    int i = 0;
    if (primed_) {
        const int n = int(std::min(size_t(frames), ringFill_));
        for (; i < n; ++i) {
            const float* s = &ring_[ringRead_ * 2];
            float g = 1.0f;
            if (fadeIn_ > 0)
                g = 1.0f - float(fadeIn_--) / kFadeInFrames;
            out[i * 2] = lastOut_[0] = s[0] * g;
            out[i * 2 + 1] = lastOut_[1] = s[1] * g;
            ringRead_ = (ringRead_ + 1) % ringCap_;
        }
        ringFill_ -= size_t(n);
        consumed_ += uint64_t(n);
        if (n < frames) {   // underrun: the board thread is late
            primed_ = false;
            underrun_ = true;
            underrunFrames_ = 0;
            ++underruns_;
        }
    }
    if (i < frames) {   // no board frames: the last sample fades out (no click), then silence
        const int missing = frames - i;
        for (; i < frames; ++i) {
            lastOut_[0] *= 0.9f;
            lastOut_[1] *= 0.9f;
            out[i * 2] = lastOut_[0];
            out[i * 2 + 1] = lastOut_[1];
        }
        if (underrun_) {
            underrunFrames_ += uint64_t(missing);
            underrunTotal_ += uint64_t(missing);
        }
    }
    ringSpace_.notify_all();
}

// HOTD_AUDIO_FAKE_DEVICE (tests): no device, a thread that pulls the mix as WASAPI would (10 ms periods, two of them
// buffered, time-critical like SDL's device thread) and counts the periods that came too late (an audible gap)
void Audio::fakeDeviceLoop()
{
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
    audioThreadBoost();
    constexpr int kPeriod = 441;
    constexpr uint64_t kPeriodNs = 10'000'000;
    std::vector<float> buf(size_t(kPeriod) * 2);
    const uint64_t start = SDL_GetTicksNS();
    uint64_t t0 = start, k = 0, late = 0, lateNs = 0, worst = 0, slowest = 0;
    while (threadsRun_) {
        const uint64_t due = t0 + k * kPeriodNs;
        uint64_t now = SDL_GetTicksNS();
        if (now < due)
            SDL_DelayPrecise(due - now);
        const uint64_t began = SDL_GetTicksNS();
        mix(buf.data(), kPeriod);
        now = SDL_GetTicksNS();
        slowest = std::max(slowest, now - began);
        const uint64_t deadline = t0 + (k + 1) * kPeriodNs;   // period k - 1 has played
        if (now > deadline) {
            ++late;
            lateNs += now - deadline;
            worst = std::max(worst, now - deadline);
            t0 += now - deadline;   // the device goes on from the gap
        }
        ++k;
    }
    char line[320];
    std::snprintf(line, sizeof line, "audio: fake device %.1f s, %llu periods: %llu late (%.1f ms in all, worst %.2f ms), "
                  "slowest pull %.2f ms; board ring underruns %llu (%.1f ms)", (SDL_GetTicksNS() - start) / 1e9,
                  (unsigned long long)k, (unsigned long long)late, lateNs / 1e6, worst / 1e6, slowest / 1e6,
                  (unsigned long long)underruns_.load(), underrunTotal_.load() * 1000.0 / kRate);
    note(line);
}

bool Audio::musicAvailable() const
{
#ifdef HOTD_HAVE_MUSIC
    return true;
#else
    return false;
#endif
}

bool Audio::init(const std::string& pcRoot, bool device, std::string& err)
{
    root_ = pcRoot;
    tracksIndexed_ = false;
    for (const auto& [code, cue] : kCueMap)
        cueMap_[code] = cue;
    if (!loadCueTable(findPath(pcRoot, "THOTD.EXE"), err) || !loadBank(findPath(pcRoot, "SOUND/GAMESNDS.RBH"), err))
        return false;
    // spanish: GAMESNDS.RBH's dubbed sample from the Spanish copy (its sample table is the US one: the same place);
    // its other change, the reload voice (sample 36) made silent, is not taken (the reload voice stays)
    if (!voiceDir_.empty() && voiceDir_ != pcRoot) {
        std::vector<uint8_t> d;
        const auto bodies = readFile(findPath(voiceDir_, "SOUND/GAMESNDS.RBH", false), d) ? rbhBodies(d) : decltype(rbhBodies(d)){};
        if (bodies.size() >= 2 && bodies[0].second == samples_.size() * sizeof(SampleInfo)) {
            for (int k : kDubbedSamples) {
                const SampleInfo& s = samples_[size_t(k)];
                SampleInfo es;
                std::memcpy(&es, &d[bodies[0].first + size_t(k) * sizeof(SampleInfo)], sizeof es);
                if (s.source == 1 && std::memcmp(&s, &es, sizeof es) == 0 && s.offset + s.length <= bank_.size() &&
                    8 + s.offset + s.length <= bodies[1].second)
                    std::memcpy(bank_.data() + s.offset, &d[bodies[1].first + 8 + s.offset], s.length);
            }
        } else {
            note("spanish voices: cannot read " + voiceDir_ + "'s SOUND/GAMESNDS.RBH");
        }
    }
    // audioload: the board thread (with a device; tests: HOTD_AUDIO_FAKE_DEVICE = a paced puller instead of the
    // device, HOTD_AUDIO_RING_TEST = --audio-wav through the ring, no lead: the same file as without it)
    const bool fake = !device && std::getenv("HOTD_AUDIO_FAKE_DEVICE") != nullptr;
    const bool ringTest = !device && !fake && std::getenv("HOTD_AUDIO_RING_TEST") != nullptr;
    auto startThreads = [&]() {
        const char* ms = std::getenv("HOTD_AUDIO_AHEAD_MS");
        // (v1.4.0: the ring on Android only, user 2026-11-11 "the standalone audio crackling" with the deeper AAudio
        // buffer already on: the board runs 60 ms ahead there; the PC keeps the board in the callback, as tested for
        // 1.4.0, unless HOTD_AUDIO_AHEAD_MS asks: 46 ms was the PC's test value)
#if defined(__ANDROID__)
        constexpr int kDefaultLeadMs = 60;
#else
        constexpr int kDefaultLeadMs = 0;
#endif
        const int lead = ringTest ? 0 : std::clamp(ms ? std::atoi(ms) : kDefaultLeadMs, 0, 180) * kRate / 1000;
        {
            std::lock_guard<std::mutex> rl(ringLock_);
            ringTest_ = ringTest;
            ringOn_ = ringTest || lead > 0;   // (HOTD_AUDIO_AHEAD_MS=0: the board in the callback, as before)
            lead_ = lead;
            want_ = 0;
            ringCap_ = kRingFrames;
            ring_.assign(kRingFrames * 2, 0.0f);
            ringReset();
        }
        threadsRun_ = true;
        if (ringOn_)
            boardThread_ = std::thread(&Audio::boardLoop, this);
        if (fake)
            fakeThread_ = std::thread(&Audio::fakeDeviceLoop, this);
        if (fake)   // (=<seconds>: shutdown waits for them; "1": no wait to speak of)
            fakeUntil_ = SDL_GetTicksNS() + uint64_t(std::max(0, std::atoi(std::getenv("HOTD_AUDIO_FAKE_DEVICE")))) * 1000000000ull;
        if (ringOn_ || fake)
            note("audio: the arcade board " + std::string(ringOn_ ? "runs ahead on its own thread (" +
                 std::to_string(lead * 1000 / kRate) + " ms)" : "runs in the output's callback") +
                 (fake ? ", output: the fake device" : ringTest ? ", output: the WAV" : ""));
    };
    if (!device) {
        if (fake || ringTest)
            startThreads();
        return true;
    }
#if defined(__ANDROID__)
    // questfb (user 2026-11-10 on the Quest 3: "the audio had crackling"): AAudio's low-latency mode takes tiny bursts
    // that the game's 15-17 ms CPU spikes in Chapter 1 (the Quest's log) leave empty; its default mode keeps a deeper
    // buffer (a few ms more latency; the gun's sound is still well under a frame pair late)
    SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, "0");
    // questaudio4 (user 2026-11-11 on the Quest 3: the crackling gone with OpenSL ES, after the deeper buffer, the
    // board's ring, the 48 kHz device and more AAudio buffers all left it): SDL's OpenSL ES output, not AAudio (an
    // env SDL_AUDIO_DRIVER still wins: SDL_HINT_DEFAULT)
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "openslES", SDL_HINT_DEFAULT);
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        err = std::string("audio: ") + SDL_GetError();
        return false;
    }
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    // questaudio (user 2026-11-11, the Quest 3 standalone: "constant audio crackling", unchanged by the deeper AAudio
    // buffer and by the board's ring): SDL opens the device at the stream's rate, so Android had to resample our
    // 44.1 kHz to the headset's native 48 kHz itself (Meta: "the native sample rate is 48000 Hz"; 44.1 kHz "forces the
    // platform to resample at runtime"). On Android the device is opened at 48 kHz and SDL's own resampler makes it
    // from our 44.1 kHz stream; elsewhere as before (the device at 44.1 kHz). HOTD_AUDIO_DEVICE_RATE=<Hz> asks for a
    // rate anywhere (tests), 0 = the old way
#if defined(__ANDROID__)
    int deviceRate = 48000;
#else
    int deviceRate = 0;
#endif
    if (const char* r = std::getenv("HOTD_AUDIO_DEVICE_RATE"))
        deviceRate = std::atoi(r);
    if (deviceRate > 0) {
        const SDL_AudioSpec want{SDL_AUDIO_F32, 2, deviceRate};
        device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
        if (device_) {
            SDL_PauseAudioDevice(device_);   // (as SDL_OpenAudioDeviceStream: starts with SDL_ResumeAudioStreamDevice)
            stream_ = SDL_CreateAudioStream(&spec, &want);   // (bound: its output becomes the device's real format)
            if (stream_ && !(SDL_SetAudioStreamGetCallback(stream_, &Audio::callback, this) && SDL_BindAudioStream(device_, stream_))) {
                SDL_DestroyAudioStream(stream_);
                stream_ = nullptr;
            }
            if (!stream_) {
                SDL_CloseAudioDevice(device_);
                device_ = 0;
            }
        }
    } else {
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Audio::callback, this);
    }
    if (!stream_) {
        err = std::string("audio device: ") + SDL_GetError();
        return false;
    }
    {   // the log: what the device really is (its buffer = one pull of the callback)
        SDL_AudioSpec got{};
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &got, &frames) && got.freq > 0) {
            deviceBufNs_ = uint64_t(frames) * 1000000000ull / uint64_t(got.freq);
            note("audio: device " + std::to_string(got.freq) + " Hz, " + SDL_GetAudioFormatName(got.format) + ", " +
                 std::to_string(got.channels) + " channels, " + std::to_string(frames) + " frames a buffer (" +
                 std::to_string(deviceBufNs_ / 1000000) + " ms), driver " + (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?") +
                 "; our stream " + std::to_string(kRate) + " Hz");
        }
    }
    startThreads();
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

bool Audio::openWav(const std::string& path, std::string& err)
{
    closeWav();
    wav_ = std::fopen(path.c_str(), "wb");
    if (!wav_) {
        err = "cannot write " + path;
        return false;
    }
    uint8_t header[44] = {};
    std::fwrite(header, 1, sizeof header, wav_);   // written again by closeWav with the sizes
    wavFrames_ = 0;
    wavFrac_ = 0;
    return true;
}

void Audio::renderFrame()
{
    if (!wav_)
        return;
    // one game frame of output: 1 / tickHz_ s (60 Hz: 735 frames at 44.1 kHz; arcade 57.524 Hz: 766.6)
    wavFrac_ += kRate / tickHz_;
    const int frames = int(wavFrac_);
    wavFrac_ -= frames;
    if (mixBuf_.size() < size_t(frames) * 2)
        mixBuf_.resize(size_t(frames) * 2);
    mix(mixBuf_.data(), frames);
    std::vector<int16_t> pcm(size_t(frames) * 2);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = int16_t(std::lround(std::clamp(mixBuf_[i], -1.0f, 1.0f) * 32767.0f));
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), wav_);
    wavFrames_ += uint64_t(frames);
}

void Audio::closeWav()
{
    if (!wav_)
        return;
    auto put32 = [](uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); };
    const uint32_t data = uint32_t(wavFrames_ * 4);
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
    put32(h + 4, 36 + data);
    put32(h + 24, kRate);
    put32(h + 28, kRate * 4);
    h[32] = 4;
    h[34] = 16;
    std::memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    std::fseek(wav_, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, wav_);
    std::fclose(wav_);
    wav_ = nullptr;
}

void Audio::shutdown()
{
    if (fakeThread_.joinable() && SDL_GetTicksNS() < fakeUntil_) {   // (the load test: the device for its whole time)
        note("audio: the fake device runs on to its " + std::string(std::getenv("HOTD_AUDIO_FAKE_DEVICE")) + " s");
        SDL_DelayNS(fakeUntil_ - SDL_GetTicksNS());
    }
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (device_) {   // (questaudio: the device opened by hand)
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
    if (const uint64_t n = lateTotal_.exchange(0))
        note("audio: the output's pulls came late " + std::to_string(n) + " times in all");
    {   // audioload: the board thread / the fake device
        std::lock_guard<std::mutex> rl(ringLock_);
        threadsRun_ = false;
        ringSpace_.notify_all();
        ringData_.notify_all();
    }
    if (fakeThread_.joinable())
        fakeThread_.join();
    if (boardThread_.joinable())
        boardThread_.join();
    {
        std::lock_guard<std::mutex> rl(ringLock_);
        ringOn_ = ringTest_ = false;
        if (const uint64_t f = underrunLog_.exchange(0))
            note("audio: underrun " + std::to_string((f * 1000 + kRate / 2) / kRate) + " ms");
        if (underruns_.load())
            note("audio: " + std::to_string(underruns_.load()) + " underruns, " +
                 std::to_string(underrunTotal_.load() * 1000 / kRate) + " ms in all");
    }
    closeWav();
    music_.reset();
}

// the cue table in THOTD.EXE's data (the file offset from the PE section headers)
bool Audio::loadCueTable(const std::string& exePath, std::string& err)
{
    std::vector<uint8_t> e;
    if (!readFile(exePath, e) || e.size() < 0x400) {
        err = "PC files: cannot read " + exePath;
        return false;
    }
    const uint32_t pe = le32(&e[0x3C]);
    if (pe + 0x18 > e.size() || std::memcmp(&e[pe], "PE\0\0", 4) != 0) {
        err = exePath + ": not a PE file";
        return false;
    }
    const uint32_t sections = e[pe + 6] | e[pe + 7] << 8, optSize = e[pe + 0x14] | e[pe + 0x15] << 8;
    const uint32_t base = le32(&e[pe + 0x18 + 28]);
    auto fileOf = [&](uint32_t va) -> int64_t {
        for (uint32_t s = 0; s < sections; ++s) {
            const uint8_t* h = &e[pe + 0x18 + optSize + s * 40];
            const uint32_t vsize = le32(h + 8), vaddr = le32(h + 12) + base, rsize = le32(h + 16), raw = le32(h + 20);
            if (va >= vaddr && va < vaddr + std::max(vsize, rsize) && va - vaddr < rsize)
                return int64_t(raw) + (va - vaddr);
        }
        return -1;
    };
    const int64_t t = fileOf(kCueTable);
    if (t < 0 || size_t(t) + kCues * 12 > e.size()) {
        err = exePath + ": no cue table at 0x5CBDA8 (not the 1998 THOTD.EXE?)";
        return false;
    }
    cues_.resize(kCues);
    for (int i = 0; i < kCues; ++i) {
        const uint8_t* c = &e[size_t(t) + size_t(i) * 12];
        cues_[i].type = int32_t(le32(c));
        cues_[i].value = int32_t(le32(c + 8));
        const int64_t n = cues_[i].type == 4 ? fileOf(le32(c + 4)) : -1;
        if (n >= 0)
            cues_[i].name = reinterpret_cast<const char*>(&e[size_t(n)]);
    }
    return true;
}

// GAMESNDS.RBH: BODY 0 = the sample entries (40 bytes each), BODY 1 = {u32 load offset 0, u16 1, u16 1} (the O_ files'
// 8-byte header) + the bank's 8-bit data (and, at its end, the area the PC loads O_ files into). An entry's offset counts
// from after that header: THOTD's buffer setup (0x57BFC0) copies from BODY + 8 + offset - the load offset. Issue #3:
// read from the header on, every bank sample started 8 bytes early (its last 8 bytes cut) with those bytes, mostly 0x00
// (= -1.0), in front: a click at each start and, on the loops played from 0 (the chainsaw 57, the electric 60 / 61 / 63),
// at every round
bool Audio::loadBank(const std::string& path, std::string& err)
{
    std::vector<uint8_t> d;
    if (!readFile(path, d)) {
        err = "PC files: cannot read " + path;
        return false;
    }
    const auto bodies = rbhBodies(d);
    if (bodies.size() < 2 || bodies[0].second % sizeof(SampleInfo) != 0 || bodies[1].second < 8) {
        err = path + ": not the sound bank layout";
        return false;
    }
    samples_.resize(bodies[0].second / sizeof(SampleInfo));
    std::memcpy(samples_.data(), &d[bodies[0].first], bodies[0].second);
    // relfix2 (user 2026-11-03: the SMG silent in Survival): playExtra's sounds are samples appended past the bank's;
    // every game start loads the bank again (init), which dropped them while extras_ kept their old numbers (the
    // second game on: play() refused them, nothing sounded). Made again from the new bank on their next use
    std::lock_guard<std::recursive_mutex> g(lock_);
    auto drop = [&](Voice& v) {   // (a voice of a dropped sound: its data goes)
        for (const auto& [name, e] : extras_)
            if (v.data && !e.data.empty() && v.data == e.data.data())
                v = Voice{};
    };
    for (Voice& v : voices_) drop(v);
    for (Voice& v : tails_) drop(v);
    extras_.clear();
    bank_.assign(d.begin() + std::ptrdiff_t(bodies[1].first + 8), d.begin() + std::ptrdiff_t(bodies[1].first + bodies[1].second));
    return true;
}

bool Audio::isDubbedFile(const std::string& name)
{
    std::string n = name;
    for (char& ch : n)
        ch = char(std::toupper(uint8_t(ch)));
    if (n.size() == 5 && n.compare(0, 3, "VOC") == 0 && std::isdigit(uint8_t(n[3])) && std::isdigit(uint8_t(n[4]))) {
        const int k = std::atoi(n.c_str() + 3);
        return k >= 2 && k <= 88 && k != 4;
    }
    for (const char* l : kDubbedLines)
        if (n == l)
            return true;
    return false;
}

bool Audio::isVoiceCue(int n) const
{
    if (n < 0 || n >= int(cues_.size()))
        return false;
    const Cue& c = cues_[size_t(n)];
    if (c.type == 4)
        return isDubbedFile(c.name);
    if (c.type == 1)
        for (int k : kDubbedSamples)
            if ((c.value & 0xFFFF) == k)
                return true;
    return false;
}

void Audio::setVoiceDir(const std::string& dir)
{
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (dir == voiceDir_)
        return;
    voiceDir_ = dir;
    // the loaded lines go (a voice playing one ends first: its data is freed)
    for (Voice& v : voices_)
        for (const auto& [name, data] : oneShots_)
            if (v.data && !data.empty() && v.data == data.data())
                v = Voice{};
    for (Voice& v : tails_)
        for (const auto& [name, data] : oneShots_)
            if (v.data && !data.empty() && v.data == data.data())
                v = Voice{};
    oneShots_.clear();
}

// SOUND\O_<name>.RBH: one BODY = {u32 load offset, u16 source 2, u16 file number} + the 8-bit data
const std::vector<uint8_t>* Audio::loadOneShot(const std::string& name)
{
    auto it = oneShots_.find(name);
    if (it != oneShots_.end())
        return it->second.empty() ? nullptr : &it->second;
    std::vector<uint8_t> d, data;
    // spanish: a dubbed line from the Spanish copy (setVoiceDir)
    const bool es = !voiceDir_.empty() && voiceDir_ != root_ && isDubbedFile(name);
    const std::string path = es ? findPath(voiceDir_, "SOUND/O_" + name + ".RBH", false) : findPath(root_, "SOUND/O_" + name + ".RBH");
    if (readFile(path, d)) {
        const auto bodies = rbhBodies(d);
        if (!bodies.empty() && bodies[0].second > 8)
            data.assign(d.begin() + std::ptrdiff_t(bodies[0].first + 8), d.begin() + std::ptrdiff_t(bodies[0].first + bodies[0].second));
    }
    if (data.empty())
        note("sound: cannot read " + path);
    else if (es && traceCues)
        std::printf("[sound] O_%s: the Spanish line (%s)\n", name.c_str(), path.c_str());
    auto& slot = oneShots_[name] = std::move(data);
    return slot.empty() ? nullptr : &slot;
}

int Audio::pcCueOf(uint32_t arcadeCode) const
{
    auto it = cueMap_.find(arcadeCode);
    return it == cueMap_.end() ? -1 : it->second;
}

bool Audio::isMusicCode(uint32_t arcadeCode) const
{
    const int n = pcCueOf(arcadeCode);
    if (n < 0 || n >= int(cues_.size()))
        return false;
    const Cue& c = cues_[size_t(n)];
    return c.type == 3 || (c.type == 5 && c.value == 1);
}

bool Audio::isMusicStart(uint32_t arcadeCode) const
{
    const int n = pcCueOf(arcadeCode);
    return n >= 0 && n < int(cues_.size()) && cues_[size_t(n)].type == 3 && cues_[size_t(n)].value != -1;
}

namespace {
// a RIFF WAV file's PCM (format 1, 8 / 16-bit, 1 / 2 channels) as 8-bit unsigned mono; false: not one
bool readWav8(const std::string& path, std::vector<uint8_t>& out, uint32_t& rate)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24); };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return false;
    uint32_t fmt = 0, channels = 0, bits = 0;
    rate = 0;
    for (size_t o = 12; o + 8 <= d.size();) {
        const uint32_t len = u32(o + 4);
        const size_t body = o + 8;
        if (body + len > d.size())
            break;
        if (std::memcmp(d.data() + o, "fmt ", 4) == 0 && len >= 16) {
            fmt = u16(body), channels = u16(body + 2), rate = u32(body + 4), bits = u16(body + 14);
        } else if (std::memcmp(d.data() + o, "data", 4) == 0 && fmt == 1 && (channels == 1 || channels == 2) &&
                   (bits == 8 || bits == 16) && rate > 0) {
            const size_t frame = channels * (bits / 8), n = len / frame;
            out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                int sum = 0;
                for (uint32_t c = 0; c < channels; ++c) {
                    const size_t at = body + i * frame + c * (bits / 8);
                    sum += bits == 8 ? int(d[at]) - 128 : int(int16_t(u16(at))) / 256;
                }
                out[i] = uint8_t(std::clamp(sum / int(channels), -128, 127) + 128);
            }
            return n > 0;
        }
        o = body + len + (len & 1);
    }
    return false;
}

// totd8 (user 2026-11-05: "make it sound arcadey or PC depending on the audio. Compress it like how the game would"):
// a WAV (PCM 8 / 16 / 24-bit, IEEE float 32, any channels) as float mono -1..1 at its own rate; false: not one
bool readWavF(const std::string& path, std::vector<float>& out, uint32_t& rate)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24); };
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return false;
    uint32_t fmt = 0, channels = 0, bits = 0;
    rate = 0;
    for (size_t o = 12; o + 8 <= d.size();) {
        const uint32_t len = u32(o + 4);
        const size_t body = o + 8;
        if (body + len > d.size())
            break;
        if (std::memcmp(d.data() + o, "fmt ", 4) == 0 && len >= 16) {
            fmt = u16(body), channels = u16(body + 2), rate = u32(body + 4), bits = u16(body + 14);
            if (fmt == 0xFFFE && len >= 26)   // (WAVE_FORMAT_EXTENSIBLE: its sub-format's first word)
                fmt = u16(body + 24);
        } else if (std::memcmp(d.data() + o, "data", 4) == 0 && channels >= 1 && rate > 0 &&
                   ((fmt == 1 && (bits == 8 || bits == 16 || bits == 24)) || (fmt == 3 && bits == 32))) {
            const size_t bytes = bits / 8, frame = channels * bytes, n = len / frame;
            out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                float sum = 0;
                for (uint32_t c = 0; c < channels; ++c) {
                    const size_t at = body + i * frame + c * bytes;
                    if (fmt == 3) {
                        float v;
                        std::memcpy(&v, &d[at], 4);
                        sum += v;
                    } else if (bits == 8)
                        sum += (float(d[at]) - 128.0f) / 128.0f;
                    else if (bits == 16)
                        sum += float(int16_t(u16(at))) / 32768.0f;
                    else
                        sum += float(int32_t((u16(at) << 8 | uint32_t(d[at + 2]) << 24)) >> 8) / 8388608.0f;
                }
                out[i] = sum / float(channels);
            }
            return n > 0;
        }
        o = body + len + (len & 1);
    }
    return false;
}

// totd8: `in` (float mono at `from` Hz) as the game's own samples are on the sound in use: resampled to `to` Hz (a
// windowed-sinc low-pass at 0.45 of the new rate, as a sample editor of the time converted; no dither), the peak
// brought to `peak` of full scale, then 8-bit unsigned (0x80 silence) rounded to the nearest step. The mixer plays it
// as any 8-bit sample: linear interpolation between the steps (the SCSP's too)
std::vector<uint8_t> degrade8(const std::vector<float>& in, uint32_t from, uint32_t to, float peak)
{
    constexpr double kPi = 3.14159265358979323846;
    std::vector<float> r;
    if (from == to || in.empty())
        r = in;
    else {
        const double ratio = double(from) / double(to), cut = 0.45 * std::min(1.0, 1.0 / ratio);   // (in input cycles)
        const int half = int(std::ceil(16.0 * std::max(1.0, ratio)));
        const size_t n = size_t(double(in.size()) / ratio);
        r.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double c = double(i) * ratio;
            const int c0 = int(std::floor(c));
            double acc = 0, wsum = 0;
            for (int k = c0 - half + 1; k <= c0 + half; ++k) {
                if (k < 0 || k >= int(in.size())) continue;
                const double x = double(k) - c;
                const double sinc = std::abs(x) < 1e-9 ? 2.0 * cut : std::sin(2.0 * kPi * cut * x) / (kPi * x);
                const double w = 0.42 + 0.5 * std::cos(kPi * x / half) + 0.08 * std::cos(2.0 * kPi * x / half);   // Blackman
                acc += double(in[size_t(k)]) * sinc * w;
                wsum += sinc * w;
            }
            r[i] = float(wsum != 0 ? acc / wsum : 0.0);   // (DC gain 1)
        }
    }
    float top = 1e-6f;
    for (float v : r)
        top = std::max(top, std::abs(v));
    const float g = peak > 0.0f ? std::clamp(peak, 0.0f, 1.0f) / top : 1.0f;
    std::vector<uint8_t> out(r.size());
    for (size_t i = 0; i < r.size(); ++i)
        out[i] = uint8_t(std::clamp(int(std::lround(128.0f + r[i] * g * 127.0f)), 0, 255));
    return out;
}
} // namespace

bool Audio::playExtra(const std::string& dir, const std::string& name, int cue, float pitch, float clip, float volume, float normalize)
{
    const float asked = volume;
    // totd8 (user 2026-11-05: "make it sound arcadey or PC depending on the audio. Compress it like how the game
    // would"): a typing sound's own WAV (normalize > 0) is cut down to the samples of the sound playing it: the arcade
    // board's effects (8-bit, 22050 Hz: the gun shot's slot, docs/typing.md) or the 1998 PC's (8-bit, 11025 Hz,
    // mono: its sound set-up). One copy per board, made on first use; the file on disk is left as it is
    static const bool raw = std::getenv("HOTD_TOTD_RAWWAV") != nullptr;   // (testing: the WAV as before, 8-bit at its rate)
    const bool degrade = normalize > 0.0f && !raw;
    const std::string key = degrade ? name + (boardOn_ ? "#arcade" : "#pc") : name;
    auto it = extras_.find(key);
    if (it == extras_.end()) {
        Extra e;
        uint32_t rate = 0;
        SampleInfo s{};
        s.bits = 8, s.unk = -1, s.source = 3;
        std::vector<float> wav;
        if (degrade && readWavF(dir + "/" + name + ".wav", wav, rate)) {
            const uint32_t to = boardOn_ ? 22050u : 11025u;
            e.data = degrade8(wav, rate, to, normalize);
            e.file = true;
            s.rate = to;
            note("typing: " + dir + "/" + name + ".wav as " + (boardOn_ ? "the arcade board's" : "the PC's") + " 8-bit " + std::to_string(to) + " Hz");
        } else if (readWav8(dir + "/" + name + ".wav", e.data, rate)) {
            e.file = true;
            s.rate = rate;
            note("survival: " + dir + "/" + name + ".wav");
        } else if (cue >= 0 && cue < int(cues_.size()) && cues_[size_t(cue)].type == 1) {
            const int k = cues_[size_t(cue)].value & 0xFFFF;
            if (k >= 0 && k < int(samples_.size()) && samples_[size_t(k)].source == 1 &&
                samples_[size_t(k)].offset + samples_[size_t(k)].length <= bank_.size()) {
                const SampleInfo& from = samples_[size_t(k)];
                const uint32_t n = std::max<uint32_t>(1, uint32_t(double(from.length) * std::clamp(clip, 0.05f, 1.0f)));
                e.data.assign(bank_.begin() + from.offset, bank_.begin() + from.offset + n);
                s.rate = uint32_t(double(from.rate ? from.rate : 11025) * std::clamp(pitch, 0.25f, 4.0f));
                volume *= from.volume / 255.0f;
            }
        } else if (cue >= 0 && cue < int(cues_.size()) && cues_[size_t(cue)].type == 4) {
            // totd5: a file cue's one-shot (O_*.RBH, e.g. the life chime O_LIFEUP) as the stand-in, at its slot's rate
            const int k = cues_[size_t(cue)].value & 0xFFFF;
            if (const auto* d = loadOneShot(cues_[size_t(cue)].name); d && !d->empty() && k >= 0 && k < int(samples_.size())) {
                const uint32_t n = std::max<uint32_t>(1, uint32_t(double(d->size()) * std::clamp(clip, 0.05f, 1.0f)));
                e.data.assign(d->begin(), d->begin() + n);
                const uint32_t r = samples_[size_t(k)].rate ? samples_[size_t(k)].rate : 22050;
                s.rate = uint32_t(double(r) * std::clamp(pitch, 0.25f, 4.0f));
            }
        }
        // totd5 (user 2026-11-04: "I could barely hear the typing sounds"): normalize > 0: the sound's peak brought to
        // `normalize` of full scale (8-bit around 128) and the asked volume as it is (not the bank's own), so a quiet bank
        // sample is as clear as the next
        if (normalize > 0.0f && !e.data.empty()) {
            int peak = 1;
            for (uint8_t b : e.data)
                peak = std::max(peak, std::abs(int(b) - 128));
            const float g = std::clamp(normalize, 0.0f, 1.0f) * 127.0f / float(peak);
            for (uint8_t& b : e.data)
                b = uint8_t(std::clamp(int(std::lround(128.0f + (float(b) - 128.0f) * g)), 0, 255));
            volume = asked;
        }
        if (!e.data.empty()) {
            s.length = uint32_t(e.data.size());
            s.volume = uint16_t(std::clamp(volume, 0.0f, normalize > 0.0f ? 4.0f : 1.0f) * 255.0f);
            e.sample = int(samples_.size());
            samples_.push_back(s);
        }
        it = extras_.emplace(key, std::move(e)).first;
    }
    if (it->second.sample < 0)
        return false;
    std::lock_guard<std::recursive_mutex> g(lock_);
    nextDirect_ = normalize > 0.0f;   // (totd7: the typing sounds after the limiter, never squeezed by the music)
    const bool ok = play(it->second.sample, it->second.data.data(), uint32_t(it->second.data.size()));
    nextDirect_ = false;
    return ok;
}

std::string Audio::extraSource(const std::string& name) const
{
    auto it = extras_.find(name + (boardOn_ ? "#arcade" : "#pc"));
    if (it == extras_.end())
        it = extras_.find(name);
    return it == extras_.end() || it->second.sample < 0 ? std::string() : it->second.file ? "file" : "stand-in";
}

std::string Audio::describe(int n) const
{
    if (n < 0 || n >= int(cues_.size()))
        return "?";
    const Cue& c = cues_[size_t(n)];
    static const char* const kTypes[] = {"none", "sample", "stop", "track", "file", "command"};
    char buf[96];
    if (c.type == 4)
        std::snprintf(buf, sizeof buf, "file O_%s (slot %d)", c.name.c_str(), c.value & 0xFFFF);
    else
        std::snprintf(buf, sizeof buf, "%s %d", c.type >= 0 && c.type <= 5 ? kTypes[c.type] : "type?", c.value);
    return buf;
}

// smallfix2 (HOTD_TOTD_SOUNDLIST=1, testing): every short bank sample / file cue as the typing sounds would play it
// (brought to the peak as playExtra's normalize): its length, its RMS in dB below full scale and the pitch its zero
// crossings suggest, loudest (by RMS) first, to pick a clearer miss sound from the game's own
void Audio::reportCues(float peak)
{
    struct Row { double rms; std::string text; };
    std::vector<Row> rows;
    for (size_t n = 0; n < cues_.size(); ++n) {
        const Cue& c = cues_[n];
        std::vector<uint8_t> d;
        uint32_t rate = 11025;
        const int k = c.value & 0xFFFF;
        if (c.type == 1 && k >= 0 && k < int(samples_.size()) && samples_[size_t(k)].source == 1 &&
            samples_[size_t(k)].offset + samples_[size_t(k)].length <= bank_.size()) {
            const SampleInfo& s = samples_[size_t(k)];
            d.assign(bank_.begin() + s.offset, bank_.begin() + s.offset + s.length);
            rate = s.rate ? s.rate : 11025;
        } else if (c.type == 4) {
            if (const auto* o = loadOneShot(c.name); o && k >= 0 && k < int(samples_.size())) {
                d = *o;
                rate = samples_[size_t(k)].rate ? samples_[size_t(k)].rate : 22050;
            }
        }
        if (d.size() < 64)
            continue;
        int pk = 1;
        for (uint8_t b : d) pk = std::max(pk, std::abs(int(b) - 128));
        double sum = 0;
        int zc = 0;
        for (size_t i = 0; i < d.size(); ++i) {
            const double v = (double(d[i]) - 128.0) / double(pk) * peak;
            sum += v * v;
            if (i && ((int(d[i]) - 128) < 0) != ((int(d[i - 1]) - 128) < 0)) ++zc;
        }
        const double ms = 1000.0 * double(d.size()) / double(rate), rms = std::sqrt(sum / double(d.size()));
        char b[200];
        std::snprintf(b, sizeof b, "cue %3zu %-28s %6.0f ms  rms %6.1f dB  ~%5.0f Hz", n, describe(int(n)).c_str(), ms,
                      20.0 * std::log10(std::max(rms, 1e-6)), double(zc) * double(rate) / (2.0 * double(d.size())));
        rows.push_back({rms, b});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.rms > b.rms; });
    for (const Row& r : rows)
        std::printf("SOUNDLIST %s\n", r.text.c_str());
}

void Audio::cue(uint32_t arcadeCode)
{
    // (testing / trailer: HOTD_MUTE_CODE=0xA01472 leaves that one code out, e.g. the attract's white flash)
    static const uint32_t muted = std::getenv("HOTD_MUTE_CODE") ? uint32_t(std::strtoul(std::getenv("HOTD_MUTE_CODE"), nullptr, 0)) : 0u;
    if (muted && arcadeCode == muted) return;
    ++codeCount[arcadeCode];
    auto it = cueMap_.find(arcadeCode);
    if (traceCues) {
        std::printf("[sound %llu] 0x%06X -> %s\n", (unsigned long long)frameNo, arcadeCode,
                    it == cueMap_.end() ? "no PC cue" : ("cue " + std::to_string(it->second) + " " + describe(it->second)).c_str());
    }
    if (!reloadVoice_ && it != cueMap_.end() && it->second == kReloadCue) {   // spanish: Reload Voice Off (the board's too)
        if (traceCues)
            std::printf("[sound %llu] 0x%06X -> left out (Reload Voice: Off)\n", (unsigned long long)frameNo, arcadeCode);
        return;
    }
#ifdef HOTD_HAVE_ARCADE_SOUND
    if (boardOn_) {   // the arcade board plays every code itself (0xBExxxx too)
        // spanish: Voice = Spanish: a dubbed line plays the Spanish copy's sample instead of the board's English one
        if (!voiceDir_.empty() && it != cueMap_.end() && isVoiceCue(it->second)) {
            if (traceCues)
                std::printf("[sound %llu] 0x%06X -> the Spanish voice (cue %d), not the board\n", (unsigned long long)frameNo,
                            arcadeCode, it->second);
            pcCue(it->second);
            return;
        }
        if (!boardCommand(arcadeCode))   // (audioload: into the ring at its frame, or directly)
            board_->command(arcadeCode);
        return;
    }
#endif
    // THOTD's own calls where the arcade sends these (PC harness, sound_cue 0x412000 on Chapter 1, 2026-10-19):
    // 0xBE5000 (the board's setting, after 0xA00002) = cue 7, every sample off (0x43E6E4: cues 2 + 7 in title_start,
    // mode9_story_start, ranking_start, game_over_start; kind17_researcher_frame: 7); the clear music 0xA01005 comes
    // after cue 1, every sample off (kind176_chapter_clear 0x51A7AC: cues 1 + 13)
    if (arcadeCode == 0xBE5000 || arcadeCode == 0xA01005) {
        const int extra = arcadeCode == 0xBE5000 ? 7 : 1;
        if (traceCues)
            std::printf("[sound %llu] 0x%06X -> cue %d %s (THOTD's own)\n", (unsigned long long)frameNo, arcadeCode, extra,
                        describe(extra).c_str());
        pcCue(extra);
        if (arcadeCode == 0xBE5000)
            return;
    }
    if (it == cueMap_.end()) {
        // 0xBExxxx: a sound-board setting (event.cpp / hangedman_flyby: 0xBE5000), no sound; the PC has none
        // 0xA00000: the arcade data's "no sound" entry (sent from a sound table at Chapter 3's start, with 0xA01249 /
        // 0xA0135D in the same frame; the voice messages skip it themselves, scene_objects.cpp): sound 0, no PC cue
        if ((arcadeCode >> 16) != 0xA0 || arcadeCode == 0xA00000)
            return;
        if (unmapped.insert(arcadeCode).second) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "sound: arcade code 0x%06X has no PC cue", arcadeCode);
            note(buf);
        }
        return;
    }
    pcCue(it->second);
}

// the PC's sound_cue (0x412000)
void Audio::pcCue(int n)
{
    if (n < 0 || n >= int(cues_.size()) || (!reloadVoice_ && n == kReloadCue))
        return;
    ++cueCount[n];
    const Cue& c = cues_[size_t(n)];
    switch (c.type) {
    case 1:
        if (!play(c.value & 0xFFFF, nullptr, 0))
            silent.insert(n);
        break;
    case 2:
        stopSample(c.value);
        break;
    case 3:
        if (c.value != -1)
            playTrack(c.value, n != 11 && n != 13);
        break;
    case 4:
        if (const auto* data = loadOneShot(c.name)) {
            stopSample(c.value);
            if (!play(c.value & 0xFFFF, data->data(), uint32_t(data->size())))
                silent.insert(n);
        } else {
            silent.insert(n);
        }
        break;
    case 5:
        if (c.value == 1)
            stopMusic();
        else if (c.value == 2 || c.value == 3 || c.value == 7)
            stopAll();
        break;
    default:
        break;
    }
}

bool Audio::play(int sample, const uint8_t* data, uint32_t size)
{
    if (sample < 0 || sample >= int(samples_.size()))
        return false;
    const SampleInfo& s = samples_[size_t(sample)];
    if (!data) {
        if (s.source != 1 || s.offset + s.length > bank_.size())
            return false;
        data = bank_.data() + s.offset;
        size = s.length;
    }
    std::lock_guard<std::recursive_mutex> g(lock_);
    // the same sample started again within 1/15 s (a cue sent every frame, e.g. the reload while the trigger is held
    // off screen): that voice restarts instead of stacking copies (our own rule)
    const double recent = double(s.rate ? s.rate : 11025) / 15.0;
    Voice* v = nullptr;
    for (Voice& x : voices_)
        if (x.data == data && x.sample == sample && x.pos < recent && !x.held)
            v = &x;
    if (!v) {
        // a free voice, else the one furthest through its sample
        v = &voices_[0];
        for (Voice& x : voices_) {
            if (!x.data) { v = &x; break; }
            if (x.pos / std::max<uint32_t>(x.length, 1) > v->pos / std::max<uint32_t>(v->length, 1))
                v = &x;
        }
    }
    cut(*v);   // a restart or a stolen voice: the old sound ramps down in a tail
    v->data = data;
    v->length = std::min(s.length, size);
    v->loopStart = s.loopStart < s.length ? s.loopStart : 0;
    v->loop = s.loopStart != 0 || (s.flags & 0x1000000) != 0;
    v->pos = 0;
    v->step = double(s.rate ? s.rate : 11025) / kRate;
    v->volume = s.volume / 255.0f;
    v->sample = sample;
    v->held = false;   // a sound started while the game is paused (the pause menu's own cues) plays
    v->quiet = quietEffects_ && !v->loop;   // (a cut-scene's skip: its voices and one-shots unheard; loops play on)
    v->direct = nextDirect_;
    return v->length > 0 && s.volume > 0;
}

void Audio::stopSample(int sample)
{
    std::lock_guard<std::recursive_mutex> g(lock_);
    for (Voice& v : voices_)
        if (v.data && v.sample == (sample & 0xFFFF))
            cut(v);
}

void Audio::stopAll()
{
    std::lock_guard<std::recursive_mutex> g(lock_);
    for (Voice& v : voices_)
        cut(v);
}

// (lock_ held) a voice that sounds goes on as a tail ramping down over kFade frames (the quietest tail gives way when
// all are busy); a held voice (the pause) or one not yet heard ends at once
void Audio::cut(Voice& v)
{
    if (v.data && !v.held && v.amp > 0) {
        Voice* t = &tails_[0];
        for (Voice& x : tails_) {
            if (!x.data) { t = &x; break; }
            if (x.amp < t->amp)
                t = &x;
        }
        *t = v;
        t->release = true;
    }
    v = Voice{};
}

// musicdirs (user 2026-10-17): the PC release played CD audio, so a PC dump often has no Music folder and players
// bring their own rips. The tracks are looked for, in order of place: the PC source's Music\ (folder or zip), a Music
// folder next to the PC zip / folder, the package's data\music, the PC zip's own folder. In each place: loose files
// and (except the last place) the files inside its .zip files; a name "TrackNN" or any name whose last number is the
// track ("House of the Dead 02.mp3" = track 2); .ogg (stb_vorbis) and .mp3 (dr_mp3), whichever the build has. Within
// a place TrackNN beats other names, .ogg beats .mp3, a loose file beats one in a zip. Each track's file is logged.
//
// Numbering (launcher "numbering", --music-numbering): the Sega Saturn disc orders the same songs differently. Saturn
// -> PC CD (the user's C:\Games\HOTD\Music\README.txt): 02 03 04 -> same, 10 -> 05 (game over), 07 -> 06 (boss), 09 ->
// 07 (stage clear), 11 -> 08 (credits), 05 -> 09 (Chapter 3), 08 -> 10 (final boss), 12 -> 11 (name entry /
// ranking / menu), 06 -> 12 and 13 (Chapter 4: the PC's track 12 is cue 22 MUSIC_STG4BOSS_3, the arcade's Chapter 4
// cue 0xA0100E; 13 is the user's optional patch 7). Saturn 12 went to 12 too before the user's patch 9 (2026-10-19). Auto: TrackNN files are the PC's; a set with other names is the Saturn's when its
// 09 and 10 are the short jingles (each under a quarter of its 03; on the PC CD those are 05 / 07).
void Audio::indexTracks()
{
    namespace fs = std::filesystem;
    tracks_.clear();
    tracksIndexed_ = true;
    loggedTracks_.clear();
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    struct Cand { std::string path, where; int number, rank; bool named; uint64_t size; };
    std::vector<Cand> cands;
    // a candidate: rank = place * 16 + (TrackNN ? 0 : 4) + (in a zip ? 2 : 0) + (.ogg ? 0 : 1)
    auto add = [&](const std::string& path, std::string name, int place, bool inZip, const std::string& where, uint64_t size) {
        name = lower(name);
        const size_t dot = name.rfind('.');
        if (dot == std::string::npos)
            return;
        const std::string ext = name.substr(dot + 1), base = name.substr(0, dot);
        bool ok = ext == "wav";   // (the setup's CD rips)
#ifdef HOTD_HAVE_STB_VORBIS
        ok |= ext == "ogg";
#endif
#ifdef HOTD_HAVE_DR_MP3
        ok |= ext == "mp3";
#endif
        if (!ok)
            return;
        size_t e = base.size();
        while (e > 0 && !std::isdigit(static_cast<unsigned char>(base[e - 1]))) --e;
        size_t b = e;
        while (b > 0 && std::isdigit(static_cast<unsigned char>(base[b - 1]))) --b;
        if (b == e || e - b > 2)
            return;
        const int number = std::atoi(base.substr(b, e - b).c_str());
        const bool named = base.rfind("track", 0) == 0 && b == 5 && e == base.size();
        const int rank = place * 16 + (named ? 0 : 4) + (inZip ? 2 : 0) + (ext == "ogg" ? 0 : 1);
        cands.push_back(Cand{path, where, number, rank, named, size});
    };
    auto addZip = [&](const std::string& zip, int place, const std::string& where, const std::string& prefix) {
        std::vector<pcfs::ZipItem> items;
        std::string err;
        if (!pcfs::listZip(zip, items, err))
            return;
        for (const pcfs::ZipItem& it : items) {
            if (!prefix.empty() && it.rel.rfind(prefix, 0) != 0)
                continue;
            const size_t slash = it.rel.find_last_of('/');
            add(it.path, slash == std::string::npos ? it.rel : it.rel.substr(slash + 1), place, true, where, it.size);
        }
    };
    auto addDir = [&](const std::string& dir, int place, const std::string& where, bool zips) {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            return;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string path = entry.path().string(), name = entry.path().filename().string();
            if (zips && pcfs::isZip(path))
                addZip(path, place, where, "");
            else
                add(path, name, place, false, where, uint64_t(entry.file_size(ec)));
        }
    };
    // the folder the PC zip / folder sits in (for "<zip>/<folder inside>": the zip's)
    fs::path src = fs::path(root_).lexically_normal();
    if (!src.has_filename())
        src = src.parent_path();   // a trailing separator
    while (!src.empty() && !pcfs::isZip(src.string()) && pcfs::inZip(src.string() + "/x"))
        src = src.parent_path();
    const bool zipped = pcfs::isZip(src.string());
    const std::string parent = src.parent_path().string();
    if (zipped)
        addZip(src.string(), 0, "the PC zip's Music folder", "music/");
    else
        addDir(findPath(root_, "Music"), 0, "the PC files' Music folder", true);
    addDir(findPath(parent, "Music"), 1, "the Music folder next to the PC files", true);
    if (!packageRoot.empty())
        addDir(findPath(packageRoot, "data/music"), 2, "data\\music", true);
    if (zipped)
        addDir(parent, 3, "the PC zip's folder", false);

    // the numbering: explicit, or auto (the other-named files' 09 / 10 against their 03)
    bool saturn = musicNumbering == 2;
    if (musicNumbering == 0) {
        std::map<int, const Cand*> other;   // the best other-named file per number
        for (const Cand& c : cands)
            if (!c.named && (!other.count(c.number) || c.rank < other[c.number]->rank))
                other[c.number] = &c;
        if (other.count(3) && other.count(9) && other.count(10)) {
            const uint64_t ref = other[3]->size;
            saturn = other[9]->size * 4 < ref && other[10]->size * 4 < ref;
            note(std::string("music: numbering auto: the other-named set is ") +
                 (saturn ? "the Saturn disc's (its 09 / 10 are the short jingles)" : "the PC CD's"));
        }
    }
    static const std::pair<int, int> kSaturnToPc[] = {{2, 2}, {3, 3}, {4, 4}, {10, 5}, {7, 6}, {9, 7}, {11, 8},
                                                      {5, 9}, {8, 10}, {12, 11}, {6, 12}, {6, 13}};
    auto put = [&](int track, const Cand& c, const std::string& where) {
        auto it = tracks_.find(track);
        if (it == tracks_.end() || c.rank < it->second.rank)
            tracks_[track] = TrackFile{c.path, where, c.rank};
    };
    for (const Cand& c : cands) {
        // explicit Saturn: every file; auto: the other-named ones (TrackNN stays the PC's)
        if (!(saturn && (musicNumbering == 2 || !c.named))) {
            put(c.number, c, c.where);
            continue;
        }
        for (const auto& [from, to] : kSaturnToPc)
            if (from == c.number)
                put(to, c, c.where + " (Saturn " + std::to_string(c.number) + ")");
    }
    // the US PC CD has tracks 2 and 6 the other way round from the EU and Japanese CDs and from what the game asks for
    // (GitHub #12; checked 2026-11-02 against the arcade: 2 = the title song, about a minute; 6 = the boss song, about
    // five): rips of it put back (a 02 file over twice the size of the 06 file, same format)
    if (!saturn) {
        auto t2 = tracks_.find(2), t6 = tracks_.find(6);
        if (t2 != tracks_.end() && t6 != tracks_.end()) {
            uint64_t s2 = 0, s6 = 0;
            for (const Cand& c : cands) {
                if (c.path == t2->second.path) s2 = c.size;
                if (c.path == t6->second.path) s6 = c.size;
            }
            const std::string e2 = lower(fs::path(t2->second.path).extension().string()), e6 = lower(fs::path(t6->second.path).extension().string());
            if (e2 == e6 && s6 > 0 && s2 > 2 * s6) {
                std::swap(t2->second, t6->second);
                note("music: tracks 2 and 6 swapped (the US CD's order: its track 2 is the boss song, 6 the title song)");
            }
        }
    }
    note("music: " + std::to_string(tracks_.size()) + " track(s) found");
}

void Audio::playTrack(int track, bool loop)
{
    char name[32];
    std::snprintf(name, sizeof name, "Music/Track%02d", track);
#ifdef HOTD_HAVE_MUSIC
    if (!tracksIndexed_)
        indexTracks();
    const auto found = tracks_.find(track);
    if (found == tracks_.end()) {
        note(std::string("music: no file for track ") + std::to_string(track) + " (" + name +
             ".ogg / .mp3 or a name ending in the number; see README)");
        return;
    }
    const TrackFile& tf = found->second;
    if (loggedTracks_.insert(track).second)
        note("music: track " + std::to_string(track) + " from " + tf.where + ": " + tf.path);
    auto m = std::make_unique<Music>();
    if (!pcfs::readFile(tf.path, m->data) || m->data.empty()) {
        note("music: cannot read " + tf.path);
        return;
    }
    const bool mp3 = tf.path.size() > 4 && SDL_strcasecmp(tf.path.c_str() + tf.path.size() - 4, ".mp3") == 0;
    const bool wav = tf.path.size() > 4 && SDL_strcasecmp(tf.path.c_str() + tf.path.size() - 4, ".wav") == 0;
    bool ok = false;
    if (wav) {   // RIFF / WAVE: "fmt " (PCM, 16 bits) and "data"
        const std::vector<uint8_t>& d = m->data;
        auto u32 = [&](size_t at) { return uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 | uint32_t(d[at + 3]) << 24; };
        auto u16 = [&](size_t at) { return unsigned(d[at]) | unsigned(d[at + 1]) << 8; };
        bool fmtOk = false;
        if (d.size() >= 12 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "WAVE", 4) == 0)
            for (size_t at = 12; at + 8 <= d.size();) {
                const uint32_t len = u32(at + 4);
                const size_t body = at + 8, have = std::min<size_t>(len, d.size() - body);
                if (std::memcmp(d.data() + at, "fmt ", 4) == 0 && have >= 16) {
                    m->channels = int(u16(body + 2));
                    m->rate = int(u32(body + 4));
                    fmtOk = u16(body) == 1 && u16(body + 14) == 16 && m->channels > 0 && m->rate > 0;
                } else if (std::memcmp(d.data() + at, "data", 4) == 0 && fmtOk) {
                    m->pcm = reinterpret_cast<const int16_t*>(d.data() + body);
                    m->pcmFrames = have / (2 * size_t(m->channels));
                    m->total = m->pcmFrames;
                    ok = m->pcmFrames > 0;
                    break;
                }
                at = body + len + (len & 1);
            }
    } else if (!mp3) {
#ifdef HOTD_HAVE_STB_VORBIS
        int error = 0;
        m->v = stb_vorbis_open_memory(m->data.data(), int(m->data.size()), &error, nullptr);
        if (m->v) {
            const stb_vorbis_info info = stb_vorbis_get_info(m->v);
            m->channels = info.channels;
            m->rate = int(info.sample_rate);
            ok = true;
        }
#endif
    } else {
#ifdef HOTD_HAVE_DR_MP3
        if (drmp3_init_memory(&m->mp3, m->data.data(), m->data.size(), nullptr)) {
            m->mp3Open = true;
            m->channels = int(m->mp3.channels);
            m->rate = int(m->mp3.sampleRate);
            ok = m->rate > 0;
        }
#endif
    }
    if (!ok) {
        note("music: cannot open " + tf.path);
        return;
    }
    m->loop = loop;
#ifdef HOTD_HAVE_STB_VORBIS
    if (m->v) m->total = stb_vorbis_stream_length_in_samples(m->v);
#endif
#ifdef HOTD_HAVE_DR_MP3
    if (m->mp3Open) {   // (counting decodes the file once; then back to its start)
        m->total = drmp3_get_pcm_frame_count(&m->mp3);
        drmp3_seek_to_pcm_frame(&m->mp3, 0);
    }
#endif
    std::lock_guard<std::recursive_mutex> g(lock_);
    music_ = std::move(m);
    musicHeld_ = false;
#else
    (void)loop;
    note(std::string("music: ") + name + " (built without stb_vorbis / dr_mp3: no music)");
#endif
}

bool Audio::playMusicTrack(int track, bool loop)
{
    {
        std::lock_guard<std::recursive_mutex> g(lock_);
        music_.reset();
    }
    playTrack(track, loop);
    return musicPlaying();
}

bool Audio::hasMusicTrack(int track)
{
#ifdef HOTD_HAVE_MUSIC
    if (!tracksIndexed_)
        indexTracks();
    return tracks_.count(track) != 0;
#else
    (void)track;
    return false;
#endif
}

bool Audio::musicPlaying() const
{
    return music_ != nullptr;
}

bool Audio::musicTime(double& pos, double& len, int& loops) const
{
    pos = len = 0;
    loops = 0;
#ifdef HOTD_HAVE_MUSIC
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!music_)
        return false;
    const double rate = music_->rate > 0 ? music_->rate : kRate;
    pos = double(music_->played) / rate;
    len = double(music_->total) / rate;
    loops = music_->loops;
    return true;
#else
    return false;
#endif
}

void Audio::seekMusic(double seconds)
{
#ifdef HOTD_HAVE_MUSIC
    std::lock_guard<std::recursive_mutex> g(lock_);
    if (!music_)
        return;
    uint64_t f = uint64_t(std::max(0.0, seconds) * (music_->rate > 0 ? music_->rate : kRate));
    if (music_->total && f >= music_->total)
        f = music_->total - 1;
    music_->seek(f);
#else
    (void)seconds;
#endif
}

double Audio::cueLength(int cue)
{
    const int t = cueType(cue);
    if (t != 1 && t != 4)
        return 0;
    const int s = cues_[size_t(cue)].value & 0xFFFF;
    if (s < 0 || s >= int(samples_.size()))
        return 0;
    const SampleInfo& si = samples_[size_t(s)];
    uint32_t len = si.length;
    if (t == 4) {
        const auto* d = loadOneShot(cues_[size_t(cue)].name);
        len = d ? std::min<uint32_t>(len, uint32_t(d->size())) : 0;
    }
    return double(len) / double(si.rate ? si.rate : 11025);
}

bool Audio::cueLoops(int cue) const
{
    const int t = cueType(cue);
    if (t != 1 && t != 4)
        return false;
    const int s = cues_[size_t(cue)].value & 0xFFFF;
    if (s < 0 || s >= int(samples_.size()))
        return false;
    return samples_[size_t(s)].loopStart != 0 || (samples_[size_t(s)].flags & 0x1000000) != 0;
}

double Audio::trackLength(int track)
{
    static std::map<std::pair<std::string, int>, double> known;   // (the files' place, track)
    const auto key = std::make_pair(root_ + "|" + packageRoot + "|" + std::to_string(musicNumbering), track);
    if (auto it = known.find(key); it != known.end())
        return it->second;
    double len = 0;
    {
        std::lock_guard<std::recursive_mutex> g(lock_);
        std::unique_ptr<Music> keep = std::move(music_);   // (playTrack replaces music_: the one playing comes back)
        const bool held = musicHeld_;
        playTrack(track, false);
        if (music_ && music_->rate > 0)
            len = double(music_->total) / music_->rate;
        music_ = std::move(keep);
        musicHeld_ = held;
    }
    known[key] = len;
    return len;
}

bool Audio::cueSounding(int cue)
{
    const int t = cueType(cue);
    if (t != 1 && t != 4)
        return false;
    const int s = cues_[size_t(cue)].value & 0xFFFF;
    std::lock_guard<std::recursive_mutex> g(lock_);
    for (const Voice& v : voices_)
        if (v.data && v.sample == s)
            return true;
    return false;
}

void Audio::stopMusic()
{
    std::lock_guard<std::recursive_mutex> g(lock_);
    music_.reset();
}

void Audio::mix(float* out, int frames)
{
    std::fill(out, out + frames * 2, 0.0f);
    const uint64_t waitFrom = SDL_GetTicksNS();
    std::lock_guard<std::recursive_mutex> g(lock_);
    lockWaitNs_ = SDL_GetTicksNS() - waitFrom;   // (questaudio2: the output callback's wait, logged there)
    // totd7: the direct voices (TotD 0's typing sounds) into their own buffer, added after the limiter and the board
    if (directBuf_.size() < size_t(frames) * 2)
        directBuf_.resize(size_t(frames) * 2);
    std::fill(directBuf_.begin(), directBuf_.begin() + frames * 2, 0.0f);
    bool anyDirect = false;
    float* const out_ = out;
    auto mixVoice = [&](Voice& v) {
        if (!v.data || v.held)   // held by the pause: kept where it is
            return;
        float* const out = v.direct ? directBuf_.data() : out_;
        anyDirect = anyDirect || v.direct;
        const float gain = v.quiet ? 0.0f : v.volume * sfxVolume_ * kSfxGain * kPcLevel;
        const float ramp = 1.0f / kFade;
        for (int i = 0; i < frames; ++i) {
            uint32_t p = uint32_t(v.pos);
            if (p >= v.length) {
                if (!v.loop) { v = Voice{}; break; }
                v.pos = v.loopStart + (v.pos - v.length);
                p = uint32_t(v.pos);
                if (p >= v.length) { v = Voice{}; break; }
            }
            if (v.release) {
                if ((v.amp -= ramp) <= 0) { v = Voice{}; break; }
            } else if (v.amp < 1) {
                v.amp = std::min(1.0f, v.amp + ramp);
            }
            float env = v.amp;
            if (!v.loop)   // a one-shot's last kFade output frames ramp down to its end
                env = std::min(env, float((v.length - v.pos) / v.step) * ramp);
            // linear interpolation between 8-bit unsigned samples
            const float a = (v.data[p] - 128) / 128.0f;
            const float b = p + 1 < v.length ? (v.data[p + 1] - 128) / 128.0f : a;
            const float t = float(v.pos - p);
            const float s = (a + (b - a) * t) * gain * env;
            out[i * 2] += s;
            out[i * 2 + 1] += s;
            v.pos += v.step;
        }
    };
    for (Voice& v : voices_)
        mixVoice(v);
    for (Voice& v : tails_)
        mixVoice(v);
#ifdef HOTD_HAVE_MUSIC
    if (music_ && musicOn_ && !musicHeld_) {
        Music& m = *music_;
        const double step = double(m.rate) / kRate;
        for (int i = 0; i < frames; ++i) {
            // nearest frame at the track's rate (the tracks are 44.1 kHz: one frame each)
            bool ok = true;
            for (m.frac += step; m.frac >= 1.0 && ok; m.frac -= 1.0)
                ok = m.next();
            if (!ok) { music_.reset(); break; }
            out[i * 2] += m.cur[0] * musicVolume_ * kPcLevel;
            out[i * 2 + 1] += m.cur[1] * musicVolume_ * kPcLevel;
        }
    }
#endif
    // a soft limiter above 0.7 on the PC sound (effects and music) instead of hard clipping when many voices add up
    for (int i = 0; i < frames * 2; ++i) {
        const float a = std::fabs(out[i]);
        if (a > 0.7f)
            out[i] = std::copysign(0.7f + 0.3f * std::tanh((a - 0.7f) / 0.3f), out[i]);
    }
#ifdef HOTD_HAVE_ARCADE_SOUND
    // the board after the limiter: its output is MAME's SCSP mix with one linear gain, nothing else (TODO "Arcade =
    // MAME" step 4). x5.5 (user 2026-10-18: as loud as the PC sound): the SCSP's own mix peaked at ~0.14 of full
    // scale; with the gain the Chapter 1 / 2 runs peak below 1.0 (docs/teaser_status/audio_fixes.md)
    if (boardOn_ && !boardHeld_) {   // the board runs in step with the output (held by the pause)
        // (into its own buffer first: its level for the Jukebox; boardVolume_: the Jukebox's music volume)
        if (boardBuf_.size() < size_t(frames) * 2)
            boardBuf_.resize(size_t(frames) * 2);
        std::fill(boardBuf_.begin(), boardBuf_.begin() + frames * 2, 0.0f);
        if (ringOn_)   // (audioload: made ahead by the board thread)
            takeBoard(boardBuf_.data(), frames);
        else {
            std::lock_guard<std::mutex> bl(boardLock_);
            board_->render(boardBuf_.data(), frames, 1.0f);
        }
        const float gain = 5.5f * (boardVolume_ >= 0 ? boardVolume_ : sfxVolume_ / 0.8f);
        float peak = 0;
        for (int i = 0; i < frames * 2; ++i) {
            peak = std::max(peak, std::fabs(boardBuf_[size_t(i)]));
            out[i] += boardBuf_[size_t(i)] * gain;
        }
        boardPeak_ = peak;
    }
#endif
    // totd7 (user 2026-11-05: "the typing sounds are still not loud enough"): the direct voices on top, after the
    // limiter (totd5's went into it with the music and were squeezed whenever the track was loud); where they sound,
    // a soft knee above 0.9 keeps the sum from clipping (elsewhere the mix is untouched)
    if (anyDirect)
        for (int i = 0; i < frames * 2; ++i)
            if (const float d = directBuf_[size_t(i)]; d != 0.0f) {
                const float s = out[i] + d, a = std::fabs(s);
                out[i] = a > 0.9f ? std::copysign(0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f), s) : s;
            }
}

// the pause (THOTD.EXE 0x41ACA8 / 0x41ACF0, game/pause.h): hold = the CD's pause (0x565BE0) and message 0x418 (every
// playing DirectSound buffer Stop()s, keeping its position); go on = the CD's resume (0x565B90) and 0x419 (those
// buffers Play() again). Here the voices that sound and the track stay where they are and go on from there: nothing is
// cued again (the user's patched game resumes only the track that played, patches 6 / 8; ours keeps its place too).
// Sounds started while held (the pause menu's cues) play at once.
void Audio::pause(bool hold)
{
    std::lock_guard<std::recursive_mutex> g(lock_);
    for (Voice& v : voices_)
        if (v.data)
            v.held = hold;
    musicHeld_ = hold && music_ != nullptr;   // patch 6: only a track that plays is paused (and so resumed)
    boardHeld_ = hold;
}

void Audio::callback(void* self, SDL_AudioStream* stream, int additional, int)
{
    Audio& a = *static_cast<Audio*>(self);
    const int frames = additional / int(sizeof(float) * 2);
    if (frames <= 0)
        return;
    // questaudio (the log): SDL pulls once per device buffer; a pull more than two buffers after the last one means
    // the device ran dry meanwhile (SDL's AAudio output keeps three buffers): counted, logged every 10 s at most
    if (a.deviceBufNs_) {
        const uint64_t now = SDL_GetTicksNS();
        if (a.lastPullNs_ && now - a.lastPullNs_ > 2 * a.deviceBufNs_ + 2000000) {
            ++a.lateCount_;
            ++a.lateTotal_;
            a.lateWorstNs_ = std::max(a.lateWorstNs_, now - a.lastPullNs_);
        }
        a.lastPullNs_ = now;
        // questaudio2: every 10 s, the late pulls and the callback's worst own time / wait for lock_ (the game
        // thread holding it while the board thread computes: the output stalls without a late pull being counted)
        if (now - a.lateLogNs_ > 10000000000ull) {
            a.note("audio: the output pulled late " + std::to_string(a.lateCount_) + " times (worst gap " +
                   std::to_string(a.lateWorstNs_ / 1000000) + " ms; a device buffer is " +
                   std::to_string(a.deviceBufNs_ / 1000000) + " ms); the callback's worst " +
                   std::to_string(a.cbWorstNs_ / 1000) + " us (lock wait " + std::to_string(a.lockWaitWorstNs_ / 1000) +
                   " us), " + std::to_string(a.cbSlow_) + " over half a buffer");
            a.lateCount_ = 0;
            a.lateWorstNs_ = 0;
            a.cbWorstNs_ = a.lockWaitWorstNs_ = a.cbSlow_ = 0;
            a.lateLogNs_ = now;
        }
    }
    if (a.mixBuf_.size() < size_t(frames) * 2)
        a.mixBuf_.resize(size_t(frames) * 2);
    const uint64_t cb0 = SDL_GetTicksNS();
    a.mix(a.mixBuf_.data(), frames);
    SDL_PutAudioStreamData(stream, a.mixBuf_.data(), frames * int(sizeof(float) * 2));
    if (a.deviceBufNs_) {
        const uint64_t took = SDL_GetTicksNS() - cb0;
        a.cbWorstNs_ = std::max(a.cbWorstNs_, took);
        a.lockWaitWorstNs_ = std::max<uint64_t>(a.lockWaitWorstNs_, a.lockWaitNs_);
        if (took * 2 > a.deviceBufNs_) ++a.cbSlow_;
    }
}

} // namespace hotd::game

// audioload: the audio threads into Windows' multimedia class "Pro Audio" (as SDL's WASAPI thread): scheduled before
// the ordinary threads of every process (the user's "crackling" with the CPU saturated). Elsewhere SDL's priority only
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace hotd::game {
namespace {
void audioThreadBoost()
{
    using SetMm = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    static const SetMm setMm = [] {
        const HMODULE avrt = LoadLibraryW(L"avrt.dll");
        return avrt ? reinterpret_cast<SetMm>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))) : nullptr;
    }();
    DWORD task = 0;
    if (setMm)
        setMm(L"Pro Audio", &task);
}
} // namespace
} // namespace hotd::game
#else
namespace hotd::game {
namespace {
void audioThreadBoost() {}
} // namespace
} // namespace hotd::game
#endif
