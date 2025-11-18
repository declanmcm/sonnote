#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

struct AudioParams {
    std::atomic<float> freq{440.0f};   // used when not playing a sequence
    std::atomic<float> gain{0.2f};
    std::atomic<bool>  running{true};
};

struct SeqEvent {
    uint8_t type; // 1 for start, 0 for end
    uint64_t sample = 0;
    float    freq = 440.f;
};

bool audio_save_wav(const char* filepath,
                    const std::vector<SeqEvent>& events,
                    uint32_t sampleRate,
                    float masterGain,
                    float attackSec,
                    float releaseSec);

bool audio_start(AudioParams& params);
void audio_stop();

extern float gAttackLength; 
extern float gReleaseLength;

// Sequencer control
void audio_load_sequence(const std::vector<SeqEvent>& events);
float getSR(void);
void setAttack(float a);
void setDecay(float d);
void audio_play(bool on);
bool audio_is_playing();
void audio_set_gain(float g);
