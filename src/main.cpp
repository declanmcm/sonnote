#include <SDL.h>
// #include "imgui.h"
#include "vulkan_renderer.hpp"
// #include "backends/imgui_impl_sdl2.h"
// #include "backends/imgui_impl_opengl3.h"
#include <cstdio>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cmath>
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <iostream>
#include <sndfile.h>
#include <fftw3.h>

#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION
  #include <OpenGL/gl3.h>
#else
  #include <GL/gl.h>
  #include <GL/glu.h>
#endif


#include "audio_engine.hpp"

struct Note {
    float beat_on;   // inclusive
    float beat_off;  // exclusive
    int velocity;
    float octave;    // fixed at first click
    float origin;
    bool selected;
    int profile;
};

struct FocusChange {
    float time;
    float octave;
};

struct MidiEvent {
    double tick;
    std::vector<uint8_t> data; 
};

struct TreeNode {
    int value;
    std::vector<std::unique_ptr<TreeNode>> children;

    explicit TreeNode(int val) : value(val) {}

    // Add a child and return a pointer to it
    TreeNode* addChild(int val) {
        children.push_back(std::make_unique<TreeNode>(val));
        return children.back().get();
    }
};

struct Rect {
    float x;
    float y;
    float w;
    float h;
    float r;
    float g;
    float b;
    float utility;
    int function;
};

struct Button {
    float x1;
    float y1;
    float x2;
    float y2;
    char* label;
    bool hit;
    float value;
    char* text;
};

struct CurrentBox {
    float x1;
    float y1;
    float x2;
    float y2;
};

#define F_MARGIN_PANEL 0
#define F_BORDER 1
#define F_NOTE_WINDOW 2
#define F_CONTROL_WINDOW 3
#define F_BUTTON_CW 4
#define F_SLIDER_CW 5
#define F_TEXT_BOX_CW 6

#define NW_HEIGHT 0.75f

constexpr int FFT_SIZE = 2048;
constexpr int HOP_SIZE = 512;
constexpr int NUM_BINS = FFT_SIZE / 2 + 1;

TextRenderer g_textRenderer;
VulkanDeviceContext g_deviceContext; // You'd need to populate this
VkRenderPass g_renderPass;           // Your main render pass

// Recursive print function
void printTree(const TreeNode* node, int depth = 0) {
    if (!node) return;
    printf("%d %d\n", depth, node->value);
    // std::cout << std::string(depth * 2, ' ') << node->value << "\n";
    for (const auto& child : node->children) {
        printTree(child.get(), depth + 1);
    }
}

TreeNode* findNode(TreeNode* node, int target) {
    if (!node) return nullptr;

    for (auto& child : node->children) {
        if (child->value == target) {
            return child.get();
        }
    }
    return nullptr;
}

struct AudioData {
    int sampleRate = 0;
    std::vector<float> mono;   // [-1,1] float samples
};

inline AudioData loadWavMono(const std::string& path)
{
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) throw std::runtime_error("sf_open failed");

    std::vector<float> interleaved((size_t)info.frames * (size_t)info.channels);
    sf_readf_float(f, interleaved.data(), info.frames);
    sf_close(f);

    AudioData out;
    out.sampleRate = info.samplerate;
    out.mono.resize((size_t)info.frames);

    if (info.channels == 1) {
        out.mono = std::move(interleaved);
    } else {
        for (sf_count_t i = 0; i < info.frames; ++i) {
            double s = 0.0;
            for (int ch = 0; ch < info.channels; ++ch)
                s += interleaved[(size_t)i * info.channels + ch];
            out.mono[(size_t)i] = (float)(s / info.channels);
        }
    }
    return out;
}

struct SpectrogramState {
    bool loaded = false;
    float durationSeconds = 0.0f;
    int textureWidth = 0;   // columns
};

SpectrogramState spectrogram;

struct STFTMag {
    int sampleRate = 0;
    int fftSize = 0;
    int hopSize = 0;
    int numBins = 0;           // fftSize/2+1
    int numFrames = 0;
    std::vector<float> mag;    // flattened: mag[frame*numBins + bin]
};

inline std::vector<float> makeHann(int n)
{
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1)));
    return w;
}

inline STFTMag computeSTFT_FFTW(
    const std::vector<float>& samples,
    int sampleRate,
    int fftSize = 2048,
    int hopSize = 512
) {
    if ((int)samples.size() < fftSize) throw std::runtime_error("audio too short for fftSize");

    STFTMag out;
    out.sampleRate = sampleRate;
    out.fftSize = fftSize;
    out.hopSize = hopSize;
    out.numBins = fftSize / 2 + 1;
    out.numFrames = ((int)samples.size() - fftSize) / hopSize + 1;
    out.mag.assign((size_t)out.numFrames * (size_t)out.numBins, 0.0f);

    auto window = makeHann(fftSize);

    std::vector<float> in((size_t)fftSize);
    std::vector<fftwf_complex> spec((size_t)out.numBins);

    fftwf_plan plan = fftwf_plan_dft_r2c_1d(
        fftSize, in.data(), spec.data(), FFTW_MEASURE
    );

    for (int frame = 0; frame < out.numFrames; ++frame) {
        int offset = frame * hopSize;

        for (int i = 0; i < fftSize; ++i)
            in[(size_t)i] = samples[(size_t)offset + (size_t)i] * window[(size_t)i];

        fftwf_execute(plan);

        float* dst = out.mag.data() + (size_t)frame * (size_t)out.numBins;
        for (int b = 0; b < out.numBins; ++b) {
            float re = spec[(size_t)b][0];
            float im = spec[(size_t)b][1];
            dst[(size_t)b] = sqrtf(re * re + im * im);
        }
    }

    fftwf_destroy_plan(plan);
    return out;
}

inline void magToDbInPlace(STFTMag& stft, float floorDb = -80.0f)
{
    for (float& v : stft.mag) {
        float db = 20.0f * log10f(v + 1e-9f);
        if (db < floorDb) db = floorDb;
        v = db;
    }
}

inline void normalize01InPlace(std::vector<float>& a)
{
    float mn = 1e30f, mx = -1e30f;
    for (float v : a) { mn = std::min(mn, v); mx = std::max(mx, v); }
    float range = (mx - mn);
    if (range < 1e-12f) range = 1.0f;
    for (float& v : a) v = (v - mn) / range;
}

struct LogSpec {
    int width = 0;   // time columns (frames or downsampled)
    int height = 0;  // log rows
    std::vector<float> v; // [y*width + x] in [0,1]
};

// helper
inline float binHz(int bin, int sampleRate, int fftSize)
{
    return (float)bin * (float)sampleRate / (float)fftSize;
}

inline LogSpec remapToLogFreq(
    const STFTMag& stft,
    int rows = 512,
    float fMin = 20.0f,
    float fMax = 20000.0f
) {
    LogSpec out;
    out.width  = stft.numFrames;
    out.height = rows;
    out.v.assign((size_t)out.width * (size_t)out.height, 0.0f);

    // Precompute bin edges for each log row:
    // row r covers [f0,f1) where f is exponentially spaced.
    std::vector<int> binLo((size_t)rows), binHi((size_t)rows);

    const float logMin = logf(fMin);
    const float logMax = logf(fMax);

    for (int r = 0; r < rows; ++r) {
        float t0 = (float)r / (float)rows;
        float t1 = (float)(r + 1) / (float)rows;
        float f0 = expf(logMin + (logMax - logMin) * t0);
        float f1 = expf(logMin + (logMax - logMin) * t1);

        int lo = (int)floorf(f0 * stft.fftSize / stft.sampleRate);
        int hi = (int)ceilf (f1 * stft.fftSize / stft.sampleRate);

        lo = std::max(lo, 0);
        hi = std::min(hi, stft.numBins - 1);
        if (hi <= lo) hi = std::min(lo + 1, stft.numBins - 1);

        binLo[(size_t)r] = lo;
        binHi[(size_t)r] = hi;
    }

    // Average magnitude in each band
    for (int x = 0; x < stft.numFrames; ++x) {
        const float* frame = stft.mag.data() + (size_t)x * (size_t)stft.numBins;

        for (int r = 0; r < rows; ++r) {
            int lo = binLo[(size_t)r];
            int hi = binHi[(size_t)r];

            double sum = 0.0;
            int count = 0;
            for (int b = lo; b <= hi; ++b) { sum += frame[(size_t)b]; count++; }

            // IMPORTANT: If you want low freq at bottom, flip r in storage or shader.
            out.v[(size_t)r * (size_t)out.width + (size_t)x] = (float)(sum / std::max(1, count));
        }
    }

    // normalize to 0..1 (assumes input is already dB-ish)
    normalize01InPlace(out.v);
    return out;
}

inline LogSpec downsampleTimeMax(const LogSpec& in, int targetWidth)
{
    if (targetWidth >= in.width) return in;

    LogSpec out;
    out.width  = targetWidth;
    out.height = in.height;
    out.v.assign((size_t)out.width * (size_t)out.height, 0.0f);

    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            int x0 = (int)((int64_t)x     * in.width / targetWidth);
            int x1 = (int)((int64_t)(x+1) * in.width / targetWidth);
            if (x1 <= x0) x1 = x0 + 1;

            float m = 0.0f;
            for (int xi = x0; xi < x1; ++xi) {
                m = std::max(m, in.v[(size_t)y * (size_t)in.width + (size_t)xi]);
            }
            out.v[(size_t)y * (size_t)out.width + (size_t)x] = m;
        }
    }
    return out;
}

struct GrayImage {
    int w=0, h=0;
    std::vector<uint8_t> px; // size w*h
};

inline GrayImage toR8(const LogSpec& spec, bool flipY = true)
{
    GrayImage img;
    img.w = spec.width;
    img.h = spec.height;
    img.px.resize((size_t)img.w * (size_t)img.h);

    for (int y = 0; y < img.h; ++y) {
        int sy = flipY ? (img.h - 1 - y) : y; // flip so low freq is bottom if desired
        for (int x = 0; x < img.w; ++x) {
            float v = spec.v[(size_t)sy * (size_t)img.w + (size_t)x];
            int iv = (int)lrintf(v * 255.0f);
            iv = std::min(255, std::max(0, iv));
            img.px[(size_t)y * (size_t)img.w + (size_t)x] = (uint8_t)iv;
        }
    }
    return img;
}

inline GrayImage buildSpectrogramTextureR8(
    const std::string& wavPath,
    int fftSize = 2048,
    int hopSize = 512,
    int logRows = 512,
    int targetWidth = 4096,
    float floorDb = -80.0f,
    float fMin = 20.0f,
    float fMax = 20000.0f
) {
    AudioData a = loadWavMono(wavPath);

    STFTMag stft = computeSTFT_FFTW(a.mono, a.sampleRate, fftSize, hopSize);
    magToDbInPlace(stft, floorDb);

    // note: remapToLogFreq expects stft.mag in “some scale”; dB works best.
    LogSpec logSpec = remapToLogFreq(stft, logRows, fMin, fMax);
    LogSpec ds = downsampleTimeMax(logSpec, targetWidth);

    return toR8(ds, /*flipY=*/true);
}


float perfectIntervals[] = {
    3.0 / 2.0, // Fifth
    4.0 / 3.0, // Fourth
    5.0 / 4.0, // Major third
    6.0 / 5.0, // Minor third
    7.0 / 6.0, // Major second
    8.0 / 7.0,
    9.0 / 8.0,
    16.0 / 15.0, // Diatonic semitone
    7.0 / 5.0, // Tritone
    // Inversions
    8.0 / 5.0,
    10.0 / 6.0,
    12.0 /  7.0,
    7.0 / 4.0,
    16.0 / 9.0,
    15.0 / 8.0,
    10.0 / 7.0
};

struct Vec2 {
    float x;
    float y;
};

float playhead_time = 0.0f;
bool play_triggered_from_space = false;

static char jump[64] = "";
static char split[64] = "";

// 5.25
// 5.375
// 5.027777

// + 0.125
// - 0.3472222

float c4 = 220.0f * std::pow(2.0f, 3.0/12.0);

// Map octave -> frequency (C4 at octave 4)
static inline float octave_to_freq(float octave) {
    return c4 * std::pow(2.0f, octave - 4.0f);
}

// Helper: big-endian writers
static void writeBE16(std::ofstream& f, uint16_t v) {
    f.put((v >> 8) & 0xFF);
    f.put(v & 0xFF);
}
static void writeBE32(std::ofstream& f, uint32_t v) {
    f.put((v >> 24) & 0xFF);
    f.put((v >> 16) & 0xFF);
    f.put((v >> 8) & 0xFF);
    f.put(v & 0xFF);
}

