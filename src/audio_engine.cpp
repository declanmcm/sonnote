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
#include <fftw3.h>
#include <complex>
#include "audio_engine.hpp"

struct AudioEngine::JackState {
        jack_client_t* client = nullptr;
        jack_port_t* outL = nullptr;
        jack_port_t* outR = nullptr;
        jack_port_t* in = nullptr;
        AudioEngine* engine = nullptr;
    };
FILE* out = nullptr;
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
float phaseF[10] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
//std::complex<__float128> left = -0.7419066419869599251f + 0.1260499811608772194i;
//std::complex<__float128> right = -0.741906641986960028347337263f + 0.126049981160877173433591185i;

// ======================================================
// ================== JACK CALLBACK ======================
// ======================================================

int jackProcess(jack_nframes_t nframes, void* arg)
{
    AudioEngine::JackState* js = static_cast<AudioEngine::JackState*>(arg);
    AudioEngine* engine = js->engine;

    float* outL = (float*)jack_port_get_buffer(js->outL, nframes);
    float* outR = (float*)jack_port_get_buffer(js->outR, nframes);
    if (engine->tunerEnabled) {
      float* in = (float*)jack_port_get_buffer(js->in, nframes);
      memcpy(audioIn + audioi, in, sizeof(float) * (audioi + nframes <= audioBufferSize ? nframes : audioBufferSize - audioi));
      if (audioi + nframes >= audioBufferSize) {
        newAudioIn = true;
        audioi = 0;
      } else {
        audioi += nframes;
        if (audioi > audioBufferSize)
          audioi = audioBufferSize;
      }
    }

    /*printf("i: %d\n", audioOuti);
    if (newAudioOut && audioOuti == audioBufferSize) {
      printf("reset\n");
      memcpy(audioOut, pendingAudioOut, audioBufferSize);
      audioOuti = 0;
      newAudioOut = false;
    }

    if (audioOuti != audioBufferSize) {
      int len = (audioOuti + nframes > audioBufferSize) ? audioBufferSize - audioOuti : nframes;
      memcpy(outL, audioOut + audioOuti, len);
      memcpy(outR, audioOut + audioOuti, len);
      //printf("audioOuti: %d, nframes: %d, len: %d, outL: %f, audioOut:%f\n", audioOuti, nframes, len, outL[0], audioOut[audioOuti]);
      audioOuti += len;
      if (audioOuti > audioBufferSize) {
        audioOuti = audioBufferSize;
      }
    } else {
      printf("none\n");
    }*/

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
        profiles[i].settings[0].freq = 0.0f;
        profiles[i].settings[1].freq = 0.0f;
        profiles[i].settings[2].freq = 0.0f;
    }

    for (int i = 0; i < 10; i++) {
        profilesPerc[i].settings[0] = startingProfile;
        profilesPerc[i].settings[1] = startingProfile;
        profilesPerc[i].settings[2] = startingProfile;
        profilesPerc[i].settings[1].gain = 0.0f;
        profilesPerc[i].settings[2].gain = 0.0f;
        float freq = 3133.375 * std::log(1.00737564 * (i + 1));
        printf("freq: %f\n", freq);
        profilesPerc[i].settings[0].freq = freq;
        profilesPerc[i].settings[1].freq = freq;
        profilesPerc[i].settings[2].freq = freq;
        profilesPerc[i].settings[0].hold = 0.1f;
        profilesPerc[i].settings[1].hold = 0.1f;
        profilesPerc[i].settings[2].hold = 0.1f;
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

    if (!pd.init((int)sampleRate))
        std::cerr << "libpd failed to initialise; Pd patches won't play\n";

    jack->outL = jack_port_register(
        jack->client, "out_L",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput, 0);

    jack->outR = jack_port_register(
        jack->client, "out_R",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsOutput, 0);

    jack->in = jack_port_register(
        jack->client, "in_L",
        JACK_DEFAULT_AUDIO_TYPE,
        JackPortIsInput, 0);

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

void AudioEngine::setJump(float j) {
  Message m;
  m.type = MessageType::JumpSize;
  m.fValue = j;
  messageQueue.push(m);
}

void AudioEngine::setGain(float g) {
    Message m;
    m.type = MessageType::SetGain;
    m.fValue = g;
    messageQueue.push(m);
}

void AudioEngine::setPlayState(bool on) {
  if (!out)
    out = fopen("out.csv", "w+");
  else
    out = nullptr;
  printf("play\n");
  Message m;
  m.type = MessageType::SetPlayState;
  m.bValue = on;
  messageQueue.push(m);
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

void AudioEngine::setProfile(Profile& p, int n, bool noteMode) {
  if (noteMode)
    profiles[n] = p;
  else
    profilesPerc[n] = p;
}

Profile AudioEngine::getProfile(int n, bool noteMode) {
  if (noteMode) {
    return profiles[n];
  } else {
    return profilesPerc[n];
  }
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

void AudioEngine::resetSampleCounter(float playhead_beat, float loop_end) {
    const double secondsPerBeat = 60.0 / tempoBPM;
    const double samplesPerBeat = secondsPerBeat * sampleRate;
    int newSc = std::floor(playhead_beat * samplesPerBeat);
    if (newSc == -1)
      sampleCounter = 0;
    else
      sampleCounter = newSc;
    printf("sampleCounter: %ld\n", sampleCounter);
    loopStartSamples = sampleCounter;
    if (loop_end < 0)
      loopEndSamples = 0;
    else
      loopEndSamples = std::floor(loop_end * samplesPerBeat);
    Message m;
    m.type = MessageType::ResetSampleCounter;
    messageQueue.push(m);
}

void AudioEngine::toggleTuner() {
  tunerEnabled = !tunerEnabled;
}



// ======================================================
// =============== APPLY MESSAGES (RT-SAFE) ==============
// ======================================================

void AudioEngine::applyMessages() {
    Message msg;
    std::fflush(stdout);
    while (messageQueue.pop(msg)) {
        switch (msg.type) {

        case MessageType::SetGain:
            gain = msg.fValue;
            break;

        case MessageType::SetPlayState:
            isPlaying = msg.bValue;
            pd.setPlaying(isPlaying);
            if (!isPlaying) pd.allNotesOff();
            std::fflush(stdout);
            break;

        case MessageType::SetFrequency:
            ref = msg.fValue;
            pd.setRefFreq(ref);
            for (int i = 0; i < 10; i++)
              phaseF[i] = 0.0f;
            break;

        case MessageType::JumpSize:
            power = msg.fValue;
            for (int i = 0; i < 10; i++)
              phaseF[i] = 0.0f;
            break;

        case MessageType::SetWaveform:
            waveform = msg.waveform;
            break;

        case MessageType::SetTempo:
            tempoBPM = msg.fValue;
            pd.setTempo(tempoBPM);
            break;

        case MessageType::SetADSR:
            adsr = msg.adsr;
            break;

        case MessageType::Freeze:
            freeze = true;
            break;

        case MessageType::ResetSampleCounter:
            pd.allNotesOff();
            currentBeat = 0.0;
            nextEventIndex = 0;
            // Clear voices
            for (size_t i = 0; i < MAX_VOICES; ++i) {
                for (int j = 0; j < 3; j++) {
                    voiceStates[i][j] = 0;
                    voiceEnvs[i][j] = 0.0f;
                    voicePhases[i][j] = 0.0f;
                    voiceFreqs[i][j] = 0.0f;
                }
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
        while (currentEv && currentEv->left) {
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

/*void AudioEngine::processAudio(float* outL, float* outR, uint32_t frames) {
    // Zero buffers
    memset(outL, 0, frames * sizeof(float));
    memset(outR, 0, frames * sizeof(float));

    applyMessages();

    if (!isPlaying) {
        sampleCounter += frames;
        return;
    }

    const float sr = sampleRate;
    //ref += 0.01;

    const double secondsPerBeat = 60.0 / tempoBPM;
    const double samplesPerBeat = secondsPerBeat * sampleRate;
    for (uint32_t in = 0; in < frames; ++in)
    {
        float s = 0.0f;

        // Phase & sample
        float freq = ref;
        int i = 0;
        int iters = std::floor((std::log(20000) / std::log(freq)) / power);
        while (freq < 20000) {

          float phase = phaseF[i];

          phase += freq / sr;
          if (phase >= 1.0f) phase -= 1.0f;
          phaseF[i] = phase;
          float voiceSample = generateSample(WaveformType::Sine, phase);

          s += voiceSample / 20;
          freq = std::pow(freq, power);
          i++;
        }
        if (s > 1.0f) {
          s = 1.0f;
        }
        if (s < -1.0f) {
          s = -1.0f;
        }
        outL[in] = s;
        outR[in] = s;
        sampleCounter++;
    }
}*/

// Sequencer + Pd. Notes are sent to the Pd patch of their profile; every
// patch's [dac~] output is mixed by libpd and written to the JACK buffers.
void AudioEngine::processAudio(float* outL, float* outR, uint32_t frames)
{
    memset(outL, 0, frames * sizeof(float));
    memset(outR, 0, frames * sizeof(float));

    // Take libpd for this block. If the main thread is busy with it, Pd is
    // silent for this block and note messages are queued, not lost.
    pd.beginBlock();
    applyMessages();
    pd.sendParams();   // gains and song parameters that changed (tempo, play state, ...)

    pd.diag.playing = isPlaying;
    pd.diag.frozen = freeze;
    pd.diag.nextBeat = currentEv ? currentEv->beat : -1.0f;

    const double samplesPerBeat = (60.0 / tempoBPM) * sampleRate;

    for (uint32_t in = 0; in < frames; ++in) {
        if (isPlaying) {
            currentBeat = double(sampleCounter) / samplesPerBeat;
            b = currentBeat;

            // Fire every event the playhead has reached
            while (!freeze && currentEv && currentEv->beat <= currentBeat) {
                if (currentEv->type == EV_NOTE_START) {
                    if (pd.routes(currentEv->profile))
                        pd.noteOn(currentEv->profile, currentEv->id,
                                  ref * currentEv->ratio.eval(), 1.0f);
                    else
                        pd.diag.unroutedNotes++;   // profile has no Pd patch
                } else if (currentEv->type == EV_NOTE_END) {
                    pd.noteOff(currentEv->id);
                }
                pd.diag.events++;
                currentEv = next_note(currentEv);
            }
        }

        // Pd keeps running while stopped, so release tails finish and
        // patches being edited stay audible.
        float l = 0.0f, r = 0.0f;
        pd.renderFrame(l, r);
        outL[in] = std::clamp(l, -1.0f, 1.0f);
        outR[in] = std::clamp(r, -1.0f, 1.0f);

        if (!isPlaying) continue;
        if (!freeze) sampleCounter++;

        // Loop: jump back, release everything, restart the event list
        if (loopEndSamples > 0 && sampleCounter > loopEndSamples) {
            sampleCounter = loopStartSamples;
            pd.allNotesOff();
            currentEv = activeEvents.get_first();
        }
    }

    if (!isPlaying) sampleCounter += frames;
    pd.diag.beat = (float)currentBeat;
    pd.endBlock();
}




// ======================================================
// ====================== EXPORT WAV =====================
// ======================================================
//
// This uses libsndfile and DOES NOT TOUCH JACK or RT threads.
// Purely offline rendering.
//

bool AudioEngine::exportWav(const char* path, float playhead_beat, float durationSeconds)
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
    printf("duration: %f, totalFrames: %ld, sampleRate: %f\n", durationSeconds, totalFrames, sampleRate);

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
    uint64_t sc = playhead_beat * samplesPerBeat; // offline sample counter
    int totalZero = 0;
    int totalNonZero = 0;

    Event* currentEv = events.root;
    while (currentEv->left)
      currentEv = currentEv->left;
    while (currentEv && currentEv->type != EV_NOTE_START)
      currentEv = next(currentEv);
    if (currentEv) {
      printf("FIRSTEV: %f\n", currentEv->beat);
    } else {
      printf("no event\n");
    }

    printf("totalFrames: %ld\n", totalFrames);

    for (uint64_t in = 0; in < totalFrames; ++in)
    {
        float s = 0.0f;

        // --- Sequencer: map sampleCounter → beat and fire events when crossing beat positions ---
        currentBeat = double(sc) / samplesPerBeat;
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
                    printf("note on: %d at %ld\n", currentEv->id, in);

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

                    voiceIDs[voiceIndex] = currentEv->id;
                    for (int i = 0; i < 3; i++) {
                        voiceFreqs[voiceIndex][i] = ref * currentEv->ratio.eval();
                        voicePhases[voiceIndex][i] = 0.0f;
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
                          //printf("heylo\n");
                            voiceStates[v][0] = 3;
                            voiceStates[v][1] = 3;
                            voiceStates[v][2] = 3;
                            break;
                        }
                    }
                }

                currentEv = next_note(currentEv);
                if (currentEv) {
                  printf("nextEv beat: %f\n", currentEv->beat);
                } else {
                  printf("no more notes\n");
                }

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
            //const float freq = 220.0f * std::pow(2.0f, 3.0/12.0) * std::pow(2, voiceFreqs[v]->evaluate(b, t, 0, 0) - 4.0f);

            for (int i = 0; i < 3; i++) {
                const float freq = voiceFreqs[v][i];
                //printf("voice %d freq: %f\n", v, freq);
                float phase = voicePhases[v][i];

                phase += freq / sr;
                if (phase >= 1.0f) phase -= 1.0f;
                voicePhases[v][i] = phase;
                //printf("gain: %f\n", profiles[voiceProfiles[v]].settings[i].gain);
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

            s /= divisorSmooth;
        } else {
            divisorSmooth = 1.0f;
        }

        // Apply global gain
        // s *= gain;

        // Clamp
        if (s > 1.0f) {
          //printf("YO");
          s = 1.0f;
        }
        if (s < -1.0f) {
          //printf("YO");
          s = -1.0f;
        }

        buffer[in * 2] = s;
        buffer[in * 2 + 1] = s;
        sc++;
        //if (s > 1e-6)
        //  printf("s: %f\n", s);
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
    }
    printf("currentBeat: %f\n", b);

    printf("totalZero: %d\n", totalZero);
    printf("totatNonZero: %d\n", totalNonZero);

    // ========== WRITE TO FILE ==========
    sf_writef_float(snd, buffer.data(), totalFrames);
    sf_close(snd);
    return true;
}

