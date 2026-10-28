#ifndef AUDIO_ENGINE_HPP
#define AUDIO_ENGINE_HPP

#include <vector>
#include <atomic>
#include <cstdint>
#include <functional>
#include <array>
#include <mutex>
#include <string>
//#include "exprtk.hpp"
#include "parser.hpp"
#include "utils.hpp"
#include "pd_host.hpp"
#include "vst3_host.hpp"
#include <memory>
#include <algorithm>

// =============================================
// ========== PUBLIC AUDIO ENGINE API ==========
// =============================================
//
// Professional-grade audio engine using:
//   • JACK for real-time audio output
//   • libsndfile for WAV export
//   • Lock-free message queue for RT-safety
//   • Double-buffered sequencer event lists
//   • Musical-time sequencer (beats)
//   • Tempo changes affect playback speed
//   • Stereo float output
//
// =============================================


// ===============================
// Waveform types
// ===============================
enum class WaveformType {
    Sine,
    Square,
    Saw,
    Triangle,
    Noise
};

// ===============================
// Sequencer event stored in beats
// ===============================
struct SeqEvent {
    float beat = 0.0f;     // beat position
    int   type = 0;        // for future extension
    float value = 0.0f;    // frequency or other value
    int profile = 0;
};

//extern exprtk::symbol_table<double> table;
//extern exprtk::parser<double> parser;
extern double b;
extern double t;

extern const uint16_t audioBufferSize;
extern float audioIn[4096];
extern float audioOut[4096];
extern float pendingAudioOut[4096];
extern int audioOuti;
extern int audioi;
extern bool newAudioIn;
extern bool newAudioOut;

#define EV_TEMPO 2
#define EV_NOTE_START 1
#define EV_NOTE_END 0
#define EV_BEAT 3
#define EV_FOCUS 4
#define EV_PERC 5
// Audio clip extents. id = the clip's index in g_clips[profile], profile = track.
#define EV_AUDIO_START 6
#define EV_AUDIO_END 7

// Using AVL tree
struct Event {
    float beat;
    float time = 0.0f;
    std::string string_expr;
    ArithTree function;
    //exprtk::expression<double> functionOld;
    Ratio ratio;
    int height;
    Event* left;
    Event* right;
    Event* parent;
    float beatL = 0.0f;
    float beatR = 0.0f;
    uint32_t num = 0;
    uint32_t den = 0;
    std::vector<uint32_t> primeFactors;
    Event* origin;
    bool selected = false;
    bool finalEv = false;   // last tempo event; used by Canvas2D::accum_beats
    float gainDb = 0.0f;    // note gain in dB (g), sent to the note's Pd patch
    int profile;
    int type;
    uint32_t id;

    Event(float b, int t) : beat(b), function(), type(t) {
        left = NULL;
        right = NULL;
        parent = NULL;
        origin = NULL;
        height = 0;
        id = 0;
        den = 0;
        profile = 0;
        selected = false;
        ratio = {0,0};
    }

    Event(float b, int t, Ratio r) : beat(b), ratio(r), type(t) {
        left = NULL;
        right = NULL;
        parent = NULL;
        origin = NULL;
        height = 0;
        id = 0;
        den = 0;
        profile = 0;
        selected = false;
    }

    Event(float b, const std::string& expr_string, int t) : beat(b), function(const_cast<char*>(expr_string.c_str())), height(1), type(t) {
        //functionOld.register_symbol_table(table);
        //if (!parser.compile(expr_string, functionOld)) {
        //    throw std::runtime_error("Expression parse error: " + parser.error());
        //}
        //printf("functionOld value: %f\n", functionOld.value());
        //function = ArithTree(const_cast<char*>(expr_string.c_str()));
        printf("function value: %f\n", function.evaluate(0 ,0 ,0 ,0));
        string_expr = expr_string;
        left = NULL;
        right = NULL;
        parent = NULL;
        origin = NULL;
        height = 0;
        id = 0;
        den = 0;
        profile = 0;
        selected = false;
        ratio = {0,0};
    }

