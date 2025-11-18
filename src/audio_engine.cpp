#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_engine.hpp"
#include <cmath>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <chrono>

static ma_context gContext;
static bool gContextInited = false;
static ma_device gDevice;
static AudioParams* gParams = nullptr;

// Immutable event list pointer, swapped atomically.
static std::atomic<const std::vector<SeqEvent>*> gSeq{nullptr};
static uint64_t nextEvIndex = 0;
static std::atomic<bool>  gPlaying{false};
static std::atomic<uint64_t> gPlayhead{0}; // samples since play started

// Current active note for monophonic playback
static std::vector<float> gCurFreqs;
static std::vector<double> gCurPhases;
static std::vector<uint8_t> gCurStates;
static std::vector<double> gCurEnvs;
float gAttackLength = 0.01f;
float gReleaseLength = 0.01f;
// static float gCurFreq = 440.0f;
// static uint64_t gCurOff = 0;

float getSR(void) {
    return (float)gDevice.sampleRate;
}

bool audio_save_wav(const char* filepath,
                    const std::vector<SeqEvent>& eventsIn,
                    uint32_t sr,
                    float masterGain,
                    float attackSec,
                    float releaseSec)
{
    if (!filepath || eventsIn.empty()) return false;

    std::vector<SeqEvent> ev = eventsIn;
    std::sort(ev.begin(), ev.end(), [](const SeqEvent& a, const SeqEvent& b){
        if (a.sample != b.sample) return a.sample < b.sample;
        return a.type < b.type; // OFF(0) before ON(1)
    });

    uint64_t lastSample = ev.back().sample;
    const uint64_t tail = (uint64_t)std::ceil(std::max(0.0f, releaseSec) * sr) + (uint64_t)(0.05 * sr);
    uint64_t totalFrames = lastSample + tail;
    if (totalFrames < 1) totalFrames = 1;

    std::vector<float>  freqs;  std::vector<double> phase;
    std::vector<float>  env;    std::vector<uint8_t> state; // 1=attack,2=sustain,3=release
    const float atkStep = (attackSec  > 0.f) ? (1.0f / (attackSec  * sr)) : 1.0f;
    const float relStep = (releaseSec > 0.f) ? (1.0f / (releaseSec * sr)) : 1.0f;

    std::vector<float> out; out.resize((size_t)totalFrames * 2, 0.0f);

    FILE* outcsv = fopen("out.csv", "w+");
    FILE* outdiff = fopen("outdiff.csv", "w+");

    size_t nextEv = 0;
    for (uint64_t t = 0; t < totalFrames; ++t) {
        while (nextEv < ev.size() && ev[nextEv].sample == t && ev[nextEv].type == 0) {
            float f = ev[nextEv].freq;
            for (size_t i = 0; i < freqs.size(); ++i) {
                if (std::abs(freqs[i] - f) < 1e-6f && state[i] != 3) { state[i] = 3; break; }
            }
            ++nextEv;
        }
        while (nextEv < ev.size() && ev[nextEv].sample == t && ev[nextEv].type == 1) {
            float f = ev[nextEv].freq;
            bool exists = false;
            for (size_t i = 0; i < freqs.size(); ++i) {
                if (std::abs(freqs[i] - f) < 1e-6f) { state[i] = 1; env[i] = 0.0f; exists = true; break; }
            }
            if (!exists) { freqs.push_back(f); phase.push_back(0.0); env.push_back(0.0f); state.push_back(1); }
            ++nextEv;
        }

        float s = 0.0f;
        float prev = 0.0f;
        for (size_t i = 0; i < freqs.size(); ) {
            phase[i] += 2.0 * M_PI * (double)freqs[i] / (double)sr;
            if (phase[i] > 2.0 * M_PI) phase[i] -= 2.0 * M_PI;

            if (state[i] == 1) { env[i] += atkStep; if (env[i] >= 1.0f){ env[i]=1.0f; state[i]=2; } }
            else if (state[i] == 3) { env[i] -= relStep; if (env[i] <= 0.0f){
                freqs.erase(freqs.begin()+i);
                phase.erase(phase.begin()+i);
                env.erase(env.begin()+i);
                state.erase(state.begin()+i);
                continue; // don't ++i
            } }
            s += (float)std::sin(phase[i]) * env[i];
            ++i;
        }

        // if (!freqs.empty()) s *= 0.6f / (float)freqs.size();
        if (s > 1.0f) {
            printf("HOY!");
            s = 1.0;
        }
        if (s < -1.0f) {
            printf("HOE!");
            s = -1.0;
        }
        s *= masterGain;

        out[(size_t)t*2 + 0] = s;
        out[(size_t)t*2 + 1] = s;
        fprintf(outcsv, "%.3f\n", s);
        fprintf(outdiff, "%.3f\n", std::abs(s - prev));
        prev = s;
    }

    fclose(outcsv);
    fclose(outdiff);

    // ---- miniaudio v2 encoder usage (FIX) ----
    ma_encoder_config cfg = ma_encoder_config_init(
        ma_encoding_format_wav,   // <-- FIXED enum
        ma_format_f32,
        2,
        sr
    );

    ma_encoder enc;
    if (ma_encoder_init_file(filepath, &cfg, &enc) != MA_SUCCESS) {
        std::fprintf(stderr, "encoder init failed for %s\n", filepath);
        return false;
    }

    ma_uint64 framesWritten = 0;
    ma_result wr = ma_encoder_write_pcm_frames(
        &enc,
        out.data(),
        (ma_uint64)totalFrames,
        &framesWritten                 // <-- FIX: provide pointer
    );
    ma_encoder_uninit(&enc);

    if (wr != MA_SUCCESS || framesWritten != (ma_uint64)totalFrames) {
        std::fprintf(stderr, "encoder write failed: %d (wrote %llu/%llu)\n",
                     wr, (unsigned long long)framesWritten, (unsigned long long)totalFrames);
        return false;
    }
    return true;
}

