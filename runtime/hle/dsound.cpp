// HLE of Xbox DirectSound (XDK 4928) entry points the game calls, and the mixer.
//
// Objects are guest allocations and buffers own guest memory, so Lock/Unlock/
// SetBufferData behave. A 48 kHz stereo mixer (runtime/audio_out.cpp drives
// it) plays every buffer and stream: PCM (8/16-bit) and Xbox ADPCM, mono or
// stereo, resampled to their frequency, with their volume. The mixer is also
// the clock: play cursors and stream packet completion advance by what it has
// played, which is what CRI's ADX and Sofdec players time themselves by.
// Not yet: 3D positioning (positional sounds play centred), effects.
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#include <string>

#ifndef _WIN32
#define _fseeki64 fseeko
#endif

#include "audio_out.h"
#include "kobj.h"
#include "settings.h"

namespace tje {

std::string guest_printf(uint32_t fmt, uint32_t args);
uint32_t open_host_file_for_guest(const std::string& guest_path, uint64_t* size);

namespace {

enum : uint32_t { DS_OK = 0, E_FAIL = 0x80004005u };
constexpr int kMixRate = 48000;

std::recursive_mutex g_audio_m;                 // buffers and streams (guest threads and the mixer)

// WAVEFORMATEX { wFormatTag, nChannels, nSamplesPerSec, nAvgBytesPerSec, nBlockAlign, wBitsPerSample }
struct Format { uint16_t tag = 1, channels = 2, bits = 16; uint32_t rate = 44100; };

Format read_format(uint32_t wfx, Format f = Format{}) {
    if (!wfx) return f;
    f.tag = R16(wfx);
    f.channels = R16(wfx + 2) ? R16(wfx + 2) : 1;
    f.rate = R32(wfx + 4) ? R32(wfx + 4) : 44100;
    f.bits = R16(wfx + 14) ? R16(wfx + 14) : 16;
    return f;
}

bool is_adpcm(const Format& f) { return f.tag == 0x69; }

// Xbox ADPCM: 36-byte blocks per channel, 65 samples each (see tools/audio/xadpcm.py).
uint32_t adpcm_block(const Format& f) { return 36u * f.channels; }
uint32_t pcm_frame(const Format& f) { return (f.bits / 8u) * f.channels; }

uint64_t total_frames(const Format& f, uint32_t bytes) {
    if (is_adpcm(f)) return (uint64_t)(bytes / adpcm_block(f)) * 65;
    uint32_t fb = pcm_frame(f);
    return fb ? bytes / fb : 0;
}
uint32_t frames_to_bytes(const Format& f, uint64_t frame) {
    if (is_adpcm(f)) return (uint32_t)((frame / 65) * adpcm_block(f));
    return (uint32_t)(frame * pcm_frame(f));
}
uint64_t bytes_to_frames(const Format& f, uint32_t bytes) { return total_frames(f, bytes); }

const int kStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

// Decoded-block cache of one voice.
struct Decoder {
    int64_t block = -1;
    int16_t pcm[65 * 2];
};

void decode_adpcm_block(const uint8_t* blk, int channels, int16_t* out) {
    int sample[2], index[2];
    for (int ch = 0; ch < channels; ch++) {
        sample[ch] = (int16_t)(blk[4 * ch] | blk[4 * ch + 1] << 8);
        index[ch] = blk[4 * ch + 2] > 88 ? 88 : blk[4 * ch + 2];
        out[ch] = (int16_t)sample[ch];
    }
    const uint8_t* data = blk + 4 * channels;
    for (int group = 0; group < 8; group++)
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t* w = data + (group * channels + ch) * 4;
            for (int k = 0; k < 8; k++) {
                int code = (w[k / 2] >> ((k & 1) * 4)) & 15;
                int step = kStep[index[ch]];
                int diff = step >> 3;
                if (code & 1) diff += step >> 2;
                if (code & 2) diff += step >> 1;
                if (code & 4) diff += step;
                sample[ch] += (code & 8) ? -diff : diff;
                sample[ch] = sample[ch] < -32768 ? -32768 : sample[ch] > 32767 ? 32767 : sample[ch];
                index[ch] += kIndex[code & 7];
                index[ch] = index[ch] < 0 ? 0 : index[ch] > 88 ? 88 : index[ch];
                out[(1 + group * 8 + k) * channels + ch] = (int16_t)sample[ch];
            }
        }
}

// One frame of a voice as stereo floats; false past the end of the data.
bool read_frame(const Format& f, uint32_t data, uint32_t bytes, uint64_t frame, Decoder& dc, float* lr) {
    int ch = f.channels > 1 ? 2 : 1;
    if (is_adpcm(f)) {
        uint64_t b = frame / 65;
        uint32_t bs = adpcm_block(f);
        if ((b + 1) * bs > bytes) return false;
        if (dc.block != (int64_t)b) {
            decode_adpcm_block((const uint8_t*)gptr(data + (uint32_t)(b * bs)), ch, dc.pcm);
            dc.block = (int64_t)b;
        }
        const int16_t* s = dc.pcm + (frame % 65) * ch;
        lr[0] = s[0] / 32768.0f;
        lr[1] = s[ch - 1] / 32768.0f;
        return true;
    }
    uint32_t fb = pcm_frame(f);
    if (!fb || (frame + 1) * fb > bytes) return false;
    const uint8_t* p = (const uint8_t*)gptr(data + (uint32_t)(frame * fb));
    if (f.bits == 8) {
        lr[0] = (p[0] - 128) / 128.0f;
        lr[1] = (p[ch - 1] - 128) / 128.0f;
    } else {
        int16_t a, b;
        std::memcpy(&a, p, 2);
        std::memcpy(&b, p + 2 * (ch - 1), 2);
        lr[0] = a / 32768.0f;
        lr[1] = b / 32768.0f;
    }
    return true;
}

float db_gain(int32_t hundredths_db) {         // DirectSound volume: hundredths of a dB, 0 = full
    if (hundredths_db <= -10000) return 0.0f;
    if (hundredths_db >= 0) return 1.0f;
    return std::pow(10.0f, hundredths_db / 2000.0f);
}

struct Buffer {
    uint32_t data = 0, bytes = 0;
    Format fmt;
    uint32_t freq = 0;                                // SetFrequency (0: the format's rate)
    float gain = 1.0f;
    bool playing = false, looping = false;
    double frame = 0;                                 // play position in source frames
    Decoder dc;
};
std::map<uint32_t, Buffer> g_buffers;           // guest object -> state

uint32_t cursor(Buffer& b) { return frames_to_bytes(b.fmt, (uint64_t)b.frame); }

// XMediaObject / IDirectSoundStream vtable served by HLE functions at fake addresses.
constexpr uint32_t kVtblFns = 0xfe800000;
uint32_t g_stream_vtbl = 0, g_wave_vtbl = 0;

uint32_t new_object(uint32_t vtbl, uint32_t size = 0x100) {
    uint32_t o = guest_alloc(size);
    W32(o, vtbl);
    W32(o + 4, 1);          // reference count (our own field)
    return o;
}

// Streams: packets queue up and the mixer plays them back to back, completing
// each when it has been played. The XMEDIAPACKET descriptor is copied when
// submitted (games pass one on their stack).
struct Packet { uint32_t data, size, completed_size, status, event, context; };
struct Stream {
    Format fmt;
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> queue;
    double frame = 0;                                // position in the front packet
    Decoder dc;
    float gain = 1.0f;
    bool paused = false;
    std::vector<Packet> done;                        // completed, callback not yet run
};
std::map<uint32_t, Stream> g_streams;           // guest object -> state

void complete_packet(const Packet& p, uint32_t done);

// Runs XMediaObject callbacks of completed packets, on the calling guest thread.
void run_callbacks(Cpu* c, uint32_t obj) {
    std::vector<Packet> done;
    uint32_t cb = 0, ctx = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        Stream& s = g_streams[obj];
        done.swap(s.done);
        cb = s.callback;
        ctx = s.context;
    }
    if (!cb) return;
    for (const Packet& p : done) guest_call(c, cb, {ctx, p.context, 0});   // (stream ctx, packet ctx, status)
}