    void update_function(const std::string& expr_string) {
        //functionOld.register_symbol_table(table);
        //if (!parser.compile(expr_string, functionOld)) {
        //    throw std::runtime_error("Expression parse error: " + parser.error());
        //}
        function = ArithTree(const_cast<char*>(expr_string.c_str()));
        string_expr = expr_string;
    }

    bool isConstant() {
        for (int i = 0; i < (int)string_expr.length(); i++) {
            if (string_expr[i] == 't' || string_expr[i] == 'b') {
              printf("t or b found\n");
              std::fflush(stdout);
              return false;
            }
        }
        return true;
    }
};

void do_order(Event* current, Event*& previous);
Event* next_type(Event* ev, int type);
Event* next_note(Event* ev);
Event* next(Event* ev);
float integrate(ArithTree* function, float t1, float t2);
float integrate_until(ArithTree* function, float t1, float y);

class EventTree {
public:
    void insert(Event* value) {
        printf("enter insert\n");
        std::fflush(stdout);
        root = insert(root, value);
        root->parent = nullptr;
        ordered = false;
    }

    void remove(float key, int id) {
        root = remove(root, key, id);
        reparent_tree();
        ordered = false;
    }

    // Removes exactly this event. Unlike remove(), it doesn't rely on
    // (beat, id) being unique (clip events reuse ids across tracks).
    void remove_event(Event* ev) {
        root = removeEv(root, ev);
        reparent_tree();
        ordered = false;
    }

    void remove_all() {
      if (root) {
        root = remove(root, root->beat, root->id);
        remove_all();
      } else
        return;
    }

    void inorder() const {
        inorder(root, 0);
        std::cout << "\n";
    }

    void find_loop() {
      find_loop(root);
    }

    void create_order() {
        if (!ordered) {
            Event* previous = nullptr;
            do_order(root, previous);
            ordered = true;
        }
    }

    void reparent_tree() {
      if (root) {
        reparent(root);
        root->parent = nullptr;
      }
    }

    // Marks the last event (in beat order) as final and clears the flag on
    // all others. Call after any change to the tempo events.
    void setFinalEv() {
      clearFinal(root);
      Event* current = root;
      while (current && current->right)
        current = current->right;
      if (current)
        current->finalEv = true;
    }

    Event* get_first() {
      Event* current = root;
      while (current && current->left)
        current = current->left;
      return current;
    }

    Event* root = nullptr;
    bool ordered;

private:

    static void clearFinal(Event* node) {
      if (!node) return;
      node->finalEv = false;
      clearFinal(node->left);
      clearFinal(node->right);
    }

    void find_loop(Event* node) {
      printf("beat, id: %f, %d\n", node->beat, node->id);
      std::fflush(stdout);
      if (node->left) {
        printf("left\n");
        std::fflush(stdout);
        find_loop(node->left);
      }
      if (node->right) {
        printf("right\n");
        std::fflush(stdout);
        find_loop(node->right);
      }
      printf("up\n");
    }


    // ---------- Utilities ----------
    int height(Event*& n) {
        return n ? n->height : 0;
    }

    void reparent(Event* ev) {
      if (ev) {
        if (ev->left) {
          ev->left->parent = ev;
          reparent(ev->left);
        }
        if (ev->right) {
          ev->right->parent = ev;
          reparent(ev->right);
        }
      }
    }

    int balanceFactor(Event*& n) {
        return n ? height(n->left) - height(n->right) : 0;
    }

    void updateHeight(Event* n) {
        n->height = 1 + std::max(height(n->left), height(n->right));
    }

    // ---------- Rotations ----------
    Event* rotateRight(Event* y, Event* p) {
        auto x = y->left;
        auto T2 = x->right;

        x->right = y;
        x->right->left = T2;

        updateHeight(x->right);
        updateHeight(x);
        reparent(x);
        x->parent = p;

        return x;
    }

