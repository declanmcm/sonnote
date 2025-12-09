#include "audio_engine.hpp"

#include <jack/jack.h>
#include <jack/thread.h>
#include <sndfile.h>

#include <cmath>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/mman.h>
#include <sched.h>
#include <iostream>

struct AudioEngine::JackState {
        jack_client_t* client = nullptr;
        jack_port_t* outL = nullptr;
        jack_port_t* outR = nullptr;
        AudioEngine* engine = nullptr;
    };

// ======================================================
// =============== AUDIO ENGINE CONSTRUCTOR =============
// ======================================================

AudioEngine::AudioEngine() {
    // Lock memory to avoid page faults in audio thread
    mlockall(MCL_CURRENT | MCL_FUTURE);
}

AudioEngine::~AudioEngine() {
    stop();
    munlockall();
}

bool freeze = false;


// ======================================================
// ================== JACK CALLBACK ======================
// ======================================================

int jackProcess(jack_nframes_t nframes, void* arg)
{
    AudioEngine::JackState* js = static_cast<AudioEngine::JackState*>(arg);
    AudioEngine* engine = js->engine;

    float* outL = (float*)jack_port_get_buffer(js->outL, nframes);
    float* outR = (float*)jack_port_get_buffer(js->outR, nframes);

    engine->processAudio(outL, outR, nframes);

    return 0;
}


// ======================================================
// ======================= START =========================
// ======================================================

bool AudioEngine::start() {
    if (jack) return true;

    jack = new JackState;
    jack->engine = this;

    const char* clientName = "AudioEngine";
    jack_status_t status;

    jack->client = jack_client_open(clientName, JackNullOption, &status);
    if (!jack->client) {
        std::cerr << "Failed to open JACK client\n";
        delete jack;
        jack = nullptr;
        return false;
    }

    sampleRate = jack_get_sample_rate(jack->client);

    jack->outL = jack_port_register(
        jack->client, "out_L",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput, 0);

    jack->outR = jack_port_register(
        jack->client, "out_R",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput, 0);

    if (!jack->outL || !jack->outR) {
        std::cerr << "Failed to register JACK ports\n";
        stop();
        return false;
    }

    // Set JACK callback
    jack_set_process_callback(jack->client, jackProcess, jack);

    if (jack_activate(jack->client) != 0) {
        std::cerr << "Failed to activate JACK client\n";
        stop();
        return false;
    }

    const char** ports;

    // Connect L
    ports = jack_get_ports(jack->client, nullptr, nullptr,
                        JackPortIsInput | JackPortIsPhysical);
    if (ports && ports[0]) {
        jack_connect(jack->client, 
            jack_port_name(jack->outL),
            ports[0]);
    }

    // Connect R
    if (ports && ports[1]) {
        jack_connect(jack->client, 
            jack_port_name(jack->outR),
            ports[1]);
    }

    // Free the list
    if (ports) free(ports);

    return true;
}


// ======================================================
// ======================== STOP =========================
// ======================================================

void AudioEngine::stop() {
    if (!jack) return;

    if (jack->client) {
        jack_client_close(jack->client);
    }

    delete jack;
    jack = nullptr;
}



// ======================================================
// ============= MESSAGE SENDING METHODS ================
// ======================================================

void AudioEngine::setFrequency(float hz) {
    Message m;
    m.type = MessageType::SetFrequency;
    m.fValue = hz;
    messageQueue.push(m);
}

void AudioEngine::setGain(float g) {
    Message m;
    m.type = MessageType::SetGain;
    m.fValue = g;
    messageQueue.push(m);
}

void AudioEngine::setPlayState(bool on) {
    Message m;
    m.type = MessageType::SetPlayState;
    m.bValue = on;
    messageQueue.push(m);
    currentBeat = 0.0f;
}

void AudioEngine::setWaveform(WaveformType w) {
    Message m;
    m.type = MessageType::SetWaveform;
    m.waveform = w;
    messageQueue.push(m);
}

void AudioEngine::setTempo(float bpm) {
    Message m;
    m.type = MessageType::SetTempo;
    m.fValue = bpm;
    messageQueue.push(m);
}

void AudioEngine::setADSR(const ADSRParams& a) {
    Message m;
    m.type = MessageType::SetADSR;
    m.adsr = a;
    messageQueue.push(m);
}

void AudioEngine::toggleFreeze() {
    freeze = !freeze;
}

void AudioEngine::setEvents(const std::vector<SeqEvent>& newEvents) {
    // Allocate vector on control thread to avoid RT alloc
    auto* v = new std::vector<SeqEvent>(newEvents);

    Message m;
    m.type = MessageType::ReplaceEvents;
    m.events = v;
    messageQueue.push(m);
}