template<typename Stream>
static void writeVarLen(Stream& f, uint32_t value) {
    uint8_t buf[5];
    int pos = 0;
    buf[pos++] = value & 0x7F;
    while ((value >>= 7))
        buf[pos++] = 0x80 | (value & 0x7F);
    for (int i = pos - 1; i >= 0; --i)
        f.put(buf[i]);
}

// Multichannel (MPE)
bool save_midi_file_mpe(const std::string& path,
                        const std::vector<Note>& notes,
                        float bpm)
{
    if (notes.empty()) return false;

    const int ticksPerQuarter = 480;
    const double secPerBeat = 60.0 / bpm;
    const double ticksPerSec = ticksPerQuarter / secPerBeat;
    const int firstChannel = 1;   // skip ch 0 (Bitwig expects 1–15)
    const int lastChannel  = 15;

    // Sort by start time
    std::vector<Note> sorted = notes;
    std::sort(sorted.begin(), sorted.end(),
              [](const Note& a, const Note& b){ return a.beat_on < b.beat_on; });

    // Event structure
    struct Ev { double tick; std::vector<uint8_t> data; };
    std::vector<Ev> events;

    // Track active notes by channel
    std::map<int, double> channelBusyUntil;
    for (int ch = firstChannel; ch <= lastChannel; ++ch)
        channelBusyUntil[ch] = 0.0;

    for (auto& n : sorted) {
        double startTick = n.beat_on * ticksPerQuarter;
        double endTick   = n.beat_off * ticksPerQuarter;

        // find a free channel
        int chosen = firstChannel;
        for (int ch = firstChannel; ch <= lastChannel; ++ch) {
            if (channelBusyUntil[ch] <= startTick) { chosen = ch; break; }
        }
        channelBusyUntil[chosen] = endTick;

        // convert octave → MIDI + pitch bend
        double freq = octave_to_freq(n.octave);
        double midiNoteFloat = 69 + 12.0 * std::log2(freq / 440.0);
        int baseNote = std::clamp((int)std::round(midiNoteFloat), 0, 127);
        double semitoneDiff = midiNoteFloat - baseNote;
        int bendValue = std::clamp((int)std::round(8192 + semitoneDiff * (8192 / 2.0)), 0, 16383);

        // Pitch bend event
        events.push_back({startTick, {uint8_t(0xE0 | chosen),
                                      uint8_t(bendValue & 0x7F),
                                      uint8_t((bendValue >> 7) & 0x7F)}});
        // Note on
        events.push_back({startTick, {uint8_t(0x90 | chosen),
                                      uint8_t(baseNote), 100}});
        // Note off
        events.push_back({endTick,   {uint8_t(0x80 | chosen),
                                      uint8_t(baseNote), 0}});
    }

    // Sort all events by time
    std::sort(events.begin(), events.end(),
              [](auto& a, auto& b){ return a.tick < b.tick; });

    // ----- Write file -----
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // f.write("MThd", 4);
    f.put('M'); f.put('T'); f.put('h'); f.put('d');
    writeBE32(f, 6);
    writeBE16(f, 1);  // format 0
    writeBE16(f, 1);  // 1 track
    writeBE16(f, ticksPerQuarter);

    std::ostringstream track;
    double lastTick = 0.0;

    // (Optional) MPE pitch bend range setup (±2 semitones)
    writeVarLen(track, 0);
    for (int ch = firstChannel; ch <= lastChannel; ++ch) {
        track.put(0xB0 | ch); track.put(0x65); track.put(0x00);
        track.put(0xB0 | ch); track.put(0x64); track.put(0x00);
        track.put(0xB0 | ch); track.put(0x06); track.put(0x02);
        track.put(0xB0 | ch); track.put(0x26); track.put(0x00);
    }

    for (int ch = firstChannel; ch <= lastChannel; ++ch) {
        track.put(0xB0 | ch); track.put(0x65); track.put(0x00);
        track.put(0xB0 | ch); track.put(0x64); track.put(0x00);
        track.put(0xB0 | ch); track.put(0x06); track.put(0x02);
        track.put(0xB0 | ch); track.put(0x26); track.put(0x00);
    }

    // Write all events
    for (auto& e : events) {
        uint32_t delta = (uint32_t)std::max(0.0, e.tick - lastTick);
        writeVarLen(track, delta);
        for (uint8_t b : e.data) track.put(b);
        lastTick = e.tick;
    }

    // End of track
    writeVarLen(track, 0);
    track.put(0xFF); track.put(0x2F); track.put(0x00);

    std::string data = track.str();
    // f.write("MTrk", 4);
    f.put('M'); f.put('T'); f.put('r'); f.put('k');
    writeBE32(f, data.size());
    f.write(data.data(), data.size());
    f.close();

    return true;
}

// Export notes to a simple Format 0 MIDI with pitch bend for fractional octaves
bool save_midi_file(const std::string& path,
                    const std::vector<Note>& notes,
                    float bpm)
{
    if (notes.empty()) return false;

    const int ticksPerQuarter = 480;
    const double secPerBeat = 60.0 / bpm;
    const double ticksPerSec = ticksPerQuarter / secPerBeat;

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // ===== Header =====
    f.write("MThd", 4);
    writeBE32(f, 6);
    writeBE16(f, 0);  // format 0
    writeBE16(f, 1);  // 1 track
    writeBE16(f, ticksPerQuarter);

    // ===== Track data =====
    std::ostringstream track;
    double lastTick = 0.0;

    auto sorted = notes;
    std::sort(sorted.begin(), sorted.end(),
              [](const Note& a, const Note& b){ return a.beat_on < b.beat_on; });

    std::vector<MidiEvent> events;

    for (auto& n : notes) {
        double startTick = n.beat_on * ticksPerQuarter;
        double endTick   = n.beat_off * ticksPerQuarter;

        double freq = octave_to_freq(n.octave);
        double midiNoteFloat = 69 + 12.0 * std::log2(freq / 440.0);
        int baseNote = std::clamp((int)std::round(midiNoteFloat), 0, 127);

        double semitoneDiff = midiNoteFloat - baseNote;
        int bendValue = (int)std::round(8192 + semitoneDiff * (8192 / 2.0));
        bendValue = std::clamp(bendValue, 0, 16383);

        // Pitch bend event
        events.push_back({ startTick, { (uint8_t)0xE0,
                                    (uint8_t)(bendValue & 0x7F),
                                    (uint8_t)((bendValue >> 7) & 0x7F) } });
        // Note on
        events.push_back({ startTick, { 0x90, (uint8_t)baseNote, 100 } });
        // Note off
        events.push_back({ endTick,   { 0x80, (uint8_t)baseNote, 0 } });
    }

    // Sort all events by tick
    std::sort(events.begin(), events.end(),
            [](auto& a, auto& b){ return a.tick < b.tick; });

    // Now write all events with correct delta times
    for (auto& e : events) {
        uint32_t delta = (uint32_t)std::max(0.0, e.tick - lastTick);
        writeVarLen(track, delta);
        for (uint8_t b : e.data) track.put(b);
        lastTick = e.tick;
    }

    // End of track
    writeVarLen(track, 0);
    track.put(0xFF); track.put(0x2F); track.put(0x00);

    std::string data = track.str();
    f.write("MTrk", 4);
    writeBE32(f, data.size());
    f.write(data.data(), data.size());
    f.close();
    return true;
}

bool save_midi_file_mpe_format1(const std::string& path,
                                const std::vector<Note>& notes,
                                float bpm)
{
    if (notes.empty()) return false;

    const int ticksPerQuarter = 480;
    const double secPerBeat = 60.0 / bpm;
    const double ticksPerSec = ticksPerQuarter / secPerBeat;
    const int firstChannel = 1;   // global ch 1
    const int firstVoiceCh = 2;   // voice channels 2–16
    const int lastVoiceCh  = 16;
    const int numVoiceCh   = lastVoiceCh - firstVoiceCh + 1;
    const int totalTracks  = 1 + numVoiceCh;  // tempo + voices

    // Sort notes
    std::vector<Note> sorted = notes;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b){ return a.beat_on < b.beat_on; });

    // Assign channels round-robin (simple)
    std::vector<int> noteChannels(sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i)
        noteChannels[i] = firstVoiceCh + (i % numVoiceCh);

    // --- open file ---
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // ===== HEADER =====
    f.put('M'); f.put('T'); f.put('h'); f.put('d');
    writeBE32(f, 6);
    writeBE16(f, 1);                  // format 1
    writeBE16(f, totalTracks);        // # tracks
    writeBE16(f, ticksPerQuarter);

    // ===== TRACK 0  (tempo/global) =====
    {
        std::ostringstream tr;
        // tempo 120 BPM
        writeVarLen(tr, 0);
        tr.put(0xFF); tr.put(0x51); tr.put(0x03);
        tr.put(0x07); tr.put(0xA1); tr.put(0x20);
        // name
        writeVarLen(tr, 0);
        tr.put(0xFF); tr.put(0x03);
        std::string name = "Sonnote MPE Export";
        tr.put((uint8_t)name.size());
        tr.write(name.data(), name.size());
        // end
        writeVarLen(tr, 0);
        tr.put(0xFF); tr.put(0x2F); tr.put(0x00);

        std::string data = tr.str();
        f.put('M'); f.put('T'); f.put('r'); f.put('k');
        writeBE32(f, data.size());
        f.write(data.data(), data.size());
    }

    // ===== PER-CHANNEL TRACKS =====
    for (int ch = firstVoiceCh; ch <= lastVoiceCh; ++ch) {
        std::ostringstream tr;
        double lastTick = 0.0;

        // Set pitch-bend range ±2 semitones
        writeVarLen(tr, 0);
        tr.put(0xB0 | ch); tr.put(0x65); tr.put(0x00);
        writeVarLen(tr, 0);
        tr.put(0xB0 | ch); tr.put(0x64); tr.put(0x00);
        writeVarLen(tr, 0);
        tr.put(0xB0 | ch); tr.put(0x06); tr.put(0x02);
        writeVarLen(tr, 0);
        tr.put(0xB0 | ch); tr.put(0x26); tr.put(0x00);

        // Notes for this channel
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (noteChannels[i] != ch) continue;
            auto& n = sorted[i];
            double startTick = n.beat_on * ticksPerQuarter;
            double endTick   = n.beat_off * ticksPerQuarter;
            double freq = octave_to_freq(n.octave);
            double midiNoteFloat = 69 + 12.0 * std::log2(freq / 440.0);
            int baseNote = std::clamp((int)std::round(midiNoteFloat), 0, 127);
            double semitoneDiff = midiNoteFloat - baseNote;
            int bendValue = std::clamp(
                (int)std::round(8192 + semitoneDiff * (8192 / 2.0)), 0, 16383);

            // pitch bend
            writeVarLen(tr, (uint32_t)std::max(0.0, startTick - lastTick));
            tr.put(0xE0 | ch);
            tr.put(bendValue & 0x7F);
            tr.put((bendValue >> 7) & 0x7F);
            lastTick = startTick;

            // note on
            writeVarLen(tr, 0);
            tr.put(0x90 | ch); tr.put(baseNote); tr.put(100);

            // note off
            writeVarLen(tr, (uint32_t)(endTick - startTick));
            tr.put(0x80 | ch); tr.put(baseNote); tr.put(0);
            lastTick = endTick;
        }

        // end of track
        writeVarLen(tr, 0);
        tr.put(0xFF); tr.put(0x2F); tr.put(0x00);

        std::string data = tr.str();
        f.put('M'); f.put('T'); f.put('r'); f.put('k');
        writeBE32(f, data.size());
        f.write(data.data(), data.size());
    }

    f.close();
    return true;
}

// --- Write TreeNode recursively ---
void writeTreeNode(std::ofstream& f, const TreeNode& node)
{
    // Write node value
    f.write(reinterpret_cast<const char*>(&node.value), sizeof(node.value));

    // Write number of children
    uint32_t numChildren = static_cast<uint32_t>(node.children.size());
    f.write(reinterpret_cast<const char*>(&numChildren), sizeof(numChildren));

    // Write each child recursively
    for (const auto& child : node.children) {
        writeTreeNode(f, *child);
    }
}

// --- Read TreeNode recursively ---
std::unique_ptr<TreeNode> readTreeNode(std::ifstream& f)
{
    int value = 0;
    f.read(reinterpret_cast<char*>(&value), sizeof(value));

    auto node = std::make_unique<TreeNode>(value);

    uint32_t numChildren = 0;
    f.read(reinterpret_cast<char*>(&numChildren), sizeof(numChildren));

    for (uint32_t i = 0; i < numChildren; ++i) {
        node->children.push_back(readTreeNode(f));
    }

    return node;
}