    Event* rotateLeft(Event* x, Event* p) {
        auto y = x->right;
        auto T2 = y->left;

        y->left = x;
        y->left->right = T2;

        updateHeight(y->left);
        updateHeight(y);
        reparent(y);
        y->parent = p;

        return y;
    }

    // ---------- Insert ----------
    Event* insert(Event* node, Event* value) {
        if (!node)
            return value;

        if (value->beat < node->beat) {
          if (node->left) {
            node->left = insert(node->left, value);
          } else {
            value->parent = node;
            node->left = value;
          }
        } else {
          if (node->right) {
            node->right = insert(node->right, value);
          } else {
            value->parent = node;
            node->right = value;
          }
        }

        updateHeight(node);

        int balance = balanceFactor(node);
        printf("balance: %d\n", balance);

        if (balance > 1) {
          if (balanceFactor(node->left) < 0)
            node->left = rotateLeft(node->left, node);
          return rotateRight(node, node->parent);
        } else if (balance < -1) {
          if (balanceFactor(node->right) > 0)
            node->right = rotateRight(node->right, node);
          return rotateLeft(node, node->parent);
        }

        return node;
    }

    // ---------- Delete ----------
    Event* remove(Event* node, float key, uint32_t id) {
        if (!node) return nullptr;

        if (key < node->beat)
            node->left = remove(node->left, key, id);
        else if (key > node->beat)
            node->right = remove(node->right, key, id);
        else if (std::abs(key - node->beat) < 1e-6) {
            if (node->id == id) {
                if (node->left && node->right) {
                    Event* left = node->left;
                    Event* right = node->right;
                    node = minValueNode(node->right);
                    node->right = remove(right, node->beat, node->id);
                    node->left = left;
                } else if (node->left) {
                    node = node->left;
                } else if (node->right) {
                    node = node->right;
                } else {
                    node = nullptr;
                }
            } else {
                if (node->left && node->right) {
                    node->left = remove(node->left, key, id);
                    node->right = remove(node->right, key, id);
                } else if (node->left)
                    node->left = remove(node->left, key, id);
                else if (node->right)
                    node->right = remove(node->right, key, id);
            }
        }

        if (node) {
            updateHeight(node);
            return rebalance(node);
        } else
            return node;
    }

    Event* removeEv(Event* node, Event* target) {
        if (!node) return nullptr;

        if (target->beat < node->beat)
            node->left = removeEv(node->left, target);
        else if (target->beat > node->beat)
            node->right = removeEv(node->right, target);
        else if (node == target) {
            if (node->left && node->right) {
                Event* left = node->left;
                Event* right = node->right;
                node = minValueNode(node->right);
                node->right = removeEv(right, node);
                node->left = left;
            } else if (node->left) {
                node = node->left;
            } else if (node->right) {
                node = node->right;
            } else {
                node = nullptr;
            }
        } else {
            // Same beat, different event: it may be on either side
            node->left = removeEv(node->left, target);
            node->right = removeEv(node->right, target);
        }

        if (node) {
            updateHeight(node);
            return rebalance(node);
        }
        return node;
    }

    Event* minValueNode(Event* node) {
        while (node->left)
            node = node->left;
        return node;
    }

    Event* rebalance(Event* node) {
        int balance = balanceFactor(node);

        if (balance > 1 && balanceFactor(node->left) >= 0)
            return rotateRight(node, node->parent);

        if (balance > 1 && balanceFactor(node->left) < 0) {
            node->left = rotateLeft(node->left, node);
            return rotateRight(node, node->parent);
        }

        if (balance < -1 && balanceFactor(node->right) <= 0)
            return rotateLeft(node, node->parent);

        if (balance < -1 && balanceFactor(node->right) > 0) {
            node->right = rotateRight(node->right, node);
            return rotateLeft(node, node->parent);
        }

        return node;
    }

    // ---------- Traversal ----------
    void inorder(const Event* node, uint8_t level) const {
        if (!node) return;
        inorder(node->left, level + 1);
        printf("%f, %d (%d) - %s", node->beat, node->id, level, node->string_expr.c_str());
        printf("\n");
        // std::cout << node->beat << "(" << level << ") ";
        inorder(node->right, level + 1);
    }
};



