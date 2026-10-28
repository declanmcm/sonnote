#include "pd_host.hpp"

#include <z_libpd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// The starter patch written when "Edit in Pd" is pressed for a file that
// doesn't exist yet: notes -> [clone sonnote-voice 16] -> gain -> [dac~].
static const char* kStarterPatch =
    "#N canvas 120 120 620 400 12;\n"
    "#X text 20 10 Sonnote sends: voice freq velocity id gain. Velocity 0 is note off.;\n"  // 0
    "#X obj 20 50 r \\$0-note;\n"                                                   // 1
    "#X obj 20 90 clone sonnote-voice 16;\n"                                         // 2
    "#X obj 20 130 *~ 0.15;\n"                                                       // 3
    "#X obj 20 250 dac~;\n"                                                          // 4
    "#X obj 200 50 r \\$0-gain;\n"                                                  // 5
    "#X msg 200 90 \\$1 20;\n"                                                       // 6
    "#X obj 200 130 line~;\n"                                                        // 7
    "#X obj 20 190 *~;\n"                                                            // 8
    "#X text 20 300 Track gain: r \\$0-gain (linear). Song: r sonnote-tempo \\, "
    "r sonnote-beatms \\, r sonnote-playing \\, r sonnote-ref;\n"                     // 9
    "#X connect 1 0 2 0;\n"
    "#X connect 2 0 3 0;\n"
    "#X connect 3 0 8 0;\n"
    "#X connect 5 0 6 0;\n"
    "#X connect 6 0 7 0;\n"
    "#X connect 7 0 8 1;\n"
    "#X connect 8 0 4 0;\n"
    "#X connect 8 0 4 1;\n";

// One voice of the starter patch: a sine with a short attack and release.
// The note's gain scales the velocity, so velocity 0 is still note off.
static const char* kStarterVoice =
    "#N canvas 140 140 420 300 12;\n"
    "#X obj 20 20 inlet;\n"           // 0
    "#X obj 20 50 unpack f f f f;\n"  // 1: freq velocity id gain
    "#X obj 20 140 osc~;\n"           // 2
    "#X obj 130 105 sel 0;\n"         // 3
    "#X msg 130 135 0 300;\n"         // 4
    "#X msg 200 135 \\$1 10;\n"       // 5
    "#X obj 130 165 vline~;\n"        // 6
    "#X obj 20 200 *~;\n"             // 7
    "#X obj 20 240 outlet~;\n"        // 8
    "#X obj 130 78 *;\n"              // 9: velocity * gain
    "#X connect 0 0 1 0;\n"
    "#X connect 1 0 2 0;\n"
    "#X connect 1 1 9 0;\n"
    "#X connect 1 3 9 1;\n"
    "#X connect 9 0 3 0;\n"
    "#X connect 3 0 4 0;\n"
    "#X connect 3 1 5 0;\n"
    "#X connect 4 0 6 0;\n"
    "#X connect 5 0 6 0;\n"
    "#X connect 2 0 7 0;\n"
    "#X connect 6 0 7 1;\n"
    "#X connect 7 0 8 0;\n";