// --- Write binary song file ---
bool save_song(const std::string& path,
               const std::vector<Note>& notes,
               const std::vector<FocusChange>& focusChanges, const TreeNode& root, Profile* profiles)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // Magic header
    f.write("SONN", 4);

    // Notes
    uint32_t numNotes = static_cast<uint32_t>(notes.size());
    f.write(reinterpret_cast<const char*>(&numNotes), sizeof(numNotes));
    for (auto& n : notes) {
        f.write(reinterpret_cast<const char*>(&n.beat_on), sizeof(float));
        f.write(reinterpret_cast<const char*>(&n.beat_off), sizeof(float));
        f.write(reinterpret_cast<const char*>(&n.octave), sizeof(float));
        f.write(reinterpret_cast<const char*>(&n.profile), sizeof(int));
    }

    // Focus changes
    uint32_t numFocus = static_cast<uint32_t>(focusChanges.size());
    f.write(reinterpret_cast<const char*>(&numFocus), sizeof(numFocus));
    for (auto& fc : focusChanges) {
        f.write(reinterpret_cast<const char*>(&fc.time), sizeof(float));
        f.write(reinterpret_cast<const char*>(&fc.octave), sizeof(float));
    }

    // Time divisions
    writeTreeNode(f, root);

    uint32_t profileCount = 10;
    f.write(reinterpret_cast<char*>(&profileCount), sizeof(profileCount));
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 3; j++) {
            OSCSettings osc = profiles[i].settings[j];

            // Write enum as fixed-width integer
            int32_t wave = static_cast<int32_t>(osc.waveType);
            f.write(reinterpret_cast<char*>(&wave), sizeof(wave));

            f.write(reinterpret_cast<const char*>(&osc.attack),  sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.decay),   sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.sustain), sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.release), sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.gain),    sizeof(float));
            printf("sustain: %f\n", osc.sustain);
        }
    }

    f.close();
    return true;
}

// --- Load binary song file ---
bool load_song(const std::string& path,
               std::vector<Note>& notes,
               std::vector<FocusChange>& focusChanges,
               std::unique_ptr<TreeNode>& root, Profile* profiles)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    char magic[4];
    f.read(magic, 4);
    if (std::strncmp(magic, "SONN", 4) != 0) {
        throw std::runtime_error("Invalid file format");
    }

    // Notes
    uint32_t numNotes = 0;
    f.read(reinterpret_cast<char*>(&numNotes), sizeof(numNotes));
    notes.resize(numNotes);
    for (uint32_t i = 0; i < numNotes; ++i) {
        f.read(reinterpret_cast<char*>(&notes[i].beat_on), sizeof(float));
        f.read(reinterpret_cast<char*>(&notes[i].beat_off), sizeof(float));
        f.read(reinterpret_cast<char*>(&notes[i].octave), sizeof(float));
        f.read(reinterpret_cast<char*>(&notes[i].profile), sizeof(int));
    }

    // Focus changes
    uint32_t numFocus = 0;
    f.read(reinterpret_cast<char*>(&numFocus), sizeof(numFocus));
    focusChanges.resize(numFocus);
    for (uint32_t i = 0; i < numFocus; ++i) {
        f.read(reinterpret_cast<char*>(&focusChanges[i].time), sizeof(float));
        f.read(reinterpret_cast<char*>(&focusChanges[i].octave), sizeof(float));
    }

    // --- Tree root ---
    root = readTreeNode(f);


    uint32_t profileCount;
    f.read(reinterpret_cast<char*>(&profileCount), sizeof(profileCount));

    if (profileCount != 10) {
        // you can decide whether to error or clamp
        return false;
    }

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 3; j++) {
            OSCSettings* osc = &(profiles[i].settings[j]);

            int32_t wave;
            f.read(reinterpret_cast<char*>(&wave), sizeof(wave));
            osc->waveType = static_cast<WaveformType>(wave);

            f.read(reinterpret_cast<char*>(&osc->attack),  sizeof(float));
            if (i == 0 && j == 0)
                printf("read attack: %f\n", osc->attack);
            f.read(reinterpret_cast<char*>(&osc->decay),   sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->sustain), sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->release), sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->gain),    sizeof(float));
        }
    }

    f.close();
    return true;
}


static std::vector<SeqEvent> build_events_from_notes(const std::vector<Note>& notes,
                                                     float bpm, uint32_t sampleRate, float playhead_time)
{
    std::vector<Note> ns = notes;
    std::sort(ns.begin(), ns.end(),
              [](const Note& a, const Note& b){ return a.beat_on < b.beat_on; });

    std::vector<SeqEvent> ev;
    if (ns.empty()) return ev;

    const double spb = (double)sampleRate * 60.0 / (double)bpm; // samples per beat

    for (const auto& n : ns) {
        // Map [beat_on, beat_off) → [sample_on, sample_off)
        double bon = n.beat_on - playhead_time;
        double boff = n.beat_off - playhead_time;
        if (boff <= bon || boff < 0.0f) continue;
        if (bon < 0) bon = 0.0f;
        if (std::abs(bon - boff) < 1e-6) continue;

        SeqEvent e1;
        e1.type = 1;
        e1.beat  = bon;
        e1.value       = c4 * std::pow(2.0f, n.octave - 4.0f);
        e1.profile = n.profile;
        ev.push_back(e1);
        SeqEvent e2;
        e2.type = 0;
        e2.beat = boff; // exclusive
        e2.value = c4 * std::pow(2.0f, n.octave - 4.0f);
        ev.push_back(e2);
        std::fflush(stdout);
    }
    printf("length: %d\n", (int)ev.size());
    return ev;
}

float parseDivisor(char* n) {
    float numerator = 0;
    float denominator = 0;
    int s = 0;
    bool found = true;
    while (n[s] != '/') {
        if (s == strlen(n) - 1) {
            found = false;
            s++;
            break;
        }
        s++;
    }
    int i = s - 1;
    int power = 0;
    while (i >= 0) {
        numerator += ((uint8_t)n[i] - 48) * std::pow(10, power);
        power++;
        i--;
    }
    if (found) {
        power = 0;
        int j = strlen(n) - 1;
        while (j > s) {
            denominator += ((uint8_t)n[j] - 48) * std::pow(10, power);
            j--;
            power++; 
        }
        printf("res: %f\n", numerator / denominator);
        std::fflush(stdout);
        return numerator / denominator;
    } else {
        return numerator;
    }
}

bool find_division(TreeNode* current, int split, float current_start, float current_end, float start, float end) {
    if (!current) {
        return false;
    }
    // Check if this node already has those bounds
    printf("current_start: %f, current_end: %f, start: %f, end: %f", current_start, current_end, start, end);
    if (std::abs(current_end - end) < 0.0001 && std::abs(current_start - start) < 0.0001) {
        current->addChild(split);
        return true;
    }
    float size = current_end - current_start;

    for (const auto& child : current->children) {
        // These are the divisions
        std::fflush(stdout);
        int denominator = child.get()->value;
        printf("denominator: %d\n", child.get()->value);
        for (int i = 0; i < denominator; i++)  {
            
            if (std::abs(current_start + ((i * size) / (float)denominator) - start) < 0.0001 && std::abs(current_start + (((i + 1) * size) / (float)denominator) - end) < 0.0001) {
                printf("bruhhhhh");
                TreeNode* current_beat = findNode(child.get(), i);
                // printf("1");
                if (!current_beat)
                    child->addChild(i);
                printf("2");
                findNode(child.get(), i)->addChild(split);
                printf("3");
                return true;
            } 
        }
        printf("7");
        for (const auto& childChild : child->children) {
            printf("8");
            // These are each divisions beats
            float division_size = (current_end - current_start) / (float)denominator;
            if (find_division(childChild.get(), split, current_start + (childChild.get()->value * division_size), current_start + ((childChild.get()->value + 1) * division_size) , start, end)) {
                return true;
            }
        }
    }
}

// Compute greatest common divisor
int gcd(int a, int b) {
    return b == 0 ? std::abs(a) : gcd(b, a % b);
}

// Convert float to simplified fraction
void floatToFraction(float value, int& num, int& den, int maxDen = 10000) {
    // Handle sign
    bool negative = value < 0;
    value = std::fabs(value);

    // Integer part
    int intPart = static_cast<int>(value);
    float fracPart = value - intPart;

    if (fracPart == 0.0f) {
        num = intPart;
        den = 1;
        if (negative) num = -num;
        return;
    }

    // Continued fraction algorithm
    float x = fracPart;
    int prevNum = 0, prevDen = 1;
    num = 1; den = 0;

    while (true) {
        int a = static_cast<int>(x);
        int tempNum = num, tempDen = den;
        num = a * num + prevNum;
        den = a * den + prevDen;

        if (den > maxDen) break;

        float approx = static_cast<float>(num) / den;
        if (std::fabs(approx - fracPart) < 1e-6f) break;

        prevNum = tempNum;
        prevDen = tempDen;
        x = 1.0f / (x - a);
    }

    num = intPart * den + num;
    if (negative) num = -num;

    // Simplify
    int g = gcd(num, den);
    num /= g;
    den /= g;
}



char buttonLabels[27][32] = {"Export WAV", 
    "BPM:          ", 
    "Attack:          ", 
    "Decay:          ", 
    "Gain:            ", 
    "Wav name:",
    "Jump size:", 
    "Split size:", 
    "Export MIDI", 
    "Save project", 
    "Load project", 
    "Change wave", 
    "Sustain:          ", 
    "Release:          ", 
    "OSC1: Sine",
    "OSC2: Sine",
    "Attack:          ", 
    "Decay:          ", 
    "Sustain:          ", 
    "Release:          ",
    "Gain:              ",
    "OSC3: Sine",
    "Attack:          ", 
    "Decay:          ", 
    "Sustain:          ", 
    "Release:          ",
    "Gain:              "
    };
char texts[3][32] = {"file.wav", "              ", "              "};
char originText[64];

struct Canvas2D {
    // World-space units
    // x = beats, y = octaves (increasing upward)
    float x0 = 0.0f;       // world x at left edge
    float y0 = 6.0f;       // world y at TOP edge (e.g., show around 6 octaves)
    float sx = 120.0f;     // pixels per beat
    float sy = 80.0f;      // pixels per octave

    bool  snap_to_grid = false;
    bool snap_harmonics = true;
    float snap_x = 0.125f;  // snap to quarter-beats
    float snap_y = 1.0/12.0;  // snap to semitones

    // Stored points in world units (beat, octave)
    std::vector<Note> notes;
    std::unique_ptr<TreeNode> splits = std::make_unique<TreeNode>(-1);
    std::vector<FocusChange> foci;

    bool placing = false;
    float lengthOrigin = 0.0f;
    Vec2 anchorW{0,0};
    float split_start = -1.0f;

    Note currently_dragged;
    bool dragging = false;

    bool hover_valid = false;
    Vec2 hoverW{0,0};
    Vec2 unsnappedHoverW{0,0};
    float anchorWXexact = 0.0f;

    TreeNode* getSplits() {
        return splits.get();
    }

    const TreeNode* getSplits() const {
        return splits.get();
    }

    void setSplits(std::unique_ptr<TreeNode> newSplits) {
        splits = std::move(newSplits);
    }

    void addCanvasRect(VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight, float rx, float ry, float rw, float rh, float r, float g, float b) {
        float x = rx;
        float y = ry;
        float w = rw;
        float h = rh;

        if (x + w < topLeft.x || x > bottomRight.x || y + h < bottomRight.y || y > topLeft.y) {
            return;
        }

        if (x < topLeft.x) {
            x = topLeft.x;
        } else if (x > bottomRight.x) {
            x = bottomRight.x;
        }
        if (y < bottomRight.y) {
            y = bottomRight.y;
        } else if (y > topLeft.y) {
            y = topLeft.y;
        }
        if (x + w > bottomRight.x) {
            w = std::abs(x  - bottomRight.x);
        } else if (std::abs(x - rx) > 1e-6) {
            w -= std::abs(x - rx);
        }
        if (y + h > topLeft.y) {
            h = std::abs(topLeft.y - y);
        } else if (std::abs(x - ry) > 1e-6) {
            h -= std::abs(y - ry);
        }
        if (w != 0 && h != 0) {
            renderer.addRect(x, y, w, h, r, g, b);
        }
    }

    // Transform: world -> screen
    Vec2 worldToScreen(Vec2 topLeft, Vec2 w) const {
        // y increases upward in world, downward on screen → invert with (y0 - y)
        float sxp = topLeft.x + (w.x - x0) * sx;
        float syp = topLeft.y + (w.y - y0) * sy;
        return {sxp, syp};
    }
    // Transform: screen -> world
    Vec2 screenToWorld(Vec2 topLeft, Vec2 s) const {
        float wx = x0 + (s.x - topLeft.x) / sx;
        float wy = y0 + (s.y - topLeft.y) / sy;
        return {wx, wy};
    }