double triangle(double x) {
    const double period = 2 * std::numbers::pi;
    double phase = std::fmod(x, period);
    if (phase < 0) phase += period;
    return (4.0 / period) * std::abs(phase - period / 2.0) - 1.0;
}

double square(double x) {
    const double period = 2 * std::numbers::pi;
    double phase = std::fmod(x, period);
    if (phase < 0) phase += period;
    return phase < (period / 2) ? 1 : -1;
}

int prevSize = 0;
float divisor = 1.0f;

static void data_callback(ma_device* device, void* out, const void*, ma_uint32 frames)
{
    float* o = (float*)out;
    const int ch = device->playback.channels;
    const float sr = (float)device->sampleRate;

    float ui_gain = gParams->gain.load(std::memory_order_relaxed);

    const float atkStep = (gAttackLength  > 0.f) ? (1.0f / (gAttackLength  * sr)) : 1.0f;
    const float relStep = (gReleaseLength > 0.f) ? (1.0f / (gReleaseLength * sr)) : 1.0f;

    for (ma_uint32 i = 0; i < frames; ++i) {
        float s = 0.f;

        if (gPlaying.load(std::memory_order_relaxed)) {
            // time in sequence
            uint64_t t = gPlayhead.load(std::memory_order_relaxed);
            auto seq = gSeq.load(std::memory_order_acquire);

            // If no active note or we just passed its end, (re)pick
            while (t == (*seq)[nextEvIndex].sample) {
                if ((*seq)[nextEvIndex].type == 1) {
                    auto it = std::find(gCurFreqs.begin(), gCurFreqs.end(), (*seq)[nextEvIndex].freq);

                    if (it == gCurFreqs.end()) {
                        gCurFreqs.push_back((*seq)[nextEvIndex].freq);
                        gCurPhases.push_back(0.0f);
                        gCurEnvs.push_back(0.0f);
                        gCurStates.push_back(1);
                    }
                } else {
                    for (size_t i = 0; i < gCurFreqs.size(); ++i) {
                        if (std::abs(gCurFreqs[i] - (*seq)[nextEvIndex].freq) < 1e-6f && gCurStates[i] != 3) { gCurStates[i] = 3; break; }
                    }
                }
                nextEvIndex++;
                if (nextEvIndex == (*seq).size()) nextEvIndex  = 0;
            }

            if (gCurFreqs.size() != 0) {
                s = 0.f;
                for (int fi = 0; fi < (int)gCurFreqs.size(); fi++) {
                    double dphi = 2.0 * M_PI * (double)gCurFreqs[fi] / (double)sr;
                    gCurPhases[fi] += dphi; if (gCurPhases[fi] > 2.0 * M_PI) gCurPhases[fi] -= 2.0 * M_PI;
                    if (gCurStates[fi] == 1) {
                        gCurEnvs[fi] += atkStep;
                        if (gCurEnvs[fi] >= 1.0f) {
                            gCurEnvs[fi] = 1.0f;
                            gCurStates[fi] = 2;
                        }
                    } else if (gCurStates[fi] == 3) {
                        gCurEnvs[fi] -= relStep;
                        if (gCurEnvs[fi] <= 0.0f) {
                            gCurFreqs.erase(gCurFreqs.begin() + fi);
                            gCurPhases.erase(gCurPhases.begin() + fi);
                            gCurEnvs.erase(gCurEnvs.begin() + fi);
                            gCurStates.erase(gCurStates.begin() + fi);
                        }
                    }
                    // s += (float)std::sin(gCurPhases[fi]) * gCurEnvs[fi];
                    s += (float)square(gCurPhases[fi]) * gCurEnvs[fi];
                    // printf("i: %d, ", fi);
                    // printf("f: %f, ", gCurFreqs[fi]);
                    // printf("ph: %f, ", gCurPhases[fi]);
                    // printf("a: %f, ", gCurAttacks[fi]);
                    // printf("d: %f, \n", gCurDecays[fi]);
                    // std::fflush(stdout);
                }
            } else {
                // rest
                s = 0.f;
            }

            gPlayhead.store(t + 1, std::memory_order_relaxed);
        } else {
            // Free play: simple sine from UI freq
            // float f = gParams->freq.load(std::memory_order_relaxed);
            // double dphi = 2.0 * M_PI * (double)f / (double)sr;
            // gPhase += dphi; if (gPhase > 2.0 * M_PI) gPhase -= 2.0 * M_PI;
            // s = (float)std::sin(gPhase);
        }

        s *= ui_gain;

        if ((std::abs(divisor - (int)gCurFreqs.size()) > 1e-6)) {
            divisor += ((int)gCurFreqs.size() - divisor) / 100.0;
        } else {
            divisor = (int)gCurFreqs.size();
        }
        if ((int)gCurFreqs.size() != 0)
            s = s / divisor;

        if (s > 1.0f) {
            printf("HEY!");
            s = 1.0f;
        }
        if (s < -1.0f) {
            printf("HEE!");
            s = -1.0f;
        }
        for (int c = 0; c < ch; ++c) *o++ = s;
    }
}

