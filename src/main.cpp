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
    const char* label;
    bool hit;
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

#define NW_HEIGHT 0.75f

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

// --- Write binary song file ---
bool save_song(const std::string& path,
               const std::vector<Note>& notes,
               const std::vector<FocusChange>& focusChanges)
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
    }

    // Focus changes
    uint32_t numFocus = static_cast<uint32_t>(focusChanges.size());
    f.write(reinterpret_cast<const char*>(&numFocus), sizeof(numFocus));
    for (auto& fc : focusChanges) {
        f.write(reinterpret_cast<const char*>(&fc.time), sizeof(float));
        f.write(reinterpret_cast<const char*>(&fc.octave), sizeof(float));
    }

    f.close();
    return true;
}

// --- Load binary song file ---
bool load_song(const std::string& path,
               std::vector<Note>& notes,
               std::vector<FocusChange>& focusChanges)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    char magic[4];
    f.read(magic, 4);
    if (std::strncmp(magic, "SONN", 4) != 0) {
        throw std::runtime_error("Invalid file format");
    }

    uint32_t numNotes = 0;
    f.read(reinterpret_cast<char*>(&numNotes), sizeof(numNotes));
    notes.resize(numNotes);
    for (uint32_t i = 0; i < numNotes; ++i) {
        f.read(reinterpret_cast<char*>(&notes[i].beat_on), sizeof(float));
        f.read(reinterpret_cast<char*>(&notes[i].beat_off), sizeof(float));
        f.read(reinterpret_cast<char*>(&notes[i].octave), sizeof(float));
    }

    uint32_t numFocus = 0;
    f.read(reinterpret_cast<char*>(&numFocus), sizeof(numFocus));
    focusChanges.resize(numFocus);
    for (uint32_t i = 0; i < numFocus; ++i) {
        f.read(reinterpret_cast<char*>(&focusChanges[i].time), sizeof(float));
        f.read(reinterpret_cast<char*>(&focusChanges[i].octave), sizeof(float));
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

        SeqEvent e1;
        e1.type = 1;
        e1.beat  = n.beat_on;
        e1.value       = c4 * std::pow(2.0f, n.octave - 4.0f);
        ev.push_back(e1);
        SeqEvent e2;
        e2.type = 0;
        e2.beat = n.beat_off; // exclusive
        e2.value = c4 * std::pow(2.0f, n.octave - 4.0f);
        ev.push_back(e2);
        std::fflush(stdout);
    }
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
                printf("1");
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

// Put this near your other statics:
// static Canvas2D gBeatOctCanvas;

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

    fflush(stdout);
    // Audio
    AudioEngine engine;
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine." << std::endl;
        return 1;
    }
    std::cout << "Audio engine started." << std::endl;

    engine.setGain(0.4f);
    engine.setWaveform(WaveformType::Sine);
    engine.setTempo(120.0f);

    std::vector<Rect> rects;

    CurrentBox nw;
    CurrentBox cw;
    float noteX, noteY;
    noteX = noteY = -1.0f;

    const char* buttonLabels[10];
    buttonLabels[0] = "Test";
    std::vector<Button> buttons;
    buttons.emplace_back(0.0f, 0.0f, 0.0f, 0.0f, buttonLabels[0]);


    rects.push_back({0.0f, 0.0f, 1.0f, NW_HEIGHT, 0.07f, 0.07f, 0.07f, 2.0f, F_MARGIN_PANEL});
    rects.push_back({0.0f, 0.0f, 1.0f, NW_HEIGHT, 0.0f, 0.0f, 0.0f, 5.0f, F_NOTE_WINDOW});
    rects.push_back({0.0f, NW_HEIGHT, 1.0f, 1 - NW_HEIGHT, 0.15f, 0.15f, 0.15f, 2.0f, F_MARGIN_PANEL});
    rects.push_back({0.0f, NW_HEIGHT, 1.0f, 1 - NW_HEIGHT, 0.01f, 0.03f, 0.07f, 5.0f, F_CONTROL_WINDOW});
    rects.push_back({8.0f, 20.0f, 60.0f, 15.0f, 0.8f, 0.7f, 0.3f, 0.0f, F_BUTTON_CW});

    bool running = true;
    bool playing = false;
    bool size_changed = false;
    bool rerender = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            rerender = true;
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window)) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                size_changed = true;
                renderer.onResize(window);
            }
            if (e.type == SDL_MOUSEBUTTONDOWN) {
                int dw, dh;
                SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
                float ax = e.button.x;
                float ay = (float)dh - e.button.y;
                float nx = e.button.x  / (float)dh;
                float ny = ((float)dh - e.button.y) / (float)dh;

                if (e.button.button == SDL_BUTTON_LEFT) {
                    for (int i = 0; i < (int)buttons.size(); i++) {
                        if (ax >= buttons[i].x1 && ax <= buttons[i].x2 && \
                            ay >= buttons[i].y1 && ay <= buttons[i].y2) {
                            buttons[i].hit = true;
                        }
                    }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {

                }
            }
            if (e.type == SDL_MOUSEMOTION) {
                int dw, dh;
                SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
                float nx = e.button.x  / (float)dh;
                float ny = ((float)dh - e.button.y) / (float)dh;
                
                if (nx >= nw.x1 && nx <= nw.x2 && ny >= nw.y1 && ny <= nw.y2) {
                    noteX = nx;
                    noteY = ny;
                } else {
                    noteX = -1.0f;
                    noteY = -1.0f;
                }
            }
        }

        // Process events
        

        // Refer to notes for what was here

        // Clear vertex list and rebuild from rectangles
        if (rerender) {
            renderer.vertices.clear();
            int dw, dh;
            SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
            for (auto& r : rects) {
                float x, y, w, h;
                x = r.x * dw;
                y = r.y * dh;
                w = r.w * dw;
                h = r.h * dh;
                switch (r.function) {
                    case F_BUTTON_CW: {
                        // x is distance from left edge of control window
                        // y is distances from top edge of control window
                        x = r.x + (cw.x1 * dw);
                        y = (cw.y2 * (int)dh) - r.y - r.h;
                        printf("cw.y2: %f, dh: %d, r.y: %f, r.h: %f, x: %f, y: %f\n", cw.y2, dh, r.y, r.h, x, y);
                        w = r.w;
                        h = r.h;
                        buttons[(int)r.utility].x1 = x;
                        buttons[(int)r.utility].x2 = x + w;
                        buttons[(int)r.utility].y1 = y;
                        buttons[(int)r.utility].y2 = y + h;
                        if (x >= 0.0f && y >= 0.0f)
                        renderer.addText(buttons[(int)r.utility].label, x, y, 10.0f, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
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

                renderer.addRect(x, y, w, h, r.r, r.g, r.b);
            }

            renderer.updateVertexData();
            renderer.drawFrame();
            if (size_changed) size_changed = false;
            rerender = false;
        }
    }

    engine.stop();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