    // Round a value to nearest step (for snapping)
    static float snap(float v, float step) {
        return step > 0.f ? std::round(v / step) * step : v;
    }

    std::vector<float> get_bar_points(TreeNode* current, float currentStart, float currentEnd) {
        std::vector<float> list = {currentStart, currentEnd};
        float size = std::abs(currentEnd - currentStart);
        // printf("size: %f\n", size);
        for (const auto& child : current->children) {
            // These are the divisions
            int division = child->value;
            // printf("division: %d\n", division);
            for (int i = 1; i < division; i++) {
                list.push_back(currentStart + ((i * size)/(float)division));
                // printf("point: %f\n", currentStart + ((i * size)/(float)division));
            }

            for (const auto& childChild : child->children) {
                // These are the beat within a division
                int beat = childChild->value;
                // printf("New start: %f, new end: %f\n", currentStart + ((beat * size) / (float)division),  (((beat + 1) * size) /  (float)division));
                std::vector<float> currentList = get_bar_points(childChild.get(), currentStart + ((beat * size) / (float)division), currentStart + (((beat + 1) * size) /  (float)division));
                for (int i = 0; i < (int)currentList.size(); i++) {
                    list.push_back(currentList[i]);
                }
                // printf("\n");
                // std::fflush(stdout);
            }
        }
        return list;
    }

    float snap_split(float v) {
        TreeNode* current_beat = findNode(splits.get(), std::floor(v));
        std::vector<float> points;
        if (!current_beat) {
            points.push_back(std::floor(v));
            points.push_back(std::ceil(v));
        } else {
            points = get_bar_points(current_beat, std::floor(v), std::ceil(v));
        }
        for (int i = 0; i < (int)points.size(); i++) {
            // printf("%f, ", points[i]);
        }
        // printf("\n\n");
        float distance = 99.0;
        float closest = std::floor(v);
        for (int i = 0; i < (int)points.size(); i++) {
            if (std::abs(points[i] - v) < distance) {
                distance = std::abs(points[i] - v);
                closest = points[i];
            }
        }
        return closest;
    }

