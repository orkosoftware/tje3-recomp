// Sound output: a host thread asks the mixer for 48 kHz stereo audio in 5 ms
// slices and plays it with waveOut (Windows' basic audio API, always present).
// Without a device (or with TJE_NO_AUDIO set) the thread keeps the same pace
// and discards the audio, so the game's audio clock behaves the same.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#elif defined(__ANDROID__)
#include <aaudio/AAudio.h>
#include "gpu/window.h"
#endif

#include "audio_out.h"
#include "guest.h"

namespace tje {

namespace {

constexpr int kRate = 48000, kSliceFrames = 240, kSlices = 10;     // 5 ms slices, 50 ms queued

void silent_loop(MixFn mix) {
    std::vector<float> buf(kSliceFrames * 2);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        mix(buf.data(), kSliceFrames);
        next += std::chrono::microseconds(1000000LL * kSliceFrames / kRate);
        std::this_thread::sleep_until(next);
    }
}

}  // namespace

void audio_out_start(MixFn mix) {
    std::thread([mix] {
#ifdef _WIN32
        if (!std::getenv("TJE_NO_AUDIO")) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = kRate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = 4;
            wf.nAvgBytesPerSec = kRate * 4;
            HWAVEOUT wo = nullptr;
            if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) == MMSYSERR_NOERROR) {
                hle_log("[audio] waveOut 48 kHz stereo");
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
                std::vector<std::vector<int16_t>> data(kSlices, std::vector<int16_t>(kSliceFrames * 2));
                std::vector<WAVEHDR> hdr(kSlices);
                std::vector<float> mixbuf(kSliceFrames * 2);
                auto fill = [&](int i) {
                    mix(mixbuf.data(), kSliceFrames);
                    for (int k = 0; k < kSliceFrames * 2; k++) {
                        float v = mixbuf[k] * 32767.0f;
                        data[i][k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
                    }
                    hdr[i] = WAVEHDR{};
                    hdr[i].lpData = (LPSTR)data[i].data();
                    hdr[i].dwBufferLength = kSliceFrames * 4;
                    waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
                    waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR));
                };
                for (int i = 0; i < kSlices; i++) fill(i);
                for (;;) {
                    WaitForSingleObject(ev, 20);
                    for (int i = 0; i < kSlices; i++)
                        if (hdr[i].dwFlags & WHDR_DONE) {
                            waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
                            fill(i);
                        }
                }
            }
            hle_log("[audio] no audio device: running silently");
        }
#elif defined(__ANDROID__)
        AAudioStreamBuilder* builder = nullptr;
        AAudioStream* stream = nullptr;
        if (!std::getenv("TJE_NO_AUDIO") && AAudio_createStreamBuilder(&builder) == AAUDIO_OK) {
            AAudioStreamBuilder_setSampleRate(builder, kRate);
            AAudioStreamBuilder_setChannelCount(builder, 2);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
            if (AAudioStreamBuilder_openStream(builder, &stream) != AAUDIO_OK) stream = nullptr;
            AAudioStreamBuilder_delete(builder);
        }
        if (stream && AAudioStream_requestStart(stream) == AAUDIO_OK) {
            hle_log("[audio] AAudio %d Hz stereo", AAudioStream_getSampleRate(stream));
            std::vector<float> mixbuf(kSliceFrames * 2);
            std::vector<int16_t> data(kSliceFrames * 2);
            for (;;) {
                if (!gpu::window_foreground()) {         // silent while the player is away
                    AAudioStream_requestPause(stream);
                    gpu::window_wait_foreground();
                    AAudioStream_requestStart(stream);
                }
                mix(mixbuf.data(), kSliceFrames);
                for (int k = 0; k < kSliceFrames * 2; k++) {
                    float v = mixbuf[k] * 32767.0f;
                    data[k] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
                }
                AAudioStream_write(stream, data.data(), kSliceFrames, 100000000);   // blocks: sets the pace
            }
        }
        hle_log("[audio] no audio device: running silently");
#endif
        silent_loop(mix);
    }).detach();
}

}  // namespace tje