void AudioEngine::resetSampleCounter() {
    Message m;
    m.type = MessageType::ResetSampleCounter;
    messageQueue.push(m);
}



// ======================================================
// =============== APPLY MESSAGES (RT-SAFE) ==============
// ======================================================

void AudioEngine::applyMessages() {
    Message msg;
    while (messageQueue.pop(msg)) {
        switch (msg.type) {

        case MessageType::SetGain:
            gain = msg.fValue;
            break;

        case MessageType::SetPlayState:
            isPlaying = msg.bValue;
            break;

        case MessageType::SetWaveform:
            waveform = msg.waveform;
            break;

        case MessageType::SetTempo:
            tempoBPM = msg.fValue;
            break;

        case MessageType::SetADSR:
            adsr = msg.adsr;
            break;

        case MessageType::Freeze:
            freeze = true;
            break;

        case MessageType::ResetSampleCounter:
            sampleCounter = 0;
            currentBeat = 0.0;
            nextEventIndex = 0;
            // Clear voices
            for (size_t i = 0; i < MAX_VOICES; ++i) {
                voiceStates[i] = 0;
                voiceEnvs[i] = 0.0f;
                voicePhases[i] = 0.0f;
                voiceFreqs[i] = 0.0f;
            }
            divisorSmooth = 1.0f;
            break;

        case MessageType::ReplaceEvents:
            stagingEvents = *msg.events;
            delete msg.events;
            hasPendingEventSwap = true;
            break;
        }
    }

    // Swap event list outside message loop
    if (hasPendingEventSwap) {
        activeEvents.swap(stagingEvents);
        hasPendingEventSwap = false;

        // Sort events by beat
        std::sort(activeEvents.begin(), activeEvents.end(),
                  [](const SeqEvent& a, const SeqEvent& b) {
                      return a.beat < b.beat;
                  });

        nextEventIndex = 0;
    }
}



// ======================================================
// ================= WAVEFORM GENERATION =================
// ======================================================

inline float generateSample(WaveformType wf, float phase) {
    switch (wf) {
    case WaveformType::Sine:
        return sinf(2.0f * M_PI * phase);

    case WaveformType::Square:
        return (phase < 0.5f ? 1.0f : -1.0f);

    case WaveformType::Saw:
        return 2.0f * (phase - 0.5f);

    case WaveformType::Triangle:
        return 2.0f * fabsf(2.0f * (phase - 0.5f)) - 1.0f;

    case WaveformType::Noise:
        return ((float)rand() / RAND_MAX) * 2.0f - 1.0f;
    }
    return 0.0f;
}



// ======================================================
// ======================= ADSR ==========================
// ======================================================

// float AudioEngine::updateADSR(float dt)
// {
//     ADSRParams& p = adsr;

//     if (noteOn) {
//         if (envelope < 1.0f) {
//             envelope += dt / p.attack;
//             if (envelope >= 1.0f) envelope = 1.0f;
//         }
//         else if (envelope > p.sustain) {
//             envelope -= dt / p.decay;
//             if (envelope <= p.sustain) envelope = p.sustain;
//         }
//     }
//     else {
//         envelope -= dt / p.release;
//         if (envelope < 0.0f) envelope = 0.0f;
//     }

//     return envelope;
// }




// ======================================================
// ==================== PROCESS AUDIO ====================
// ======================================================