// XMEDIAPACKET { pvBuffer, dwMaxSize, pdwCompletedSize, pdwStatus, hCompletionEvent, prtTimestamp, pContext }
Packet read_packet(uint32_t pkt) {
    return {R32(pkt), R32(pkt + 4), R32(pkt + 8), R32(pkt + 12), R32(pkt + 16), R32(pkt + 24)};
}

void complete_packet(const Packet& p, uint32_t done) {
    if (p.completed_size) W32(p.completed_size, done);
    if (p.status) W32(p.status, 0);                    // XMEDIAPACKET_STATUS_SUCCESS
    if (p.event) {
        KObject* e = handle_get(p.event);
        if (e) { e->signaled = true; sched_wake_all(); }
    }
}

// --- media object methods (stdcall, this first) ---
void mo_addref(Cpu* c) { uint32_t o = A(c, 0); W32(o + 4, R32(o + 4) + 1); RET(c, R32(o + 4), 1); }

void mo_getinfo(Cpu* c) {           // XMEDIAINFO { dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead }
    uint32_t i = A(c, 1);
    if (i) { W32(i, 0); W32(i + 4, 0x800); W32(i + 8, 0x800); W32(i + 12, 0); }
    RET(c, DS_OK, 2);
}
void mo_getstatus(Cpu* c) {          // XMEDIASTATUS_ACCEPT_INPUT | ACCEPT_OUTPUT
    if (A(c, 1)) W32(A(c, 1), 3);
    RET(c, DS_OK, 2);
}
// Release: a stream that goes away forgets its queued packets, whose memory the
// game is about to free (completing them later would write into freed memory).
void mo_release(Cpu* c) {
    uint32_t o = A(c, 0);
    uint32_t r = R32(o + 4);
    if (r) W32(o + 4, --r);
    if (!r) {
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        g_streams.erase(o);
    }
    RET(c, r, 1);
}
void stream_getstatus(Cpu* c) {      // accepts input while it has room for another packet
    uint32_t o = A(c, 0);
    bool room;
    {
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        Stream& s = g_streams[o];
        room = s.queue.size() < s.max_packets;
    }
    run_callbacks(c, o);
    if (A(c, 1)) W32(A(c, 1), room ? 1u : 0u);   // XMEDIASTATUS_ACCEPT_INPUT
    RET(c, DS_OK, 2);
}
void stream_process(Cpu* c) {        // Process(pThis, pInput, pOutput): queue the input packet
    uint32_t o = A(c, 0), in = A(c, 1);
    if (in) {
        if (R32(in + 8)) W32(R32(in + 8), 0);
        if (R32(in + 12)) W32(R32(in + 12), 0x8000000au);       // XMEDIAPACKET_STATUS_PENDING
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        Stream& s = g_streams[o];
        s.queue.push_back(read_packet(in));
    }
    run_callbacks(c, o);
    RET(c, DS_OK, 3);
}
void stream_flush(Cpu* c) {          // Flush(pThis): drop queued packets (status flushed)
    uint32_t o = A(c, 0);
    {
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        Stream& s = g_streams[o];
        for (const Packet& p : s.queue) {
            if (p.status) W32(p.status, 0x80004004u);   // XMEDIAPACKET_STATUS_FLUSHED (E_ABORT)
            if (p.event) {
                KObject* e = handle_get(p.event);
                if (e) e->signaled = true;
            }
        }
        s.queue.clear();
        s.frame = 0;
        s.dc.block = -1;
    }
    sched_wake_all();
    RET(c, DS_OK, 1);
}
void wave_process(Cpu* c) {          // wave file: fill the output packet from the file
    uint32_t o = A(c, 0), out = A(c, 2);
    uint32_t fp_handle = R32(o + 8);
    uint32_t done = 0;
    if (out) {
        KObject* f = handle_get(fp_handle);
        uint32_t buf = R32(out), max = R32(out + 4);
        if (f && f->fp) {
            _fseeki64(f->fp, (int64_t)f->pos, SEEK_SET);
            done = (uint32_t)std::fread(gptr(buf), 1, max, f->fp);
            f->pos += done;
        }
        complete_packet(read_packet(out), done);
    }
    RET(c, DS_OK, 3);
}
void mo_noarg(Cpu* c) { RET(c, DS_OK, 1); }       // Discontinuity / Flush
// XFileMediaObject (wave files): object +8 file handle, +12 data chunk offset, +16 data bytes.
void wave_seek(Cpu* c) {             // Seek(pThis, lOffset, dwOrigin, pdwAbsolute)
    uint32_t o = A(c, 0);
    KObject* f = handle_get(R32(o + 8));
    int64_t base = R32(o + 12), len = R32(o + 16), off = (int32_t)A(c, 1);
    int64_t cur = f ? (int64_t)f->pos - base : 0;
    int64_t p = A(c, 2) == 1 ? cur + off : A(c, 2) == 2 ? len + off : off;   // FILE_BEGIN / CURRENT / END
    if (p < 0) p = 0;
    if (p > len) p = len;
    if (f) f->pos = (uint64_t)(base + p);
    if (A(c, 3)) W32(A(c, 3), (uint32_t)p);
    RET(c, DS_OK, 4);
}
void wave_length(Cpu* c) {           // GetLength(pThis, pdwLength)
    if (A(c, 1)) W32(A(c, 1), R32(A(c, 0) + 16));
    RET(c, DS_OK, 2);
}

