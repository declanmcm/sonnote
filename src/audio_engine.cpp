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
#include "audio_engine.hpp"

struct AudioEngine::JackState {
        jack_client_t* client = nullptr;
        jack_port_t* outL = nullptr;
        jack_port_t* outR = nullptr;
        AudioEngine* engine = nullptr;
    };

Event* currentEv;

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

    OSCSettings startingProfile = {WaveformType::Sine, 0.2f, 0.2f, 0.7f, 0.025f, 0.16f};
    for (int i = 0; i < 10; i++) {
        profiles[i].settings[0] = startingProfile;
        profiles[i].settings[1] = startingProfile;
        profiles[i].settings[2] = startingProfile;
        profiles[i].settings[1].gain = 0.0f;
        profiles[i].settings[2].gain = 0.0f;
    }

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

void AudioEngine::setProfile(Profile& p, int n) {
    profiles[n] = p;
}

Profile AudioEngine::getProfile(int n) {
    return profiles[n];
}

void AudioEngine::toggleFreeze() {
    freeze = !freeze;
}

void AudioEngine::setEvents(EventTree* newEvents) { 
    b = 0.0f;
    Message m;
    m.type = MessageType::ReplaceEvents;
    m.events = newEvents;
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
                for (int j = 0; j < 3; j++) {
                    voiceStates[i][j] = 0;
                    voiceEnvs[i][j] = 0.0f;
                }
                voicePhases[i] = 0.0f;
                voiceFreqs[i] = nullptr;
                voiceProfiles[i] = 0;
            }
            divisorSmooth = 1.0f;
            break;

        case MessageType::ReplaceEvents:
            stagingEvents = *msg.events;
            hasPendingEventSwap = true;
            break;
        }
    }

    // Swap event list outside message loop
    if (hasPendingEventSwap) {
        activeEvents = stagingEvents;
        currentEv = activeEvents.root;
        while (currentEv->left) {
          currentEv = currentEv->left;
        }
        hasPendingEventSwap = false;

        nextEventIndex = 0;
    }
}