void AudioEngine::processAudio(float* outL, float* outR, uint32_t frames)
{
    // Zero buffers
    memset(outL, 0, frames * sizeof(float));
    memset(outR, 0, frames * sizeof(float));

    // Apply queued parameter messages
    applyMessages();

    if (!isPlaying) {
        sampleCounter += frames;
        return;
    }

    const float sr = sampleRate;
    const float atkStep = (adsr.attack  > 0.f) ? (1.0f / (adsr.attack  * sr)) : 1.0f;
    const float relStep = (adsr.release > 0.f) ? (1.0f / (adsr.release * sr)) : 1.0f;
    const float decStep = (adsr.decay   > 0.f) ? (1.0f / (adsr.decay   * sr)) : 1.0f;

    const double secondsPerBeat = 60.0 / tempoBPM;
    const double samplesPerBeat = secondsPerBeat * sampleRate;
    for (uint32_t i = 0; i < frames; ++i)
    {
        float s = 0.0f;

        // --- Sequencer: map sampleCounter → beat and fire events when crossing beat positions ---
        currentBeat = double(sampleCounter) / samplesPerBeat;

        // Trigger all events whose beat <= current beat (looping)
        if (!activeEvents.empty() && !freeze) {

            // double lastBeat = currentBeat;
            // currentBeat = beat;  // update for envelope or external code, if needed

            size_t count = activeEvents.size();

            // If beat jumped backwards (tempo change) or we reset
            // we restart scanning from index 0
            if (currentBeat < lastBeat) {
                nextEventIndex = 0;
            }

            // Consume events while currentBeat passes their beat positions
            while (nextEventIndex < count &&
                activeEvents[nextEventIndex].beat <= currentBeat)
            {
                const SeqEvent& ev = activeEvents[nextEventIndex];

                // Only trigger if we *passed* this beat within this audio block
                if (ev.beat > lastBeat && ev.beat <= currentBeat) {
                    if (ev.type == 1) {
                        // NOTE ON
                        size_t voiceIndex = MAX_VOICES;
                        for (size_t v = 0; v < MAX_VOICES; ++v) {
                            if (voiceStates[v] == 0) {
                                voiceIndex = v;
                                break;
                            }
                        }
                        if (voiceIndex == MAX_VOICES)
                            voiceIndex = 0; // steal voice 0

                        voiceFreqs[voiceIndex]  = ev.value;
                        voicePhases[voiceIndex] = 0.0f;
                        voiceEnvs[voiceIndex]   = 0.0f;
                        voiceStates[voiceIndex] = 1;
                    }
                    else {
                        // NOTE OFF
                        for (size_t v = 0; v < MAX_VOICES; ++v) {
                            if (voiceStates[v] != 0 &&
                                std::abs(voiceFreqs[v] - ev.value) < 1e-4f &&
                                voiceStates[v] != 3)
                            {
                                voiceStates[v] = 3;
                                break;
                            }
                        }
                    }
                }

                nextEventIndex++;

                // Loop the sequence
                // if (nextEventIndex >= count) {
                //     nextEventIndex = 0;

                //     // If we wrapped but the next event is in the future,
                //     // we should stop scanning here.
                //     if (activeEvents[0].beat > currentBeat)
                //         break;
                // }
            }
        }

        // --- Per-voice synthesis (similar to old data_callback) ---
        int activeCount = 0;
        for (size_t v = 0; v < MAX_VOICES; ++v) {
            if (voiceStates[v] == 0) continue; // inactive

            activeCount++;

            // Envelope update
            if (voiceStates[v] == 1) {
                // Attack
                voiceEnvs[v] += atkStep;
                if (voiceEnvs[v] >= 1.0f) {
                    voiceEnvs[v] = 1.0f;
                    voiceStates[v] = 2; // sustain/decay
                }
            } else if (voiceStates[v] == 2) {
                // Decay toward sustain
                if (voiceEnvs[v] > adsr.sustain) {
                    voiceEnvs[v] -= decStep;
                    if (voiceEnvs[v] <= adsr.sustain) {
                        voiceEnvs[v] = adsr.sustain;
                    }
                }
            } else if (voiceStates[v] == 3) {
                // Release
                voiceEnvs[v] -= relStep;
                if (voiceEnvs[v] <= 0.0f) {
                    voiceEnvs[v] = 0.0f;
                    voiceStates[v] = 0; // off
                    continue;
                }
            }

            // Phase & sample
            const float freq = voiceFreqs[v];
            float phase = voicePhases[v];

            phase += freq / sr;
            if (phase >= 1.0f) phase -= 1.0f;
            voicePhases[v] = phase;

            float voiceSample = generateSample(waveform, phase) * voiceEnvs[v];
            s += voiceSample;
        }

        // Smooth divisor (like old "divisor" logic)
        if (activeCount > 0) {
            if (std::abs(divisorSmooth - (float)activeCount) > 1e-6f) {
                divisorSmooth += ((float)activeCount - divisorSmooth) / 100.0f;
            } else {
                divisorSmooth = (float)activeCount;
            }

            // s /= divisorSmooth;
        } else {
            divisorSmooth = 1.0f;
        }

        // Apply global gain
        s *= gain;

        // Clamp
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;

        outL[i] = s;
        outR[i] = s;

        if (!freeze)
            sampleCounter++;
    }
}


// ======================================================
// ====================== EXPORT WAV =====================
// ======================================================
//
// This uses libsndfile and DOES NOT TOUCH JACK or RT threads.
// Purely offline rendering.
//