void install_vtables() {
    static const GuestFn stream_fns[] = {mo_addref, mo_release, mo_getinfo, stream_getstatus, stream_process,
                                         mo_noarg, stream_flush};
    static const GuestFn wave_fns[] = {mo_addref, mo_release, mo_getinfo, mo_getstatus, wave_process,
                                       mo_noarg, mo_noarg, wave_seek, wave_length};
    g_stream_vtbl = guest_alloc(0x80);
    g_wave_vtbl = guest_alloc(0x80);
    for (int i = 0; i < 7; i++) {
        uint32_t a = kVtblFns + 0x10 * i;
        register_fn(a, stream_fns[i]);
        W32(g_stream_vtbl + 4 * i, a);
    }
    for (int i = 0; i < 9; i++) {
        uint32_t b = kVtblFns + 0x100 + 0x10 * i;
        register_fn(b, wave_fns[i]);
        W32(g_wave_vtbl + 4 * i, b);
    }
}

uint32_t make_buffer(uint32_t desc) {
    // DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin }
    if (!g_stream_vtbl) install_vtables();
    uint32_t o = new_object(0);
    Buffer b;
    b.bytes = desc ? R32(desc + 8) : 0;
    if (b.bytes) b.data = guest_alloc(b.bytes);
    if (desc) b.fmt = read_format(R32(desc + 12));
    std::lock_guard<std::recursive_mutex> lk(g_audio_m);
    g_buffers[o] = b;
    return o;
}

