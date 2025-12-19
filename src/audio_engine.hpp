#ifndef AUDIO_ENGINE_HPP
#define AUDIO_ENGINE_HPP

#include <vector>
#include <atomic>
#include <cstdint>
#include <functional>
#include <array>

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
    std::vector<SeqEvent>* events = nullptr; // pointer to new event list
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
    void setEvents(const std::vector<SeqEvent>& newEvents);

    // Reset playback position
    void resetSampleCounter();

    // Export WAV file using libsndfile
    bool exportWav(const char* path, float durationSeconds);

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
    std::vector<SeqEvent> activeEvents;      // used by audio thread
    std::vector<SeqEvent> stagingEvents;     // used to store incoming events

    bool hasPendingEventSwap = false;

    // ============================
    // Polyphonic voice state
    // ============================
    static constexpr size_t MAX_VOICES = 64;
    std::array<float, MAX_VOICES> voiceFreqs{};
    std::array<float, MAX_VOICES> voicePhases{};
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