// ===============================
// ADSR Envelope Parameters
// ===============================
struct ADSRParams {
    float attack  = 0.01f;  // seconds
    float decay   = 0.1f;   // seconds
    float sustain = 0.8f;   // linear 0–1
    float release = 0.2f;   // seconds
};


// =========================================================
// =================== MESSAGE SYSTEM =======================
// =========================================================
//
// Lock-free queue of messages that the control thread sends
// to the audio thread. The audio thread executes them at
// block boundaries in full real-time safety.
// =========================================================

enum class MessageType {
    SetFrequency,
    SetGain,
    SetPlayState,
    ReplaceEvents,
    ResetSampleCounter,
    SetADSR,
    SetWaveform,
    JumpSize,
    SetTempo,
    Freeze,
};

struct Message {
    MessageType type;

    // Union-like storage
    float fValue = 0.0f;         // generic float (frequency, gain, tempo)
    bool  bValue = false;        // boolean (play state)
    ADSRParams adsr;             // envelope
    WaveformType waveform;       // waveform
    EventTree* events = nullptr; // pointer to new event list
};

struct OSCSettings  {
    WaveformType waveType;
    float attack;
    float decay;
    float sustain;
    float release;
    float gain;
    float freq;
    float hold;
};

// Built-in synth settings for one track. The track's Pd patch and gain live
// on the Track itself (see main.cpp).
struct Profile {
    OSCSettings settings[3];
};

// One Pd slot and one synth profile per track.
constexpr int MAX_TRACKS = PdHost::SLOTS;

// A recorded piece of audio on an audio track. Its position on the timeline
// is given by an EV_AUDIO_START / EV_AUDIO_END pair (id = index of the clip
// in g_clips[track]); startSample is the first sample that plays, so
// trimming the clip's start moves it forward.
struct Clip {
    std::string file;            // the .wav it was saved to / loaded from
    std::vector<float> samples;  // mono, at the engine's sample rate
    uint64_t startSample = 0;

    // Low-res waveform for drawing: the peak of each PEAK_BLOCK samples
    static constexpr int PEAK_BLOCK = 256;
    std::vector<float> peaks;
    void computePeaks() {
        peaks.assign((samples.size() + PEAK_BLOCK - 1) / PEAK_BLOCK, 0.0f);
        for (size_t i = 0; i < samples.size(); i++) {
            float a = samples[i] < 0 ? -samples[i] : samples[i];
            if (a > peaks[i / PEAK_BLOCK]) peaks[i / PEAK_BLOCK] = a;
        }
    }
};

// Clips per track: g_clips[track][clip]. Lists only grow (an id must keep
// pointing at the same clip), and always has MAX_TRACKS entries.
// Hold g_clipsMutex while changing the lists themselves (not needed for a
// clip's startSample); the audio thread only try-locks it, skipping clip
// playback for a block rather than waiting.
extern std::vector<std::vector<Clip>> g_clips;
extern std::mutex g_clipsMutex;

// ---- Automation ----
// A curve for one VST3 parameter of one of a track's plugins. Points are
// normalised values (0-1) at beats, joined by straight lines; before the
// first point and after the last the value stays level. A lane with no
// points leaves the parameter alone.
struct AutoPoint {
    float beat = 0.0f;
    float value = 0.0f;
};

// Which of the track's plugins: the instrument, or the k-th effect (1-based)
constexpr int AUTO_INSTRUMENT = 0;

struct AutoLane {
    int plugin = AUTO_INSTRUMENT;
    uint32_t param = 0;            // VST3 ParamID
    std::string name;              // for display, e.g. "Vital: Filter 1 Cutoff"
    std::vector<AutoPoint> points; // sorted by beat
    bool held = false;             // being recorded: playback leaves it alone

    // Audio thread: the value last sent, and in which pass (see autoEpoch)
    float lastSent = 0.0f;
    uint32_t sentEpoch = 0;

