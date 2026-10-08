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
    "#X text 20 10 Sonnote sends: voice freq velocity id. Velocity 0 is note off.;\n"  // 0
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
static const char* kStarterVoice =
    "#N canvas 140 140 420 300 12;\n"
    "#X obj 20 20 inlet;\n"
    "#X obj 20 50 unpack f f f;\n"
    "#X obj 20 140 osc~;\n"
    "#X obj 130 80 sel 0;\n"
    "#X msg 130 110 0 300;\n"
    "#X msg 200 110 \\$1 10;\n"
    "#X obj 130 140 vline~;\n"
    "#X obj 20 180 *~;\n"
    "#X obj 20 220 outlet~;\n"
    "#X connect 0 0 1 0;\n"
    "#X connect 1 0 2 0;\n"
    "#X connect 1 1 3 0;\n"
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

static void sendVis(const std::string& canvas, bool on) {
    if (canvas.empty() || !libpd_exists(canvas.c_str())) return;
    libpd_start_message(1);
    libpd_add_float(on ? 1.0f : 0.0f);
    libpd_finish_message(canvas.c_str(), "vis");
}

PdHost::~PdHost() {
    if (!ready) return;
    std::lock_guard<std::mutex> lock(mtx);
    bindThread();
    for (int i = 0; i < SLOTS; i++) {
        if (inst[i].handle && libpd_exists(inst[i].canvas.c_str()))
            libpd_closefile(inst[i].handle);
        inst[i] = Instance{};
    }
    if (gui) libpd_stop_gui();
}

// Pd's console output (errors from patches, "print" objects) goes to stderr.
static void pdPrint(const char* s) { std::fputs(s, stderr); }

void PdHost::bindThread() const {
    if (instance) libpd_set_instance(instance);
}

bool PdHost::init(int sampleRate) {
    for (int i = 0; i < SLOTS; i++) {
        slotInst[i] = -1; instDz[i] = 0;
        if (!gain[i].known) gain[i].set(1.0f);   // 0 dB until the track says otherwise
    }
    libpd_init();
    instance = libpd_this_instance();
    libpd_set_printhook(pdPrint);
    if (libpd_init_audio(0, 2, sampleRate) != 0) {
        std::fprintf(stderr, "[pd] libpd_init_audio failed\n");
        return false;
    }
    libpd_start_message(1);
    libpd_add_float(1.0f);
    libpd_finish_message("pd", "dsp");
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

int PdHost::openInstance(const std::string& file, bool visible) {
    // Tracks that share a file share one running copy of the patch.
    for (int i = 0; i < SLOTS; i++) {
        if (inst[i].file == file && inst[i].handle) { inst[i].refs++; return i; }
    }
    int i = 0;
    while (i < SLOTS && !inst[i].file.empty()) i++;
    if (i == SLOTS) return -1;

    fs::path p = file;
    std::string dir = p.parent_path().string(), base = p.filename().string();
    void* h = libpd_openfile(base.c_str(), dir.c_str());
    if (!h) return -1;

    inst[i].file = file;
    inst[i].canvas = "pd-" + base;
    inst[i].handle = h;
    inst[i].refs = 1;
    if (gui && !visible) sendVis(inst[i].canvas, false);
    instDz[i] = libpd_getdollarzero(h);
    return i;
}

void PdHost::releaseInstance(int i) {
    if (i < 0 || --inst[i].refs > 0) return;
    instDz[i] = 0;
    // If its editor window was closed, Pd has already freed the patch.
    if (inst[i].handle && libpd_exists(inst[i].canvas.c_str()))
        libpd_closefile(inst[i].handle);
    inst[i] = Instance{};
}

bool PdHost::setPatch(int slot, const std::string& name) {
    if (!ready || slot < 0 || slot >= SLOTS) return false;
    std::string file = resolve(name);

    std::lock_guard<std::mutex> lock(mtx);
    bindThread();
    int cur = slotInst[slot];
    if (cur >= 0 && file == slotFile[slot] && inst[cur].handle) return true;  // already loaded

    slotInst[slot] = -1;          // audio thread stops routing first
    releaseInstance(cur);
    slotFile[slot].clear();

    if (file.empty()) { status = "Track " + std::to_string(slot + 1) + ": no patch"; return true; }
    if (!fs::exists(file)) {
        status = "Not found: " + file;
        return false;
    }
    int i = openInstance(file, false);
    if (i < 0) { status = "Pd couldn't open " + file; return false; }

    slotFile[slot] = file;
    slotInst[slot] = i;
    resendAll = true;   // give the patch the current gain and song parameters
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

void PdHost::swapSlots(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= SLOTS || b >= SLOTS) return;
    std::lock_guard<std::mutex> lock(mtx);   // the audio thread never sees a half swap
    std::swap(slotFile[a], slotFile[b]);
    int ia = slotInst[a];
    slotInst[a] = slotInst[b].load();
    slotInst[b] = ia;
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
    bindThread();
    if (!gui) {
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
        gui = true;
    }
    sendVis(inst[slotInst[slot]].canvas, true);   // show / raise this patch
    status = "Editing " + fs::path(file).filename().string();
    return true;
}

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
    for (int s = 0; s < SLOTS; s++) {
        int i = slotInst[s];
        if (i >= 0) std::fprintf(stderr, " track %d -> %s ($0=%d)", s + 1,
                                 fs::path(inst[i].file).filename().c_str(), (int)instDz[i]);
    }
    std::fprintf(stderr, "\n");
}