// Adds one voice to the mix; returns false when a one-shot voice ran out.
template <class Next>
void mix_voice(float* out, int frames, double step, float gain, Next next) {
    float a[2], b[2];
    for (int i = 0; i < frames; i++) {
        double t;
        if (!next(a, b, &t)) return;
        out[2 * i] += (a[0] + (b[0] - a[0]) * (float)t) * gain;
        out[2 * i + 1] += (a[1] + (b[1] - a[1]) * (float)t) * gain;
    }
    (void)step;
}

void mix_stats(const float* out, int frames);

void mix(float* out, int frames) {
    std::memset(out, 0, sizeof(float) * 2 * frames);
    std::lock_guard<std::recursive_mutex> lk(g_audio_m);
    float master = setting_int("master_volume") / 100.0f;
    for (auto& [obj, b] : g_buffers) {
        if (!b.playing || !b.data || !b.bytes) continue;
        uint64_t total = total_frames(b.fmt, b.bytes);
        if (!total) { b.playing = false; continue; }
        double step = (double)(b.freq ? b.freq : b.fmt.rate) / kMixRate;
        Buffer* bp = &b;
        mix_voice(out, frames, step, b.gain * master, [&](float* x, float* y, double* t) {
            Buffer& v = *bp;
            if (v.frame >= (double)total) {
                if (!v.looping) { v.playing = false; v.frame = 0; return false; }
                v.frame = std::fmod(v.frame, (double)total);
            }
            uint64_t f0 = (uint64_t)v.frame, f1 = f0 + 1 < total ? f0 + 1 : (v.looping ? 0 : f0);
            *t = v.frame - (double)f0;
            if (!read_frame(v.fmt, v.data, v.bytes, f0, v.dc, x)) { v.playing = false; return false; }
            if (!read_frame(v.fmt, v.data, v.bytes, f1, v.dc, y)) std::memcpy(y, x, 8);
            v.frame += step;
            return true;
        });
    }
    for (auto& [obj, st] : g_streams) {
        if (st.paused) continue;
        double step = (double)st.fmt.rate / kMixRate;
        Stream* sp = &st;
        mix_voice(out, frames, step, st.gain * master, [&](float* x, float* y, double* t) {
            Stream& s = *sp;
            for (;;) {
                if (s.queue.empty()) return false;                 // underrun: silence
                const Packet& p = s.queue.front();
                uint64_t total = total_frames(s.fmt, p.size);
                if (s.frame < (double)total) break;
                s.frame -= (double)total;
                complete_packet(p, p.size);
                s.done.push_back(p);
                s.queue.pop_front();
                s.dc.block = -1;
            }
            const Packet& p = s.queue.front();
            uint64_t f0 = (uint64_t)s.frame;
            *t = s.frame - (double)f0;
            if (!read_frame(s.fmt, p.data, p.size, f0, s.dc, x)) { s.frame += step; std::memset(x, 0, 8); }
            if (!read_frame(s.fmt, p.data, p.size, f0 + 1, s.dc, y)) std::memcpy(y, x, 8);
            s.frame += step;
            return true;
        });
    }
    if (log_enabled("audio")) mix_stats(out, frames);
}

