// midi_export.hpp — export Sonnote beatEvents and tempo to a Standard MIDI File.
//
// Each note becomes its nearest 12-TET key plus a per-note pitch bend. Every
// sounding note gets its own channel so its bend can't affect other notes:
//   - MPE mode (default): lower zone, master = MIDI ch 1, members = ch 2..16 (15 voices)
//   - Non-MPE mode: channel rotation over all 16 channels (skipping ch 10 by default)
//
// Include this in main.cpp after parser.hpp. It relies on these globals/functions
// from main.cpp: `ref`, `b`, `t`, and `next()`.
#pragma once

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "audio_engine.hpp"

extern float  ref;
extern double b;
extern double t;
Event* next(Event* ev);

struct MidiExportOptions {
    // 27720 = lcm(1..12), so splits into 2..12 parts land exactly on ticks.
    // The SMF header allows up to 32767. Use 960 if a program complains.
    uint16_t ppq             = 27720;
    bool     mpe             = true;   // false = plain channel rotation
    float    bendRange       = 2.0f;  // semitones; must match the receiving synth
    bool     sendBendRangeRPN = true;  // write RPN 0 on every voice channel
    bool     skipDrumChannel = true;   // non-MPE only: avoid MIDI ch 10
    uint8_t  velocity        = 100;
    bool     includePerc     = false;  // export EV_PERC blips as short notes
    float    percLengthBeats = 0.25f;
};

namespace midi_export_detail {

struct ByteBuf {
    std::vector<uint8_t> v;
    void put(uint8_t c) { v.push_back(c); }
    void put(std::initializer_list<uint8_t> cs) { v.insert(v.end(), cs); }
};

inline void varLen(ByteBuf& o, uint32_t value) {
    uint8_t buf[5];
    int pos = 0;
    buf[pos++] = value & 0x7F;
    while ((value >>= 7)) buf[pos++] = 0x80 | (value & 0x7F);
    while (pos--) o.put(buf[pos]);
}

inline void be32(std::ostream& f, uint32_t x) {
    char c[4] = { char(x >> 24), char(x >> 16), char(x >> 8), char(x) };
    f.write(c, 4);
}
inline void be16(std::ostream& f, uint16_t x) {
    char c[2] = { char(x >> 8), char(x) };
    f.write(c, 2);
}

// Track builder that converts absolute ticks to delta times.
struct Track {
    ByteBuf  data;
    uint32_t lastTick = 0;
    void ev(uint32_t tick, std::initializer_list<uint8_t> bytes) {
        varLen(data, tick - lastTick);
        lastTick = tick;
        data.put(bytes);
    }
    void meta(uint32_t tick, uint8_t type, const std::vector<uint8_t>& payload) {
        varLen(data, tick - lastTick);
        lastTick = tick;
        data.put({0xFF, type});
        varLen(data, (uint32_t)payload.size());
        data.v.insert(data.v.end(), payload.begin(), payload.end());
    }
    void name(const std::string& s) { meta(0, 0x03, std::vector<uint8_t>(s.begin(), s.end())); }
    void end(uint32_t tick) { meta(std::max(tick, lastTick), 0x2F, {}); }
};

// RPN write: select, data entry MSB/LSB, then deselect (null RPN).
inline void rpn(Track& tr, uint32_t tick, uint8_t ch, uint8_t msb, uint8_t lsb,
                uint8_t dataMsb, uint8_t dataLsb) {
    uint8_t cc = 0xB0 | ch;
    tr.ev(tick, {cc, 101, msb});
    tr.ev(tick, {cc, 100, lsb});
    tr.ev(tick, {cc, 6,   dataMsb});
    tr.ev(tick, {cc, 38,  dataLsb});
    tr.ev(tick, {cc, 101, 127});
    tr.ev(tick, {cc, 100, 127});
}

struct Span {
    uint32_t startTick, endTick;
    uint8_t  key;
    uint16_t bend;
};

// freq -> nearest key + 14-bit bend. Returns false if outside MIDI's key range.
inline bool freqToKeyBend(double freq, float bendRange, uint8_t& key, uint16_t& bend) {
    if (!(freq > 0.0)) return false;
    double m = 69.0 + 12.0 * std::log2(freq / 440.0);
    long k = std::lround(m);
    if (k < 0 || k > 127) return false;
    double frac = m - (double)k;                       // semitones, within ±0.5
    long bv = 8192 + std::lround(frac / bendRange * 8192.0);
    key  = (uint8_t)k;
    bend = (uint16_t)std::clamp(bv, 0L, 16383L);
    return true;
}

} // namespace midi_export_detail

