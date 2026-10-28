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
std::vector<std::vector<AutoLane>> g_automation(MAX_TRACKS);
std::mutex g_automationMutex;

AudioEngine::AudioEngine() {
    // Lock memory to avoid page faults in audio thread
    mlockall(MCL_CURRENT | MCL_FUTURE);
    for (auto& g : trackGain) g = 1.0f;
    for (auto& a : trackAudible) a = true;
    recRing.assign(REC_RING, 0.0f);
    busData.assign((size_t)MAX_TRACKS * 2 * MAX_BLOCK, 0.0f);
    clipInData.assign((size_t)MAX_TRACKS * MAX_BLOCK, 0.0f);
    metroBuf.assign(MAX_BLOCK, 0.0f);
    fxScratch.assign(2 * MAX_BLOCK, 0.0f);
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

void AudioEngine::setTrackAudible(int track, bool on) {
    if (track >= 0 && track < MAX_TRACKS) trackAudible[track] = on;
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
            if (!isPlaying) {
                pd.allNotesOff(); instrumentsAllNotesOff(0); stopAllClips();
                for (auto& o : percOffs) o.active = false;
            }
            autoEpoch++;
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
            instrumentsAllNotesOff(0);
            stopAllClips();
            for (auto& o : percOffs) o.active = false;
            autoEpoch++;
            currentBeat = 0.0;
            nextEventIndex = 0;
            metroBeat = INT64_MIN;   // click again if restarting on a beat
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

// One sample of the metronome at sample index `sc`: a short sine blip that
// starts whenever the playhead reaches a whole beat.
float AudioEngine::metronomeSample(uint64_t sc, double samplesPerBeat) {
    int64_t beat = (int64_t)std::floor((double)sc / samplesPerBeat);
    if (beat != metroBeat) {
        // Only click on the beat itself, not when starting (or looping) mid-beat
        bool onBeat = (double)sc - beat * samplesPerBeat < 1.0;
        if (onBeat && metronome.load(std::memory_order_relaxed) && !exporting) {
            metroFreq = (beat % 4 == 0) ? 1760.0f : 1320.0f;
            metroPhase = 0.0f;
            metroEnv = 0.35f;
        }
        metroBeat = beat;
    }
    if (metroEnv < 1e-4f) return 0.0f;
    float s = metroEnv * sinf(2.0f * (float)M_PI * metroPhase);
    metroPhase += metroFreq / sampleRate;
    if (metroPhase >= 1.0f) metroPhase -= 1.0f;
    metroEnv *= std::exp(-1.0f / (0.015f * sampleRate));   // ~15 ms decay
    return s;
}

// Fires every event the playhead (currentBeat) has reached. `offset` is
// where in the current block that is, for the VST3 plugins.
void AudioEngine::fireDueEvents(double samplesPerBeat, uint32_t blockFrames, uint32_t offset) {
    while (currentEv && currentEv->beat <= currentBeat) {
        if (currentEv->type == EV_NOTE_START) {
            int t = currentEv->profile;
            float freq = ref * currentEv->ratio.eval();
            float noteGain = PdHost::dbToGain(currentEv->gainDb);
            bool played = false;
            if (pd.routes(t)) {
                pd.noteOn(t, currentEv->id, freq, 1.0f, noteGain);
                played = true;
            }
            if (t >= 0 && t < MAX_TRACKS && plugins[t].instrument) {
                // The note's gain becomes its velocity: 0 dB = 100/127
                float velocity = std::clamp(noteGain * (100.0f / 127.0f), 0.0f, 1.0f);
                plugins[t].instrument->noteOn((int)offset, currentEv->id, freq, velocity);
                played = true;
            }
            if (!played) pd.diag.unroutedNotes++;   // track has no patch or instrument
        } else if (currentEv->type == EV_NOTE_END) {
            pd.noteOff(currentEv->id);
            for (TrackPlugins& tp : plugins)
                if (tp.instrument) tp.instrument->noteOff((int)offset, currentEv->id);
        } else if (currentEv->type == EV_PERC) {
            // Starting (or looping back) mid-song skips past every
            // earlier event at once; only play hits that are due now.
            double lateSamples = (currentBeat - currentEv->beat) * samplesPerBeat;
            if (lateSamples <= blockFrames) {
                // The blip's velocity is kept as a gain, like a note's (0 dB = 1)
                float g = PdHost::dbToGain(currentEv->gainDb);
                pd.hit(currentEv->profile, g);
                percNoteOn(currentEv->profile, currentEv->id, g, offset);
            }
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

// Sequencer + mixer. Each track renders into its own bus (its Pd patch, or
// its clips when it has no patch); the buses are then summed with the
// metronome into the output. Works in pieces of at most MAX_BLOCK frames.
void AudioEngine::processAudio(float* outL, float* outR, const float* input, uint32_t frames)
{
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

    for (uint32_t done = 0; done < frames;) {
        uint32_t n = std::min<uint32_t>(MAX_BLOCK, frames - done);
        processBlock(outL + done, outR + done, input ? input + done : nullptr, n,
                     samplesPerBeat, clipLock.owns_lock());
        done += n;
    }

    pd.diag.beat = (float)currentBeat;
    playBeat.store(currentBeat, std::memory_order_relaxed);
    pd.endBlock();
}

// One piece of at most MAX_BLOCK frames. While playing it's cut into
// segments that end where the next event falls due or the loop wraps, so
// events fire on the right sample and everything within a segment can be
// rendered as a block.
void AudioEngine::processBlock(float* outL, float* outR, const float* input, uint32_t n,
                               double samplesPerBeat, bool clipsReadable)
{
    for (int t = 0; t < MAX_TRACKS; t++) {
        std::fill_n(busL(t), n, 0.0f);
        std::fill_n(busR(t), n, 0.0f);
    }
    std::fill_n(metroBuf.data(), n, 0.0f);
    const uint64_t blockStart = sampleCounter;   // for the plugins' transport

    for (uint32_t pos = 0; pos < n;) {
        uint32_t len = n - pos;
        if (isPlaying) {
            currentBeat = double(sampleCounter) / samplesPerBeat;
            b = currentBeat;
            if (!freeze) {
                fireDueEvents(samplesPerBeat, n, pos);
                // End the segment where the next event falls due (at least one
                // frame, in case rounding leaves it just short)
                if (currentEv) {
                    double due = std::ceil((double)currentEv->beat * samplesPerBeat) - (double)sampleCounter;
                    if (due < len) len = (uint32_t)std::max(1.0, due);
                }
            }
            // ...or where the counter passes the loop end
            if (loopEndSamples > 0) {
                int64_t toWrap = (int64_t)loopEndSamples + 1 - (int64_t)sampleCounter;
                if (toWrap < 1) len = 1;
                else if (toWrap < (int64_t)len) len = (uint32_t)toWrap;
            }
            // ...or where a perc hit on a VST3 instrument ends
            len = firePercOffs(pos, len);
            if (!freeze) applyAutomation(pos, len, samplesPerBeat);
        }

        renderSegment(pos, len, input, samplesPerBeat, clipsReadable);
        pos += len;
        if (!isPlaying) continue;

        if (!freeze) sampleCounter += len;

        // Loop: jump back, release everything, restart the event list
        if (loopEndSamples > 0 && sampleCounter > loopEndSamples) {
            sampleCounter = loopStartSamples;
            metroBeat = INT64_MIN;
            pd.allNotesOff();
            instrumentsAllNotesOff((int)std::min(pos, n - 1));
            stopAllClips();
            for (auto& o : percOffs) o.active = false;
            autoEpoch++;
            // While recording, each pass round the loop becomes its own take
            int k = recPassCount;
            if (recActive && k < (int)recPassEnds.size()) {
                recPassEnds[k] = recCaptured;
                recPassCount = k + 1;
            }
            currentEv = activeEvents.get_first();
        }
    }

    // VST3 instruments add to their track's bus, then its effects process it
    processPlugins(n, blockStart, samplesPerBeat);

    // Mix: every audible track's bus plus the metronome. Muted tracks are
    // still rendered (so Pd and VST3 tails carry on), just not heard.
    std::array<bool, MAX_TRACKS> audible;
    for (int t = 0; t < MAX_TRACKS; t++)
        audible[t] = trackAudible[t].load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; i++) {
        float l = metroBuf[i], r = metroBuf[i];
        for (int t = 0; t < MAX_TRACKS; t++) {
            if (!audible[t]) continue;
            l += busL(t)[i];
            r += busR(t)[i];
        }
        outL[i] = std::clamp(l, -1.0f, 1.0f);
        outR[i] = std::clamp(r, -1.0f, 1.0f);
    }

    if (!isPlaying) sampleCounter += n;
}

// Renders frames [pos, pos + len) of the current block into the track buses.
// No events fall due inside the segment, so sample i of it is at
// sampleCounter + i (or stays at sampleCounter while frozen).
void AudioEngine::renderSegment(uint32_t pos, uint32_t len, const float* input,
                                double samplesPerBeat, bool clipsReadable)
{
    const uint64_t sc = sampleCounter;
    const uint32_t step = freeze ? 0 : 1;

    // Clips, positioned by the sample counter so they stay in time. A track
    // with a Pd patch plays them through it ([tabreceive~ $0-in]); otherwise
    // they go straight onto its bus.
    std::array<bool, MAX_TRACKS> hasClipIn{};
    if (isPlaying && clipsReadable) {
        for (const ClipVoice& v : clipVoices) {
            if (!v.active || v.track < 0 || v.track >= MAX_TRACKS || v.track >= (int)g_clips.size()) continue;
            const auto& clips = g_clips[v.track];
            if (v.clip >= clips.size()) continue;
            const Clip& c = clips[v.clip];
            const float g = trackGain[v.track].load(std::memory_order_relaxed);
            const bool toPd = pd.routes(v.track);
            float* dst = toPd ? clipIn(v.track) : busL(v.track) + pos;
            if (toPd && !hasClipIn[v.track]) {
                std::fill_n(dst, len, 0.0f);
                hasClipIn[v.track] = true;
            }
            int64_t first = (int64_t)sc - v.startSc + (int64_t)c.startSample;
            for (uint32_t i = 0; i < len; i++) {
                int64_t p = first + (int64_t)(i * step);
                if (p < 0 || p >= (int64_t)c.samples.size()) continue;
                dst[i] += c.samples[(size_t)p] * g;
            }
            if (!toPd)   // mono clip: same on both sides
                for (uint32_t i = 0; i < len; i++) busR(v.track)[pos + i] = busL(v.track)[pos + i];
        }
    }

    // Pd runs for every track with a patch, playing or not, so release tails
    // finish and patches being edited stay audible.
    for (int t = 0; t < MAX_TRACKS; t++)
        if (pd.routes(t))
            pd.render(t, hasClipIn[t] ? clipIn(t) : nullptr, busL(t) + pos, busR(t) + pos, (int)len);

    if (!isPlaying) return;

    for (uint32_t i = 0; i < len; i++) {
        metroBuf[pos + i] = metronomeSample(sc + i * step, samplesPerBeat);

        // Capture the input while recording
        if (recActive && input) {
            size_t w = recWrite.load(std::memory_order_relaxed);
            size_t next = (w + 1) % REC_RING;
            // Dropped only if the main thread stalls for the whole ring (~40 s)
            if (next != recRead.load(std::memory_order_acquire)) {
                recRing[w] = input[pos + i];
                recWrite.store(next, std::memory_order_release);
                recCaptured++;
            }
        }
    }
}




void AudioEngine::instrumentsAllNotesOff(int offset) {
    for (TrackPlugins& tp : plugins)
        if (tp.instrument) tp.instrument->allNotesOff(offset);
}

// A perc blip on a track with a VST3 instrument: a short note at the
// track's hit frequency (settings[0].freq; C2 if it isn't set, where GM
// drum maps put the kick), sent with the track's tuning mode like any note.
void AudioEngine::percNoteOn(int t, uint32_t id, float gain, uint32_t offset) {
    if (t < 0 || t >= MAX_TRACKS || !plugins[t].instrument) return;
    vst3::Plugin& inst = *plugins[t].instrument;
    float freq = profiles[t].settings[0].freq;
    if (!(freq > 0.0f)) freq = 65.406f;
    // A blip still sounding is ended first, so its note-off can't cut the new one
    for (PercOff& o : percOffs)
        if (o.active && o.track == t && o.id == id) { inst.noteOff((int)offset, id); o.active = false; }
    inst.noteOn((int)offset, id, freq, std::clamp(gain * (100.0f / 127.0f), 0.0f, 1.0f));
    uint64_t due = sampleCounter + (uint64_t)std::max(1.0f, PERC_GATE * sampleRate);
    for (PercOff& o : percOffs) {
        if (o.active) continue;
        o = {true, t, id, due};
        return;
    }
    inst.noteOff((int)offset, id);   // no room to wait: end it straight away
}

// Ends the perc notes due by the start of this segment, and shortens the
// segment to end where the next one is due.
uint32_t AudioEngine::firePercOffs(uint32_t pos, uint32_t len) {
    for (PercOff& o : percOffs) {
        if (!o.active) continue;
        if (o.due <= sampleCounter) {
            if (plugins[o.track].instrument) plugins[o.track].instrument->noteOff((int)pos, o.id);
            o.active = false;
        } else if (o.due - sampleCounter < len) {
            len = (uint32_t)(o.due - sampleCounter);
        }
    }
    return len;
}

// Sends every automation lane's value to its plugin over the segment, one
// point every AUTO_STEP frames (plugins smooth between them), whenever it
// has changed.
void AudioEngine::applyAutomation(uint32_t pos, uint32_t len, double samplesPerBeat) {
    constexpr uint32_t AUTO_STEP = 64;
    std::unique_lock<std::mutex> lock(g_automationMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (int t = 0; t < MAX_TRACKS && t < (int)g_automation.size(); t++) {
        for (AutoLane& lane : g_automation[t]) {
            if (lane.points.empty() || lane.held) continue;
            vst3::Plugin* p = plugin(t, lane.plugin);
            if (!p) continue;
            for (uint32_t i = 0; i < len; i += AUTO_STEP) {
                double beat = double(sampleCounter + i) / samplesPerBeat;
                float v = lane.valueAt(beat);
                if (lane.sentEpoch == autoEpoch && std::abs(v - lane.lastSent) < 1e-5f) continue;
                p->setParameter((int)(pos + i), lane.param, v);
                lane.lastSent = v;
                lane.sentEpoch = autoEpoch;
            }
        }
    }
}

void AudioEngine::processPlugins(uint32_t n, uint64_t blockStart, double samplesPerBeat) {
    vst3::Transport tr;
    tr.playing = isPlaying;
    tr.tempo = tempoBPM;
    tr.samplePos = (int64_t)blockStart;
    tr.beat = (double)blockStart / samplesPerBeat;
    float* inL = fxScratch.data();
    float* inR = inL + MAX_BLOCK;
    for (int t = 0; t < MAX_TRACKS; t++) {
        TrackPlugins& tp = plugins[t];
        if (tp.instrument) tp.instrument->process(nullptr, nullptr, busL(t), busR(t), (int)n, tr);
        for (auto& fx : tp.effects) {
            std::copy_n(busL(t), n, inL);
            std::copy_n(busR(t), n, inR);
            std::fill_n(busL(t), n, 0.0f);
            std::fill_n(busR(t), n, 0.0f);
            fx->process(inL, inR, busL(t), busR(t), (int)n, tr);
        }
    }
}

// ======================================================
// =================== VST3 PLUGINS ======================
// ======================================================

bool AudioEngine::setInstrument(int track, const std::string& name, std::string& status) {
    if (track < 0 || track >= MAX_TRACKS) return false;
    std::unique_ptr<vst3::Plugin> plugin;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        auto& cur = plugins[track].instrument;
        if (name.empty() ? !cur : (cur && cur->query() == name)) return true;   // nothing to do
    }
    if (!name.empty()) {
        std::string error;
        plugin = vst3::Plugin::load(name, true, sampleRate, (int)MAX_BLOCK, error);
        if (!plugin) { status = error; return false; }
        plugin->setTuning(plugins[track].tuning);
        status = "Track " + std::to_string(track + 1) + ": " + plugin->name();
    }
    {
        std::lock_guard<std::mutex> lock(processMutex);
        std::swap(plugins[track].instrument, plugin);
    }
    return true;   // the old one (if any) goes here, outside the lock
}

bool AudioEngine::setEffects(int track, const std::vector<std::string>& names, std::string& status) {
    if (track < 0 || track >= MAX_TRACKS) return false;
    std::vector<std::unique_ptr<vst3::Plugin>> old;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        old = std::move(plugins[track].effects);
        plugins[track].effects.clear();
    }
    std::vector<std::unique_ptr<vst3::Plugin>> chain;
    bool ok = true;
    for (const std::string& name : names) {
        auto same = std::find_if(old.begin(), old.end(),
                                 [&](const auto& p) { return p && p->query() == name; });
        if (same != old.end()) { chain.push_back(std::move(*same)); continue; }
        std::string error;
        auto p = vst3::Plugin::load(name, false, sampleRate, (int)MAX_BLOCK, error);
        if (p) chain.push_back(std::move(p));
        else { status = error; ok = false; }
    }
    if (ok) status = "Track " + std::to_string(track + 1) + ": " + std::to_string(chain.size()) + " effect(s)";
    {
        std::lock_guard<std::mutex> lock(processMutex);
        plugins[track].effects = std::move(chain);
    }
    return ok;   // effects no longer in the chain are freed here
}

void AudioEngine::clearPlugins(int track) {
    if (track < 0 || track >= MAX_TRACKS) return;
    TrackPlugins gone;
    {
        std::lock_guard<std::mutex> lock(processMutex);
        std::swap(gone.instrument, plugins[track].instrument);
        std::swap(gone.effects, plugins[track].effects);
        plugins[track].tuning = vst3::TuningMode::PerNote;
    }
}

void AudioEngine::swapTrackPlugins(int a, int b) {
    if (a == b || a < 0 || b < 0 || a >= MAX_TRACKS || b >= MAX_TRACKS) return;
    std::lock_guard<std::mutex> lock(processMutex);
    std::swap(plugins[a], plugins[b]);
}

void AudioEngine::setTuning(int track, vst3::TuningMode m) {
    if (track < 0 || track >= MAX_TRACKS) return;
    std::lock_guard<std::mutex> lock(processMutex);
    plugins[track].tuning = m;
    if (plugins[track].instrument) plugins[track].instrument->setTuning(m);
}

vst3::Plugin* AudioEngine::instrument(int track) {
    return track >= 0 && track < MAX_TRACKS ? plugins[track].instrument.get() : nullptr;
}

vst3::Plugin* AudioEngine::plugin(int track, int which) {
    if (track < 0 || track >= MAX_TRACKS) return nullptr;
    TrackPlugins& tp = plugins[track];
    if (which == AUTO_INSTRUMENT) return tp.instrument.get();
    return which >= 1 && which <= (int)tp.effects.size() ? tp.effects[which - 1].get() : nullptr;
}

void AudioEngine::syncPluginControllers() {
    for (TrackPlugins& tp : plugins) {
        if (tp.instrument) tp.instrument->syncController();
        for (auto& fx : tp.effects) fx->syncController();
    }
}

std::vector<vst3::Plugin*> AudioEngine::effects(int track) {
    std::vector<vst3::Plugin*> out;
    if (track >= 0 && track < MAX_TRACKS)
        for (auto& p : plugins[track].effects) out.push_back(p.get());
    return out;
}

bool AudioEngine::loadPluginState(vst3::Plugin* p, const std::vector<uint8_t>& state) {
    if (!p || state.empty()) return false;
    std::lock_guard<std::mutex> lock(processMutex);
    return p->loadState(state);
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
    exporting = true;
    auto eachPlugin = [this](auto fn) {
        for (TrackPlugins& tp : plugins) {
            if (tp.instrument) fn(*tp.instrument);
            for (auto& fx : tp.effects) fn(*fx);
        }
    };
    eachPlugin([](vst3::Plugin& p) { p.setOffline(true); });

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
    exporting = false;
    eachPlugin([](vst3::Plugin& p) { p.setOffline(false); });

    sf_close(snd);
    return true;
}
