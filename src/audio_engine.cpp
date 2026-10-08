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

std::vector<std::vector<Clip>> g_clips(MAX_TRACKS);
std::mutex g_clipsMutex;

AudioEngine::AudioEngine() {
    // Lock memory to avoid page faults in audio thread
    mlockall(MCL_CURRENT | MCL_FUTURE);
    for (auto& g : trackGain) g = 1.0f;
    recRing.assign(REC_RING, 0.0f);
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

    // An offline export is running processAudio() itself: stay silent
    std::unique_lock<std::mutex> processLock(engine->processMutex, std::try_to_lock);
    if (!processLock.owns_lock()) {
        memset(outL, 0, nframes * sizeof(float));
        memset(outR, 0, nframes * sizeof(float));
        return 0;
    }
    const float* input = js->in ? (const float*)jack_port_get_buffer(js->in, nframes) : nullptr;
    if (engine->tunerEnabled && input) {
      const float* in = input;
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

    engine->processAudio(outL, outR, input, nframes);

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
    for (int i = 0; i < MAX_TRACKS; i++) {
        profiles[i].settings[0] = startingProfile;
        profiles[i].settings[1] = startingProfile;
        profiles[i].settings[2] = startingProfile;
        profiles[i].settings[1].gain = 0.0f;
        profiles[i].settings[2].gain = 0.0f;
        profiles[i].settings[0].freq = 0.0f;
        profiles[i].settings[1].freq = 0.0f;
        profiles[i].settings[2].freq = 0.0f;
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

    // Record from the first physical capture port
    ports = jack_get_ports(jack->client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                           JackPortIsOutput | JackPortIsPhysical);
    if (jack->in && ports && ports[0])
        jack_connect(jack->client, ports[0], jack_port_name(jack->in));
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

void AudioEngine::setProfile(const Profile& p, int n) {
  if (n >= 0 && n < MAX_TRACKS)
    profiles[n] = p;
}

Profile AudioEngine::getProfile(int n) {
  return (n >= 0 && n < MAX_TRACKS) ? profiles[n] : Profile{};
}

void AudioEngine::setTrackGain(int track, float db) {
    if (track >= 0 && track < MAX_TRACKS) trackGain[track] = PdHost::dbToGain(db);
}

size_t AudioEngine::drainRecording(std::vector<float>& out) {
    size_t r = recRead.load(std::memory_order_relaxed);
    size_t w = recWrite.load(std::memory_order_acquire);
    size_t n = 0;
    for (; r != w; r = (r + 1) % REC_RING, n++) out.push_back(recRing[r]);
    recRead.store(r, std::memory_order_release);
    return n;
}

void AudioEngine::startClip(int track, uint32_t clip, double beat, double spb) {
    for (auto& v : clipVoices) {
        if (v.active) continue;
        v = {true, track, clip, (int64_t)std::llround(beat * spb)};
        return;
    }
}

void AudioEngine::stopClip(int track, uint32_t clip) {
    for (auto& v : clipVoices)
        if (v.active && v.track == track && v.clip == clip) v.active = false;
}

void AudioEngine::stopAllClips() {
    for (auto& v : clipVoices) v.active = false;
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
            if (!isPlaying) { pd.allNotesOff(); stopAllClips(); }
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
            stopAllClips();
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

// Next event the sequencer acts on: note starts/ends, perc blips, clip starts/ends.
static Event* next_playable(Event* ev) {
    if (ev) ev = next(ev);
    while (ev && ev->type != EV_NOTE_START && ev->type != EV_NOTE_END && ev->type != EV_PERC &&
           ev->type != EV_AUDIO_START && ev->type != EV_AUDIO_END)
        ev = next(ev);
    return ev;
}

// Sequencer + Pd. Notes and perc blips are sent to the Pd patch of their
// track; every patch's [dac~] output is mixed by libpd and written to the
// JACK buffers.
void AudioEngine::processAudio(float* outL, float* outR, const float* input, uint32_t frames)
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

    // Recording starts with the first block of playback after it's requested
    if (recRequested && isPlaying && !recActive) {
        recStartBeat = double(sampleCounter) / samplesPerBeat;
        recCaptured = 0;
        recPassCount = 0;
        recActive = true;
    } else if (recActive && (!recRequested || !isPlaying)) {
        recActive = false;
    }

    // Clip lists are only read while the main thread isn't changing them
    std::unique_lock<std::mutex> clipLock(g_clipsMutex, std::try_to_lock);

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
                        pd.diag.unroutedNotes++;   // track has no Pd patch
                } else if (currentEv->type == EV_NOTE_END) {
                    pd.noteOff(currentEv->id);
                } else if (currentEv->type == EV_PERC) {
                    // Starting (or looping back) mid-song skips past every
                    // earlier event at once; only play hits that are due now.
                    double lateSamples = (currentBeat - currentEv->beat) * samplesPerBeat;
                    if (lateSamples <= frames)
                        pd.hit(currentEv->profile, 1.0f);
                } else if (currentEv->type == EV_AUDIO_START) {
                    // Started late (mid-clip) it still plays from the right place
                    startClip(currentEv->profile, currentEv->id, currentEv->beat, samplesPerBeat);
                } else if (currentEv->type == EV_AUDIO_END) {
                    stopClip(currentEv->profile, currentEv->id);
                }
                pd.diag.events++;
                currentEv = next_playable(currentEv);
            }
        }

        // Pd keeps running while stopped, so release tails finish and
        // patches being edited stay audible.
        float l = 0.0f, r = 0.0f;
        pd.renderFrame(l, r);

        // Clips, positioned by the sample counter so they stay in time
        if (isPlaying && clipLock.owns_lock()) {
            for (const ClipVoice& v : clipVoices) {
                if (!v.active || v.track < 0 || v.track >= (int)g_clips.size()) continue;
                const auto& clips = g_clips[v.track];
                if (v.clip >= clips.size()) continue;
                const Clip& c = clips[v.clip];
                int64_t pos = (int64_t)sampleCounter - v.startSc + (int64_t)c.startSample;
                if (pos < 0 || pos >= (int64_t)c.samples.size()) continue;
                float smp = c.samples[(size_t)pos] * trackGain[v.track].load(std::memory_order_relaxed);
                if (pd.routes(v.track)) {
                    pd.addInput(v.track, smp);   // through the track's patch
                } else {
                    l += smp;
                    r += smp;
                }
            }
        }

        outL[in] = std::clamp(l, -1.0f, 1.0f);
        outR[in] = std::clamp(r, -1.0f, 1.0f);

        if (!isPlaying) continue;

        // Capture the input while recording
        if (recActive && input) {
            size_t w = recWrite.load(std::memory_order_relaxed);
            size_t next = (w + 1) % REC_RING;
            // Dropped only if the main thread stalls for the whole ring (~40 s)
            if (next != recRead.load(std::memory_order_acquire)) {
                recRing[w] = input[in];
                recWrite.store(next, std::memory_order_release);
                recCaptured++;
            }
        }

        if (!freeze) sampleCounter++;

        // Loop: jump back, release everything, restart the event list
        if (loopEndSamples > 0 && sampleCounter > loopEndSamples) {
            sampleCounter = loopStartSamples;
            pd.allNotesOff();
            stopAllClips();
            // While recording, each pass round the loop becomes its own take
            int n = recPassCount;
            if (recActive && n < (int)recPassEnds.size()) {
                recPassEnds[n] = recCaptured;
                recPassCount = n + 1;
            }
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

bool AudioEngine::exportWav(const char* path, EventTree* events, float startBeat, float endBeat,
                            float tailSeconds)
{
    SF_INFO info{};
    info.samplerate = (int)sampleRate;
    info.channels   = 2;
    info.format     = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
    SNDFILE* snd = sf_open(path, SFM_WRITE, &info);
    if (!snd) {
        std::cerr << "Failed to open " << path << " for writing\n";
        return false;
    }

    // From here the JACK callback outputs silence and this thread runs the
    // engine: the queued messages below are applied by the first block.
    std::lock_guard<std::mutex> lock(processMutex);
    bool wasRecording = recRequested;
    recRequested = false;

    setEvents(events);
    resetSampleCounter(startBeat, -1.0f);   // no loop
    setPlayState(true);

    const double spb = samplesPerBeat();
    const uint64_t playFrames = (uint64_t)std::max(0.0, (endBeat - startBeat) * spb);
    const uint64_t totalFrames = playFrames + (uint64_t)(tailSeconds * sampleRate);

    constexpr uint32_t BLOCK = 256;
    float L[BLOCK], R[BLOCK], frames[2 * BLOCK];
    bool stopped = false;
    for (uint64_t done = 0; done < totalFrames;) {
        // Stop the transport at the end so releases and effect tails ring out
        if (!stopped && done >= playFrames) {
            setPlayState(false);
            stopped = true;
        }
        uint32_t n = (uint32_t)std::min<uint64_t>(BLOCK, totalFrames - done);
        if (!stopped) n = (uint32_t)std::min<uint64_t>(n, playFrames - done);   // stop exactly at the end
        processAudio(L, R, nullptr, n);
        for (uint32_t i = 0; i < n; i++) {
            frames[2 * i]     = L[i];
            frames[2 * i + 1] = R[i];
        }
        sf_writef_float(snd, frames, n);
        done += n;
    }
    if (!stopped) setPlayState(false);
    recRequested = wasRecording;

    sf_close(snd);
    return true;
}