void PdHost::poll() {
    if (!ready) return;
    if (debug) printDiag();
    // The editor itself is serviced inside libpd_process_float on the audio
    // thread; this only checks for patch windows that were closed.
    std::lock_guard<std::mutex> lock(mtx);
    bindThread();
    // Closing a patch window in the editor frees the patch. Reload it from
    // disk (hidden), so "edit, save, close" leaves the saved version playing.
    for (int i = 0; i < SLOTS; i++) {
        Instance& in = inst[i];
        if (!in.handle || libpd_exists(in.canvas.c_str())) continue;
        fs::path p = in.file;
        in.handle = libpd_openfile(p.filename().string().c_str(), p.parent_path().string().c_str());
        if (in.handle) {
            sendVis(in.canvas, false);
            instDz[i] = libpd_getdollarzero(in.handle);
            resendAll = true;
            status = "Reloaded " + p.filename().string();
        } else {
            instDz[i] = 0;
            status = "Couldn't reload " + p.filename().string();
        }
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

void PdHost::sendGain(int slot) {
    int i = slotInst[slot];
    if (i < 0 || !instDz[i]) return;
    char recv[32];
    std::snprintf(recv, sizeof recv, "%d-gain", (int)instDz[i]);
    libpd_float(recv, gain[slot].value);   // ignored if the patch has no [r $0-gain]
}

void PdHost::sendParams() {
    if (!locked) return;   // stays dirty, sent next block
    bool all = resendAll.exchange(false);
    auto due = [all](Param& p) { bool d = p.dirty.exchange(false); return p.known && (d || all); };

    for (int s = 0; s < SLOTS; s++)
        if (due(gain[s])) sendGain(s);
    if (due(tempo)) {
        libpd_float("sonnote-tempo", tempo.value);
        libpd_float("sonnote-beatms", 60000.0f / tempo.value);
    }
    if (due(playing)) libpd_float("sonnote-playing", playing.value);
    if (due(refFreq)) libpd_float("sonnote-ref", refFreq.value);
}

// ======================================================
// =================== AUDIO THREAD =====================
// ======================================================

bool PdHost::beginBlock() {
    locked = ready && mtx.try_lock();
    if (ready && !locked) diag.lockMisses++;
    if (locked) {
        bindThread();   // JACK's thread starts with no libpd instance set
        for (int q = 0; q < queued; q++) send(queue[q]);
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
    if (!ready || slot < 0 || slot >= SLOTS) return false;
    int i = slotInst[slot];
    return i >= 0 && instDz[i] != 0;
}

void PdHost::send(const NoteMsg& m) {
    if (!locked) {
        if (queued < MAX_QUEUE) queue[queued++] = m;
        return;
    }
    char recv[32];
    if (m.voice < 0) {
        std::snprintf(recv, sizeof recv, "%d-hit", m.dz);
        if (libpd_float(recv, m.vel) != 0) diag.sendFails++;   // no [r $0-hit] in the patch
        else diag.hits++;
        return;
    }
    std::snprintf(recv, sizeof recv, "%d-note", m.dz);
    libpd_start_message(4);
    libpd_add_float((float)m.voice);
    libpd_add_float(m.freq);
    libpd_add_float(m.vel);
    libpd_add_float((float)m.id);
    if (libpd_finish_list(recv) != 0) diag.sendFails++;   // no [r $0-note] in the patch
    if (m.vel > 0.0f) diag.notesOn++; else diag.notesOff++;
}

// A reloaded or replaced patch starts with no voices playing.
void PdHost::syncVoices(int i) {
    int dz = instDz[i];
    if (dz != seenDz[i]) {
        seenDz[i] = dz;
        voices[i] = {};
        nextVoice[i] = 0;
    }
}

void PdHost::noteOn(int slot, uint32_t id, float freq, float velocity) {
    if (!routes(slot)) return;
    int i = slotInst[slot];
    syncVoices(i);
    auto& vs = voices[i];

    // Round-robin over free voices, so a voice's release tail can finish
    // before it's reused. If all are busy, steal the next one.
    int v = -1;
    for (int k = 0; k < VOICES; k++) {
        int c = (nextVoice[i] + k) % VOICES;
        if (!vs[c].active) { v = c; break; }
    }
    if (v < 0) {
        v = nextVoice[i];
        send({seenDz[i], v, vs[v].freq, 0.0f, vs[v].id});
    }
    nextVoice[i] = (v + 1) % VOICES;
    vs[v] = {id, freq, true};
    send({seenDz[i], v, freq, velocity, id});
}

void PdHost::hit(int slot, float velocity) {
    if (!routes(slot)) return;
    send({(int)instDz[slotInst[slot]], -1, 0.0f, velocity, 0});
}

bool PdHost::noteOff(uint32_t id) {
    if (!ready) return false;
    for (int i = 0; i < SLOTS; i++) {
        syncVoices(i);
        for (int v = 0; v < VOICES; v++) {
            Voice& vo = voices[i][v];
            if (vo.active && vo.id == id) {
                vo.active = false;
                send({seenDz[i], v, vo.freq, 0.0f, id});
                return true;
            }
        }
    }
    return false;
}

void PdHost::allNotesOff() {
    if (!ready) return;
    for (int i = 0; i < SLOTS; i++) {
        syncVoices(i);
        for (int v = 0; v < VOICES; v++) {
            Voice& vo = voices[i][v];
            if (!vo.active) continue;
            vo.active = false;
            send({seenDz[i], v, vo.freq, 0.0f, vo.id});
        }
    }
}

void PdHost::addInput(int slot, float sample) {
    if (!locked || slot < 0 || slot >= SLOTS || outPos <= 0) return;
    int i = slotInst[slot];
    if (i < 0 || !instDz[i]) return;
    inBuf[i][outPos - 1] += sample;
    hasInput[i] = true;
}

void PdHost::renderFrame(float& l, float& r) {
    if (!locked) return;
    if (outPos >= outFrames) {
        // Audio-track input collected during the last tick goes in for this one
        // ([tabreceive~] replays the table every tick, so once the input
        // stops it's cleared once rather than left looping.)
        for (int i = 0; i < SLOTS; i++) {
            if ((!hasInput[i] && !tableLive[i]) || !instDz[i]) continue;
            char name[32];
            std::snprintf(name, sizeof name, "%d-in", (int)instDz[i]);
            libpd_write_array(name, 0, inBuf[i], 64);   // fails harmlessly without the table
            std::fill(std::begin(inBuf[i]), std::end(inBuf[i]), 0.0f);
            tableLive[i] = hasInput[i];
            hasInput[i] = false;
        }
        static const float noInput[1] = {0.0f};
        libpd_process_float(1, noInput, outBuf);
        outFrames = libpd_blocksize();
        outPos = 0;
        diag.ticks++;
    }
    float pl = outBuf[2 * outPos], pr = outBuf[2 * outPos + 1];
    blockPeak = std::max(blockPeak, std::max(std::fabs(pl), std::fabs(pr)));
    l += pl;
    r += pr;
    outPos++;
}