// The starter perc patch: every hit restarts an amplitude envelope (squared,
// for a sharper decay) over a tone that drops in pitch plus high-passed noise.
// Placeholders: %1$s start Hz, %2$s end Hz, %3$s pitch drop ms, %4$s tone
// level, %5$s noise level, %6$s noise high-pass Hz, %7$s decay ms.
static const char* kPercPatch =
    "#N canvas 120 120 660 460 12;\n"
    "#X text 20 10 Sonnote perc: r \\$0-hit gets a velocity (0..1) for every blip.;\n"  // 0
    "#X obj 20 50 r \\$0-hit;\n"                                                       // 1
    "#X msg 20 90 \\$1 \\, 0 %7$s 1;\n"                                              // 2
    "#X obj 20 130 vline~;\n"                                                           // 3
    "#X msg 200 90 %1$s \\, %2$s %3$s;\n"                                              // 4
    "#X obj 200 130 vline~;\n"                                                          // 5
    "#X obj 200 170 osc~;\n"                                                            // 6
    "#X obj 200 210 *~ %4$s;\n"                                                         // 7
    "#X obj 360 130 noise~;\n"                                                          // 8
    "#X obj 360 170 hip~ %6$s;\n"                                                       // 9
    "#X obj 360 210 *~ %5$s;\n"                                                         // 10
    "#X obj 200 260 *~;\n"                                                              // 11
    "#X obj 20 170 *~;\n"                                                               // 12
    "#X obj 480 50 r \\$0-gain;\n"                                                     // 13
    "#X msg 480 90 \\$1 20;\n"                                                         // 14
    "#X obj 480 130 line~;\n"                                                           // 15
    "#X obj 200 300 *~;\n"                                                              // 16
    "#X obj 200 350 dac~;\n"                                                            // 17
    "#X connect 1 0 2 0;\n"
    "#X connect 1 0 4 0;\n"
    "#X connect 2 0 3 0;\n"
    "#X connect 3 0 12 0;\n"
    "#X connect 3 0 12 1;\n"
    "#X connect 4 0 5 0;\n"
    "#X connect 5 0 6 0;\n"
    "#X connect 6 0 7 0;\n"
    "#X connect 7 0 11 0;\n"
    "#X connect 8 0 9 0;\n"
    "#X connect 9 0 10 0;\n"
    "#X connect 10 0 11 0;\n"
    "#X connect 12 0 11 1;\n"
    "#X connect 11 0 16 0;\n"
    "#X connect 13 0 14 0;\n"
    "#X connect 14 0 15 0;\n"
    "#X connect 15 0 16 1;\n"
    "#X connect 16 0 17 0;\n"
    "#X connect 16 0 17 1;\n";

// The starter audio-track patch: the track's audio straight to the output.
// Effects go between [tabreceive~] and [dac~].
static const char* kAudioPatch =
    "#N canvas 120 120 560 320 12;\n"
    "#X text 20 10 Sonnote audio track: tabreceive~ \\$0-in plays the track's clips.;\n"  // 0
    "#X obj 20 50 table \\$0-in 64;\n"                                                   // 1
    "#X obj 20 100 tabreceive~ \\$0-in;\n"                                               // 2
    "#X obj 20 220 dac~;\n"                                                               // 3
    "#X text 20 260 Put effects between tabreceive~ and dac~.;\n"                         // 4
    "#X connect 2 0 3 0;\n"
    "#X connect 2 0 3 1;\n";

// Shows or hides a patch window. Its slot's instance must be bound.
static void sendVis(const std::string& canvas, bool on) {
    if (canvas.empty() || !libpd_exists(canvas.c_str())) return;
    libpd_start_message(1);
    libpd_add_float(on ? 1.0f : 0.0f);
    libpd_finish_message(canvas.c_str(), "vis");
}

PdHost::~PdHost() {
    if (!ready) return;
    std::lock_guard<std::mutex> lock(mtx);
    for (Slot& s : slots) {
        if (!s.pd) continue;
        unload(s);
        if (s.gui) libpd_stop_gui();
        libpd_free_instance(s.pd);
        s.pd = nullptr;
    }
}

// Pd's console output (errors from patches, "print" objects) goes to stderr.
static void pdPrint(const char* s) { std::fputs(s, stderr); }

void PdHost::bind(const Slot& s) {
    if (s.pd) libpd_set_instance(s.pd);
}

bool PdHost::init(int sr) {
    sampleRate = sr;
    for (int i = 0; i < SLOTS; i++) {
        live[i] = false;
        if (!gain[i].known) gain[i].set(1.0f);   // 0 dB until the track says otherwise
    }
    libpd_init();
    libpd_set_printhook(pdPrint);
    // Every slot gets an instance of its own; check that this libpd can
    // make them (the first one is kept for slot 0).
    std::lock_guard<std::mutex> lock(mtx);
    if (!ensureInstance(slots[0])) {
        status = "libpd has no multi-instance support: Pd patches won't play";
        std::fprintf(stderr, "[pd] %s\n", status.c_str());
        return false;
    }
    ready = true;
    return true;
}

// ======================================================
// =================== MAIN THREAD ======================
// ======================================================

std::string PdHost::resolve(const std::string& name) const {
    if (name.empty()) return "";
    fs::path p = name;
    if (p.extension() != ".pd") p += ".pd";
    if (name.find('/') == std::string::npos) p = fs::path(patchDir) / p;
    return fs::absolute(p).lexically_normal().string();
}