    void drawRhythmLine(Vec2 s0, VulkanRenderer& renderer, std::vector<int>& divisions, Vec2 topLeft, Vec2 bottomRight) {
        addCanvasRect(renderer, topLeft, bottomRight, s0.x, s0.y - 10, 1, 20, 0.47, 0.47, 0.63);
        if ((int)divisions.size() > 1) {
            switch (divisions.back()) {
                case 8:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 16, 6, 32, 0.47, 0.47, 0.63);
                case 4:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 13, 6, 1, 0.47, 0.47, 0.63);
                case 2:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 10, 6, 1, 0.47, 0.47, 0.63);
                    break;
                case 3:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 11, 6, 2, 0.47, 0.47, 0.63);
                    break;
                case 5:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 12, 3, 2, 0.47, 0.47, 0.63);
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x, s0.y - 12, 3, 2, 0.47, 0.47, 0.63);
                    break;
                case 7:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x, s0.y - 12, 3, 2, 0.47, 0.47, 0.63);
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 12, 3, 2, 0.47, 0.47, 0.63);
                    break;
                case 6:
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 8, 6, 4, 0.47, 0.47, 0.63);
                    addCanvasRect(renderer, topLeft, bottomRight, s0.x - 3, s0.y - 8, 6, 4, 0.47, 0.47, 0.63);
                    break;
            }
        }
    }

    // divisions holds the list of recursive split sizes
    void drawRhythmLines(TreeNode* current, const std::vector<Note>& sortedStart, const std::vector<Note>& sortedEnd, \
        float start, float end, std::vector<int> divisions, std::vector<float>& currently_held, VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight) {
        // printf("start\n");
        float beat_size = std::abs(end - start) / divisions.back();
        int latest_starti = 0;
        int latest_endi = 0;
        // printf("start: %f, end: %f, beat_size %f, divisions: %d\n", start, end, beat_size, divisions.back());
        // Currently dealing with the last-appended number to divisions - the current number to split into
        for (int i = 0; i < divisions.back() + 1; i++) {
            float current_beat = start + i * beat_size;
            int starti = latest_starti;
            int endi = latest_endi;
            // Only draw a line if there is a note playing, keep track of playing notes to perform averages
            // Only draw lines in the middles if these are subdivisions
            if ((int)sortedStart.size() > 0 && ((int)divisions.size() == 1 || (i != 0 && i != divisions.back()))) {
                // printf("endi: %d, sEndsize:%d, sEndBeat_OFF: %f\n", endi, (int)sortedEnd.size(), sortedEnd[endi].beat_off);
                if (starti >= (int)sortedStart.size()) {
                    printf("ISSUE: starti: %d, latest_starti: %d\n", starti, latest_starti);
                }
                // Setup start and end indexes to line up with the next note event we'll encounter (purely an optimisation)
                while (starti < (int)sortedStart.size() && sortedStart[starti].beat_on < current_beat)
                    starti++;
                while (endi < (int)sortedEnd.size() && sortedEnd[endi].beat_off < current_beat)
                    endi++;
                // If this is true, a note is starting or ending on current_beat, we must keep track of this for the average
                if ((starti < (int)sortedStart.size() && std::abs(sortedStart[starti].beat_on - current_beat) < 1e-6) || (endi < (int)sortedEnd.size() && std::abs(sortedEnd[endi].beat_off - current_beat) < 1e-6)) {
                    while (starti < (int)sortedStart.size() && std::abs(sortedStart[starti].beat_on - current_beat) < 1e-6) {
                        currently_held.push_back(sortedStart[starti].octave);
                        Vec2 s0 = worldToScreen(topLeft, { current_beat, sortedStart[starti].octave });
                        drawRhythmLine(s0, renderer, divisions, topLeft, bottomRight);
                        latest_starti = starti;
                        starti++;
                    }
                    while (endi < (int)sortedEnd.size() && std::abs(sortedEnd[endi].beat_off - current_beat) < 1e-6) {
                        std::vector<float> newList;
                        for (int j = 0; j < (int)currently_held.size(); j++) {
                            // printf("diff: %f\n", std::abs(currently_held[j] - sortedEnd[endi].octave));
                            if (std::abs(currently_held[j] - sortedEnd[endi].octave) > 0.0001) {
                                // printf("j: %d\n", j);
                                newList.push_back(currently_held[j]);
                            }
                        }
                        // printf("removing: %f\n", sortedEnd[endi].octave);
                        currently_held = newList;
                        // std::remove(currently_held.begin(), currently_held.end(), sortedEnd[endi].octave);
                        Vec2 s0 = worldToScreen(topLeft, { current_beat, sortedEnd[endi].octave });
                        drawRhythmLine(s0, renderer, divisions, topLeft, bottomRight);
                        latest_endi = endi;
                        endi++;
                    }
                } else if ((int)currently_held.size() != 0) {
                    // If there are notes held but no note events occur on current_beat, take an average and place the marker between them
                    float avg = 0.0f;
                    for (int i = 0; i < (int)currently_held.size(); i++) {
                        avg += currently_held[i];
                    }
                    avg = avg / (int)currently_held.size();
                    Vec2 s0 = worldToScreen(topLeft, { current_beat,  avg});
                    drawRhythmLine(s0, renderer, divisions, topLeft, bottomRight);

                }
                // printf("CH:%d\n", (int)currently_held.size());
            }
            // Check if we have to go deeper on this beat
            for (int j = 0; j < (int)current->children.size(); j++) {
                TreeNode* child = current->children[j].get();
                // printf("val: %d, i: %d\n", child->value, i);
                if (child->value == ((int)divisions.size() == 1 ? i + start : i)) {
                    // printf("match\n");
                    for (const auto& childChild : child->children) {
                        // printf("gotta go inside\n");
                        divisions.push_back(childChild.get()->value);
                        drawRhythmLines(childChild.get(), sortedStart, sortedEnd, start + i * beat_size, start + (i + 1) * beat_size, divisions, currently_held, renderer, topLeft, bottomRight);
                        divisions.pop_back();
                    }
                }
            }
        }
        // printf("end\n");
    }

    Vec2 prevHover = {0.0f, 0.0f};

    bool containsKey(std::vector<char> keys, char c) {
        for (const auto& k : keys) {
            if (k == c)
                return true;
        }
        return false;
    }

    int numberContained(std::vector<char> keys) {
        for (const auto& k : keys) {
            if (k == '0')
                return 0;
            if (k == '1')
                return 1;
            if (k == '2')
                return 2;
            if (k == '3')
                return 3;
            if (k == '4')
                return 4;
            if (k == '5')
                return 5;
            if (k == '6')
                return 6;
            if (k == '7')
                return 7;
            if (k == '8')
                return 8;
            if (k == '9')
                return 9;
        }
        return -1;
    }

    void draw(VulkanRenderer& renderer, CurrentBox& nw, Vec2 hover, int click, std::vector<char> keys, char*& jump, char*& split, int dx, int dy) {
        // Reserve a canvas rect
        // Vec2 avail = ImGui::GetContentRegionAvail();
        Vec2 topLeft  = { nw.x1, nw.y2};
        Vec2 size     = { std::abs(nw.x2 - nw.x1), std::abs(nw.y2 - nw.y1) };
        Vec2 bottomRight = { nw.x2, nw.y1 };

        bool hovered = std::abs(hover.x + 1.0f) > 1e-06;

        // printf("keys: ");
        // for (int i = 0; i < (int)keys.size(); i++) {
        //     printf("%08x,", keys[i]);
        // }
        // printf("\n");

        // --- Panning with RMB drag ---
        if (hovered && dx != 0 || dy != 0) {
            if (containsKey(keys, '~')) {
                printf("hi\n");
                sx += dx;
                sy += dy;
            } else {
                // Convert pixel delta → world delta
                x0 -= dx / sx;
                y0 -= dy / sy; // + because dragging down increases screen y but decreases world y
            }
        }

        if (hovered && click == 2 && std::abs(prevHover.x) > 1e-6) {
            Vec2 d = { hover.x - prevHover.x, hover.y - prevHover.y};
            x0 -= d.x / sx;
            y0 -= d.y / sy;
        }

        prevHover = hover;

        // Compute visible world rect
        Vec2 wA = screenToWorld(topLeft,      topLeft);
        Vec2 wB = screenToWorld(topLeft,      bottomRight);

        float wxMin = std::min(wA.x, wB.x), wxMax = std::max(wA.x, wB.x);
        float wyMin = std::min(wA.y, wB.y), wyMax = std::max(wA.y, wB.y);

        // --- Grid lines (integer beats/octaves) ---
        const float gridCol[3]   = { 0.235, 0.235, 0.235};
        const float axisCol[3]   = { 0.47, 0.47, 0627};
        const float textCol[3]   = { 0.863, 0.863, 0.863};
        const float playheadCol[3] = { 0.784, 0.08, 0.08 };
        const float noteCol[3] = { 0.353, 0.63, 1.0 };
        const float selCol[3] = { 1.0, 1.0, 0.2588 };
        const float focusCol[3] = { 1.0, 0.63, 0.353 };

        // Sort notes
        std::vector<Note> sorted = notes;
        std::vector<Note> sortedEnd = notes;
        std::vector<float> currently_held;
        float end = 0.0f;
        std::sort(sorted.begin(), sorted.end(),
                [](auto& a, auto& b){ 
                    return a.beat_on < b.beat_on; });
        std::sort(sortedEnd.begin(), sortedEnd.end(),
                [](auto&a, auto& b){
                    return a.beat_off < b.beat_off;});
        if ((int)sortedEnd.size() > 0) {
            end = sortedEnd[(int)sortedEnd.size() - 1].beat_off;
        }

        // Vertical: beats at integer positions within [wA.x, wB.x]
        int beatStart = std::max(std::floor(wA.x), (int)sorted.size() > 0 ? std::floor(sorted[0].beat_on) : -99999.0f);
        int beatEnd   = std::min(std::ceil (wB.x), std::ceil(end));
        std::vector<int> divisions;
        divisions.push_back(std::abs(beatEnd - beatStart));
        drawRhythmLines(splits.get(), sorted, sortedEnd, beatStart, beatEnd, divisions, currently_held, renderer, topLeft, bottomRight);

        // Horizontal: octaves at integer positions
        int octStart = (int)std::floor(wB.y);
        int octEnd   = (int)std::ceil (wA.y);

        // Draw playhead
        Vec2 s0 = worldToScreen(topLeft, { playhead_time, wA.y });
        addCanvasRect(renderer, topLeft, bottomRight, s0.x, topLeft.y, 1, bottomRight.y - topLeft.y, gridCol[0], gridCol[1], gridCol[2]);

        // Hover snapped position (for preview dot / rubber band)
        hover_valid = false;
        if (hovered) {
            Vec2 w = screenToWorld(topLeft, hover);
            unsnappedHoverW = w;
            w.x = snap_split(w.x);
            if (snap_to_grid) {
                w.y = snap(w.y, snap_y);
            } else if (snap_harmonics) {
                float focus = 0.0f;
                for (int i = 0; i < (int)foci.size() - 1; i++) {
                    if (unsnappedHoverW.x > foci[i].time && unsnappedHoverW.x < foci[i + 1].time) {
                        focus = foci[i].octave;
                        break;
                    }
                }

                if (focus > 1e-6) {
                    float currentOctave = focus;
                    if (currentOctave > w.y) {
                        while (currentOctave > w.y)
                            currentOctave -= 1;
                    } else if (currentOctave + 1 < w.y) {
                        while (currentOctave + 1 < w.y) 
                            currentOctave += 1;
                    }

                    // printf("Y pos: %f\n", w.y);
                    float closest = currentOctave;
                    for (int i = 0; i < (int)sizeof(perfectIntervals) / 4; i++) {
                        // printf("Current: %f\n", currentOctave + std::log2(perfectIntervals[i]));
                        // printf("Distance: %f\n", std::abs(w.y - (currentOctave + std::log2(perfectIntervals[i]))));
                        // printf("Distance to closest: %f\n", std::abs(w.y - closest));
                        if (std::abs(w.y - (currentOctave + std::log2(perfectIntervals[i]))) < std::abs(w.y - closest)) {
                            // printf("YO\n");
                            closest = currentOctave + std::log2(perfectIntervals[i]);
                        }
                    }
                    w.y = closest;
                }
                hoverW = w;
                hover_valid = true;
            }
        }

        // Key handling (P to change playhead position, L to change note length, D to delete a note, F to change the note's frequency)
        if (hovered) {
            if (containsKey(keys, 'p')) 
                playhead_time = hoverW.x;
            else if (containsKey(keys, ' '))
                play_triggered_from_space = true;
            else if (containsKey(keys, 'v')) {
                // Paste notes
                float firstSelectedTime = 0.0f;
                for (int i = 0; i < (int)notes.size(); i++) {
                    if (notes[i].selected) {
                        if (firstSelectedTime < 1e-6)
                            firstSelectedTime = notes[i].beat_on;
                        Note to_push = notes[i];
                        to_push.beat_on = hoverW.x + notes[i].beat_on - firstSelectedTime;
                        to_push.beat_off = hoverW.x + notes[i].beat_off - firstSelectedTime;
                        to_push.selected = false;
                        notes.push_back(to_push);
                    }
                }
            } else if (containsKey(keys, 'r')) {
                // Remove selected notes
                std::vector<Note> newNotes;
                for (int i = 0; i < (int)notes.size(); i++) {
                    if (!notes[i].selected) newNotes.push_back(notes[i]);
                }
                notes = newNotes;
            }
        }

        if (hovered && (int)keys.size() != 0 && (int)notes.size() > 0) {
            // Find closest note and side of note (start or end)
            Note closest = notes[0];
            float distance =  99999;
            int index = 0;
            bool match_found = false;
            for (int i = 0; i < (int)notes.size(); i++) { 
                float current = std::abs(notes[i].octave - hoverW.y);
                if ((hoverW.x <= notes[i].beat_off && hoverW.x  >= notes[i].beat_on) && current < distance) {
                    match_found = true;
                    distance = current;
                    closest = notes[i];
                    index = i;
                } 
            }

            if (containsKey(keys, 's')) {
                if (split_start != -1.0f) {
                    float split_end = hoverW.x;
                    if ((split_end > split_start) && std::ceil(split_end) - std::floor(split_start) == 1) {
                        // Split time into specified amount
                        int current_beat = std::floor(split_start);
                        TreeNode* beat_node = findNode(splits.get(), current_beat);
                        if (!beat_node) {
                            // Create entry for that beat
                            splits.get()->addChild(current_beat);
                        }
                        find_division(findNode(splits.get(), current_beat), parseDivisor(split), current_beat, current_beat + 1.0f, split_start, split_end);
                        printTree(splits.get());
                    }
                    split_start = -1.0f;
                } else {
                    split_start = hoverW.x;
                }
            }

            if (match_found) {
                // Drag start or end of note
                if (containsKey(keys, 'l')) {
                    anchorW.y = closest.octave;
                    lengthOrigin = closest.origin;
                    if (std::abs(hoverW.x - closest.beat_off) < std::abs(hoverW.x - closest.beat_on)) {
                        anchorW.x = closest.beat_on;
                    } else {
                        anchorW.x = closest.beat_off;
                    }
                    placing = true;
                }

                int number = numberContained(keys);

                if (number != -1) {
                    printf("non-zero profile\n");
                    notes[index].profile = number;
                }
                
                if (containsKey(keys, 'd') || containsKey(keys, 'f') || containsKey(keys, 'l')) {
                    // Delete the note because it will be replaced
                    notes.erase(notes.begin() + index);
                }
                if (containsKey(keys, 'j')) {
                    // Jump up by the written amount
                    printf("text: %s, res: %f\n", jump, parseDivisor(jump));
                    notes[index].origin = std::abs(notes[index].origin) < 1e-6 ? notes[index].octave : notes[index].origin;
                    notes[index].octave = notes[index].octave + (std::log2(parseDivisor(jump)));
                }
                if (containsKey(keys, 'a')) {
                    notes[index].selected = !(notes[index].selected);
                }

                // Frequency drag
                if (containsKey(keys, 'f')) {
                    dragging = true;
                    currently_dragged = closest;
                }
                
                // Delete the focus if the note had focus on it
                if (containsKey(keys, 'f') || containsKey(keys, 'l') || containsKey(keys, 'd') || containsKey(keys, 'j')) {
                    std::vector<FocusChange> newFoci;
                    int count = 0;
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("i: %d\n", i);
                        if (foci[i].time == closest.beat_on) {
                            if (foci[i].octave != closest.octave) {
                                newFoci.push_back(foci[i]);
                            } else if (i != 0 && foci[i - 1].octave != 0.0f) {
                                newFoci.push_back(foci[i]);
                                newFoci[count].octave = 0.0f;
                                count++;
                            }
                        } else if (foci[i].time > closest.beat_on && foci[i].time < closest.beat_off) {
                            if (foci[i].octave == 0.0f && foci[i - 1].octave != closest.octave) {
                                newFoci.push_back(foci[i]);
                                count++;
                            } else if (foci[i].octave != closest.octave) {
                                newFoci.push_back(foci[i]);
                                count++;
                            } else if (foci[i].octave == closest.octave) {
                                newFoci.push_back(foci[i]);
                                newFoci[count].octave = 0.0f;
                                count++;
                            }
                        } else if (foci[i].time == closest.beat_off) {
                            // printf("!!!!!!!!\n");
                            if ((i != 0 && foci[i - 1].octave != closest.octave) || ((i != (int)foci.size() - 1) && foci[i].octave != 0.0f)) {
                                // printf("??????\n");
                                newFoci.push_back(foci[i]);
                                count++;
                            }
                        } else {
                            // printf("wot");
                            newFoci.push_back(foci[i]);
                            count++;
                        }
                    }
                    // printf("foci: \n");
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("i: %d, ", i);
                        // printf("octave: %f, ", foci[i].octave);
                        // printf("time: %f, ", foci[i].time);
                    }
                    foci = newFoci;
                    // printf("newFoci: \n");
                    for (int i = 0; i < (int)foci.size(); i++) {
                        // printf("i: %d, ", i);
                        // printf("octave: %f, ", foci[i].octave);
                        // printf("time: %f, ", foci[i].time);
                    }
                }
            }
        }


        if (hovered && click == 2 && !placing) {
            // Find closest note
            std::vector<Note> within;
            for (const Note& n : notes) {
                if (unsnappedHoverW.x <= n.beat_off && unsnappedHoverW.x  >= n.beat_on)
                    within.push_back(n);
            }
            if ((int)within.size() > 0) {
                Note closest = within[0];
                float distance = 999999;
                for (const Note& n : within) {
                    float current = std::abs(n.octave - unsnappedHoverW.y);
                    if (current < distance) {
                        distance = current;
                        closest = n;
                    }
                }
                // Add to focus changes as far left and as far right as we can
                int insert = 0;
                bool focus_after = true;
                bool focus_before = true;
                std::vector<FocusChange> newFoci;
                for (int i = 0; i < (int)foci.size(); i++) {
                    newFoci.push_back(foci[i]);
                    if (foci[i].time > unsnappedHoverW.x) {
                        // This index is the next focus change after the click point
                        insert = i;
                        if (foci[i].octave == 0.0f)
                            focus_after = false;
                        else if (i <= 1 || foci[i - 2].octave == 0.0f)
                            focus_before = false;
                        break;
                    } else {
                        if (i == (int)foci.size() - 1) {
                            insert = i + 1;
                        }
                    }
                }
                // printf("insert: %d\n", insert);
                // printf("focus_before: %d, focus_after: %d\n", focus_before, focus_after);
                std::fflush(stdout);
                bool removePrevious = false;
                bool removeNext = false;
                float previousOctave = 0.0f;
                if (insert > 0)
                    previousOctave = foci[insert - 1].octave;
                FocusChange start, end;

                bool closest_starts_after_prev_focus_change =  insert == 0 || closest.beat_on > foci[insert - 1].time;
                bool closest_ends_before_next_focus_change = (int)foci.size() == 0 || insert == (int)foci.size() || (insert >= 0 && insert < (int)foci.size() && closest.beat_off < foci[insert].time);

                bool closest_starts_at_prev_focus_change = insert > 0 && std::abs(closest.beat_on - foci[insert - 1].time) < 1e-6;
                bool closest_ends_at_next_focus_change = insert > 0 && insert <= (int)foci.size() - 1 && std::abs(closest.beat_off - foci[insert].time) < 1e-6;

                if (closest_starts_after_prev_focus_change) {
                    printf("A");
                    start.time = closest.beat_on;
                    start.octave = closest.octave;
                } else if (closest_starts_at_prev_focus_change) {
                    printf("B");
                    newFoci[insert - 1].time = closest.beat_on;
                    newFoci[insert - 1].octave = closest.octave;
                } else if (!focus_before) { // From here onwards we will not extend to the start of closest
                    if (insert > 1 && closest.beat_on < foci[insert - 2].time) {
                        // Change insert - 2 to be the current frequency instead of zero, then delete insert - 1
                        newFoci[insert - 2].octave = closest.octave;
                        removePrevious = true;
                    } else {
                        newFoci[insert - 1].time = closest.beat_on;
                        newFoci[insert - 1].octave = closest.octave;
                    }
                } else {
                    newFoci[insert - 1].octave = closest.octave;
                }

                if (closest_ends_before_next_focus_change) {
                    printf("C");
                    end.time = closest.beat_off;
                    end.octave = previousOctave;
                } else if (closest_ends_at_next_focus_change) {
                    printf("D");
                } else if (!focus_after) {
                    // Still must check if the one after the next happens before the end of the note
                    if (insert == (int)foci.size() || foci[insert + 1].time < closest.beat_off) {
                        removeNext = true;
                    } else {
                        newFoci[insert].time = closest.beat_off;
                        newFoci[insert].octave = 0.0f;
                    }
                }

                bool focus_exists = insert > 0 && start.time == foci[insert - 1].time && start.octave == foci[insert - 1].octave && end.time == foci[insert].time && end.octave == foci[insert].octave;
                printf("start: (%f, %f), end: (%f, %f)\n", start.time, start.octave, end.time, end.octave);

                if (!focus_exists) {
                    if (removePrevious)
                        newFoci.erase(newFoci.begin() + (insert - 1));
                    if (removeNext)
                        newFoci.erase(newFoci.begin() + insert);

                    if (closest_starts_after_prev_focus_change && closest_ends_before_next_focus_change) {
                        newFoci.insert(newFoci.begin() + insert, start);
                        newFoci.insert(newFoci.begin() + insert + 1, end);
                    } else if (closest_starts_after_prev_focus_change) {
                        newFoci.insert(newFoci.begin() + insert, start);
                    } else if (closest_ends_before_next_focus_change) {
                        newFoci.insert(newFoci.begin() + insert, end);
                    }


                    for (int i = insert + 1; i < (int)foci.size(); i++)
                        newFoci.push_back(foci[i]);

                    std::vector<FocusChange> newNewFoci;

                    for (int i = 0; i < (int)newFoci.size(); i++) {
                        if (i == 0 || newFoci[i].octave != newFoci[i - 1].octave) {
                            newNewFoci.push_back(newFoci[i]);
                        }
                    }

                    foci = newNewFoci;
                    for (int i = 0; i < (int)foci.size(); i++) {
                        printf("Focus %d: time: %f, octave: %f\n", i, foci[i].time, foci[i].octave);
                    }
                }
            }
        }
        

        // Existing notes as blue lines
        int focusIndex = 0;
        Note closest;
        closest.octave = -1.0f;
        float distance = 9999.0f;
        for (const Note& n : notes) {
            float x0w = n.beat_on, x1w = n.beat_off;
            float y   = n.octave;
            // Calculate distance
            float current_distance = 0.0f;
            if (hoverW.x >= n.beat_on && hoverW.x <= n.beat_off) {
                current_distance = std::abs(y - hoverW.y);
            } else if (hoverW.x < n.beat_on) {
                current_distance = std::sqrt(std::pow(2, hoverW.x - n.beat_on) + std::pow(2, hoverW.y - y));
            } else {
                current_distance = std::sqrt(std::pow(2, hoverW.x - n.beat_off) + std::pow(2, hoverW.y - y));
            }
            if (current_distance < distance) {
                closest = n;
                distance = current_distance;
            }
            // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed  (e.g., 0.6 octave)
            // float halfH = 1.0/24.0;
            Vec2 a = worldToScreen(topLeft, { x0w, y});
            Vec2 b = worldToScreen(topLeft, { x1w, y});
            // // ensure correct corners
            Vec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            Vec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            if (n.selected)
                addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, 2, selCol[0], selCol[1], selCol[2]);
            else
                addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, 2, noteCol[0], noteCol[1], noteCol[2]);

            float tri_size = 6.0f;
            Vec2 p1(a.x + tri_size, a.y);                       // tip of triangle
            Vec2 p2(a.x, a.y - tri_size / 2); // top base corner
            Vec2 p3(a.x, a.y); // bottom base corner

            // dl->AddTriangleFilled(p1, p2, p3, noteCol);
        }

        // Add an origin line for the closest note
        if (closest.octave >= 0.0f) {
            int res = 0;
            if (std::abs(closest.origin) > 1e-6) {
                Vec2 a = worldToScreen(topLeft, { closest.beat_on, closest.octave});
                Vec2 c = worldToScreen(topLeft, {closest.beat_on, closest.origin});
                addCanvasRect(renderer, topLeft, bottomRight, a.x, a.y, 2, c.y - a.y, 0.353, 1.0, 0.353);
                // Text describing the relationship
                bool down = false;
                float diff = closest.octave - closest.origin;
                if (diff < 0) {
                    down = true;
                    diff *= -1;
                }
                int octaves = (int)std::floor(diff);
                diff -= octaves;
                float ratio = std::pow(2, diff);
                int num, den;
                if (down) {
                    floatToFraction(ratio, den, num);
                } else {
                    floatToFraction(ratio, num, den);
                }
                if (den == 1) {
                    res = sprintf(originText, "x %d", num);
                } else {
                    res = sprintf(originText, "x %d / %d", num, den);
                }
                if (octaves != 0) {
                    if (down) {
                        res += sprintf(originText + res, " (- %d octaves)", octaves);
                    } else {
                        res += sprintf(originText + res, " (+ %d octaves)", octaves);
                    }
                }
            }
            if (res != 0) {
                res += sprintf(originText + res, ", ");
            }
            sprintf(originText + res, "profile %d", closest.profile);
            renderer.addText(originText, 10, 10, 15.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        }

        // Draw focus areas on top of existing notes
        for (int i = 0; i < (int)foci.size() - 1; i++) {
            if (foci[i].octave == 0.0f) {
                continue;
            }
            float x0w = foci[i].time;
            float x1w = foci[i + 1].time;
            float y = foci[i].octave;
            float halfH = 1.0 / 24.0;
            Vec2 a = worldToScreen(topLeft, { x0w, y });
            Vec2 b = worldToScreen(topLeft, { x1w, y });
            // ensure correct corners
            Vec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            Vec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, 2, focusCol[0], focusCol[1], focusCol[2]);
            // dl->AddRect(rmin, rmax, IM_COL32(100,60,30,220), 6.0f, 0, 2.0f);

            // float tri_size = 6.0f;
            // Vec2 p1(a.x + tri_size, a.y);                       // tip of triangle
            // Vec2 p2(a.x, a.y - tri_size / 2); // top base corner
            // Vec2 p3(a.x, a.y); // bottom base corner

            // dl->AddTriangleFilled(p1, p2, p3, focusCol);

            // addCanvasRect(renderer, topLeft, bottomRight, {a.x, a.y}, {b.x, a.y}, focusCol);

            if (snap_harmonics) {
                float startOctave = foci[i].octave;
                float endOctave = foci[i].octave;
                if (startOctave < octStart) {
                    while ((startOctave + 1) < octStart) {
                        startOctave += 1;
                    }
                }
                while (startOctave > octStart) {
                    startOctave -= 1;
                }
                if (endOctave > octEnd) {
                    while ((endOctave - 1) > octEnd) {
                        endOctave -= 1;
                    }
                }
                while (endOctave < octEnd) {
                    endOctave += 1;
                }
                for (float o = startOctave; o <= endOctave; o++) {
                    if (o != foci[i].octave) {
                        Vec2 s0 = worldToScreen(topLeft, { wB.x, o});
                        addCanvasRect(renderer, topLeft, bottomRight, rmin.x, s0.y, rmax.x - rmin.x, 1, gridCol[0], gridCol[1], gridCol[2]);
                    }
                    for (int i = 0; i < sizeof(perfectIntervals) / 4; i++) {
                        float currentOctave = o + std::log2(perfectIntervals[i]);
                        if (currentOctave > octStart && currentOctave < octEnd) {
                            Vec2 s0 = worldToScreen(topLeft, { wB.x, currentOctave});
                            addCanvasRect(renderer, topLeft, bottomRight, rmin.x, s0.y, rmax.x - rmin.x, 1, gridCol[0], gridCol[1], gridCol[2]);
                        }
                    }
                }
                std::fflush(stdout);
            }
        }

        // Rubber-band preview (after first click)
        if (placing && hover_valid) {
            float x0w = anchorW.x;
            float x1w = hoverW.x;
            float y   = anchorW.y; // octave fixed from first click
            float halfH = 1.0/24.0;
            Vec2 a = worldToScreen(topLeft, { x0w, y });
            Vec2 b = worldToScreen(topLeft, { x1w, y });

            // addCanvasRect(renderer, topLeft, bottomRight, {a.x, a.y}, {b.x, a.y}, noteCol);
            Vec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            Vec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            // printf("preview: (%f, %f, %f, %f)\n", rmin.x, rmin.y, rmax.x - rmin.x, rmax.y - rmin.y);
            addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, 2, 0.47, 0.47, 1.0);
            // dl->AddRect(rmin, rmax, IM_COL32(80,80,200,180), 6.0f, 0, 2.0f);
        }

        // Light-red preview dot (when Snap is on and hovered)
        if ((snap_to_grid || snap_harmonics) && hover_valid) {
            Vec2 sd = worldToScreen(topLeft, hoverW);
            // dl->AddCircleFilled(sd, 5.0f, IM_COL32(255,120,120,200));
            renderer.addCircle(sd.x, sd.y, 5.0f, 0.8f, 0.2f, 0.2f);
            if (dragging) {
                Note n = currently_dragged;
                float x0w = n.beat_on, x1w = n.beat_off;
                float y   = hoverW.y;
                // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed height (e.g., 0.6 octave)
                float halfH = 1.0/24.0;
                Vec2 a = worldToScreen(topLeft, { x0w, y + halfH });
                Vec2 b = worldToScreen(topLeft, { x1w, y - halfH });
                // ensure correct corners
                Vec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
                Vec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
                addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, rmax.y - rmin.y, 0.35, 0.63, 1.0);
                addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, rmax.y - rmin.y, 0.12, 0.24, 0.39);
            }
        }

        // Input: left-click to place/finish
        if (hovered && click == 1) {
            if (!placing) {
                // First click: start note (snap already applied above)
                if (hover_valid) {
                    if (dragging) {
                        currently_dragged.octave = hoverW.y;
                        if (snap_harmonics) {
                            for (int i = 1; i < (int)foci.size(); i++) {
                                if (foci[i - 1].time <= unsnappedHoverW.x && foci[i].time >= unsnappedHoverW.x) {
                                    currently_dragged.origin = foci[i - 1].octave;
                                    break;
                                }
                            }
                        }
                        notes.push_back(currently_dragged);
                        dragging = false;
                    } else {
                        placing = true;
                        anchorW = hoverW;
                        anchorWXexact= unsnappedHoverW.x;
                    }
                }
            } else {
                // Second click: finish note
                if (hover_valid) {
                    float a = anchorW.x, b = hoverW.x;
                    if (a != b) {
                        Note n;
                        n.beat_on  = std::min(a, b);
                        n.beat_off = std::max(a, b);   // exclusive
                        n.octave   = anchorW.y;        // octave fixed at first click
                        n.origin = lengthOrigin; // If the note was resized, use the origin from before
                        lengthOrigin = 0.0f;
                        if (snap_harmonics) {
                            // Check whether there's any focus at the anchor
                            for (int i = 1; i < (int)foci.size(); i++) {
                                // printf("foci[i - 1].time: %f, anchorW.x: %f, foci[i].time: %f\n", foci[i - 1].time, anchorW.x, foci[i].time);
                                
                                if (foci[i - 1].time <= anchorWXexact && foci[i].time >= anchorWXexact) {
                                    n.origin = foci[i - 1].octave;
                                    break;
                                }
                            }
                        }
                        notes.push_back(n);
                    }
                }
                placing = false;
            }
        }

        // HUD: controls + current mouse coords in world units
        if (hovered) {
            Vec2 w = screenToWorld(topLeft, hover);
            // ImGui::SetCursorScreenPos({topLeft.x + 8, bottomRight.y - 50});
            // ImGui::BeginGroup();
            // ImGui::Text("Mouse: beat %.3f, oct %.3f", w.x, w.y);
            // ImGui::SameLine();
            // ImGui::SetNextItemWidth(80); ImGui::InputFloat("dx", &snap_x, 0.0f, 0.0f, "%.2f");
            // ImGui::SameLine();
            // ImGui::SetNextItemWidth(80); ImGui::InputFloat("dy", &snap_y, 0.0f, 0.0f, "%.2f");
            // ImGui::EndGroup();
        } else {
            // Put the cursor back to layout after InvisibleButton
            // ImGui::Dummy(Vec2(0,0));
        }

        // ImGui::Checkbox("Snap", &snap_to_grid);
        // ImGui::Checkbox("Harmonic Snap", &snap_harmonics);

        // ImGui::End();
    }
};