// TJE_LOG=audio: every 5 s, the output level and how many voices played.
void mix_stats(const float* out, int frames) {
    static double sum = 0;
    static uint64_t n = 0;
    static int voices_max = 0;
    static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int i = 0; i < frames * 2; i++) sum += (double)out[i] * out[i];
    n += (uint64_t)frames * 2;
    int voices = 0;
    for (auto& [o, b] : g_buffers) voices += b.playing;
    for (auto& [o, st] : g_streams) voices += !st.paused && !st.queue.empty();
    if (voices > voices_max) voices_max = voices;
    if (std::chrono::steady_clock::now() >= next) {
        hle_log("[audio] rms %.4f, up to %d voices, %zu buffers, %zu streams", std::sqrt(sum / (double)(n ? n : 1)),
                voices_max, g_buffers.size(), g_streams.size());
        sum = 0;
        n = 0;
        voices_max = 0;
        next += std::chrono::seconds(5);
    }
}

void start_mixer() {
    static bool started = false;
    if (started) return;
    started = true;
    audio_out_start(mix);
}

}  // namespace

void hle_0018ab9b(Cpu* c) {          // DirectSoundCreate(pGuid, ppDS, pUnkOuter)
    if (!g_stream_vtbl) install_vtables();
    start_mixer();
    static uint32_t ds = 0;
    if (!ds) ds = new_object(0);
    if (A(c, 1)) W32(A(c, 1), ds);
    RET(c, DS_OK, 3);
}
void hle_00186336(Cpu* c) { RET(c, 0, 0); }                                         // DirectSoundUseFullHRTF
void hle_00187eba(Cpu* c) {                                                          // DirectSoundDoWork
    std::vector<uint32_t> objs;
    {
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        for (auto& [o, st] : g_streams)
            if (!st.done.empty()) objs.push_back(o);
    }
    for (uint32_t o : objs) run_callbacks(c, o);
    RET(c, 0, 0);
}
void hle_001862ae(Cpu* c) { RET(c, 0, 1); }                                         // IDirectSound_Release
void hle_00187aa3(Cpu* c) { if (A(c, 1)) W32(A(c, 1), 0x00010000); RET(c, DS_OK, 2); }   // GetSpeakerConfig: stereo
void hle_00187abf(Cpu* c) {          // DownloadEffectsImage(pThis, pImage, dwSize, pImageLoc, ppImageDesc)
    if (A(c, 4)) {
        uint32_t d = guest_alloc(0x800);
        W32(d, 0);                   // dwEffectCount
        W32(A(c, 4), d);
    }
    RET(c, DS_OK, 5);
}
void hle_00189851(Cpu* c) { RET(c, DS_OK, 1); }                                     // CommitDeferredSettings
void hle_0018a5aa(Cpu* c) { RET(c, DS_OK, 8); }                                     // SetOrientation
void hle_0018a5f4(Cpu* c) { RET(c, DS_OK, 5); }                                     // SetPosition (listener)
void hle_0018a629(Cpu* c) { RET(c, DS_OK, 3); }                                     // SetRolloffFactor
void hle_0018a682(Cpu* c) { RET(c, DS_OK, 3); }                                     // SetI3DL2Listener
void hle_0018a9e1(Cpu* c) {          // IDirectSound_CreateSoundBuffer(pThis, pdsbd, ppBuffer, pUnkOuter)
    if (A(c, 2)) W32(A(c, 2), make_buffer(A(c, 1)));
    RET(c, DS_OK, 4);
}
void hle_0018abe2(Cpu* c) {          // DirectSoundCreateBuffer(pdsbd, ppBuffer)
    if (A(c, 1)) W32(A(c, 1), make_buffer(A(c, 0)));
    RET(c, DS_OK, 2);
}
void hle_0018ac39(Cpu* c) {          // DirectSoundCreateStream(pdssd, ppStream)
    if (!g_stream_vtbl) install_vtables();
    uint32_t s = new_object(g_stream_vtbl);
    // DSSTREAMDESC { dwFlags, dwMaxAttachedPackets, lpwfxFormat, lpfnCallback, lpvContext, lpMixBins }
    uint32_t d = A(c, 0);
    {
        std::lock_guard<std::recursive_mutex> lk(g_audio_m);
        Stream& st = g_streams[s];
        if (d) {
            st.max_packets = R32(d + 4) ? R32(d + 4) : 1;
            st.fmt = read_format(R32(d + 8));
            st.callback = R32(d + 12);
            st.context = R32(d + 16);
        }
    }
    if (A(c, 1)) W32(A(c, 1), s);
    RET(c, DS_OK, 2);
}