bool PdHost::ensureInstance(Slot& s) {
    if (s.pd) { bind(s); return true; }
    s.pd = libpd_new_instance();   // also makes it the current instance
    if (!s.pd) return false;
    libpd_set_printhook(pdPrint);
    if (libpd_init_audio(0, 2, sampleRate) != 0)
        std::fprintf(stderr, "[pd] libpd_init_audio failed\n");
    libpd_start_message(1);
    libpd_add_float(1.0f);
    libpd_finish_message("pd", "dsp");
    return true;
}

bool PdHost::loadFile(Slot& s, const std::string& file, bool visible) {
    if (!ensureInstance(s)) return false;
    fs::path p = file;
    std::string dir = p.parent_path().string(), base = p.filename().string();
    void* h = libpd_openfile(base.c_str(), dir.c_str());
    if (!h) return false;
    s.file = file;
    s.canvas = "pd-" + base;
    s.handle = h;
    s.dz = libpd_getdollarzero(h);
    if (s.gui && !visible) sendVis(s.canvas, false);
    // A new patch starts with no voices playing and gets every parameter
    s.voices = {};
    s.nextVoice = 0;
    s.outFrames = s.outPos = 0;
    std::fill(std::begin(s.in), std::end(s.in), 0.0f);
    s.hasInput = s.tableLive = false;
    s.resend = true;
    return true;
}

void PdHost::unload(Slot& s) {
    bind(s);
    // If its editor window was closed, Pd has already freed the patch.
    if (s.handle && libpd_exists(s.canvas.c_str()))
        libpd_closefile(s.handle);
    s.file.clear();
    s.canvas.clear();
    s.handle = nullptr;
    s.dz = 0;
    s.voices = {};
}

bool PdHost::setPatch(int slot, const std::string& name) {
    if (!ready || slot < 0 || slot >= SLOTS) return false;
    std::string file = resolve(name);

    std::lock_guard<std::mutex> lock(mtx);
    Slot& s = slots[slot];
    if (!file.empty() && file == s.file && s.dz) return true;  // already loaded

    live[slot] = false;
    unload(s);

    if (file.empty()) { status = "Track " + std::to_string(slot + 1) + ": no patch"; return true; }
    if (!fs::exists(file)) {
        status = "Not found: " + file;
        return false;
    }
    if (!loadFile(s, file, false)) {
        status = s.pd ? "Pd couldn't open " + file : "Couldn't make a Pd instance for track " + std::to_string(slot + 1);
        return false;
    }
    markLive(slot);
    status = "Track " + std::to_string(slot + 1) + ": " + fs::path(file).filename().string();
    return true;
}

bool PdHost::writeStarterPatch(const std::string& file) {
    fs::path p = file;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << kStarterPatch;
    fs::path voice = p.parent_path() / "sonnote-voice.pd";
    if (!fs::exists(voice)) std::ofstream(voice) << kStarterVoice;
    return fs::exists(p);
}

bool PdHost::writePercPatch(const std::string& file, int variant) {
    struct Sound { const char *from, *to, *drop, *tone, *noise, *hip, *decay; };
    static const Sound sounds[] = {
        {"160", "45",  "60", "0.7",  "0.05", "2000", "350"},   // kick
        {"260", "180", "30", "0.35", "0.5",  "1200", "180"},   // snare
        {"100", "100", "1",  "0",    "0.45", "7000", "60"},    // hat
        {"220", "110", "80", "0.6",  "0.05", "2000", "260"},   // tom
    };
    const Sound& s = sounds[std::abs(variant) % 4];
    char buf[4096];
    std::snprintf(buf, sizeof buf, kPercPatch, s.from, s.to, s.drop, s.tone, s.noise, s.hip, s.decay);

    fs::path p = file;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << buf;
    return fs::exists(p);
}

bool PdHost::writeAudioPatch(const std::string& file) {
    fs::path p = file;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << kAudioPatch;
    return fs::exists(p);
}

bool PdHost::ensureAudioPatch(const std::string& name) {
    std::string file = resolve(name);
    if (file.empty()) return false;
    return fs::exists(file) || writeAudioPatch(file);
}

bool PdHost::ensurePercPatch(const std::string& name, int variant) {
    std::string file = resolve(name);
    if (file.empty()) return false;
    return fs::exists(file) || writePercPatch(file, variant);
}