    float valueAt(double beat) const {
        if (points.empty()) return 0.0f;
        if (beat <= points.front().beat) return points.front().value;
        if (beat >= points.back().beat)  return points.back().value;
        auto it = std::upper_bound(points.begin(), points.end(), beat,
                                   [](double b, const AutoPoint& p) { return b < p.beat; });
        const AutoPoint& b = *it;
        const AutoPoint& a = *(it - 1);
        if (b.beat - a.beat < 1e-9f) return b.value;
        return a.value + (b.value - a.value) * (float)((beat - a.beat) / (b.beat - a.beat));
    }
};

// Lanes per track: g_automation[track][lane], always MAX_TRACKS entries.
// Hold g_automationMutex while changing them; the audio thread only
// try-locks it, skipping automation for a block rather than waiting.
extern std::vector<std::vector<AutoLane>> g_automation;
extern std::mutex g_automationMutex;

// =========================================================
// =============== LOCK-FREE MESSAGE QUEUE =================
// =========================================================
// Single-producer, single-consumer ring buffer.
// No dynamic allocation in audio thread.
// =========================================================

template<size_t Capacity>
class MessageQueue {
public:
    MessageQueue() : head(0), tail(0) {}

    bool push(const Message& msg) {
        size_t h = head.load(std::memory_order_relaxed);
        size_t next = (h + 1) % Capacity;

        if (next == tail.load(std::memory_order_acquire)) {
            // queue full
            return false;
        }

        buffer[h] = msg;
        head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Message& out) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) {
            return false; // empty
        }

        out = buffer[t];
        tail.store((t + 1) % Capacity, std::memory_order_release);
        return true;
    }

private:
    std::atomic<size_t> head;
    std::atomic<size_t> tail;
    Message buffer[Capacity];
};


// =========================================================
// ===================== AUDIO ENGINE ======================
// =========================================================

class AudioEngine {
public:
    struct JackState;

    friend int jackProcess(uint32_t, void*);

    AudioEngine();
    ~AudioEngine();

    bool start();          // start JACK
    void stop();           // stop JACK

    // ================================
    // Control-thread API (safe to call)
    // ================================
    void setFrequency(float hz);
    void setJump(float j);
    void setGain(float g);
    void setPlayState(bool on);
    void setWaveform(WaveformType type);
    void setTempo(float bpm);
    void toggleFreeze();
    void setADSR(const ADSRParams& a);
    void setProfile(const Profile& p, int n);
    void setTrackGain(int track, float db);   // clip playback gain (PdHost::MIN_DB = silent)
    void setTrackAudible(int track, bool on); // mute / solo: silences the track's whole output

    // ---- VST3 plugins (main thread) ----
    // Each track can have a VST3 instrument, which plays its notes (alongside
    // its Pd patch, if it has one), and a chain of VST3 effects on its audio.
    // Plugins are loaded off the audio thread's path and swapped in under
    // processMutex, so don't call these during exportWav().
    // setInstrument("") removes it. Returns false (and sets `status`) if a
    // plugin couldn't be loaded.
    bool setInstrument(int track, const std::string& name, std::string& status);
    // Effects in order. Ones already loaded under the same name are kept, with their settings.
    bool setEffects(int track, const std::vector<std::string>& names, std::string& status);
    void clearPlugins(int track);
    void swapTrackPlugins(int a, int b);
    void setTuning(int track, vst3::TuningMode m);
    vst3::Plugin* instrument(int track);
    std::vector<vst3::Plugin*> effects(int track);
    // AUTO_INSTRUMENT, or the k-th effect (1-based); nullptr if there's none
    vst3::Plugin* plugin(int track, int which);
    // Main thread, every pass: lets plugin windows follow automation
    void syncPluginControllers();
    bool loadPluginState(vst3::Plugin* p, const std::vector<uint8_t>& state);