// IDirectSoundBuffer (every method takes the audio lock: the mixer reads the same state)
#define BUF(c) std::lock_guard<std::recursive_mutex> lk_(g_audio_m); Buffer& b = g_buffers[A(c, 0)]
void hle_001862e4(Cpu* c) { RET(c, 0, 1); }                                         // Release
void hle_00187bf1(Cpu* c) { BUF(c); b.gain = db_gain((int32_t)A(c, 1)); RET(c, DS_OK, 2); }   // SetVolume(pThis, lVolume)
void hle_00187c7d(Cpu* c) { RET(c, DS_OK, 2); }                                     // SetHeadroom
// Play(pThis, dwReserved1, dwReserved2, dwFlags): DSBPLAY_LOOPING = 1
void hle_00187cd1(Cpu* c) {
    BUF(c);
    b.looping = A(c, 3) & 1;
    b.playing = true;
    RET(c, DS_OK, 4);
}
void hle_00187d19(Cpu* c) { BUF(c); b.playing = false; RET(c, DS_OK, 1); }          // Stop
void hle_00187d31(Cpu* c) { BUF(c); b.playing = false; RET(c, DS_OK, 4); }          // StopEx(pThis, rt (8), flags)
void hle_00187d55(Cpu* c) {                                                          // Pause(pThis, dwPause)
    BUF(c);
    if (A(c, 1) == 1) b.playing = false;
    else if (A(c, 1) == 0) b.playing = true;
    RET(c, DS_OK, 2);
}
void hle_00187db5(Cpu* c) {                                                          // GetStatus(pThis, pdwStatus)
    BUF(c);
    if (A(c, 1)) W32(A(c, 1), b.playing ? (b.looping ? 5u : 1u) : 0u);           // DSBSTATUS_PLAYING | LOOPING
    RET(c, DS_OK, 2);
}
void hle_00187dd1(Cpu* c) {                                                          // GetCurrentPosition(pThis, pPlay, pWrite)
    BUF(c);
    uint32_t p = cursor(b);
    // The write cursor runs a little ahead of the play cursor (what the mixer has already read).
    uint32_t ahead = frames_to_bytes(b.fmt, (uint64_t)((b.freq ? b.freq : b.fmt.rate) / 100) + 65);
    if (A(c, 1)) W32(A(c, 1), p);
    if (A(c, 2)) W32(A(c, 2), b.bytes ? (p + ahead) % b.bytes : 0);
    RET(c, DS_OK, 3);
}
void hle_00187df1(Cpu* c) {                                                          // SetCurrentPosition(pThis, pos)
    BUF(c);
    b.frame = (double)bytes_to_frames(b.fmt, b.bytes ? A(c, 1) % b.bytes : 0);
    b.dc.block = -1;
    RET(c, DS_OK, 2);
}
void hle_00187e0d(Cpu* c) {          // Lock(pThis, off, bytes, pp1, pn1, pp2, pn2, flags)
    BUF(c);
    uint32_t off = A(c, 1), n = A(c, 2);
    if (!b.data || off + n > b.bytes) {          // grow on demand
        uint32_t need = off + n;
        uint32_t d = guest_alloc(need);
        if (b.data) { std::memcpy(gptr(d), gptr(b.data), b.bytes); guest_free(b.data); }
        b.data = d; b.bytes = need;
    }
    if (A(c, 3)) W32(A(c, 3), b.data + off);
    if (A(c, 4)) W32(A(c, 4), n);
    if (A(c, 5)) W32(A(c, 5), 0);
    if (A(c, 6)) W32(A(c, 6), 0);
    b.dc.block = -1;                              // the data under the cursor may change
    RET(c, DS_OK, 8);
}
void hle_001862c4(Cpu* c) { BUF(c); b.dc.block = -1; RET(c, DS_OK, 5); }            // Unlock
void hle_00189885(Cpu* c) { BUF(c); b.freq = A(c, 1); RET(c, DS_OK, 2); }           // SetFrequency(pThis, Hz; 0 = default)
void hle_001899be(Cpu* c) { RET(c, DS_OK, 5); }                                     // SetPosition (3D: not mixed yet)
void hle_00189ab8(Cpu* c) { RET(c, DS_OK, 3); }                                     // SetI3DL2Source
void hle_0018a6a2(Cpu* c) {          // SetBufferData(pThis, pvData, dwBytes): the game owns the memory
    BUF(c);
    b.data = A(c, 1);
    b.bytes = A(c, 2);
    b.dc.block = -1;
    if (b.frame >= (double)total_frames(b.fmt, b.bytes)) b.frame = 0;
    RET(c, DS_OK, 3);
}
#undef BUF