// Put this near your other statics:
static Canvas2D gBeatOctCanvas;

int main(int, char**)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Sonnote",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    VulkanRenderer renderer = VulkanRenderer(window);


    // Audio
    AudioEngine engine;
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine." << std::endl;
        return 1;
    }
    std::cout << "Audio engine started." << std::endl;

    engine.setGain(0.4f);
    engine.setWaveform(WaveformType::Sine);
    int waveType = 0;
    engine.setTempo(120.0f);

    std::vector<Rect> rects;

    CurrentBox nw;
    CurrentBox cw;

    std::vector<Button> buttons;
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[0], false); // Export wav
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[1], false, 0.5f); // BPM
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[2], false, 0.06667f); // Attack 1 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[3], false, 0.06667f); // Decay 1 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[4], false, 0.16f); // Gain 1 (value is sent as is)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[5], false, 0.0f, texts[0]); // WAV name
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[6], false, 0.0f, texts[1]); // Jump size
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[7], false, 0.0f, texts[2]); // Split size
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[8], false); // export MIDI
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[9], false); // Save project
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[10], false); // Load project
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[11], false); // Change wave
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[12], false, 0.7f); // Sustain 1
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[13], false, 0.0083333f); // Release 1
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[14], false, 0.0f); // Wavetype 1
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[15], false, 0.0f); // Wavetype 2
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[16], false, 0.06667f); // Attack 2 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[17], false, 0.06667f); // Decay 2 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[18], false, 0.7f); // Sustain 2 (value is sent as is)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[19], false, 0.0083333f); // Release 2 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[20], false, 0.0f); // Gain 2 (value is sent as is)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[21], false, 0.0f); // Wavetype 3
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[22], false, 0.06667f); // Attack 3 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[23], false, 0.06667f); // Decay 3 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[24], false, 0.7f); // Sustain 3 (value is sent as is)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[25], false, 0.0083333f); // Release 3 (value * 3 is sent)
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[26], false, 0.0f); // Gain 3 (value is sent as is)
    engine.setADSR({buttons[2].value * 3.0f, buttons[3].value * 3.0f, buttons[12].value, buttons[13].value * 3.0f});

    rects.push_back({0.0f, 0.0f, 1.0f, NW_HEIGHT, 0.07f, 0.07f, 0.07f, 2.0f, F_MARGIN_PANEL});
    rects.push_back({0.0f, 0.0f, 1.0f, NW_HEIGHT, 0.0f, 0.0f, 0.0f, 5.0f, F_NOTE_WINDOW});
    rects.push_back({0.0f, NW_HEIGHT, 1.0f, 1 - NW_HEIGHT, 0.15f, 0.15f, 0.15f, 2.0f, F_MARGIN_PANEL});
    rects.push_back({0.0f, NW_HEIGHT, 1.0f, 1 - NW_HEIGHT, 0.01f, 0.03f, 0.07f, 5.0f, F_CONTROL_WINDOW});
    // BPM
    rects.push_back({10.0f, 10.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 1.0f, F_SLIDER_CW});
    // OSC1 Settings
    rects.push_back({140.0f, 10.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 14.0f, F_BUTTON_CW});
    rects.push_back({140.0f, 30.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 2.0f, F_SLIDER_CW});
    rects.push_back({140.0f, 50.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 3.0f, F_SLIDER_CW});
    rects.push_back({140.0f, 70.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 12.0f, F_SLIDER_CW});
    rects.push_back({140.0f, 90.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 13.0f, F_SLIDER_CW});
    rects.push_back({140.0f, 110.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 4.0f, F_SLIDER_CW});
    // WAV Save name, jump size, split size text boxes
    rects.push_back({10.0f, 145.0f, 120.0f, 15.0f, 0.2f, 0.2f, 0.2f, 5.0f, F_TEXT_BOX_CW});
    rects.push_back({10.0f, 180.0f, 120.0f, 15.0f, 0.2f, 0.2f, 0.2f, 6.0f, F_TEXT_BOX_CW});
    rects.push_back({10.0f, 215.0f, 120.0f, 15.0f, 0.2f, 0.2f, 0.2f, 7.0f, F_TEXT_BOX_CW});
    // Export WAV, export MIDI, save project, load project buttons
    rects.push_back({10.0f, 30.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 0.0f, F_BUTTON_CW});
    rects.push_back({10.0f, 50.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 8.0f, F_BUTTON_CW});
    rects.push_back({10.0f, 70.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 9.0f, F_BUTTON_CW});
    rects.push_back({10.0f, 90.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 10.0f, F_BUTTON_CW});
    rects.push_back({10.0f, 110.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 11.0f, F_BUTTON_CW});
    // OSC2 Settings
    rects.push_back({270.0f, 10.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 15.0f, F_BUTTON_CW});
    rects.push_back({270.0f, 30.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 16.0f, F_SLIDER_CW});
    rects.push_back({270.0f, 50.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 17.0f, F_SLIDER_CW});
    rects.push_back({270.0f, 70.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 18.0f, F_SLIDER_CW});
    rects.push_back({270.0f, 90.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 19.0f, F_SLIDER_CW});
    rects.push_back({270.0f, 110.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 20.0f, F_SLIDER_CW});
    // OSC3 Settings
    rects.push_back({400.0f, 10.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 21.0f, F_BUTTON_CW});
    rects.push_back({400.0f, 30.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 22.0f, F_SLIDER_CW});
    rects.push_back({400.0f, 50.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 23.0f, F_SLIDER_CW});
    rects.push_back({400.0f, 70.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 24.0f, F_SLIDER_CW});
    rects.push_back({400.0f, 90.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 25.0f, F_SLIDER_CW});
    rects.push_back({400.0f, 110.0f, 120.0f, 15.0f, 0.8f, 0.7f, 0.3f, 26.0f, F_SLIDER_CW});

    // 1) Build spectrogram on CPU
    GrayImage img = buildSpectrogramTextureR8(
        "file.wav",
        2048,        // FFT size
        512,         // hop
        512,         // log rows
        4096,        // texture width
        -80.0f,      // dB floor
        20.0f,
        20000.0f
    );

    // for (int i = 0; i < (int)img.px.size(); i++) {
    //     printf("%d ", img.px[i]);
    // }

    AudioData audio = loadWavMono("file.wav");

    renderer.createSpectrogramPipeline();

    // 2) Upload to GPU
    renderer.uploadSpectrogramR8(
        img.px.data(),
        img.w,
        img.h
    );

    // --- Cache metadata for rendering ---
    // spectrogram.loaded = true;
    // spectrogram.durationSeconds =
    //     (float)audio.mono.size() / (float)audio.sampleRate;
    // spectrogram.textureWidth = img.w;

    bool running = true;
    bool playing = false;
    bool size_changed = false;
    bool rerender = true;
    bool ctrl = false;
    int selected = -1;
    int bpm = 110;
    int click = 0;
    int dx, dy;
    int sens = 15;
    int currentProfile = 0;
    bool windowActive = true;
    dx = dy = 0;
    Vec2 hover = {-1.0f, -1.0f};
    std::vector<char> keys;
    Profile profileSettings;
    while (running) {
        SDL_Event e;
        keys.clear();
        if (ctrl) {
            keys.push_back('~');
        }
        dx = dy = 0;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_WINDOWEVENT) {
                switch (e.window.event) {
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                        windowActive = true;
                        break;

                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        windowActive = false;
                        break;
                }
            }
            rerender = true;
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window)) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                size_changed = true;
                renderer.onResize(window);
            }
            if (e.type == SDL_MOUSEBUTTONDOWN) {
                selected = -1;
                int dw, dh;
                SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
                float ax = e.button.x;
                float ay = (float)dh - e.button.y;
                float nx = e.button.x  / (float)dh;
                float ny = ((float)dh - e.button.y) / (float)dh;

                if (e.button.button == SDL_BUTTON_LEFT) {
                    click = 1;
                    for (int i = 0; i < (int)buttons.size(); i++) {
                        if (ax >= buttons[i].x1 && ax <= buttons[i].x2 && \
                            ay >= buttons[i].y1 && ay <= buttons[i].y2) {
                            if (i == 5 || i == 6 || i == 7) selected = i;
                            buttons[i].hit = true;
                            if (i != 14 && i != 15 && i != 21)
                                buttons[i].value = (ax - buttons[i].x1) / (buttons[i].x2  - buttons[i].x1);
                            if (i == 1) {
                                engine.setTempo(buttons[1].value * 180 + 20);
                            } else if (i == 2 || i == 3 || i == 4 || (i >= 12 && i <= 26)) {
                                // Set and send current profile
                                profileSettings.settings[0].attack = buttons[2].value * 3.0f;
                                profileSettings.settings[0].decay = buttons[3].value * 3.0f;
                                profileSettings.settings[0].gain = buttons[4].value;
                                profileSettings.settings[0].sustain = buttons[12].value;
                                profileSettings.settings[0].release = buttons[13].value * 3.0f;
                                if (i == 14) {
                                    buttons[14].value = buttons[14].value + 1.0f;
                                    if (std::floor(buttons[14].value) == 5) {
                                        buttons[14].value = 0.0f;
                                    }
                                }
                                int type = (int)buttons[14].value;
                                if (type == 0) {
                                    profileSettings.settings[0].waveType = WaveformType::Sine;
                                    sprintf(buttons[14].label, "OSC1: Sine");
                                } else if (type == 1) {
                                    profileSettings.settings[0].waveType = WaveformType::Square;
                                    sprintf(buttons[14].label, "OSC1: Square");
                                } else if (type == 2) {
                                    profileSettings.settings[0].waveType = WaveformType::Saw;
                                    sprintf(buttons[14].label, "OSC1: Saw");
                                } else if (type == 3) {
                                    profileSettings.settings[0].waveType = WaveformType::Triangle;
                                    sprintf(buttons[14].label, "OSC1: Triangle");
                                } else if (type == 4) {
                                    profileSettings.settings[0].waveType = WaveformType::Noise;
                                    sprintf(buttons[14].label, "OSC1: Noise");
                                }
                                if (i == 15) {
                                    buttons[15].value = buttons[15].value + 1.0f;
                                    if (std::floor(buttons[15].value) == 5) {
                                        buttons[15].value = 0.0f;
                                    }
                                }
                                type = (int)buttons[15].value;
                                if (type == 0) {
                                    profileSettings.settings[1].waveType = WaveformType::Sine;
                                    sprintf(buttons[15].label, "OSC2: Sine");
                                } else if (type == 1) {
                                    profileSettings.settings[1].waveType = WaveformType::Square;
                                    sprintf(buttons[15].label, "OSC2: Square");
                                } else if (type == 2) {
                                    profileSettings.settings[1].waveType = WaveformType::Saw;
                                    sprintf(buttons[15].label, "OSC2: Saw");
                                } else if (type == 3) {
                                    profileSettings.settings[1].waveType = WaveformType::Triangle;
                                    sprintf(buttons[15].label, "OSC2: Triangle");
                                } else if (type == 4) {
                                    profileSettings.settings[1].waveType = WaveformType::Noise;
                                    sprintf(buttons[15].label, "OSC2: Noise");
                                }
                                profileSettings.settings[1].attack = buttons[16].value * 3.0f;
                                profileSettings.settings[1].decay = buttons[17].value * 3.0f;
                                profileSettings.settings[1].sustain = buttons[18].value;
                                profileSettings.settings[1].release = buttons[19].value * 3.0f;
                                profileSettings.settings[1].gain = buttons[20].value;
                                if (i == 21) {
                                    buttons[i].value = buttons[21].value + 1.0f;
                                    if (std::floor(buttons[i].value) == 5) {
                                        buttons[i].value = 0.0f;
                                    }
                                }
                                type = (int)buttons[21].value;
                                if (type == 0) {
                                    profileSettings.settings[2].waveType = WaveformType::Sine;
                                    sprintf(buttons[21].label, "OSC3: Sine");
                                } else if (type == 1) {
                                    profileSettings.settings[2].waveType = WaveformType::Square;
                                    sprintf(buttons[21].label, "OSC3: Square");
                                } else if (type == 2) {
                                    profileSettings.settings[2].waveType = WaveformType::Saw;
                                    sprintf(buttons[21].label, "OSC3: Saw");
                                } else if (type == 3) {
                                    profileSettings.settings[2].waveType = WaveformType::Triangle;
                                    sprintf(buttons[21].label, "OSC3: Triangle");
                                } else if (type == 4) {
                                    profileSettings.settings[2].waveType = WaveformType::Noise;
                                    sprintf(buttons[21].label, "OSC3: Noise");
                                }
                                profileSettings.settings[2].attack = buttons[22].value * 3.0f;
                                profileSettings.settings[2].decay = buttons[23].value * 3.0f;
                                profileSettings.settings[2].sustain = buttons[24].value;
                                profileSettings.settings[2].release = buttons[25].value * 3.0f;
                                profileSettings.settings[2].gain = buttons[26].value;
                                printf("SP");
                                engine.setProfile(profileSettings, currentProfile);
                                // engine.setADSR({buttons[2].value * 3.0f, buttons[3].value * 3.0f, buttons[12].value, buttons[13].value * 3.0f});
                            } else if (i == 4) {
                                engine.setGain(buttons[4].value);
                            }
                        }
                    }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    click = 2;
                }
            }
            if (e.type == SDL_MOUSEBUTTONUP) {
                click = 0;
            }
            if (e.type == SDL_KEYDOWN) {
                char key = e.key.keysym.sym;
                if (e.key.keysym.sym == SDLK_LCTRL) {
                    ctrl = true;
                } else {
                    keys.push_back(key);
                }
                if (key == SDLK_ESCAPE) running = false;

                if (key >= 48 && key <= 57) {
                    currentProfile = (int)key - 48;
                    profileSettings = engine.getProfile(currentProfile);
                    switch (profileSettings.settings[0].waveType) {
                        case (WaveformType::Sine):
                            buttons[14].value = 0.0f;
                            sprintf(buttons[14].label, "OSC1: Sine");
                            break;
                        case (WaveformType::Square):
                            buttons[14].value = 1.0f;
                            sprintf(buttons[14].label, "OSC1: Square");
                            break;
                        case (WaveformType::Saw):
                            buttons[14].value = 2.0f;
                            sprintf(buttons[14].label, "OSC1: Saw");
                            break;
                        case (WaveformType::Triangle):
                            buttons[14].value = 3.0f;
                            sprintf(buttons[14].label, "OSC1: Triangle");
                            break;
                        default:
                            buttons[14].value = 4.0f;
                            sprintf(buttons[14].label, "OSC1: Noise");
                    }
                    switch (profileSettings.settings[1].waveType) {
                        case (WaveformType::Sine):
                            buttons[15].value = 0.0f;
                            sprintf(buttons[15].label, "OSC2: Sine");
                            break;
                        case (WaveformType::Square):
                            buttons[15].value = 1.0f;
                            sprintf(buttons[15].label, "OSC2: Square");
                            break;
                        case (WaveformType::Saw):
                            buttons[15].value = 2.0f;
                            sprintf(buttons[15].label, "OSC2: Saw");
                            break;
                        case (WaveformType::Triangle):
                            buttons[15].value = 3.0f;
                            sprintf(buttons[15].label, "OSC2: Triangle");
                            break;
                        default:
                            buttons[15].value = 4.0f;
                            sprintf(buttons[15].label, "OSC2: Noise");
                    }
                    switch (profileSettings.settings[2].waveType) {
                        case (WaveformType::Sine):
                            buttons[21].value = 0.0f;
                            sprintf(buttons[21].label, "OSC3: Sine");
                            break;
                        case (WaveformType::Square):
                            buttons[21].value = 1.0f;
                            sprintf(buttons[21].label, "OSC3: Square");
                            break;
                        case (WaveformType::Saw):
                            buttons[21].value = 2.0f;
                            sprintf(buttons[21].label, "OSC3: Saw");
                            break;
                        case (WaveformType::Triangle):
                            buttons[21].value = 3.0f;
                            sprintf(buttons[21].label, "OSC3: Triangle");
                            break;
                        default:
                            buttons[21].value = 4.0f;
                            sprintf(buttons[21].label, "OSC3: Noise");
                    }
                    buttons[2].value = profileSettings.settings[0].attack / 3.0f;
                    buttons[3].value = profileSettings.settings[0].decay / 3.0f;
                    buttons[4].value = profileSettings.settings[0].gain;
                    buttons[12].value = profileSettings.settings[0].sustain;
                    buttons[13].value = profileSettings.settings[0].release / 3.0f;
                    buttons[16].value = profileSettings.settings[1].attack / 3.0f;
                    buttons[17].value = profileSettings.settings[1].decay / 3.0f;
                    buttons[18].value = profileSettings.settings[1].sustain;
                    buttons[19].value = profileSettings.settings[1].release / 3.0f;
                    buttons[20].value = profileSettings.settings[1].gain;
                    buttons[22].value = profileSettings.settings[2].attack / 3.0f;
                    buttons[23].value = profileSettings.settings[2].decay / 3.0f;
                    buttons[24].value = profileSettings.settings[2].sustain;
                    buttons[25].value = profileSettings.settings[2].release / 3.0f;
                    buttons[26].value = profileSettings.settings[2].gain;
                }
                if (selected != -1) {
                    if (key >= 32 && key <= 126 && (selected == 5 || selected == 6 || selected == 7) && strlen(buttons[selected].text) <= 14) {
                        if (buttons[selected].text[0] == ' ') {
                            sprintf(buttons[selected].text, "%c", key); 
                        } else
                            sprintf(buttons[selected].text, "%s%c", buttons[selected].text, key);
                    }
                    else if ((key == 127 || key == 8) && strlen(buttons[selected].text) > 0) {
                        buttons[selected].text[strlen(buttons[selected].text) - 1] = '\0';
                    }
                } else {
                    if (key == 'n') {
                        engine.toggleFreeze();
                    }
                }
            }
            if (e.type == SDL_KEYUP) {
                if (e.key.keysym.sym == SDLK_LCTRL) {
                    ctrl = false;
                }
            }
            if (e.type == SDL_MOUSEMOTION) {
                int dw, dh;
                SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
                float nx = e.button.x  / (float)dw;
                float ny = ((float)dh - e.button.y) / (float)dh;
                
                if (nx >= nw.x1 && nx <= nw.x2 && ny >= nw.y1 && ny <= nw.y2) {
                    hover.x = nx;
                    hover.y = ny;
                } else {
                    hover.x = -1.0f;
                    hover.y = -1.0f;
                }
            }
            if (e.type == SDL_MOUSEWHEEL) {
                dx = e.wheel.x * sens;
                dy = e.wheel.y * sens;

                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    dx = -dx;
                    dy = -dy;
                }
            }
        }

        // Process events

        bpm = (int)(buttons[1].value * 180 + 20);

        if (play_triggered_from_space) { // Handle play
            play_triggered_from_space = false;
            if (!playing) {
                // Build events from current points
                uint32_t sr = 48000; // if you don’t have gDevice here, store it globally or pass it out

                auto ev = build_events_from_notes(gBeatOctCanvas.notes, bpm, sr, playhead_time);
                
                printf("first; %f\n", ev[0].beat);

                engine.setEvents(ev);
                engine.resetSampleCounter();
                engine.setPlayState(true);
            } else {
                engine.setPlayState(false);
            }
            playing = !playing;
        }

        if (buttons[0].hit) {
            float max = -1.0f;
            for (int i = 0; i < (int)gBeatOctCanvas.notes.size(); i++) {
                if (gBeatOctCanvas.notes[i].beat_off > max) max = gBeatOctCanvas.notes[i].beat_off;
            }

            float lengthSecs = (60.0 / (float)bpm) * (max - playhead_time);
            if (lengthSecs > 0.0) {
                auto ev = build_events_from_notes(gBeatOctCanvas.notes, bpm, 48000, playhead_time);
                    
                engine.setEvents(ev);
                SDL_Delay(50);
                engine.exportWav(buttons[5].text, playhead_time, lengthSecs);
            }
            buttons[0].hit = false;
        }

        if (buttons[8].hit) { // if export midi button is pressed
            save_midi_file_mpe_format1("take1.mid", gBeatOctCanvas.notes, bpm);
            buttons[8].hit = false;
        }

        if (buttons[9].hit) { // if save project button is pressed
            Profile ps[10];
            for (int i = 0; i < 10; i++) {
                ps[i] = engine.getProfile(i);
                if (i == 0) {
                    printf("attack: %f\n", ps[i].settings[0].attack);
                    uint32_t val_number32;
                    memcpy(&val_number32, &ps[i].settings[0].attack, sizeof(val_number32));
                    printf("attackNum: %08x", val_number32);
                }
            }
            save_song("session.son", gBeatOctCanvas.notes, gBeatOctCanvas.foci, *gBeatOctCanvas.getSplits(), ps);
            buttons[9].hit = false;
        }

        if (buttons[10].hit) { // If load project button is pressed
            std::unique_ptr<TreeNode> loadedSplits;
            Profile ps[10];
            load_song("session.son", gBeatOctCanvas.notes, gBeatOctCanvas.foci, loadedSplits, ps);
            for (int i = 0; i < 10; i++) {
                if (i == 0) {
                    printf("attack: %f", ps[i].settings[0].attack);
                }
                engine.setProfile(ps[i], i);
            }
            gBeatOctCanvas.setSplits(std::move(loadedSplits));
            buttons[10].hit = false;
        }

        if (buttons[11].hit) {
            switch (waveType) {
                case 0:
                    engine.setWaveform(WaveformType::Square);
                    break;
                case 1:
                    engine.setWaveform(WaveformType::Saw);
                    break;
                case 2:
                    engine.setWaveform(WaveformType::Triangle);
                    break;
                case 3:
                    engine.setWaveform(WaveformType::Sine);
                    break;
            }
            printf("waveType: %d\n", waveType);
            waveType++;
            if (waveType == 4) waveType = 0;
            buttons[11].hit = false;
        }

        // Refer to notes for what was here
        // Clear vertex list and rebuild from rectangles
        if (windowActive && rerender) {
            renderer.vertices.clear();
            int dw, dh;
            SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
            for (auto& r : rects) {
                float x, y, w, h;
                float sliderPoint = -1.0f;
                x = r.x * dw;
                y = r.y * dh;
                w = r.w * dw;
                h = r.h * dh;
                bool selection = false;
                switch (r.function) {
                    case F_SLIDER_CW:
                    case F_TEXT_BOX_CW:
                    case F_BUTTON_CW: {
                        // x is distance from left edge of control window
                        // y is distances from top edge of control window
                        x = r.x + (cw.x1 * dw);
                        y = (cw.y2 * (int)dh) - r.y - r.h;
                        w = r.w;
                        h = r.h;
                        buttons[(int)r.utility].x1 = x;
                        buttons[(int)r.utility].x2 = x + w;
                        buttons[(int)r.utility].y1 = y;
                        buttons[(int)r.utility].y2 = y + h;
                        if (r.function == F_SLIDER_CW) {
                            sliderPoint = x + r.w * buttons[(int)r.utility].value;
                            if (buttons[(int)r.utility].label[0] == 'B') {
                                sprintf(buttons[(int)r.utility].label, "BPM: %d", (int)std::floor(buttons[(int)r.utility].value * 180 + 20));
                            } else if (buttons[(int)r.utility].label[0] == 'A') {
                                sprintf(buttons[(int)r.utility].label, "Attack: %.3f", buttons[(int)r.utility].value * 3.0f);
                            } else if (buttons[(int)r.utility].label[0] == 'D') {
                                sprintf(buttons[(int)r.utility].label, "Decay: %.3f", buttons[(int)r.utility].value * 3.0f);
                            } else if (buttons[(int)r.utility].label[0] == 'S') {
                                sprintf(buttons[(int)r.utility].label, "Sustain: %.3f", buttons[(int)r.utility].value);
                            } else if (buttons[(int)r.utility].label[0] == 'R') {
                                sprintf(buttons[(int)r.utility].label, "Release: %.3f", buttons[(int)r.utility].value * 3.0f);
                            } else if (buttons[(int)r.utility].label[0] == 'G') {
                                sprintf(buttons[(int)r.utility].label, "Gain: %.3f", buttons[(int)r.utility].value * 5.0);
                            }
                        } else if (r.function == F_TEXT_BOX_CW) {
                            if (selected == (int)r.utility)
                                selection = true;
                            renderer.addText(buttons[(int)r.utility].label, x + 2, y + 19, 15.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                            renderer.addText(buttons[(int)r.utility].text, x + 2, y + 2, 15.0f, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                        }
                        if (r.function != F_TEXT_BOX_CW)
                            renderer.addText(buttons[(int)r.utility].label, x + 2, y + 2, 15.0f, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                        break;
                    } case F_BORDER:
                        x -= r.utility;
                        y -= r.utility;
                        w += r.utility * 2;
                        h += r.utility * 2;
                        break;
                    case F_MARGIN_PANEL:
                    case F_NOTE_WINDOW:
                    case F_CONTROL_WINDOW:
                        x += r.utility;
                        y += r.utility;
                        w -= r.utility * 2;
                        h -= r.utility * 2;
                        if (size_changed) {
                            if (r.function == F_NOTE_WINDOW) {
                                nw.x1 = x / dw;
                                nw.y1 = y / dh;
                                nw.x2 = (x + w) / dw;
                                nw.y2 = (y + h) / dh;
                            } else if (r.function == F_CONTROL_WINDOW) {
                                cw.x1 = x / dw;
                                cw.y1 = y / dh;
                                cw.x2 = (x + w) / dw;
                                cw.y2 = (y + h) / dh;
                            }
                        }
                        break;
                }

                if (!selection)
                    renderer.addRect(x, y, w, h, r.r, r.g, r.b);
                else
                    renderer.addRect(x, y, w, h, 0.4f, 0.4f, 0.8f);
                if (sliderPoint != -1.0f)
                    renderer.addRect(sliderPoint - 1, y, 2, h, 0.8f, 0.0, 0.0);
            }
            // printf("1");

            CurrentBox screenNw;
            screenNw.x1 = nw.x1 * dw;
            screenNw.x2 = nw.x2 * dw;
            screenNw.y1 = nw.y1 * dh;
            screenNw.y2 = nw.y2 * dh;

            Vec2 screenHover;
            screenHover.x = hover.x * dw;
            screenHover.y = hover.y * dh;

            gBeatOctCanvas.draw(renderer, screenNw, screenHover, click, keys, buttons[6].text, buttons[7].text, dx, dy);

            renderer.updateVertexData();
            renderer.drawFrame(std::floor(screenNw.x1), std::floor(screenNw.y1), std::floor(screenNw.x2 - screenNw.x1), std::floor(screenNw.y2 - screenNw.y1));
            if (size_changed) size_changed = false;
            rerender = false;
        } else {
            SDL_Delay(50);
        }
    }

    engine.stop();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