float toPrint[100];
int printIndex = 0;
int countdown = 52;

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
    // const float atkStep = (adsr.attack > 1e-6) ? (1.0f / (float)(adsr.attack  * sr)) : 1.0f;
    // const float relStep = (adsr.release > 1e-6) ? (1.0f / (float)(adsr.release * sr)) : 1.0f;
    // const float decStep = (adsr.decay   > 1e-6) ? (1.0f / (float)(adsr.decay   * sr)) : 1.0f;


    // printf("steps: (%f, %f)\n", atkStep, decStep);

    const double secondsPerBeat = 60.0 / tempoBPM;
    const double samplesPerBeat = secondsPerBeat * sampleRate;
    for (uint32_t i = 0; i < frames; ++i)
    {
        float s = 0.0f;

        // --- Sequencer: map sampleCounter → beat and fire events when crossing beat positions ---
        currentBeat = double(sampleCounter) / samplesPerBeat;
        b = currentBeat;

        // Trigger all events whose beat <= current beat (looping)
        if (!freeze) {

            // double lastBeat = currentBeat;
            // currentBeat = beat;  // update for envelope or external code, if needed

            //size_t count = activeEvents.size();

            // If beat jumped backwards (tempo change) or we reset
            // we restart scanning from index 0
            if (currentBeat < lastBeat) {
                nextEventIndex = 0;
            }

            if (countdown < 51) countdown--;

            // Consume events while currentBeat passes their beat positions
            while (currentEv &&
                currentEv->beat <= currentBeat)
            {
                countdown--; 

                // Only trigger if we *passed* this beat within this audio block
                if (currentEv->type == EV_NOTE_START) {
                    printf("note on: %d\n", currentEv->id);

                    // NOTE ON
                    size_t voiceIndex = MAX_VOICES;
                    for (size_t v = 0; v < MAX_VOICES; ++v) {
                        if (voiceStates[v][0] == 0 && voiceStates[v][1] == 0 && voiceStates[v][2] == 0) {
                            voiceIndex = v;
                            break;
                        }
                    }
                    if (voiceIndex == MAX_VOICES)
                        voiceIndex = 0; // steal voice 0

                    voiceFreqs[voiceIndex]  = &currentEv->function;
                    voicePhases[voiceIndex] = 0.0f;
                    voiceIDs[voiceIndex] = currentEv->id;
                    for (int i = 0; i < 3; i++) {
                        voiceEnvs[voiceIndex][i]   = 0.0f;
                        voiceStates[voiceIndex][i] = 1;
                    }
                    voiceProfiles[voiceIndex] = currentEv->profile;
                }
                else {
                    // NOTE OFF
                    printf("note off: %d\n", currentEv->id);
                    for (size_t v = 0; v < MAX_VOICES; ++v) {
                        if ((voiceStates[v][0] != 0 || voiceStates[v][1] != 0 || \ 
                              voiceStates[v][2] != 0) &&
                            currentEv->id == voiceIDs[v])
                        {
                          printf("heylo\n");
                            voiceStates[v][0] = 3;
                            voiceStates[v][1] = 3;
                            voiceStates[v][2] = 3;
                            break;
                        }
                    }
                }

                currentEv = next_note(currentEv);

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
            // printf("voiceStates: %d, %d, %d\n", voiceStates[v][0], voiceStates[v][1], voiceStates[2]);
            if (voiceStates[v][0] == 0 && voiceStates[v][1] == 0 && voiceStates[v][2] == 0) continue; // inactive

            activeCount++;

            for (int e = 0; e < 3; e++) {
                // Envelope update OSC i
                if (voiceStates[v][e] == 1) {
                    // Attack
                    float atkStep = (profiles[voiceProfiles[v]].settings[e].attack > 1e-6) ? (1.0f / (float)(profiles[voiceProfiles[v]].settings[e].attack  * sr)) : 1.0f;
                    voiceEnvs[v][e] += atkStep;
                    if (voiceEnvs[v][e] >= 1.0f) {
                        voiceEnvs[v][e] = 1.0f;
                        voiceStates[v][e] = 2; // sustain/decay
                    }
                } else if (voiceStates[v][e] == 2) {
                    // Decay toward sustain
                    float decStep = (profiles[voiceProfiles[v]].settings[e].decay   > 1e-6) ? (1.0f / (float)(profiles[voiceProfiles[v]].settings[e].decay  * sr)) : 1.0f;
                    if (voiceEnvs[v][e] > profiles[voiceProfiles[v]].settings[e].sustain) {
                        voiceEnvs[v][e] -= decStep;
                        if (voiceEnvs[v][e] <= profiles[voiceProfiles[v]].settings[e].sustain) {
                            voiceEnvs[v][e] = profiles[voiceProfiles[v]].settings[e].sustain;
                        }
                    }
                } else if (voiceStates[v][e] == 3) {
                    // Release
                    float relStep = (profiles[voiceProfiles[v]].settings[e].release > 1e-6) ? (1.0f / (float)(profiles[voiceProfiles[v]].settings[e].release * sr)) : 1.0f;
                    voiceEnvs[v][e] -= relStep;
                    if (voiceEnvs[v][e] <= 0.0f) {
                        voiceEnvs[v][e] = 0.0f;
                        voiceStates[v][e] = 0; // off
                    }
                }
            }

            // Phase & sample
            const float freq = 220.0f * std::pow(2.0f, 3.0/12.0) * std::pow(2, voiceFreqs[v]->evaluate(b, t, 0, 0) - 4.0f);
            printf("voice %d freq: %f\n", v, freq);
            float phase = voicePhases[v];

            phase += freq / sr;
            if (phase >= 1.0f) phase -= 1.0f;
            voicePhases[v] = phase;

            for (int i = 0; i < 3; i++) {
                float voiceSample = generateSample(profiles[voiceProfiles[v]].settings[i].waveType, phase) * voiceEnvs[v][i] * profiles[voiceProfiles[v]].settings[i].gain;

                s += voiceSample;
                // printf("s: %f gain: %f\n", s, profiles[voiceProfiles[v]].settings[i].gain);
                // fflush(stdout);
            }
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
        // s *= gain;

        // Clamp
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;

        outL[i] = s;
        // printf("s: %f\n", s);
        outR[i] = s;
        // toPrint[printIndex] = s;
        // printIndex++;
        // if (printIndex == 100) printIndex = 0;

        // if (countdown == 0) {
        //     printf("hey");
        //     // Print to file
        //     FILE* out = fopen("out.csv", "w+");
        //     char str[16];
        //     for (int i = 0; i < 99; i++) {
        //         printIndex++;
        //         if (printIndex == 100) printIndex = 0;
        //         sprintf(str, "%f,", toPrint[printIndex]);
        //         fputs(str, out);
        //     }
        //     sprintf(str, "%f", toPrint[++printIndex]);
        //     fputs(str, out);
        //     fclose(out);
        // }

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

bool AudioEngine::exportWav(const char* path, float playhead_time, float durationSeconds)
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
    printf("duration: %f, totalFrames: %d, sampleRate: %f\n", durationSeconds, totalFrames, sampleRate);

    std::vector<float> buffer(totalFrames * 2, 0.0f);

    // ========== LOCAL OFFLINE STATE ==========
    const float sr = sampleRate;

    // Local polyphonic arrays
    std::vector<float> freq(MAX_VOICES, 0.0f);
    std::vector<float> phase(MAX_VOICES, 0.0f);
    std::vector<uint8_t> voicePs(MAX_VOICES, 0);
    // std::vector<float> env(MAX_VOICES, 0.0f);
    float env[MAX_VOICES][3];
    uint8_t state[MAX_VOICES][3];
    memset(env,   0, sizeof(env));
    memset(state, 0, sizeof(state));
    // std::vector<uint8_t> state(MAX_VOICES, 0); // 0=off,1=attack,2=sustain/decay,3=release

    float divisorSmooth = 1.0f;

    // Event sequencing (beat-based)
    size_t evIndex = 0;  
    double secondsPerBeat  = 60.0 / tempoBPM;
    double samplesPerBeat  = secondsPerBeat * sampleRate;

    // Pre-sort events (offline safety, though normally sorted already)
    EventTree events = activeEvents;

    // For wrap-around event playback
    bool hasEvents = events.root ? true : false;
    if (activeEvents.root) {
        printf("empty\n");
    }
    if (!hasEvents) {
        // No events? Write silence.
        sf_writef_float(snd, buffer.data(), totalFrames);
        sf_close(snd);
        printf("hey!\n");
        return true;
    }

    // ========== PER-SAMPLE OFFLINE RENDER LOOP ==========
    uint64_t sc = playhead_time * samplesPerBeat; // offline sample counter
    int totalZero = 0;
    int totalNonZero = 0;

    Event* currentEv = events.root;
    while (currentEv->left)
      currentEv = currentEv->left;

    for (uint64_t i = 0; i < totalFrames; ++i)
    {
        float mix = 0.0f;

        // --- Sequencer / Event triggering ---
        double beat = double(sc) / samplesPerBeat;
        b = beat;
        while (currentEv && currentEv->beat <= beat) {
            //const SeqEvent& ev; // TODO Remove


            if (currentEv->type == EV_NOTE_START) {
                // NOTE ON: find free voice
                size_t v = MAX_VOICES;
                for (size_t k = 0; k < MAX_VOICES; ++k) {
                    if (state[k][0] == 0 && state[k][1] == 0 && state[k][2] == 0) {
                      v = k; break;
                    }
                }
                if (v == MAX_VOICES) v = 0; // steal voice 0

                freq[v]  = currentEv->function.evaluate(b, t, 0, 0);
                phase[v] = 0.0f;
                for (int i = 0; i < 3; i++) {
                    env[v][i] = 0.0f;
                    state[v][i] = 1;
                }
                voicePs[v] = currentEv->profile;
            } else {
                // NOTE OFF: release matching voice
                for (size_t k = 0; k < MAX_VOICES; ++k) {
                    if ((state[k][0] != 0 || state[k][1] != 0 || state[k][2] != 0) &&
                        fabsf(freq[k] - currentEv->function.evaluate(b, t, 0, 0)) < 1e-4f)
                    {
                        state[k][0] = 3;
                        state[k][1] = 3;
                        state[k][2] = 3;
                        break;
                    }
                }
            }

            // advance to next event
            currentEv = next_note(currentEv);
        }

        // --- Polyphonic synthesis ---
        int activeVoices = 0;

        // float atkStep = (adsr.attack  > 0) ? (1.0f / (adsr.attack  * sr)) : 1.0f;
        // float relStep = (adsr.release > 0) ? (1.0f / (adsr.release * sr)) : 1.0f;
        // float decStep = (adsr.decay   > 0) ? (1.0f / (adsr.decay   * sr)) : 1.0f;

        for (size_t v = 0; v < MAX_VOICES; ++v)
        {
            if (state[v][0] == 0 && state[v][1] == 0 && state[v][2] == 0)
                continue;

            activeVoices++;
            for (int i = 0; i < 3; i++) {
                // Envelope update
                if (state[v][i] == 1) {
                    float atkStep = (profiles[voicePs[v]].settings[i].attack > 1e-6) ? (1.0f / (float)(profiles[voicePs[v]].settings[i].attack  * sr)) : 1.0f;
                    env[v][i] += atkStep;
                    if (env[v][i] >= 1.0f) {
                        env[v][i] = 1.0f;
                        state[v][i] = 2; // decay/sustain
                    }
                } 
                else if (state[v][i] == 2) {
                    float decStep = (profiles[voicePs[v]].settings[i].decay   > 1e-6) ? (1.0f / (float)(profiles[voicePs[v]].settings[i].decay  * sr)) : 1.0f;
                    if (env[v][i] > profiles[voicePs[v]].settings[i].sustain) {
                        env[v][i] -= decStep;
                        if (env[v][i] <= profiles[voicePs[v]].settings[i].sustain)
                            env[v][i] = profiles[voicePs[v]].settings[i].sustain;
                    }
                } else if (state[v][i] == 3) {
                    float relStep = (profiles[voicePs[v]].settings[i].release > 1e-6) ? (1.0f / (float)(profiles[voicePs[v]].settings[i].release * sr)) : 1.0f;
                    env[v][i] -= relStep;
                    if (env[v][i] <= 0.0f) {
                        env[v][i] = 0;
                        state[v][i] = 0;
                    }
                }
            }


            // Phase + waveform
            phase[v] += freq[v] / sr;
            if (phase[v] >= 1.0f) phase[v] -= 1.0f;
            
            for (int i = 0; i < 3; i++) {
                float voiceSample =
                    generateSample(profiles[voicePs[v]].settings[i].waveType, phase[v]) * env[v][i] * profiles[voicePs[v]].settings[i].gain;
                if (voiceSample< 1e-6) {
                    totalZero++;
                } else {
                    totalNonZero++;
                }
                mix += voiceSample;
            }
        }

        // --- voice count smoothing ---
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
        // mix *= gain;

        // clamp
        if (mix > 1.0f) mix = 1.0f;
        if (mix < -1.0f) mix = -1.0f;

        buffer[i * 2 + 0] = mix;
        buffer[i * 2 + 1] = mix;

        sc++;
    }

    printf("totalZero: %d\n", totalZero);
    printf("totatNonZero: %d\n", totalNonZero);

    // ========== WRITE TO FILE ==========
    sf_writef_float(snd, buffer.data(), totalFrames);
    sf_close(snd);
    return true;
}