bool AudioEngine::exportWav(const char* path, float durationSeconds)
{
    // ========== SETUP LIBSNDFILE ==========
    SF_INFO info{};
    info.samplerate = (int)sampleRate;
    info.channels   = 2;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    SNDFILE* snd = sf_open(path, SFM_WRITE, &info);
    if (!snd) {
        std::cerr << "Failed to open WAV file for writing\n";
        return false;
    }

    const uint64_t totalFrames = (uint64_t)(durationSeconds * sampleRate);

    std::vector<float> buffer(totalFrames * 2, 0.0f);

    // ========== LOCAL OFFLINE STATE ==========
    const float sr = sampleRate;

    // Local polyphonic arrays
    std::vector<float> freq(MAX_VOICES, 0.0f);
    std::vector<float> phase(MAX_VOICES, 0.0f);
    std::vector<float> env(MAX_VOICES, 0.0f);
    std::vector<uint8_t> state(MAX_VOICES, 0); // 0=off,1=attack,2=sustain/decay,3=release

    float divisorSmooth = 1.0f;

    // Event sequencing (beat-based)
    size_t evIndex = 0;  
    double secondsPerBeat  = 60.0 / tempoBPM;
    double samplesPerBeat  = secondsPerBeat * sampleRate;

    // Pre-sort events (offline safety, though normally sorted already)
    std::vector<SeqEvent> events = activeEvents;
    std::sort(events.begin(), events.end(),
              [](auto& a, auto& b){ return a.beat < b.beat; });

    // For wrap-around event playback
    bool hasEvents = !events.empty();
    if (!hasEvents) {
        // No events? Write silence.
        sf_writef_float(snd, buffer.data(), totalFrames);
        sf_close(snd);
        return true;
    }

    // ========== PER-SAMPLE OFFLINE RENDER LOOP ==========
    uint64_t sc = 0; // offline sample counter

    for (uint64_t i = 0; i < totalFrames; ++i)
    {
        float mix = 0.0f;

        // --- Sequencer / Event triggering ---
        double beat = double(sc) / samplesPerBeat;

        while (evIndex < events.size() && events[evIndex].beat <= beat) {
            const SeqEvent& ev = events[evIndex];

            if (ev.type == 1) {
                // NOTE ON: find free voice
                size_t v = MAX_VOICES;
                for (size_t k = 0; k < MAX_VOICES; ++k) {
                    if (state[k] == 0) { v = k; break; }
                }
                if (v == MAX_VOICES) v = 0; // steal voice 0

                freq[v]  = ev.value;
                phase[v] = 0.0f;
                env[v]   = 0.0f;
                state[v] = 1; // attack
            } else {
                // NOTE OFF: release matching voice
                for (size_t k = 0; k < MAX_VOICES; ++k) {
                    if (state[k] != 0 &&
                        fabsf(freq[k] - ev.value) < 1e-4f &&
                        state[k] != 3)
                    {
                        state[k] = 3; // release
                        break;
                    }
                }
            }

            // advance to next event
            evIndex++;
        }

        // --- Polyphonic synthesis ---
        int activeVoices = 0;

        float atkStep = (adsr.attack  > 0) ? (1.0f / (adsr.attack  * sr)) : 1.0f;
        float relStep = (adsr.release > 0) ? (1.0f / (adsr.release * sr)) : 1.0f;
        float decStep = (adsr.decay   > 0) ? (1.0f / (adsr.decay   * sr)) : 1.0f;

        for (size_t v = 0; v < MAX_VOICES; ++v)
        {
            if (state[v] == 0)
                continue;

            activeVoices++;

            // Envelope update
            if (state[v] == 1) {
                env[v] += atkStep;
                if (env[v] >= 1.0f) {
                    env[v] = 1.0f;
                    state[v] = 2; // decay/sustain
                }
            } 
            else if (state[v] == 2) {
                if (env[v] > adsr.sustain) {
                    env[v] -= decStep;
                    if (env[v] <= adsr.sustain)
                        env[v] = adsr.sustain;
                }
            }
            else if (state[v] == 3) {
                env[v] -= relStep;
                if (env[v] <= 0.0f) {
                    env[v] = 0;
                    state[v] = 0;
                    continue;
                }
            }

            // Phase + waveform
            phase[v] += freq[v] / sr;
            if (phase[v] >= 1.0f) phase[v] -= 1.0f;

            float voiceSample =
                generateSample(waveform, phase[v]) * env[v];

            mix += voiceSample;
        }

        // --- voice count smoothing (your divisor trick) ---
        if (activeVoices > 0) {
            if (fabs(divisorSmooth - (float)activeVoices) > 1e-6f) {
                divisorSmooth += ((float)activeVoices - divisorSmooth) / 100.0f;
            } else {
                divisorSmooth = (float)activeVoices;
            }
            // mix /= divisorSmooth;
        } else {
            divisorSmooth = 1.0f;
        }

        // global gain
        mix *= gain;

        // clamp
        if (mix > 1.0f) mix = 1.0f;
        if (mix < -1.0f) mix = -1.0f;

        buffer[i * 2 + 0] = mix;
        buffer[i * 2 + 1] = mix;

        sc++;
    }

    // ========== WRITE TO FILE ==========
    sf_writef_float(snd, buffer.data(), totalFrames);
    sf_close(snd);
    return true;
}

