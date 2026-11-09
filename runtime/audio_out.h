#pragma once

namespace tje {

// Fills `frames` interleaved stereo float samples at 48 kHz.
using MixFn = void (*)(float* stereo, int frames);

// Starts the output thread; it calls `mix` at the playback pace (with or without a device).
void audio_out_start(MixFn mix);

}  // namespace tje