    // ---- Recording ----
    // While on and playing, the input is captured from the moment playback
    // starts, through any loop wraps. The main thread collects it with
    // drainRecording(); after turning it off, wait for recordingActive() to go
    // false before taking the last samples. recordPassEnds() gives, for each
    // loop wrap, how many samples had been captured by then (each pass after
    // a wrap starts again at the loop start, which is where recording began).
    void setRecording(bool on) { recRequested = on; }
    bool recordingActive() const { return recActive; }
    size_t drainRecording(std::vector<float>& out);
    double recordStartBeat() const { return recStartBeat; }
    std::vector<uint64_t> recordPassEnds() const {
        std::vector<uint64_t> out;
        for (int i = 0; i < recPassCount; i++) out.push_back(recPassEnds[i]);
        return out;
    }
    double samplesPerBeat() const { return (60.0 / tempoBPM) * sampleRate; }
    // Where playback is, in beats (updated every block)
    double playheadBeat() const { return playBeat.load(std::memory_order_relaxed); }
    void toggleTuner();
    Profile getProfile(int n);

    Profile profiles[MAX_TRACKS];

    // Embedded Pure Data. Main thread: pd.setPatch / pd.openEditor / pd.poll.
    PdHost pd;
    float sampleRate = 48000.0f;

    bool tunerEnabled = false;

    // Click on every beat while playing (higher on every 4th). Left out of
    // exports. Safe to flip from any thread.
    std::atomic<bool> metronome{false};

    // Replace the entire event list
    // (Audio thread uses double-buffer swap)
    void setEvents(EventTree* newEvents);

    // Reset playback position
    void resetSampleCounter(float playhead_beat, float loop_end);

    // Renders the song offline, as fast as possible, through the same
    // processAudio() as playback (Pd patches, perc, clips), from startBeat to
    // endBeat plus tailSeconds for releases. JACK outputs silence meanwhile.
    // Call with playback stopped.
    bool exportWav(const char* path, EventTree* events, float startBeat, float endBeat,
                   float tailSeconds = 2.0f);

    // Held by whoever runs processAudio(): the JACK callback try-locks it
    // (outputting silence if it can't), exportWav() holds it throughout.
    std::mutex processMutex;

private:
    // =================================
    // JACK internal handles (CPP only)
    // =================================
    JackState* jack = nullptr;
    float updateADSR(float dt);

    // Current DSP parameters (updated via messages)
    float gain = 0.5f;
    float ref = 440.0f;
    float power = 4.0 / 3.0;
    bool isPlaying = false;
    WaveformType waveform = WaveformType::Sine;
    ADSRParams adsr;
    float tempoBPM = 120.0f;

    // Musical timeline position
    double currentBeat = 0.0;    // increments based on BPM
    double lastBeat = -0.001;
    uint64_t sampleCounter = 0;  // absolute sample index
    uint64_t loopEndSamples = 0;
    uint64_t loopStartSamples = 0;
    size_t nextEventIndex = 0;

    // Sequencer event storage (double-buffer)
    EventTree activeEvents;      // used by audio thread
    EventTree stagingEvents;     // used to store incoming events

    bool hasPendingEventSwap = false;

    // ============================
    // Polyphonic voice state
    // ============================
    static constexpr size_t MAX_VOICES = 64;
    std::array<uint32_t, MAX_VOICES> voiceIDs{};
    // std::array<float, MAX_VOICES> voiceEnvs{};
    // std::array<uint8_t, MAX_VOICES> voiceStates{}; // 0=off,1=attack,2=sustain/decay,3=release
    float voiceEnvs[64][3];
    float voiceStates[64][3];
    float voiceFreqs[64][3];
    float voicePhases[64][3];
    int voiceTimers[64][3];
    std::array<uint8_t, MAX_VOICES> voiceProfiles{};

    // ---- Clip playback (audio thread) ----
    std::array<std::atomic<float>, MAX_TRACKS> trackGain;   // linear
    std::array<std::atomic<bool>, MAX_TRACKS> trackAudible; // false = bus left out of the mix
    struct ClipVoice {
        bool     active = false;
        int      track = 0;
        uint32_t clip = 0;
        int64_t  startSc = 0;   // sampleCounter at which the clip's startSample plays
    };
    std::array<ClipVoice, 64> clipVoices{};
    void startClip(int track, uint32_t clip, double beat, double samplesPerBeat);
    void stopClip(int track, uint32_t clip);
    void stopAllClips();