// Returns false if the file can't be written. Prints a warning for any notes
// skipped (out of MIDI range) or voices stolen (more than the channel count
// sounding at once).
inline bool export_midi(const std::string& path,
                        EventTree& beatEvents,
                        const TempoMap& tempoMap,
                        const MidiExportOptions& opt = {})
{
    using namespace midi_export_detail;
    // Division values above 0x7FFF would be read as SMPTE timing.
    const uint16_t division = std::clamp<uint16_t>(opt.ppq, 1, 0x7FFF);
    const double ppq = division;
    auto toTick = [&](double beat) -> uint32_t {
        double v = std::round(beat * ppq);
        return v <= 0.0 ? 0u : (uint32_t)v;
    };

    // ---- 1. Collect note spans from the event tree ----
    std::vector<Span> spans;
    std::map<uint32_t, Event*> open;  // id -> NOTE_START
    int skipped = 0;

    for (Event* e = beatEvents.get_first(); e; e = next(e)) {
        if (e->type == EV_NOTE_START) {
            open[e->id] = e;
        } else if (e->type == EV_NOTE_END) {
            auto it = open.find(e->id);
            if (it == open.end()) continue;
            Event* s = it->second;
            open.erase(it);

            Span sp;
            sp.startTick = toTick(s->beat);
            sp.endTick   = toTick(e->beat);
            if (sp.endTick <= sp.startTick) continue;
            double freq = (double)s->ratio.eval() * (double)ref;
            if (!freqToKeyBend(freq, opt.bendRange, sp.key, sp.bend)) { skipped++; continue; }
            spans.push_back(sp);
        } else if (opt.includePerc && e->type == EV_PERC) {
            // Perc blips store their height as an octave value (C4 = octave 4).
            double octave = e->function.evaluate(b, t, 0, 0);
            double c4 = 220.0 * std::pow(2.0, 3.0 / 12.0);
            double freq = c4 * std::pow(2.0, octave - 4.0);
            Span sp;
            sp.startTick = toTick(e->beat);
            sp.endTick   = toTick(e->beat + opt.percLengthBeats);
            if (!freqToKeyBend(freq, opt.bendRange, sp.key, sp.bend)) { skipped++; continue; }
            spans.push_back(sp);
        }
    }

    // ---- 2. Build a timeline of on/off actions (offs before ons at equal ticks) ----
    struct Action { uint32_t tick; bool on; int span; };
    std::vector<Action> actions;
    actions.reserve(spans.size() * 2);
    for (int i = 0; i < (int)spans.size(); i++) {
        actions.push_back({spans[i].startTick, true,  i});
        actions.push_back({spans[i].endTick,   false, i});
    }
    std::stable_sort(actions.begin(), actions.end(), [](const Action& a, const Action& c) {
        if (a.tick != c.tick) return a.tick < c.tick;
        return !a.on && c.on;
    });

    // ---- 3. Voice channels ----
    std::vector<uint8_t> voiceCh;
    if (opt.mpe) {
        for (uint8_t c = 1; c <= 15; c++) voiceCh.push_back(c);
    } else {
        for (uint8_t c = 0; c <= 15; c++)
            if (!(opt.skipDrumChannel && c == 9)) voiceCh.push_back(c);
    }

    Track notes;
    notes.name("Sonnote");

    // Setup at tick 0
    if (opt.mpe) {
        // MPE Configuration Message on the master channel: lower zone, 15 members.
        rpn(notes, 0, 0, 0, 6, (uint8_t)voiceCh.size(), 0);
    }
    if (opt.sendBendRangeRPN) {
        float r = std::clamp(opt.bendRange, 0.0f, 127.0f);
        uint8_t semis = (uint8_t)r;
        uint8_t cents = (uint8_t)std::lround((r - semis) * 100.0f);
        for (uint8_t c : voiceCh) rpn(notes, 0, c, 0, 0, semis, cents);
    }
    for (uint8_t c : voiceCh) notes.ev(0, {uint8_t(0xE0 | c), 0x00, 0x40}); // centre bends

    // Channel state: which span is sounding (-1 = free) and when it was last released.
    struct ChState { int span = -1; uint32_t freedAt = 0; uint32_t startedAt = 0; };
    std::vector<ChState> ch(voiceCh.size());
    std::vector<int> spanCh(spans.size(), -1);   // -1 = not on / stolen
    int stolen = 0;
    uint32_t lastTick = 0;

    for (const Action& a : actions) {
        lastTick = std::max(lastTick, a.tick);
        Span& sp = spans[a.span];
        if (!a.on) {
            int ci = spanCh[a.span];
            if (ci < 0) continue;                  // was stolen earlier
            notes.ev(a.tick, {uint8_t(0x80 | voiceCh[ci]), sp.key, 64});
            ch[ci].span = -1;
            ch[ci].freedAt = a.tick;
            spanCh[a.span] = -1;
            continue;
        }

        // Pick the free channel released longest ago, so release tails aren't re-bent.
        int best = -1;
        for (int i = 0; i < (int)ch.size(); i++)
            if (ch[i].span < 0 && (best < 0 || ch[i].freedAt < ch[best].freedAt)) best = i;

        if (best < 0) {
            // All channels busy: steal the one whose note started earliest.
            best = 0;
            for (int i = 1; i < (int)ch.size(); i++)
                if (ch[i].startedAt < ch[best].startedAt) best = i;
            int victim = ch[best].span;
            notes.ev(a.tick, {uint8_t(0x80 | voiceCh[best]), spans[victim].key, 64});
            spanCh[victim] = -1;
            stolen++;
        }

        uint8_t c = voiceCh[best];
        notes.ev(a.tick, {uint8_t(0xE0 | c), uint8_t(sp.bend & 0x7F), uint8_t(sp.bend >> 7)});
        notes.ev(a.tick, {uint8_t(0x90 | c), sp.key, opt.velocity});
        ch[best].span = a.span;
        ch[best].startedAt = a.tick;
        spanCh[a.span] = best;
    }
    notes.end(lastTick);

    // ---- 4. Tempo track ----
    // MIDI tempos are steps, so a ramp becomes one every eighth of a beat.
    Track tempo;
    tempo.name("Tempo");
    {
        auto put = [&](double beat, double bpm) {
            uint32_t usPerQ = (uint32_t)std::lround(60000000.0 / bpm);
            tempo.meta(toTick(beat), 0x51,
                       { uint8_t(usPerQ >> 16), uint8_t(usPerQ >> 8), uint8_t(usPerQ) });
        };
        const auto& nodes = tempoMap.nodes;
        for (size_t i = 0; i < nodes.size(); i++) {
            put(nodes[i].beat, nodes[i].bpm);
            if (i + 1 == nodes.size() || nodes[i].bpm == nodes[i + 1].bpm) continue;
            for (double b = nodes[i].beat + 0.125; b < nodes[i + 1].beat; b += 0.125)
                put(b, tempoMap.bpmAt(b));
        }
    }
    tempo.end(0);

    // ---- 5. Write the file (format 1: tempo track + note track) ----
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write("MThd", 4);
    be32(f, 6);
    be16(f, 1);          // format 1
    be16(f, 2);          // two tracks
    be16(f, division);
    for (Track* tr : {&tempo, &notes}) {
        f.write("MTrk", 4);
        be32(f, (uint32_t)tr->data.v.size());
        f.write(reinterpret_cast<const char*>(tr->data.v.data()), (std::streamsize)tr->data.v.size());
    }
    f.close();

    if (skipped) std::printf("export_midi: skipped %d note(s) outside MIDI key range\n", skipped);
    if (stolen)  std::printf("export_midi: %d voice(s) stolen (more than %d notes at once)\n",
                             stolen, (int)voiceCh.size());
    return true;
}
