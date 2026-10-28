#pragma once
// Embedded Pure Data (libpd) host.
//
// Each track (slot 0..SLOTS-1) can reference a Pd patch file. Notes on that
// track are sent to the patch, and the patch's [dac~] output becomes that
// track's audio (render()), which the engine mixes with the other tracks.
// Every slot runs its patch in a libpd instance of its own, so tracks never
// share an output, even when they use the same file. This needs a libpd
// built with multi-instance support (PDINSTANCE), as Arch's is.
//
// ---- What a patch receives ----
//   [r $0-note]  gets lists:   voice  freq  velocity  id  gain
//     voice     0..VOICES-1, allocated by Sonnote (feed straight into [clone])
//     freq      Hz
//     velocity  0..1; velocity 0 means note off for that voice
//     id        Sonnote's note id (stable for the whole note)
//     gain      the note's own gain (set with g in the note window), as a
//               linear amplitude: 0 dB = 1, -6 dB = 0.5
//   Send the list into [clone <voice-abstraction> 16] and each voice gets
//   "freq velocity id gain". The starter patch written by openEditor() shows
//   this. Older voices that unpack only "freq velocity id" ignore the gain.
//
//   [r $0-hit]   gets a velocity for every perc blip on the track: the blip's
//                gain as a linear amplitude (velocity 100 = 0 dB = 1, so a
//                full 127 is a little over 1).
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
//   Main thread : setPatch(), swapSlots(), openEditor(), poll()
//   Audio thread: beginBlock() / note*() / render() / endBlock()
// libpd is not thread-safe, so both sides share a mutex, which also guards
// every slot's state. The audio thread only try_locks it: if the main thread
// holds it (patch loading, editor activity), that block's Pd output is
// silent and note messages are queued until the next block, so no note-on
// or note-off is lost.
//
// The editor: each slot's instance has its own Pd GUI, started the first
// time "Edit in Pd" is used on that track.

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

    // Moves the patches (and their instances) of two slots, so tracks can be
    // reordered without reloading anything.
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

    bool routes(int slot) const;   // does this track have a running patch?
    void noteOn(int slot, uint32_t id, float freq, float velocity, float noteGain = 1.0f);
    void noteOff(uint32_t id);     // whichever slot is playing it
    void hit(int slot, float velocity);   // perc blip -> [r $0-hit]
    void allNotesOff();

    // Runs the slot's patch for n frames and adds its output to l and r.
    // `in` (may be null) is the patch's input for the same frames
    // ([tabreceive~ $0-in]); it reaches the patch one Pd tick late.
    // Does nothing for a slot without a patch, or while libpd isn't ours.
    void render(int slot, const float* in, float* l, float* r, int n);

private:
    struct Voice {
        uint32_t id = 0;
        float freq = 0.0f;
        float gain = 1.0f;
        bool active = false;
    };

    // One track's Pd: its own libpd instance and the patch running in it.
    // Guarded by mtx (see Threads above).
    struct Slot {
        struct _pdinstance* pd = nullptr;  // created the first time a patch is loaded
        std::string file;      // resolved path, "" = no patch
        std::string canvas;    // "pd-<filename>", Pd's name for the patch window
        void* handle = nullptr;
        int dz = 0;            // $0 of the running patch, 0 = none
        bool gui = false;      // this instance's editor has been started
        bool resend = false;   // patch (re)loaded: send it every parameter
        std::array<Voice, VOICES> voices{};
        int nextVoice = 0;
        float out[2 * 64] = {};       // one Pd tick of output, interleaved
        int outFrames = 0, outPos = 0;
        float in[64] = {};            // input collected for the next tick
        bool hasInput = false;        // `in` has something to send
        bool tableLive = false;       // the patch's $0-in table holds non-silence
    };

    // A note request. Carried out straight away while the audio thread holds
    // libpd, otherwise queued until the next block that does.
    enum class Req : uint8_t { NoteOn, NoteOff, Hit, AllOff };
    struct NoteMsg { Req kind; int slot; uint32_t id; float freq; float vel; float gain; };

    // A value waiting to be sent to Pd. `known` = it has been set at least once.
    struct Param {
        std::atomic<float> value{0.0f};
        std::atomic<bool>  dirty{false};
        std::atomic<bool>  known{false};
        void set(float v) { value = v; known = true; dirty = true; }
    };
    std::array<Param, SLOTS> gain;           // per slot, linear
    Param tempo, playing, refFreq;
    void sendGain(Slot& s, int slot);        // audio thread, locked, bound

    bool ready = false;
    int sampleRate = 48000;
    std::chrono::steady_clock::time_point lastDiag{};
    void printDiag();
    std::mutex mtx;

    std::array<Slot, SLOTS> slots;
    // slots[i] has a running patch. Readable without mtx (routes()).
    std::array<std::atomic<bool>, SLOTS> live{};

    // Audio-thread side
    bool locked = false;
    std::array<NoteMsg, MAX_QUEUE> queue{};
    int queued = 0;
    float blockPeak = 0.0f;

    // libpd keeps a current instance (per thread, in Arch's build), so every
    // call into libpd must select the right one first.
    static void bind(const Slot& s);
    bool ensureInstance(Slot& s);                              // needs mtx; binds it
    bool loadFile(Slot& s, const std::string& file, bool visible);  // needs mtx
    void unload(Slot& s);                                      // needs mtx
    void markLive(int slot) { live[slot] = slots[slot].dz != 0; }

    std::string resolve(const std::string& name) const;
    void request(const NoteMsg& m);                            // audio thread
    void execute(const NoteMsg& m);                            // audio thread, locked
    void sendNote(Slot& s, int voice, float freq, float vel, uint32_t id, float gain);
    static bool writeStarterPatch(const std::string& file);
    static bool writePercPatch(const std::string& file, int variant);
    static bool writeAudioPatch(const std::string& file);
    static std::string findGuiDir();
    static std::string guiProblem();
};