    // ---- Recording: audio thread -> ring buffer -> main thread ----
    static constexpr size_t REC_RING = 1 << 21;   // ~43 s at 48 kHz between drains
    std::vector<float> recRing;
    std::atomic<size_t> recWrite{0}, recRead{0};
    std::atomic<bool>   recRequested{false}, recActive{false};
    std::atomic<double> recStartBeat{0.0};
    uint64_t recCaptured = 0;                          // audio thread: samples captured this take
    std::array<std::atomic<uint64_t>, 256> recPassEnds{};
    std::atomic<int> recPassCount{0};

    float divisorSmooth = 1.0f;  // for dividing by voice count smoothly

    // ---- Metronome (audio thread) ----
    bool    exporting = false;    // exportWav() is running: no clicks
    int64_t metroBeat = INT64_MIN; // beat the playhead was last in
    float   metroPhase = 0.0f, metroEnv = 0.0f, metroFreq = 0.0f;
    float   metronomeSample(uint64_t sc, double samplesPerBeat);

    // ---- Per-track buses (audio thread) ----
    // processAudio() works in pieces of at most MAX_BLOCK frames. Each track
    // renders into its own stereo bus, which are then mixed to the output.
    static constexpr uint32_t MAX_BLOCK = 1024;
    std::vector<float> busData;      // per track: L then R, MAX_BLOCK each
    std::vector<float> clipInData;   // per track: clip audio going into its Pd patch
    std::vector<float> metroBuf;
    float* busL(int t)   { return busData.data() + (size_t)t * 2 * MAX_BLOCK; }
    float* busR(int t)   { return busL(t) + MAX_BLOCK; }
    float* clipIn(int t) { return clipInData.data() + (size_t)t * MAX_BLOCK; }
    void processBlock(float* outL, float* outR, const float* in, uint32_t n,
                      double samplesPerBeat, bool clipsReadable);
    void renderSegment(uint32_t pos, uint32_t len, const float* in,
                       double samplesPerBeat, bool clipsReadable);
    void fireDueEvents(double samplesPerBeat, uint32_t blockFrames, uint32_t offset);

    // ---- VST3 plugins (audio thread reads them; changed under processMutex) ----
    struct TrackPlugins {
        std::unique_ptr<vst3::Plugin> instrument;
        std::vector<std::unique_ptr<vst3::Plugin>> effects;
        vst3::TuningMode tuning = vst3::TuningMode::PerNote;
    };
    std::array<TrackPlugins, MAX_TRACKS> plugins;
    std::vector<float> fxScratch;   // effect input, L then R
    void instrumentsAllNotesOff(int offset);

    // Perc hits played on a VST3 instrument end after PERC_GATE seconds
    static constexpr float PERC_GATE = 0.1f;
    struct PercOff { bool active = false; int track = 0; uint32_t id = 0; uint64_t due = 0; };
    std::array<PercOff, 64> percOffs{};
    void percNoteOn(int track, uint32_t id, float velocity, uint32_t offset);
    uint32_t firePercOffs(uint32_t pos, uint32_t len);   // returns len, cut at the next one due

    // Automation (audio thread). autoEpoch changes whenever playback jumps,
    // so every lane sends its value again.
    uint32_t autoEpoch = 1;
    void applyAutomation(uint32_t pos, uint32_t len, double samplesPerBeat);
    std::atomic<double> playBeat{0.0};
    void processPlugins(uint32_t n, uint64_t blockStart, double samplesPerBeat);

    // =================================
    // Lock-free message queue
    // =================================
    MessageQueue<1024> messageQueue;

    // =================================
    // Real-time DSP entry point
    // =================================
    void processAudio(float* outL, float* outR, const float* in, uint32_t frames);

    // Called in JACK RT thread to apply pending messages
    void applyMessages();
};

#endif // AUDIO_ENGINE_HPP