bool audio_start(AudioParams& params)
{
    gParams = &params;

    ma_backend backends[] = { ma_backend_pulseaudio, ma_backend_alsa, ma_backend_jack };
    ma_context_config cconfig = ma_context_config_init();
    ma_result r = ma_context_init(backends, (ma_uint32)(sizeof(backends)/sizeof(backends[0])), &cconfig, &gContext);
    if (r != MA_SUCCESS) { std::fprintf(stderr, "ma_context_init failed: %d\n", r); return false; }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 48000;
    cfg.dataCallback      = data_callback;

    r = ma_device_init(&gContext, &cfg, &gDevice);
    if (r != MA_SUCCESS) { std::fprintf(stderr, "ma_device_init failed: %d\n", r); ma_context_uninit(&gContext); return false; }

    std::fprintf(stderr, "[audio] sr=%u ch=%u\n", gDevice.sampleRate, gDevice.playback.channels);
    r = ma_device_start(&gDevice);
    if (r != MA_SUCCESS) { 
        std::fprintf(stderr, "ma_device_start failed: %d\n", r); 
        ma_device_uninit(&gDevice); 
        ma_context_uninit(&gContext);
        gContextInited = false;
        return false; 
    }
    return true;
}

void audio_stop()
{
    ma_device_uninit(&gDevice);
    ma_context_uninit(&gContext);
    gContextInited = false;
}

// === Sequencer API ===
void audio_load_sequence(const std::vector<SeqEvent>& events)
{
    // Make an immutable copy and publish atomically.
    auto* v = new std::vector<SeqEvent>(events);
    // Ensure sorted by sample_on
    std::sort(v->begin(), v->end(), [](auto& a, auto& b){ return a.sample < b.sample; });
    const auto* old = gSeq.exchange(v, std::memory_order_acq_rel);
    if (old) delete old;
}

void audio_play(bool on)
{
    if (on) {
        gPlayhead.store(0, std::memory_order_relaxed);
        //gPhase = 0.0;
        gPlaying.store(true, std::memory_order_release);
    } else {
        gPlaying.store(false, std::memory_order_release);
    }
}

bool audio_is_playing()
{
    return gPlaying.load(std::memory_order_acquire);
}

void audio_set_gain(float g)
{
    gParams->gain.store(g, std::memory_order_relaxed);
}
