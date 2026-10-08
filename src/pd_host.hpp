#pragma once
// Embedded Pure Data (libpd) host.
//
// Each track (slot 0..SLOTS-1) can reference a Pd patch file. Notes on that
// track are sent to the patch instead of the built-in synth, and every
// patch's [dac~] output is mixed into Sonnote's stereo output.
//
// ---- What a patch receives ----
//   [r $0-note]  gets lists:   voice  freq  velocity  id
//     voice     0..VOICES-1, allocated by Sonnote (feed straight into [clone])
//     freq      Hz
//     velocity  0..1; velocity 0 means note off for that voice
//     id        Sonnote's note id (stable for the whole note)
//   Send the list into [clone <voice-abstraction> 16] and each voice gets
//   "freq velocity id". The starter patch written by openEditor() shows this.
//
//   [r $0-hit]   gets a velocity (0..1) for every perc blip on the track.
//   The patch decides what the hit sounds like; writePercPatch() makes a
//   starter one (a tone with a pitch drop plus filtered noise).
//
//   [tabreceive~ $0-in]  audio tracks: the track's clips, after its gain /
//                        mute / solo. Needs [table $0-in 64] in the patch;
//                        arrives one Pd block (64 samples) late.
//                        writeAudioPatch() makes a starter (pass-through).
//
//   [r $0-gain]          the track's gain as a linear amplitude
//                        (0 dB = 1, -6 dB = 0.5, bottom of the slider = 0)
//
// ---- Song parameters (global, so use them without $0) ----
//   [r sonnote-tempo]    tempo in BPM
//   [r sonnote-beatms]   length of one beat in ms (handy for [delread~])
//   [r sonnote-playing]  1 when playback starts, 0 when it stops
//   [r sonnote-ref]      reference frequency in Hz (the 1/1)
// Each is sent whenever it changes, and again whenever a patch is loaded or
// reloaded, so a patch always starts with the current values.
//
// ---- Threads ----
//   Main thread : setPatch(), openEditor(), poll()
//   Audio thread: beginBlock() / note*() / renderFrame() / endBlock()
// libpd is not thread-safe, so both sides share a mutex. The audio thread
// only try_locks it: if the main thread holds it (patch loading, editor
// activity), that block's Pd output is silent and note messages are queued
// until the next block, so no note-on or note-off is lost.

#include <array>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class PdHost {
public:
    static constexpr int SLOTS     = 32;  // one per track
    static constexpr int VOICES    = 16;  // voices per patch, matches [clone ... 16]
    static constexpr int MAX_QUEUE = 256; // note messages held while locked out

    // Folder that patch names are resolved against (relative to the working
    // directory unless absolute). A name containing '/' is used as given.
    std::string patchDir = "patches";

    PdHost() = default;
    PdHost(const PdHost&) = delete;
    PdHost& operator=(const PdHost&) = delete;
    ~PdHost();

    // Call once, before audio starts, with the device sample rate.
    bool init(int sampleRate);

    // ---------------- Main thread ----------------

    // Points track `slot` at a patch file and loads it. An empty name
    // unloads it, so that track goes back to the built-in synth.
    // Returns false (and leaves the slot unloaded) if the file can't be opened.
    bool setPatch(int slot, const std::string& name);

    // Moves the patches of two slots, so tracks can be reordered without
    // reloading anything.
    void swapSlots(int a, int b);

    // Writes the starter perc patch for `name` if the file doesn't exist yet.
    // `variant` picks the starter sound (kick, snare, hat, tom, ...).
    bool ensurePercPatch(const std::string& name, int variant);
    // Same for the starter audio-track patch.
    bool ensureAudioPatch(const std::string& name);

    // Opens the slot's patch in Pd's own editor. If the file doesn't exist,
    // a starter patch is written first. Starts the Pd GUI if needed.
    bool openEditor(int slot, const std::string& name);

    // Call regularly from the main loop: reloads a patch from disk after its
    // editor window is closed. (The editor is serviced by the audio thread.)
    void poll();

    bool guiRunning() const { return gui; }

    // ---------------- Values sent to patches ----------------
    // Callable from any thread. Each keeps only the latest value, which is
    // delivered by sendParams() at the start of the next audio block.

    static constexpr float MIN_DB = -60.0f;   // at or below this, gain is 0
    static float dbToGain(float db);

    void setGain(int slot, float db);   // -> [r $0-gain] of the slot's patch
    void setTempo(float bpm);           // -> sonnote-tempo, sonnote-beatms
    void setPlaying(bool on);           // -> sonnote-playing
    void setRefFreq(float hz);          // -> sonnote-ref

    // Human-readable result of the last setPatch/openEditor, for the UI.
    std::string status;

    // Diagnostics, printed by poll() once a second. Set debug = false to silence.
    bool debug = false;
    struct Diag {
        std::atomic<uint32_t> ticks{0}, lockMisses{0};
        std::atomic<uint32_t> notesOn{0}, notesOff{0}, sendFails{0};
        std::atomic<uint32_t> hits{0};           // perc hits sent
        std::atomic<uint32_t> unroutedNotes{0};  // note-ons whose track has no Pd patch
        std::atomic<float> peak{0.0f};           // loudest Pd sample since last print
        // Transport state, filled in by AudioEngine::processAudio
        std::atomic<bool>  playing{false};
        std::atomic<bool>  frozen{false};
        std::atomic<uint32_t> events{0};         // sequencer events consumed (any type)
        std::atomic<float> beat{0.0f};           // current playback beat
        std::atomic<float> nextBeat{-1.0f};      // beat of the next event, -1 = none
    } diag;

    // ---------------- Audio thread ----------------

    bool beginBlock();   // returns true if libpd is ours for this block
    void endBlock();
    void sendParams();   // deliver changed gains / song parameters (call after beginBlock)

    bool routes(int slot) const;   // does this track play through Pd?
    void noteOn(int slot, uint32_t id, float freq, float velocity);
    bool noteOff(uint32_t id);     // true if the note was playing in Pd
    void hit(int slot, float velocity);   // perc blip -> [r $0-hit]
    // Adds to this frame's input of the slot's patch ([tabreceive~ $0-in]).
    // Call after renderFrame() for the same frame.
    void addInput(int slot, float sample);
    void allNotesOff();

    // Adds this frame of Pd output to l and r.
    void renderFrame(float& l, float& r);