std::string PdHost::findGuiDir() {
    // The directory that contains tcl/pd-gui.tcl in a Pd vanilla install.
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("PD_GUI_DIR")) dirs.push_back(env);
    dirs.insert(dirs.end(), {
        "/usr/lib/puredata", "/usr/local/lib/pd", "/usr/lib/pd",
        "/Applications/Pd.app/Contents/Resources",
    });
    for (auto& d : dirs)
        if (fs::exists(fs::path(d) / "tcl" / "pd-gui.tcl")) return d;
    return "";
}

// Returns why the Pd editor can't start here, or "" if it should.
std::string PdHost::guiProblem() {
#if defined(__linux__)
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY"))
        return "No display for the Pd editor";
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
    // Pd's editor is a Tcl/Tk program launched with `wish`.
    bool found = false;
    if (const char* path = std::getenv("PATH")) {
        std::string ps = path;
        size_t start = 0;
        while (!found) {
            size_t end = ps.find(':', start);
            std::string d = ps.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!d.empty() && fs::exists(fs::path(d) / "wish")) found = true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (!found) return "Pd editor needs Tcl/Tk (wish): install tk";
#endif
    return "";
}

void PdHost::swapSlots(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= SLOTS || b >= SLOTS) return;
    std::lock_guard<std::mutex> lock(mtx);   // the audio thread never sees a half swap
    std::swap(slots[a], slots[b]);
    markLive(a);
    markLive(b);
}

bool PdHost::openEditor(int slot, const std::string& name) {
    if (!ready) return false;
    if (name.empty()) { status = "Type a patch name first"; return false; }

    std::string file = resolve(name);
    if (!fs::exists(file) && !writeStarterPatch(file)) {
        status = "Couldn't create " + file;
        return false;
    }
    if (!setPatch(slot, name)) return false;

    std::lock_guard<std::mutex> lock(mtx);
    Slot& s = slots[slot];
    bind(s);
    if (!s.gui) {
        std::string dir = findGuiDir();
        if (dir.empty()) {
            status = "Pd GUI not found: install Pd or set PD_GUI_DIR";
            return false;
        }
        // libpd_start_gui blocks until the editor connects, so make sure it
        // can actually start rather than hanging the app.
        std::string problem = guiProblem();
        if (!problem.empty()) { status = problem; return false; }
        if (libpd_start_gui(dir.c_str()) != 0) {
            status = "Couldn't start the Pd GUI from " + dir;
            return false;
        }
        s.gui = true;
    }
    sendVis(s.canvas, true);   // show / raise this patch
    status = "Editing " + fs::path(file).filename().string();
    return true;
}

// Needs mtx (it reads the slots).
void PdHost::printDiag() {
    auto now = std::chrono::steady_clock::now();
    if (now - lastDiag < std::chrono::seconds(1)) return;
    lastDiag = now;
    uint32_t ticks = diag.ticks.exchange(0), miss = diag.lockMisses.exchange(0);
    uint32_t on = diag.notesOn.exchange(0), off = diag.notesOff.exchange(0);
    uint32_t fails = diag.sendFails.exchange(0), unrouted = diag.unroutedNotes.exchange(0);
    uint32_t hits = diag.hits.exchange(0);
    float peak = diag.peak.exchange(0.0f);
    std::fprintf(stderr, "[pd] ticks=%u lockMisses=%u notesOn=%u notesOff=%u hits=%u sendFails=%u "
                         "unroutedNotes=%u peak=%.4f | playing=%d frozen=%d events=%u beat=%.3f nextEventBeat=%.3f |",
                 ticks, miss, on, off, hits, fails, unrouted, peak,
                 (int)diag.playing, (int)diag.frozen, diag.events.exchange(0),
                 (float)diag.beat, (float)diag.nextBeat);
    for (int i = 0; i < SLOTS; i++) {
        const Slot& s = slots[i];
        if (s.dz) std::fprintf(stderr, " track %d -> %s ($0=%d)", i + 1,
                               fs::path(s.file).filename().c_str(), s.dz);
    }
    std::fprintf(stderr, "\n");
}

