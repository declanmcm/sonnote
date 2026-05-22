#ifndef AUDIO_ENGINE_HPP
#define AUDIO_ENGINE_HPP

#include <vector>
#include <atomic>
#include <cstdint>
#include <functional>
#include <array>
#include "exprtk.hpp"

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

extern exprtk::symbol_table<double> table;
extern exprtk::parser<double> parser;
extern double b;
extern double t;

#define EV_TEMPO 2
#define EV_NOTE_START 1
#define EV_NOTE_END 0
#define EV_BEAT 3
#define EV_FOCUS 4

// Using AVL tree
struct Event {
    float beat;
    float time;
    std::string string_expr;
    exprtk::expression<double> function;
    int height;
    Event* left;
    Event* right;
    Event* next_ev;
    Event* parent;
    float origin;
    bool selected;
    bool finalEv;
    int velocity;
    int profile;
    int type;
    uint32_t id;

    Event(float b, int t) : beat(b), type(t) {
        left = NULL;
        right = NULL;
        parent = NULL;
        next_ev = NULL;
        height = 0;
        id = 0;
        profile = 0;
    }

    Event(float b, const std::string& expr_string, int t) : beat(b), height(1), type(t) {
        function.register_symbol_table(table);
        if (!parser.compile(expr_string, function)) {
            throw std::runtime_error("Expression parse error: " + parser.error());
        }
        string_expr = expr_string;
        left = NULL;
        right = NULL;
        parent = NULL;
        next_ev = NULL;
        height = 0;
        id = -1;
        profile = 0;
    }

    void update_function(const std::string& expr_string) {
        function.register_symbol_table(table);
        if (!parser.compile(expr_string, function)) {
            throw std::runtime_error("Expression parse error: " + parser.error());
        }
        string_expr = expr_string;
    }

    bool isConstant() {
        for (int i = 0; i < string_expr.length(); i++) {
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
Event* next_tempo(Event* ev);
Event* next_focus(Event* ev);
Event* next_note(Event* ev);
float integrate(const exprtk::expression<double>& function, float t1, float t2);
float integrate_until(const exprtk::expression<double>& function, float t1, float y);

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

    Event* get_first() {
      Event* current = root;
      while (current && current->left)
        current = current->left;
      return current;
    }

    Event* root = nullptr;
    bool ordered;

private:

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

        Event* newNode;
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

        // LL
        if (balance > 1 && value->beat < node->left->beat)
            return rotateRight(node, node->parent);

        // RR
        if (balance < -1 && value->beat > node->right->beat)
            return rotateLeft(node, node->parent);

        // LR
        if (balance > 1 && value->beat > node->left->beat) {
            node->left = rotateLeft(node->left, node);
            return rotateRight(node, node->parent);
        }

        // RL
        if (balance < -1 && value->beat < node->right->beat) {
            node->right = rotateRight(node->right, node);
            return rotateLeft(node, node->parent);
        }

        return node;
    }

    // ---------- Delete ----------
    Event* remove(Event* node, float key, int id) {
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
        printf("%f (%d) ", node->beat, level);
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
};

struct Profile {
    OSCSettings settings[3];
};

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
    void setGain(float g);
    void setPlayState(bool on);
    void setWaveform(WaveformType type);
    void setTempo(float bpm);
    void toggleFreeze();
    void setADSR(const ADSRParams& a);
    void setProfile(Profile& p, int n);
    Profile getProfile(int n);

    Profile profiles[10];

    // Replace the entire event list
    // (Audio thread uses double-buffer swap)
    void setEvents(EventTree* newEvents);

    // Reset playback position
    void resetSampleCounter();

    // Export WAV file using libsndfile
    bool exportWav(const char* path, float playhead_time, float durationSeconds);

private:
    // =================================
    // JACK internal handles (CPP only)
    // =================================
    JackState* jack = nullptr;
    float updateADSR(float dt);

    // =================================
    // Internal RT-safe data
    // =================================
    float sampleRate = 48000.0f;

    // Current DSP parameters (updated via messages)
    float gain = 0.5f;
    bool isPlaying = false;
    WaveformType waveform = WaveformType::Sine;
    ADSRParams adsr;
    float tempoBPM = 120.0f;

    // Musical timeline position
    double currentBeat = 0.0;    // increments based on BPM
    double lastBeat = -0.001;
    uint64_t sampleCounter = 0;  // absolute sample index
    size_t nextEventIndex = 0;

    // Sequencer event storage (double-buffer)
    EventTree activeEvents;      // used by audio thread
    EventTree stagingEvents;     // used to store incoming events

    bool hasPendingEventSwap = false;

    // ============================
    // Polyphonic voice state
    // ============================
    static constexpr size_t MAX_VOICES = 64;
    std::array<exprtk::expression<double>*, MAX_VOICES> voiceFreqs{};
    std::array<float, MAX_VOICES> voicePhases{};
    std::array<uint32_t, MAX_VOICES> voiceIDs{};
    // std::array<float, MAX_VOICES> voiceEnvs{};
    // std::array<uint8_t, MAX_VOICES> voiceStates{}; // 0=off,1=attack,2=sustain/decay,3=release
    float voiceEnvs[64][3];
    float voiceStates[64][3];
    std::array<uint8_t, MAX_VOICES> voiceProfiles{};

    float divisorSmooth = 1.0f;  // for dividing by voice count smoothly

    // =================================
    // Lock-free message queue
    // =================================
    MessageQueue<1024> messageQueue;

    // =================================
    // Real-time DSP entry point
    // =================================
    void processAudio(float* outL, float* outR, uint32_t frames);

    // Called in JACK RT thread to apply pending messages
    void applyMessages();
};

#endif // AUDIO_ENGINE_HPP