private:
    struct Instance {
        std::string file;      // resolved path, "" if unused
        std::string canvas;    // "pd-<filename>", Pd's name for the patch window
        void* handle = nullptr;
        int refs = 0;          // how many slots use this file
        bool reloadHidden = false;
    };

    struct Voice {
        uint32_t id = 0;
        float freq = 0.0f;
        bool active = false;
    };

    struct NoteMsg { int dz; int voice; float freq; float vel; uint32_t id; };  // voice -1 = perc hit

    // A value waiting to be sent to Pd. `known` = it has been set at least once.
    struct Param {
        std::atomic<float> value{0.0f};
        std::atomic<bool>  dirty{false};
        std::atomic<bool>  known{false};
        void set(float v) { value = v; known = true; dirty = true; }
    };
    std::array<Param, SLOTS> gain;           // per slot, linear
    Param tempo, playing, refFreq;
    std::atomic<bool> resendAll{false};      // a patch was (re)loaded
    void sendGain(int slot);                 // audio thread, locked

    bool ready = false;
    std::chrono::steady_clock::time_point lastDiag{};
    void printDiag();
    // libpd's instance. Multi-instance builds (e.g. Arch's libpd) keep the
    // current instance per thread, so every thread that calls libpd must set
    // it first, or libpd crashes dereferencing a null instance.
    struct _pdinstance* instance = nullptr;
    void bindThread() const;
    bool gui = false;
    std::mutex mtx;

    // Main-thread side (guarded by mtx when libpd is touched)
    std::array<Instance, SLOTS> inst;
    std::array<std::string, SLOTS> slotFile;  // resolved path per slot

    // Shared: written by main thread, read by audio thread
    std::array<std::atomic<int>, SLOTS> slotInst{};  // instance index, -1 = none
    std::array<std::atomic<int>, SLOTS> instDz{};    // $0 of instance, 0 = closed

    // Audio-thread side
    bool locked = false;
    std::array<std::array<Voice, VOICES>, SLOTS> voices{};
    std::array<int, SLOTS> nextVoice{};
    std::array<int, SLOTS> seenDz{};
    std::array<NoteMsg, MAX_QUEUE> queue{};
    int queued = 0;
    float outBuf[2 * 64] = {};
    float inBuf[SLOTS][64] = {};           // per instance: input collected for the next tick
    std::array<bool, SLOTS> hasInput{};    // per instance: inBuf has something to send
    std::array<bool, SLOTS> tableLive{};   // per instance: its $0-in table holds non-silence
    int outFrames = 0;   // frames in outBuf (one Pd tick)
    int outPos = 0;
    float blockPeak = 0.0f;

    std::string resolve(const std::string& name) const;
    int  openInstance(const std::string& file, bool visible);  // needs mtx
    void releaseInstance(int i);                               // needs mtx
    void send(const NoteMsg& m);                               // audio thread
    void syncVoices(int instIndex);                            // audio thread
    static bool writeStarterPatch(const std::string& file);
    static bool writePercPatch(const std::string& file, int variant);
    static bool writeAudioPatch(const std::string& file);
    static std::string findGuiDir();
    static std::string guiProblem();
};