void PdHost::poll() {
    if (!ready) return;
    // The editors are serviced inside libpd_process_float on the audio
    // thread; this only checks for patch windows that were closed.
    std::lock_guard<std::mutex> lock(mtx);
    if (debug) printDiag();
    // Closing a patch window in the editor frees the patch. Reload it from
    // disk (hidden), so "edit, save, close" leaves the saved version playing.
    for (int i = 0; i < SLOTS; i++) {
        Slot& s = slots[i];
        if (!s.handle) continue;
        bind(s);
        if (libpd_exists(s.canvas.c_str())) continue;
        std::string file = s.file;
        s.handle = nullptr;
        s.dz = 0;
        if (loadFile(s, file, false)) {
            status = "Reloaded " + fs::path(file).filename().string();
        } else {
            s.file = file;   // remembered, so the track still names it
            status = "Couldn't reload " + fs::path(file).filename().string();
        }
        markLive(i);
    }
}

// ======================================================
// ================ VALUES SENT TO PATCHES ===============
// ======================================================

float PdHost::dbToGain(float db) {
    return db <= MIN_DB ? 0.0f : std::pow(10.0f, db / 20.0f);
}

void PdHost::setGain(int slot, float db) {
    if (slot >= 0 && slot < SLOTS) gain[slot].set(dbToGain(db));
}
void PdHost::setTempo(float bpm)    { if (bpm > 0.0f) tempo.set(bpm); }
void PdHost::setPlaying(bool on)    { playing.set(on ? 1.0f : 0.0f); }
void PdHost::setRefFreq(float hz)   { refFreq.set(hz); }

void PdHost::sendGain(Slot& s, int slot) {
    char recv[32];
    std::snprintf(recv, sizeof recv, "%d-gain", s.dz);
    libpd_float(recv, gain[slot].value);   // ignored if the patch has no [r $0-gain]
}

void PdHost::sendParams() {
    if (!locked) return;   // stays dirty, sent next block
    // The song parameters are global names, but each instance has its own
    // namespace, so every patch is sent its own copy.
    bool tempoDue = tempo.dirty.exchange(false);
    bool playDue  = playing.dirty.exchange(false);
    bool refDue   = refFreq.dirty.exchange(false);
    for (int i = 0; i < SLOTS; i++) {
        Slot& s = slots[i];
        bool all = s.resend;
        bool gainDue = gain[i].dirty.exchange(false);
        s.resend = false;
        if (!s.dz) continue;
        bind(s);
        if (gain[i].known && (gainDue || all)) sendGain(s, i);
        if (tempo.known && (tempoDue || all)) {
            libpd_float("sonnote-tempo", tempo.value);
            libpd_float("sonnote-beatms", 60000.0f / tempo.value);
        }
        if (playing.known && (playDue || all)) libpd_float("sonnote-playing", playing.value);
        if (refFreq.known && (refDue || all))  libpd_float("sonnote-ref", refFreq.value);
    }
}

// ======================================================
// =================== AUDIO THREAD =====================
// ======================================================

bool PdHost::beginBlock() {
    locked = ready && mtx.try_lock();
    if (ready && !locked) diag.lockMisses++;
    if (locked) {
        for (int q = 0; q < queued; q++) execute(queue[q]);
        queued = 0;
    }
    return locked;
}

void PdHost::endBlock() {
    if (blockPeak > diag.peak) diag.peak = blockPeak;
    blockPeak = 0.0f;
    if (locked) mtx.unlock();
    locked = false;
}

bool PdHost::routes(int slot) const {
    return ready && slot >= 0 && slot < SLOTS && live[slot];
}

void PdHost::request(const NoteMsg& m) {
    if (locked) execute(m);
    else if (queued < MAX_QUEUE) queue[queued++] = m;
}

void PdHost::sendNote(Slot& s, int voice, float freq, float vel, uint32_t id, float noteGain) {
    char recv[32];
    std::snprintf(recv, sizeof recv, "%d-note", s.dz);
    libpd_start_message(5);
    libpd_add_float((float)voice);
    libpd_add_float(freq);
    libpd_add_float(vel);
    libpd_add_float((float)id);
    libpd_add_float(noteGain);
    if (libpd_finish_list(recv) != 0) diag.sendFails++;   // no [r $0-note] in the patch
    if (vel > 0.0f) diag.notesOn++; else diag.notesOff++;
}

