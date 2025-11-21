#include <SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"
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


// ======= Pannable 2D Canvas (beats x octaves) =======
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
    ImVec2 anchorW{0,0};
    float split_start = -1.0f;

    Note currently_dragged;
    bool dragging = false;

    bool hover_valid = false;
    ImVec2 hoverW{0,0};

    // Transform: world -> screen
    ImVec2 worldToScreen(ImVec2 topLeft, ImVec2 w) const {
        // y increases upward in world, downward on screen → invert with (y0 - y)
        float sxp = topLeft.x + (w.x - x0) * sx;
        float syp = topLeft.y + (y0 - w.y) * sy;
        return {sxp, syp};
    }
    // Transform: screen -> world
    ImVec2 screenToWorld(ImVec2 topLeft, ImVec2 s) const {
        float wx = x0 + (s.x - topLeft.x) / sx;
        float wy = y0 - (s.y - topLeft.y) / sy;
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
        float distance = 9.0;
        float closest = std::floor(v);
        for (int i = 0; i < (int)points.size(); i++) {
            if (std::abs(points[i] - v) < distance) {
                distance = std::abs(points[i] - v);
                closest = points[i];
            }
        }
        return closest;
    }

    void drawRhythmLine(ImVec2 s0, ImDrawList* dl, std::vector<int>& divisions) {
        dl->AddLine({s0.x, s0.y + 10}, {s0.x, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
        if ((int)divisions.size() > 1) {
            switch (divisions.back()) {
                case 8:
                    dl->AddLine({s0.x - 3, s0.y - 16}, {s0.x + 3, s0.y - 16}, IM_COL32(120,120,160,255), 1.0f);
                case 4:
                    dl->AddLine({s0.x - 3, s0.y - 13}, {s0.x + 3, s0.y - 13}, IM_COL32(120,120,160,255), 1.0f);
                case 2:
                    dl->AddLine({s0.x - 3, s0.y - 10}, {s0.x + 3, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 3:
                    dl->AddLine({s0.x - 3, s0.y - 11}, {s0.x + 3, s0.y - 9}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 5:
                    dl->AddLine({s0.x - 3, s0.y - 12}, {s0.x, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
                    dl->AddLine({s0.x + 3, s0.y - 12}, {s0.x, s0.y - 10}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 7:
                    dl->AddLine({s0.x + 3, s0.y - 10}, {s0.x, s0.y - 12}, IM_COL32(120,120,160,255), 1.0f);
                    dl->AddLine({s0.x - 3, s0.y - 10}, {s0.x, s0.y - 12}, IM_COL32(120,120,160,255), 1.0f);
                    break;
                case 6:
                    dl->AddLine({s0.x - 3, s0.y - 12}, {s0.x + 3, s0.y - 8}, IM_COL32(120,120,160,255), 1.0f);
                    dl->AddLine({s0.x - 3, s0.y - 8}, {s0.x + 3, s0.y - 12}, IM_COL32(120,120,160,255), 1.0f);
                    break;
            }
        }
    }

    void drawRhythmLines(TreeNode* current, const std::vector<Note>& sortedStart, const std::vector<Note>& sortedEnd, \
        float start, float end, std::vector<int> divisions, std::vector<float>& currently_held, ImVec2 topLeft, ImDrawList* dl) {
        // printf("start\n");
        float beat_size = std::abs(end - start) / divisions.back();
        int latest_starti = 0;
        int latest_endi = 0;
        // printf("start: %f, end: %f, beat_size %f, divisions: %d\n", start, end, beat_size, divisions.back());
        for (int i = 0; i < divisions.back() + 1; i++) {
            float current_beat = start + i * beat_size;
            // printf("current_beat: %f\n", current_beat);
            int starti = latest_starti;
            int endi = latest_endi;
            if ((int)sortedStart.size() > 0 && !((int)divisions.size() > 1 && i == 0) && !((int)divisions.size() > 1 && i == divisions.back())) {
                // printf("endi: %d, sEndsize:%d, sEndBeat_OFF: %f\n", endi, (int)sortedEnd.size(), sortedEnd[endi].beat_off);
                while (sortedStart[starti].beat_on < current_beat)
                    starti++;
                while (sortedEnd[endi].beat_off < current_beat)
                    endi++;
                if ((starti < (int)sortedStart.size() && sortedStart[starti].beat_on == current_beat) || (endi < (int)sortedEnd.size() && sortedEnd[endi].beat_off == current_beat)) {
                    while (starti < (int)sortedStart.size() && sortedStart[starti].beat_on == current_beat) {
                        // printf("adding: %f\n", sortedStart[starti].octave);
                        currently_held.push_back(sortedStart[starti].octave);
                        ImVec2 s0 = worldToScreen(topLeft, { current_beat, sortedStart[starti].octave });
                        drawRhythmLine(s0, dl, divisions);
                        latest_starti = starti;
                        starti++;
                    }
                    while (endi < (int)sortedEnd.size() && sortedEnd[endi].beat_off == current_beat) {
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
                        ImVec2 s0 = worldToScreen(topLeft, { current_beat, sortedEnd[endi].octave });
                        drawRhythmLine(s0, dl, divisions);
                        latest_endi = endi;
                        endi++;
                    }
                } else if ((int)currently_held.size() != 0) {
                    float avg = 0.0f;
                    for (int i = 0; i < (int)currently_held.size(); i++) {
                        avg += currently_held[i];
                    }
                    avg = avg / (int)currently_held.size();
                    ImVec2 s0 = worldToScreen(topLeft, { current_beat,  avg});
                    drawRhythmLine(s0, dl, divisions);

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
                        drawRhythmLines(childChild.get(), sortedStart, sortedEnd, start + i * beat_size, start + (i + 1) * beat_size, divisions, currently_held, topLeft, dl);
                        divisions.pop_back();
                    }
                }
            }
        }
        // printf("end\n");
    }

    void draw(const char* label, float height = 300.0f) {
        ImGui::Begin(label);

        // Reserve a canvas rect
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 topLeft  = ImGui::GetCursorScreenPos();
        topLeft.x = topLeft.x + 20;
        ImVec2 size     = { std::max(50.0f, avail.x), std::max(50.0f, avail.y - 50.0f) };
        ImVec2 bottomRight = { topLeft.x + size.x, topLeft.y + size.y };

        // Interaction hitbox
        ImGui::InvisibleButton("canvas_area", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(topLeft, bottomRight, IM_COL32(30,30,30,255));
        dl->AddRect(topLeft, bottomRight, IM_COL32(200,200,200,255));

        // --- Panning with RMB drag ---
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            // Convert pixel delta → world delta
            x0 -= d.x / sx;
            y0 += d.y / sy; // + because dragging down increases screen y but decreases world y
        }

        // Compute visible world rect
        ImVec2 wA = screenToWorld(topLeft,      topLeft);
        ImVec2 wB = screenToWorld(topLeft,      bottomRight);

        float wxMin = std::min(wA.x, wB.x), wxMax = std::max(wA.x, wB.x);
        float wyMin = std::min(wA.y, wB.y), wyMax = std::max(wA.y, wB.y);

        // --- Grid lines (integer beats/octaves) ---
        const ImU32 gridCol   = IM_COL32(60,60,60,255);
        const ImU32 axisCol   = IM_COL32(120,120,160,255);
        const ImU32 textCol   = IM_COL32(220,220,220,255);
        const ImU32 playheadCol = IM_COL32(200, 20, 20, 255);
        const ImU32 noteCol = IM_COL32(90, 160, 255, 255);
        const ImU32 focusCol = IM_COL32(255,160,90,200);

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
        drawRhythmLines(splits.get(), sorted, sortedEnd, beatStart, beatEnd, divisions, currently_held, topLeft, dl);

        // Horizontal: octaves at integer positions
        int octStart = (int)std::floor(wB.y);
        int octEnd   = (int)std::ceil (wA.y);

        // Draw playhead
        ImVec2 s0 = worldToScreen(topLeft, { playhead_time, wA.y });
        dl->AddLine({s0.x, topLeft.y}, {s0.x, bottomRight.y}, playheadCol);

        // Hover snapped position (for preview dot / rubber band)
        hover_valid = false;
        if (hovered) {
            ImVec2 s = ImGui::GetIO().MousePos;
            ImVec2 w = screenToWorld(topLeft, s);
            std::fflush(stdout);
            w.x = snap_split(w.x);
            if (snap_to_grid) {
                w.y = snap(w.y, snap_y);
            } else if (snap_harmonics) {
                float focus = 0.0f;
                for (int i = 0; i < (int)foci.size() - 1; i++) {
                    if (w.x > foci[i].time && w.x < foci[i + 1].time) {
                        focus = foci[i].octave;
                        break;
                    }
                }

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
            // printf("w.x: %f, w.y: %f\n", w.x, w.y);
            std::fflush(stdout);
            hoverW = w;
            hover_valid = true;
        }

        // Key handling (P to change playhead position, L to change note length, D to delete a note, F to change the note's frequency)
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_P)) 
            playhead_time = hoverW.x;
        else if (hovered && ImGui::IsKeyPressed(ImGuiKey_Space))
            play_triggered_from_space = true;

        if (hovered && (ImGui::IsKeyPressed(ImGuiKey_L) || ImGui::IsKeyPressed(ImGuiKey_D) || ImGui::IsKeyPressed(ImGuiKey_F) || ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_S))) {
            // Find closest note and side of note (start or end)
            if ((int)notes.size() > 0) {
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
                if (match_found) {
                    // Drag start or end of note
                    if (ImGui::IsKeyPressed(ImGuiKey_L)) {
                        anchorW.y = closest.octave;
                        if (std::abs(hoverW.x - closest.beat_off) < std::abs(hoverW.x - closest.beat_on)) {
                            anchorW.x = closest.beat_on;
                        } else {
                            anchorW.x = closest.beat_off;
                        }
                        placing = true;
                    }

                    if (!(ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_S))) {
                        // Delete the note because it will be replaced
                        notes.erase(notes.begin() + index);
                    } else {
                        if (ImGui::IsKeyPressed(ImGuiKey_J)) {
                            // Jump up by the written amount
                            notes[index].octave = notes[index].octave + (std::log2(parseDivisor(jump)));
                        } else if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                            std::fflush(stdout);
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
                    }

                    // Frequency drag
                    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                        dragging = true;
                        currently_dragged = closest;
                    }
                    


                    // Delete the focus if the note had focus on it
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

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (!placing) {
                // Find closest note
                std::vector<Note> within;
                for (const Note& n : notes) {
                    if (hoverW.x <= n.beat_off && hoverW.x  >= n.beat_on)
                        within.push_back(n);
                }
                if ((int)within.size() > 0) {
                    Note closest = within[0];
                    float distance = 999999;
                    for (const Note& n : within) {
                        float current = std::abs(n.octave - hoverW.y);
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
                        if (foci[i].time > hoverW.x) {
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
                    bool extending_focus = closest.octave == foci[insert - 2].octave;
                    bool extend_to_note_start = insert == 0 || closest.beat_on > foci[insert - 1].time;
                    bool start_coincides_with_other_start = insert > 0 && closest.beat_on == foci[insert - 1].time;
                    if (start_coincides_with_other_start) {
                        newFoci[insert - 1].time = closest.beat_on;
                        newFoci[insert - 1].octave = closest.octave;
                    } else if (extend_to_note_start) {
                        // printf("to_start\n");
                        std::fflush(stdout);
                        // The note the user clicked on starts after than the previous focus change
                        start.time = closest.beat_on;
                        start.octave = closest.octave;
                    } else if (!focus_before) {
                        // There is a period of rest in focus before the previous focus change
                        // Nonetheless we must check if there is another focus change that happens after the note starts
                        if (insert > 1 && closest.beat_on < foci[insert - 2].time) {
                            // Change insert - 2 to be the current frequency instead of zero, then delete insert - 1
                            newFoci[insert - 2].octave = closest.octave;
                            removePrevious = true;
                        } else {
                            newFoci[insert - 1].time = closest.beat_on;
                            newFoci[insert - 1].octave = closest.octave;
                        }
                    } else {
                        // The note starts before the previous focus change and there is another focus immediately before it (or it is the first focus)
                        newFoci[insert - 1].octave = closest.octave;
                    }

                    bool extend_to_note_end = (int)foci.size() == 0 || insert == (int)foci.size() || insert != (int)foci.size() && closest.beat_off < foci[insert].time;
                    bool end_coincides_with_other_end = insert <= (int)foci.size() - 1 && closest.beat_off == foci[insert].time;
                    if (end_coincides_with_other_end) {
                        newFoci[insert].time = closest.beat_off;
                        newFoci[insert].octave = previousOctave;
                    } else if (extend_to_note_end) {
                        // The note ends before the next focus change
                        end.time = closest.beat_off;
                        end.octave = previousOctave;
                        // printf("prev: %f\n", previousOctave);
                        std::fflush(stdout);
                    } else if (!focus_after) {
                        // Still must check if the one after the next happens before the end of the note
                        if (insert == (int)foci.size() || foci[insert + 1].time < closest.beat_off) {
                            removeNext = true;
                        } else {
                            newFoci[insert].time = closest.beat_off;
                            newFoci[insert].octave = 0.0f;
                        }
                    }

                    if (removePrevious)
                        newFoci.erase(newFoci.begin() + (insert - 1));
                    if (removeNext)
                        newFoci.erase(newFoci.begin() + insert);

                    if (extend_to_note_start && extend_to_note_end) {
                        newFoci.insert(newFoci.begin() + insert, start);
                        newFoci.insert(newFoci.begin() + insert + 1, end);
                    } else if (extend_to_note_start) {
                        newFoci.insert(newFoci.begin() + insert, start);
                    } else if (extend_to_note_end) {
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
                        // printf("Focus %d: time: %f, octave: %f\n", i, foci[i].time, foci[i].octave);
                    }
                }
            }
        }
        

        // Existing notes as blue lines
        int focusIndex = 0;
        Note closest;
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
            // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed height (e.g., 0.6 octave)
            // float halfH = 1.0/24.0;
            ImVec2 a = worldToScreen(topLeft, { x0w, y});
            ImVec2 b = worldToScreen(topLeft, { x1w, y});
            // // ensure correct corners
            ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            dl->AddRectFilled(rmin, rmax, IM_COL32(90,160,255,200), 2.0f);

            float tri_size = 6.0f;
            ImVec2 p1(a.x + tri_size, a.y);                       // tip of triangle
            ImVec2 p2(a.x, a.y - tri_size / 2); // top base corner
            ImVec2 p3(a.x, a.y); // bottom base corner

            dl->AddTriangleFilled(p1, p2, p3, noteCol);
        }

        // Add an origin line for the closest note
        if (closest.origin != 0.0f) {
            ImVec2 a = worldToScreen(topLeft, { closest.beat_on, closest.octave});
            ImVec2 c = worldToScreen(topLeft, {closest.beat_on, closest.origin});
            dl->AddRectFilled(a, c, IM_COL32(90,255,90,200), 2.0f);
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
            ImVec2 a = worldToScreen(topLeft, { x0w, y });
            ImVec2 b = worldToScreen(topLeft, { x1w, y });
            // ensure correct corners
            ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            dl->AddRectFilled(rmin, rmax, IM_COL32(255,160,90,200), 2.0f);
            // dl->AddRect(rmin, rmax, IM_COL32(100,60,30,220), 6.0f, 0, 2.0f);

            // float tri_size = 6.0f;
            // ImVec2 p1(a.x + tri_size, a.y);                       // tip of triangle
            // ImVec2 p2(a.x, a.y - tri_size / 2); // top base corner
            // ImVec2 p3(a.x, a.y); // bottom base corner

            // dl->AddTriangleFilled(p1, p2, p3, focusCol);

            // dl->AddLine({a.x, a.y}, {b.x, a.y}, focusCol);

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
                        ImVec2 s0 = worldToScreen(topLeft, { wB.x, o});
                        dl->AddLine({rmin.x, s0.y}, {rmax.x, s0.y}, gridCol);
                    }
                    for (int i = 0; i < sizeof(perfectIntervals) / 4; i++) {
                        float currentOctave = o + std::log2(perfectIntervals[i]);
                        if (currentOctave > octStart && currentOctave < octEnd) {
                            ImVec2 s0 = worldToScreen(topLeft, { wB.x, currentOctave});
                            dl->AddLine({rmin.x, s0.y}, {rmax.x, s0.y}, gridCol);
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
            ImVec2 a = worldToScreen(topLeft, { x0w, y });
            ImVec2 b = worldToScreen(topLeft, { x1w, y });

            // dl->AddLine({a.x, a.y}, {b.x, a.y}, noteCol);
            ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            dl->AddRectFilled(rmin, rmax, IM_COL32(120,120,255,120), 2.0f);
            // dl->AddRect(rmin, rmax, IM_COL32(80,80,200,180), 6.0f, 0, 2.0f);
        }

        // Light-red preview dot (when Snap is on and hovered)
        if ((snap_to_grid || snap_harmonics) && hover_valid) {
            ImVec2 sd = worldToScreen(topLeft, hoverW);
            dl->AddCircleFilled(sd, 5.0f, IM_COL32(255,120,120,200));
            if (dragging) {
                Note n = currently_dragged;
                float x0w = n.beat_on, x1w = n.beat_off;
                float y   = hoverW.y;
                // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed height (e.g., 0.6 octave)
                float halfH = 1.0/24.0;
                ImVec2 a = worldToScreen(topLeft, { x0w, y + halfH });
                ImVec2 b = worldToScreen(topLeft, { x1w, y - halfH });
                // ensure correct corners
                ImVec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
                ImVec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
                dl->AddRectFilled(rmin, rmax, IM_COL32(90,160,255,200), 6.0f);
                dl->AddRect(rmin, rmax, IM_COL32(30,60,100,220), 6.0f, 0, 2.0f);
            }
        }

        // Input: left-click to place/finish
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (!placing) {
                // First click: start note (snap already applied above)
                if (hover_valid) {
                    if (dragging) {
                        currently_dragged.octave = hoverW.y;
                        notes.push_back(currently_dragged);
                        dragging = false;
                    } else {
                        placing = true;
                        anchorW = hoverW;
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
                        n.origin = 0.0f;
                        if (snap_harmonics) {
                            // Check whether there's any focus at the anchor
                            for (int i = 1; i < (int)foci.size(); i++) {
                                if (i == 0) 
                                    continue;

                                // printf("foci[i - 1].time: %f, anchorW.x: %f, foci[i].time: %f\n", foci[i - 1].time, anchorW.x, foci[i].time);
                                
                                if (foci[i - 1].time <= anchorW.x && foci[i].time >= anchorW.x) {
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
        ImVec2 mouse = ImGui::GetIO().MousePos;
        if (hovered) {
            ImVec2 w = screenToWorld(topLeft, mouse);
            ImGui::SetCursorScreenPos({topLeft.x + 8, bottomRight.y - 50});
            ImGui::BeginGroup();
            ImGui::Text("Mouse: beat %.3f, oct %.3f", w.x, w.y);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputFloat("dx", &snap_x, 0.0f, 0.0f, "%.2f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80); ImGui::InputFloat("dy", &snap_y, 0.0f, 0.0f, "%.2f");
            ImGui::EndGroup();
        } else {
            // Put the cursor back to layout after InvisibleButton
            ImGui::Dummy(ImVec2(0,0));
        }

        ImGui::Checkbox("Snap", &snap_to_grid);
        ImGui::Checkbox("Harmonic Snap", &snap_harmonics);

        ImGui::End();
    }
};

// Put this near your other statics:
static Canvas2D gBeatOctCanvas;


int main(int, char**)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }

    // OpenGL context via SDL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow(
        "Sonnote",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1); // vsync

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

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

    bool show_demo = false;
    bool running = true;
    bool playing = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                e.window.windowID == SDL_GetWindowID(window)) running = false;
        }

        // Start new ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // ImGui::Begin("Synth Controls");
        // float f = audioParams.freq.load();
        // float g = audioParams.gain.load();
        // if (ImGui::SliderFloat("Frequency (Hz)", &f, 20.f, 2000.f)) {
        //     audioParams.freq.store(f);
        // }
        // if (ImGui::SliderFloat("Gain", &g, 0.0f, 1.0f)) {
        //     audioParams.gain.store(g);
        // }
        // ImGui::Checkbox("Show ImGui Demo", &show_demo);
        // ImGui::Text("Audio running: %s", "yes");
        // ImGui::End();

        gBeatOctCanvas.draw("Note Canvas", 320.0f);

        static float bpm = 120.0f;

        ImGui::Begin("Transport");
        ImGui::SliderFloat("BPM", &bpm, 30.f, 240.f, "%.1f");
        // ImGui::SliderFloat("Attack", &gAttackLength, 0.0001f, 10.0f, "%.4f");
        // ImGui::SliderFloat("Decay", &gReleaseLength, 0.0001f, 10.0f, "%.4f");
        static char wavName[256] = "take1.wav";
        ImGui::InputText("WAV filename", wavName, IM_ARRAYSIZE(wavName));
        ImGui::InputText("Jump size", jump, IM_ARRAYSIZE(jump));
        ImGui::InputText("Split size", split, IM_ARRAYSIZE(split));

        // if (ImGui::SliderFloat("Master Gain", &g, 0.0f, 1.0f)) {
        //     audio_set_gain(g);
        // }

        // Custom Play/Stop button with a triangle
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 sz = { 40, 28 };
        ImGui::InvisibleButton("##playbtn", sz, ImGuiButtonFlags_MouseButtonLeft);
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // Button background
        ImU32 bg = hovered ? IM_COL32(70,80,90,255) : IM_COL32(55,60,66,255);
        dl->AddRectFilled(p0, ImVec2(p0.x+sz.x, p0.y+sz.y), bg, 6.0f);

        // Draw play button
        if (!playing) {
            // triangle
            ImVec2 c = { p0.x + sz.x*0.45f, p0.y + sz.y*0.5f };
            ImVec2 a = { c.x - 8, c.y - 10 };
            ImVec2 b = { c.x - 8, c.y + 10 };
            ImVec2 d = { c.x + 10, c.y };
            dl->AddTriangleFilled(a, b, d, IM_COL32(240,240,240,255));
        } else {
            // stop square
            ImVec2 a = { p0.x + sz.x*0.35f, p0.y + sz.y*0.25f };
            ImVec2 b = { p0.x + sz.x*0.65f, p0.y + sz.y*0.75f };
            dl->AddRectFilled(a, b, IM_COL32(240,240,240,255), 2.0f);
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(playing ? "Stop" : "Play");

        if (ImGui::Button("Export WAV")) {
            // Build on/off events from your notes (you already have this)
            uint32_t sr = 48000; // or 48000
            auto ev = build_events_from_notes(gBeatOctCanvas.notes, bpm, sr, 0.0f);

            // Sort OFF before ON at same sample (important to avoid spikes)
            std::sort(ev.begin(), ev.end(), [](const SeqEvent& a, const SeqEvent& b){
                if (a.beat != b.beat) return a.beat < b.beat;
                return a.type < b.type; // OFF(0) first, ON(1) after
            });

            engine.setEvents(ev);

            engine.exportWav("output.wav", 4.0f);
        }

        if (ImGui::Button("Export MIDI")) {
            if (save_midi_file_mpe_format1("take1.mid", gBeatOctCanvas.notes, bpm))
                ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "Saved take1.mid");
            else
                ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Export failed");
        }

        if (ImGui::Button("Save Project")) {
            save_song("session.son", gBeatOctCanvas.notes, gBeatOctCanvas.foci);
        }

        if (ImGui::Button("Load Project")) {
            load_song("session.son", gBeatOctCanvas.notes, gBeatOctCanvas.foci);
        }


        // Handle click: (re)build sequence and toggle
        if (clicked || play_triggered_from_space) {
            play_triggered_from_space = false;
            if (!playing) {
                // Build events from current points
                uint32_t sr = 48000; // if you don’t have gDevice here, store it globally or pass it out
                

                auto ev = build_events_from_notes(gBeatOctCanvas.notes, bpm, sr, playhead_time);
                engine.setEvents(ev);
                engine.resetSampleCounter();
                engine.setPlayState(true);
            } else {
                engine.setPlayState(false);
            }
            playing = !playing;
        }
        ImGui::End();


        if (show_demo) ImGui::ShowDemoWindow(&show_demo);	

        // Render
        ImGui::Render();
        int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    engine.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

