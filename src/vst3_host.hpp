#pragma once
// VST3 plugins on tracks, hosted with the VST3 SDK (extern/vst3sdk):
// an instrument (notes in, stereo out) and effects (stereo in, stereo out).
//
// ---- Pitch ----
// Sonnote's notes can be any frequency. Each note goes to the plugin as the
// nearest MIDI note plus how far off that is, in one of two ways:
//   PerNote     the offset is in the note-on itself (NoteOnEvent::tuning,
//               in cents). Exact, but only plugins that read it play in tune;
//               the rest play the nearest 12-TET note.
//   Mpe48/Mpe2  every sounding note gets a MIDI channel of its own (2-16)
//               with a pitch bend for the offset, for synths in MPE mode with
//               a bend range of 48 or 2 semitones (turn MPE on in the synth,
//               e.g. in Vital's settings). Up to 15 notes at once. Needs the
//               plugin to map pitch bend to a parameter (IMidiMapping), as
//               JUCE-based plugins do; otherwise it falls back to PerNote.
//
// ---- Threads ----
// load(), state, editors, setOffline(), idle(): main thread.
// note*() and process(): audio thread. Edits made in a plugin's window reach
// its processor through a lock-free queue. Whoever owns a Plugin must keep
// the audio thread out of it while calling loadState() or setOffline(), and
// while destroying it (AudioEngine uses its processMutex).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vst3 {

enum class TuningMode : int { PerNote = 0, Mpe48 = 1, Mpe2 = 2 };
constexpr int TUNING_MODES = 3;
const char* tuningName(TuningMode m);

// Song position, given to plugins for tempo-synced LFOs, delays, ...
struct Transport {
    bool    playing = false;
    double  tempo = 120.0;     // BPM
    double  beat = 0.0;        // position in beats (quarter notes)
    int64_t samplePos = 0;     // position in samples
};

struct PluginInfo {
    std::string path;          // the .vst3 bundle
    std::string name;          // class name, e.g. "Vital"
    bool instrument = false;
};

// An automatable parameter of a plugin
struct ParamInfo {
    uint32_t id = 0;           // VST3 ParamID
    std::string name;
};

// A change made in a plugin's window: parameter and normalised value (0-1)
struct ParamEdit {
    uint32_t id = 0;
    double value = 0.0;
};

// Every VST3 plugin class in the standard folders (~/.vst3, /usr/lib/vst3,
// /usr/local/lib/vst3, ...). Scanned once, on first use.
const std::vector<PluginInfo>& plugins();

class Plugin {
public:
    // Loads a plugin by class name (case-insensitive, e.g. "vital"), by
    // bundle name, or by path to a .vst3. With `instrument`, an instrument
    // class is preferred; otherwise an effect. Ready to process on return.
    static std::unique_ptr<Plugin> load(const std::string& name, bool instrument,
                                        double sampleRate, int maxBlock, std::string& error);
    ~Plugin();
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    const std::string& name() const;    // class name
    const std::string& query() const;   // what it was loaded with
    uint64_t uid() const;               // unique for the whole run, unlike its address
    bool isInstrument() const;

    // ---- Main thread ----
    // The plugin's settings (component and controller state), for saving
    std::vector<uint8_t> saveState();
    bool loadState(const std::vector<uint8_t>& state);   // audio thread kept out
    void setOffline(bool offline);                       // audio thread kept out
    void setTuning(TuningMode m);

    bool openEditor();   // its own window; false if it has none
    void closeEditor();
    bool editorOpen() const;

    // ---- Parameters (main thread) ----
    std::vector<ParamInfo> parameters();          // the automatable ones, in the plugin's order
    std::string paramName(uint32_t id);           // "" if there's no such parameter
    double paramValue(uint32_t id);               // current normalised value
    std::string paramText(uint32_t id, double v); // v as the plugin displays it, e.g. "1200 Hz"
    // Sets a parameter as if it were moved in the plugin's window: its window
    // follows, the processor gets it, and takeEdits() reports it
    void editParam(uint32_t id, double value);
    // Changes made in the plugin's window since the last call (for recording)
    void takeEdits(std::vector<ParamEdit>& out);
    // Tells the plugin's window about values set by setParameter(), so it
    // follows automation. Call every pass of the main loop.
    void syncController();

    // ---- Audio thread ----
    // Sample offsets are within the next process() call.
    void noteOn(int offset, uint32_t id, float freq, float velocity);
    void noteOff(int offset, uint32_t id);
    void allNotesOff(int offset);
    void setParameter(int offset, uint32_t id, double value);   // normalised 0-1
    // Instruments ignore the input (in* may be null). n <= maxBlock.
    void process(const float* inL, const float* inR, float* outL, float* outR, int n,
                 const Transport& transport);

    struct Impl;
private:
    explicit Plugin(std::unique_ptr<Impl> d);
    std::unique_ptr<Impl> d;
};

// Main thread, every pass of the main loop: services plugin windows (their
// X11 events, and the timers and file descriptors plugins register).
void idle();
bool editorsOpen();
void closeAllEditors();

} // namespace vst3