// IDirectSoundStream
void hle_00187e5d(Cpu* c) {                                                          // SetVolume(pThis, lVolume)
    std::lock_guard<std::recursive_mutex> lk(g_audio_m);
    g_streams[A(c, 0)].gain = db_gain((int32_t)A(c, 1));
    RET(c, DS_OK, 2);
}
void hle_00187e85(Cpu* c) {                                                          // Pause(pThis, dwPause)
    std::lock_guard<std::recursive_mutex> lk(g_audio_m);
    g_streams[A(c, 0)].paused = A(c, 1) == 1;          // DSSTREAMPAUSE_PAUSE
    RET(c, DS_OK, 2);
}
void hle_00189b74(Cpu* c) { RET(c, DS_OK, 5); }                                     // SetPosition (3D: not mixed yet)
void hle_00189c13(Cpu* c) { RET(c, DS_OK, 3); }                                     // SetI3DL2Source

// XWaveFileCreateMediaObject(pszFileName, ppwfxFormat, ppMediaObject)
void hle_001880e5(Cpu* c) {
    if (!g_stream_vtbl) install_vtables();
    std::string name = guest_cstr(A(c, 0));
    uint64_t size = 0;
    uint32_t h = open_host_file_for_guest(name, &size);
    if (!h) {
        hle_log("[dsound] XWaveFileCreateMediaObject: cannot open %s", name.c_str());
        RET(c, E_FAIL, 3);
        return;
    }
    // Parse the RIFF header: keep the format, position the file at the data chunk.
    KObject* f = handle_get(h);
    uint8_t hdr[12];
    std::fread(hdr, 1, 12, f->fp);
    uint32_t fmt = guest_alloc(0x40);
    uint64_t pos = 12;
    uint32_t data_len = 0;
    for (;;) {
        uint8_t ch[8];
        _fseeki64(f->fp, (int64_t)pos, SEEK_SET);
        if (std::fread(ch, 1, 8, f->fp) != 8) break;
        uint32_t len; std::memcpy(&len, ch + 4, 4);
        if (!std::memcmp(ch, "fmt ", 4)) std::fread(gptr(fmt), 1, len < 0x40 ? len : 0x40, f->fp);
        if (!std::memcmp(ch, "data", 4)) { f->pos = pos + 8; data_len = len; break; }
        pos += 8 + len + (len & 1);
    }
    uint32_t o = new_object(g_wave_vtbl);
    W32(o + 8, h);
    W32(o + 12, (uint32_t)f->pos);
    W32(o + 16, data_len);
    if (A(c, 1)) W32(A(c, 1), fmt);
    if (A(c, 2)) W32(A(c, 2), o);
    RET(c, DS_OK, 3);
}

}  // namespace tje