void PdHost::execute(const NoteMsg& m) {
    switch (m.kind) {
    case Req::NoteOn: {
        if (m.slot < 0 || m.slot >= SLOTS || !slots[m.slot].dz) return;
        Slot& s = slots[m.slot];
        bind(s);
        auto& vs = s.voices;
        // Round-robin over free voices, so a voice's release tail can finish
        // before it's reused. If all are busy, steal the next one.
        int v = -1;
        for (int k = 0; k < VOICES; k++) {
            int c = (s.nextVoice + k) % VOICES;
            if (!vs[c].active) { v = c; break; }
        }
        if (v < 0) {
            v = s.nextVoice;
            sendNote(s, v, vs[v].freq, 0.0f, vs[v].id, vs[v].gain);
        }
        s.nextVoice = (v + 1) % VOICES;
        vs[v] = {m.id, m.freq, m.gain, true};
        sendNote(s, v, m.freq, m.vel, m.id, m.gain);
        return;
    }
    case Req::NoteOff:
        for (Slot& s : slots) {
            for (int v = 0; v < VOICES; v++) {
                Voice& vo = s.voices[v];
                if (!vo.active || vo.id != m.id) continue;
                vo.active = false;
                bind(s);
                sendNote(s, v, vo.freq, 0.0f, vo.id, vo.gain);
                return;
            }
        }
        return;
    case Req::Hit: {
        if (m.slot < 0 || m.slot >= SLOTS || !slots[m.slot].dz) return;
        Slot& s = slots[m.slot];
        bind(s);
        char recv[32];
        std::snprintf(recv, sizeof recv, "%d-hit", s.dz);
        if (libpd_float(recv, m.vel) != 0) diag.sendFails++;   // no [r $0-hit] in the patch
        else diag.hits++;
        return;
    }
    case Req::AllOff:
        for (Slot& s : slots) {
            if (!s.dz) continue;
            bind(s);
            for (int v = 0; v < VOICES; v++) {
                Voice& vo = s.voices[v];
                if (!vo.active) continue;
                vo.active = false;
                sendNote(s, v, vo.freq, 0.0f, vo.id, vo.gain);
            }
        }
        return;
    }
}

void PdHost::noteOn(int slot, uint32_t id, float freq, float velocity, float noteGain) {
    if (!routes(slot)) return;
    request({Req::NoteOn, slot, id, freq, velocity, noteGain});
}

void PdHost::noteOff(uint32_t id) {
    if (ready) request({Req::NoteOff, -1, id, 0.0f, 0.0f, 0.0f});
}

void PdHost::hit(int slot, float velocity) {
    if (!routes(slot)) return;
    request({Req::Hit, slot, 0, 0.0f, velocity, 1.0f});
}

void PdHost::allNotesOff() {
    if (ready) request({Req::AllOff, -1, 0, 0.0f, 0.0f, 0.0f});
}

void PdHost::render(int slot, const float* in, float* l, float* r, int n) {
    if (!locked || slot < 0 || slot >= SLOTS) return;
    Slot& s = slots[slot];
    if (!s.dz) return;
    bind(s);
    for (int k = 0; k < n; k++) {
        if (s.outPos >= s.outFrames) {
            // Input collected during the last tick goes in for this one
            // ([tabreceive~] replays the table every tick, so once the input
            // stops it's cleared once rather than left looping.)
            if (s.hasInput || s.tableLive) {
                char name[32];
                std::snprintf(name, sizeof name, "%d-in", s.dz);
                libpd_write_array(name, 0, s.in, 64);   // fails harmlessly without the table
                std::fill(std::begin(s.in), std::end(s.in), 0.0f);
                s.tableLive = s.hasInput;
                s.hasInput = false;
            }
            static const float noInput[1] = {0.0f};
            libpd_process_float(1, noInput, s.out);
            s.outFrames = libpd_blocksize();
            s.outPos = 0;
            diag.ticks++;
        }
        float pl = s.out[2 * s.outPos], pr = s.out[2 * s.outPos + 1];
        blockPeak = std::max(blockPeak, std::max(std::fabs(pl), std::fabs(pr)));
        l[k] += pl;
        r[k] += pr;
        if (in) {
            s.in[s.outPos] += in[k];
            s.hasInput = true;
        }
        s.outPos++;
    }
}
