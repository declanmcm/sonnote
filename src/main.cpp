#include <SDL.h>
#include "vulkan_renderer.hpp"
#include <cstdio>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cmath>
#include <numeric>
#include <cstring>
#include <string>
#include <map>
#include <deque>
#include <memory>
#include <iostream>
#include <sndfile.h>
#include <fftw3.h>
#include "parser.hpp"
#include "midi.hpp"
#include "audio_engine.hpp"
#include "utils.hpp"
#include "ui.hpp"

#if defined(__APPLE__)
  #define GL_SILENCE_DEPRECATION
  #include <OpenGL/gl3.h>
#else
  #include <GL/gl.h>
  #include <GL/glu.h>
#endif

double b;
double t;
bool empty = true;
float inputFreq = 0.0f;
float ref = 440.0f;

enum class TrackType : int32_t { Standard = 0, Audio = 1, Perc = 2 };

struct Track {
    std::string name;
    TrackType   type  = TrackType::Standard;
    ui::Color   color;
    bool        mute  = false;
    bool        solo  = false;
    bool        armed = false;   // record enabled (audio tracks only)
    float       gainDb = 0.0f;   // sent to the Pd patch as [r $0-gain]; PdHost::MIN_DB = silent
    std::string pdPatch;         // resolved against PdHost::patchDir; "" = no patch
    Profile     profile;         // built-in synth settings (perc tracks use freq/hold)
};

// Every track, in panel order. Notes and perc blips refer to their track by
// index (Event::profile), which is also the track's Pd slot.
std::vector<Track> g_tracks;
int g_currentTrack = 0;

static const ui::Color TRACK_PALETTE[] = {
    {0.47f, 0.47f, 1.00f},   // blue (the old note colour)
    {0.35f, 0.80f, 0.45f},   // green
    {0.95f, 0.45f, 0.60f},   // pink
    {0.30f, 0.80f, 0.85f},   // cyan
    {0.75f, 0.50f, 0.95f},   // purple
    {0.90f, 0.85f, 0.35f},   // yellow
    {0.55f, 0.75f, 0.30f},   // olive
    {0.95f, 0.55f, 0.90f},   // magenta
    {0.60f, 0.65f, 0.75f},   // slate
    {0.40f, 0.60f, 0.90f},   // steel blue
};
constexpr int TRACK_PALETTE_SIZE = sizeof(TRACK_PALETTE) / sizeof(TRACK_PALETTE[0]);

static ui::Color trackColor(int i) {
    if (i >= 0 && i < (int)g_tracks.size()) return g_tracks[i].color;
    return TRACK_PALETTE[0];
}

static TrackType currentTrackType() {
    if (g_currentTrack >= 0 && g_currentTrack < (int)g_tracks.size())
        return g_tracks[g_currentTrack].type;
    return TrackType::Standard;
}

// Indices of all tracks of one type, in panel order.
static std::vector<int> tracksOfType(TrackType type) {
    std::vector<int> out;
    for (int i = 0; i < (int)g_tracks.size(); i++)
        if (g_tracks[i].type == type) out.push_back(i);
    return out;
}

// Track that new notes go on: the current track if it's a standard one,
// otherwise the first standard track. -1 if there are none.
static int noteTargetTrack() {
    if (currentTrackType() == TrackType::Standard) return g_currentTrack;
    std::vector<int> standard = tracksOfType(TrackType::Standard);
    return standard.empty() ? -1 : standard[0];
}

// Hotkeys 1-9 and 0 target the first ten tracks. Returns -1 for other keys.
static int hotkeyTrack(char key) {
    if (key >= '1' && key <= '9') return key - '1';
    if (key == '0') return 9;
    return -1;
}

static const char* trackTypeName(TrackType t) {
    switch (t) {
        case TrackType::Audio: return "Audio";
        case TrackType::Perc:  return "Perc";
        default:               return "Standard";
    }
}

// A fresh track. `index` picks its colour; `ofType` (how many tracks of this
// type already exist) numbers its name.
static Track makeTrack(TrackType type, int index, int ofType) {
    Track t;
    t.type  = type;
    t.name  = std::string(trackTypeName(type)) + " " + std::to_string(ofType + 1);
    t.color = TRACK_PALETTE[index % TRACK_PALETTE_SIZE];

    OSCSettings osc = {WaveformType::Sine, 0.2f, 0.2f, 0.7f, 0.025f, 0.16f};
    for (int j = 0; j < 3; j++) t.profile.settings[j] = osc;
    t.profile.settings[1].gain = 0.0f;
    t.profile.settings[2].gain = 0.0f;
    return t;
}

// Default Pd patch names for perc tracks: "perc-1", "perc-2", ... Each perc
// track gets its own file, so each has its own sound and gain.
static std::string percPatchName(int n) { return "perc-" + std::to_string(n); }

// Lowest-numbered default perc patch that no track uses yet.
static std::string freePercPatchName(const std::vector<Track>& tracks) {
    for (int n = 1;; n++) {
        std::string name = percPatchName(n);
        bool used = std::any_of(tracks.begin(), tracks.end(),
                                [&](const Track& t) { return t.pdPatch == name; });
        if (!used) return name;
    }
}

// Starter sound for a perc patch name: "perc-1" is a kick, "perc-2" a snare, ...
static int percVariant(const std::string& name) {
    if (name.rfind("perc-", 0) == 0) return std::max(0, std::atoi(name.c_str() + 5) - 1);
    return 0;
}

const uint16_t audioBufferSize = 4096;
float audioIn[audioBufferSize];
float pendingAudioOut[audioBufferSize];
float audioOut[audioBufferSize];
int audioOuti = 0;
int audioi = 0;
bool newAudioIn = false;
bool newAudioOut = false;

struct ProfileDisplaySettings {

};

float c4 = 220.0f * std::pow(2.0f, 3.0/12.0);

// Map octave -> frequency (C4 at octave 4)
static inline float octave_to_freq(float octave) {
    return c4 * std::pow(2.0f, octave - 4.0f);
}

static inline float freq_to_octave(float freq) {
  return std::log2(freq / c4) + 4.0f;
}

static inline float ratio_to_freq(Ratio ratio) {
  return ratio.eval() * ref;
}

static inline float ratio_to_octave(Ratio ratio) {
  return freq_to_octave(ratio_to_freq(ratio));
}

struct Sequence {
  int first;
  uint32_t ref;
  std::vector<uint32_t> seq;
};

std::vector<int> extractHRange(char* in) {
  bool noVal = true;
  int a, b;
  bool dash = false;
  std::vector<int> newRange;
  while (in[0] != '\0') {
    if (dash) {
      b = getInt(in, noVal);
      if (noVal)
        break;
      int i = a;
      while (i <= b) {
        newRange.push_back(std::pow(2, i)); i++;
      }
    } else {
      a = getInt(in, noVal);
      if (noVal)
        break;
    }
    if (in[0] == '-')
      dash = true;
    else if (in[0] == ',')
      dash = false;
    else
      break;
    in++;
  }
  return newRange;
}

bool isPow2(uint32_t n) {
  int count = 0;
  for (int i = 0; i < 32; i++) {
    if ((n & (1 << i)) == (uint32_t)(1 << i))
      count++;
  }
  return count == 1;
}

std::vector<uint32_t> extractARange(char* in) {
  bool noVal = true;
  int a, b;
  a = -1;
  bool dash = false;
  std::vector<uint32_t> newRange;
  while (in[0] != '\0') {
    if (dash) {
      b = getInt(in, noVal);
      uint32_t i = a;
      while (i <= (uint32_t)b) {
        if (!isPow2(i))
          newRange.push_back(i);
        i++;
      }
      a = -1;
    } else {
      a = getInt(in, noVal);
      if (noVal)
        break;
    }
    if (in[0] == '-')
      dash = true;
    else if (in[0] == ',') {
      if (!isPow2((uint32_t)a))
        newRange.push_back((uint32_t)a);
      a = -1;
      dash = false;
    } else {
      if (a != -1 && !isPow2(a))
        newRange.push_back((uint32_t)a);
      break;
    }
    in++;
  }
  return newRange;
}

// Compute greatest common divisor
int gcd(int a, int b) {
    return b == 0 ? std::abs(a) : gcd(b, a % b);
}

bool isPrime(int n) {
  if (n != 2 && n % 2 == 0) {
    return false;
  } else if (n != 3 && n % 3 == 0) {
    return false;
  } else {
    int i = n / 2;
    while (i > 1) {
      if (n % i == 0)
        return false;
      i--;
    }
  }
  return true;
}

std::vector<uint32_t> getPrimeFactors(int n) {
  int current = n;
  int i = 2;
  std::vector<uint32_t> res;
  while (current != 1) {
    if (isPrime(i)) {
      uint32_t count = 0;
      while (current % i == 0) {
        current = current / i;
        count++;
      }
      res.push_back(count);
    }
    i++;
  }
  return res;
}


uint32_t nextPrime(uint32_t n) {
  while (!isPrime(n)) n++;
  return n;
}

std::vector<uint32_t> getNumbersAtComplexity(uint32_t n, std::vector<std::vector<Sequence>> seqs, std::vector<uint32_t> excl) {
  std::vector<Sequence> current;
  std::vector<uint32_t> nums;
  nums.push_back(n);
  uint32_t p = 2;
  while (p < ((float)n / 2.0)) {
    uint16_t index = n - p - 4;
    uint32_t num = p;
    for (int i = 0; i < (int)seqs[index].size(); i++) {
      if (seqs[index][i].first != -1) {
        std::vector<uint32_t> newExcl;
        newExcl.reserve(seqs[index][i].seq.size() + excl.size());
        newExcl.insert(seqs[index][i].seq.end(), seqs[index][i].seq.begin(), seqs[index][i].seq.end());
        newExcl.insert(newExcl.end(), excl.begin(), excl.end());
        std::vector<uint32_t> minusPNumbers = getNumbersAtComplexity(seqs[index][i].ref, seqs, newExcl);
        for (int j = 0; j < (int)minusPNumbers.size(); j++) {
          nums.push_back(p * seqs[index][i].first * minusPNumbers[j]);
        }
      }
    }
    p = nextPrime(p);
  }
  seqs.push_back(current);
  return nums;
}

void computeComplexityOrder(int n) {
  std::vector<std::vector<uint32_t>> map;
  for (int i = 0; i < n; i++) {
    // Calculate all equivalently complex integers to i
    std::vector<uint32_t> toAdd;
    for (int j = 2; j < i; j++) {

    }
    for (int j = 1; j < i; j++) {
      // Compute possible equivalently complex intervals for j, i - j

    }
  }
}

// Convert float to simplified fraction
void floatToFraction(float value, int& num, int& den, int maxDen = 10000) {
    // Handle sign
    bool negative = value < 0;
    value = std::fabs(value);

    // Integer part
    int intPart = static_cast<int>(value);
    float fracPart = value - intPart;

    if (fracPart == 0.0f) {
        num = intPart;
        den = 1;
        if (negative) num = -num;
        return;
    }

    // Continued fraction algorithm
    float x = fracPart;
    int prevNum = 0, prevDen = 1;
    num = 1; den = 0;

    while (true) {
        int a = static_cast<int>(x);
        int tempNum = num, tempDen = den;
        num = a * num + prevNum;
        den = a * den + prevDen;

        if (den > maxDen) break;

        float approx = static_cast<float>(num) / den;
        if (std::fabs(approx - fracPart) < 1e-6f) break;

        prevNum = tempNum;
        prevDen = tempDen;
        x = 1.0f / (x - a);
    }

    num = intPart * den + num;
    if (negative) num = -num;

    // Simplify
    int g = gcd(num, den);
    num /= g;
   den /= g;
}

void readJump(char* jump, int& num, int& den, float& val) {
  int i = 0;
  int n = 0;
  while (jump[i] != '/' && jump[i] != '.') {
    n *= 10;
    n += jump[i] - 48;
    i++;
  }
  if (jump[i] == '.') {
    i++;
    float dec = 0.0f;
    int place = -1;
    while (jump[i] != '\0') {
      dec += std::pow(10, place) * (jump[i] - 48);
      i ++;
      place--;
    }
    val = n + dec;
    return;
  } else {
    num = n;
    n = 0;
    i++;
    while (jump[i] != '\0') {
      n *= 10;
      n += jump[i] - 48;
      i++;
    }
    den = n;
  }
}

Event* findNote(Event* current, Ratio value) {
  if (value.den == 99999999 && value.num == 99999999)
    return nullptr;
  while (current) {
    if (subtractR(current->ratio, value).num == 0)
      return current;
    current = next_type(current, EV_NOTE_START);
  }
  return nullptr;
}

Event* next(Event* ev) {
  Event* current = NULL;
  if (ev->right != NULL) {
    current = ev->right;
    while (current && current->left)
      current = current->left;
  } else if (ev->parent && ev->parent->left == ev) {
    current = ev->parent;
  } else if (ev->parent && ev->parent->right == ev) {
    current = ev->parent;
    while (current->parent && current->parent->right == current) {
      current = current->parent;
    }
    if (!current->parent)
      current = nullptr;
    else
      current = current->parent;
  }
  std::fflush(stdout);
  return current;
}

Event* prev(Event* ev) {
  Event* current = NULL;
  if (ev->left != NULL) {
    current = ev->left;
    while (current && current->right)
      current = current->right;
  } else if (ev->parent && ev->parent->right == ev) {
    current = ev->parent;
  } else if (ev->parent && ev->parent->left == ev) {
    current = ev->parent;
    while (current->parent && current->parent->left == current) {
      current = current->parent;
    }
    if (!current->parent)
      current = nullptr;
    else
      current = current->parent;
  }
  return current;
}

Event* next_note(Event* ev) {
  if (ev)
    ev = next(ev);
  while (ev && !(ev->type == EV_NOTE_START || ev->type == EV_NOTE_END))
    ev = next(ev);
  return ev;
}

Event* next_type(Event* ev, int type) {
  if (ev)
    ev = next(ev);
  while (ev && ev->type != type)
    ev = next(ev);
  return ev;
}

Event* prev_type(Event* ev, int type) {
  if (ev)
    ev = prev(ev);
  while (ev && ev->type != type)
    ev = prev(ev);
  return ev;
}

Event* findBeatEv(Event* root, float v) {
  Event* current = root;
  while (current && current->left)   // start from the first event, not the root
    current = current->left;
  if (current && current->type != EV_BEAT)
    current = next_type(current, EV_BEAT);
  while (current) {
    if (std::abs(current->beat - v) < 1e-6)
      return current;
    current = next_type(current, EV_BEAT);
  }
  return nullptr;
}

float integrate(ArithTree function, float t1, float t2) {
    (void)function; (void)t1; (void)t2;
    // TODO: Implement step integration here
    return 0.0f;
}


float integrate_until(ArithTree function, float t1, float y) {
    (void)function; (void)t1; (void)y;
    // TODO: Integrate until equal to y and then return t2
    return 0.0f;
}

float calc_beat(EventTree* times, float noteTime) {
  Event* currentTempoEv = times->get_first();
  if (currentTempoEv->type != EV_TEMPO)
    currentTempoEv = next_type(currentTempoEv, EV_TEMPO);
  float beats = 0.0f;
  while (currentTempoEv) {
    if (next_type(currentTempoEv, EV_TEMPO) && next_type(currentTempoEv, EV_TEMPO)->time < noteTime) {
        if (currentTempoEv->isConstant()) {
          beats += (next_type(currentTempoEv, EV_TEMPO)->time - currentTempoEv->time) * (currentTempoEv->function.evaluate(b, t, 0, 0) / 60);
        } else {
          beats += integrate(currentTempoEv->function, currentTempoEv->time, next_type(currentTempoEv, EV_TEMPO)->time);
        }
        currentTempoEv = next_type(currentTempoEv, EV_TEMPO);
    } else {
      if (currentTempoEv->isConstant()) {
        float thing = beats + (noteTime - currentTempoEv->time) * (currentTempoEv->function.evaluate(b, t, 0, 0) / 60);
        return thing;
      } else {
        return beats + integrate(currentTempoEv->function, currentTempoEv->time, noteTime);
      }
    }
  }
  return beats;
}

float calc_time(EventTree* times, float noteBeat) {
    Event* currentTempoEv = times->get_first();
    if (currentTempoEv->type != EV_TEMPO)
        currentTempoEv = next_type(currentTempoEv, EV_TEMPO);
    float beats = 0.0f;
    while (currentTempoEv) {
        float currentTempoEvBeats = beats; // We can do this because there will always be a first tempoEv at zero
        if (next_type(currentTempoEv, EV_TEMPO)) {
            if (currentTempoEv->isConstant()) {
                beats += (next_type(currentTempoEv, EV_TEMPO)->time - currentTempoEv->time) * (currentTempoEv->function.evaluate(b, t, 0, 0) / 60);
            } else {
                beats += integrate(currentTempoEv->function, currentTempoEv->time, next_type(currentTempoEv, EV_TEMPO)->time);
            }
        }
        if (!next_type(currentTempoEv, EV_TEMPO) || beats > noteBeat) {
            if (currentTempoEv->isConstant()) {
                return (noteBeat - currentTempoEvBeats) / (currentTempoEv->function.evaluate(b, t, 0, 0) / 60.0) + currentTempoEv->time;
            } else {
                return integrate_until(currentTempoEv->function, currentTempoEv->time, noteBeat - currentTempoEvBeats) + currentTempoEv->time;
            }
        }
        currentTempoEv = next_type(currentTempoEv, EV_TEMPO);
    }
    return -1.0f;
}

void calc_times(EventTree* times, EventTree* beats) {
    Event* currentTempoEv = times->root;
    Event* currentBeatEv = beats->root;
    while (currentTempoEv->left)
        currentTempoEv = currentTempoEv->left;
    while (currentBeatEv->left)
        currentBeatEv = currentBeatEv->left;
    currentTempoEv = next_type(currentTempoEv, EV_TEMPO);
    float beatAccum = 0.0f;
    while (currentTempoEv) {
        float currentTempoEvBeats = beatAccum;
        if (currentTempoEv->isConstant()) {
            beatAccum += (next_type(currentTempoEv, EV_TEMPO)->time - currentTempoEv->time) * currentTempoEv->function.evaluate(b, t, 0, 0);
        } else {
            beatAccum += integrate(currentTempoEv->function, currentTempoEv->time, next_type(currentTempoEv, EV_TEMPO)->time);
        }
        while (currentBeatEv) {
            if (beatAccum > currentBeatEv->beat) {
                if (currentTempoEv->isConstant()) {
                    currentBeatEv->time = (currentBeatEv->beat - currentTempoEvBeats) / currentTempoEv->function.evaluate(b, t, 0, 0);
                } else {
                    currentBeatEv->time = integrate_until(currentTempoEv->function, currentTempoEv->time, currentBeatEv->beat - currentTempoEvBeats);
                }
            } else {
                break;
            }
            currentBeatEv = next(currentBeatEv);
        }

    }
}

struct MidiEvent {
    double tick;
    std::vector<uint8_t> data;
};

/*struct Rect {
    float x;
    float y;
    float w;
    float h;
    float r;
    float g;
    float b;
    float utility;
    int function;
};*/

struct Button {
    float x1;
    float y1;
    float x2;
    float y2;
    char* label;
    bool hit;
    float value;
    char* text;
};

struct CurrentBox {
    float x1;
    float y1;
    float x2;
    float y2;
};

#define F_MARGIN_PANEL 0
#define F_BORDER 1
#define F_NOTE_WINDOW 2
#define F_CONTROL_WINDOW 3
#define F_BUTTON_CW 4
#define F_SLIDER_CW 5
#define F_TEXT_BOX_CW 6
#define F_TEXT_BOXL_CW 7
#define F_SLIDER_P_CW 8

#define NW_HEIGHT 0.75f
#define CW_HEIGHT 0.1f
#define SW_HEIGHT 0.2f

constexpr int FFT_SIZE = 2048;
constexpr int HOP_SIZE = 512;
constexpr int NUM_BINS = FFT_SIZE / 2 + 1;

TextRenderer g_textRenderer;
VulkanDeviceContext g_deviceContext; // You'd need to populate this
VkRenderPass g_renderPass;           // Your main render pass

struct AudioData {
    int sampleRate = 0;
    std::vector<float> mono;   // [-1,1] float samples
};

inline AudioData loadWavMono(const std::string& path)
{
    SF_INFO info{};
    SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
    if (!f) throw std::runtime_error("sf_open failed");

    std::vector<float> interleaved((size_t)info.frames * (size_t)info.channels);
    sf_readf_float(f, interleaved.data(), info.frames);
    sf_close(f);

    AudioData out;
    out.sampleRate = info.samplerate;
    out.mono.resize((size_t)info.frames);

    if (info.channels == 1) {
        out.mono = std::move(interleaved);
    } else {
        for (sf_count_t i = 0; i < info.frames; ++i) {
            double s = 0.0;
            for (int ch = 0; ch < info.channels; ++ch)
                s += interleaved[(size_t)i * info.channels + ch];
            out.mono[(size_t)i] = (float)(s / info.channels);
        }
    }
    return out;
}

struct SpectrogramState {
    bool loaded = false;
    float durationSeconds = 0.0f;
    int textureWidth = 0;   // columns
};

SpectrogramState spectrogram;

struct STFTMag {
    int sampleRate = 0;
    int fftSize = 0;
    int hopSize = 0;
    int numBins = 0;           // fftSize/2+1
    int numFrames = 0;
    std::vector<float> mag;    // flattened: mag[frame*numBins + bin]
    float highBin;
};

inline std::vector<float> makeHann(int n)
{
    std::vector<float> w(n);
    for (int i = 0; i < n; ++i)
        w[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(n - 1)));
    return w;
}

inline void computeSTFT_FFTWn(
    const std::vector<float>& samples,
    float* outSamples,
    int sampleRate,
    int fftSize = 2048,
    int hopSize = 512
) {
  //printf("samples.size(): %d, fftSize: %d\n", (int)samples.size(), fftSize);
    if ((int)samples.size() < fftSize) throw std::runtime_error("audio too short for fftSize");

    STFTMag out;
    out.sampleRate = sampleRate;
    out.fftSize = fftSize;
    out.hopSize = hopSize;
    out.numBins = fftSize / 2 + 1;
    out.numFrames = ((int)samples.size() - fftSize) / hopSize + 1;
    out.mag.assign((size_t)out.numFrames * (size_t)out.numBins, 0.0f);

    auto windowFFT = makeHann(fftSize);

    std::vector<float> in((size_t)fftSize);
    std::vector<fftwf_complex> spec((size_t)out.numBins);
    std::vector<float> output((size_t)fftSize);
    std::vector<fftwf_complex> specExp((size_t)out.numBins);
    /*for (int i = 0; i < out.numBins; i++) {
      specExp[(size_t)i][0] = 0.0f;
      specExp[(size_t)i][1] = 0.0f;
    }*/

    fftwf_plan plan = fftwf_plan_dft_r2c_1d(
        fftSize, in.data(), spec.data(), FFTW_MEASURE
    );

    fftwf_plan reversePlan = fftwf_plan_dft_c2r_1d(
        fftSize, spec.data(), output.data(), FFTW_MEASURE
    );

    //printf("numbins: %d, numFrames: %d\n", out.numBins, out.numFrames);
    float resolution = sampleRate / audioBufferSize;
    for (int frame = 0; frame < out.numFrames; ++frame) {
        int offset = frame * hopSize;

        for (int i = 0; i < fftSize; ++i) {
          in[(size_t)i] = samples[(size_t)offset + (size_t)i] * windowFFT[(size_t)i];
          //printf("%f\n", in[(size_t)i]);
        }

        fftwf_execute(plan);

        for (int b = 0; b < out.numBins; ++b) {
            float freq = ((float)sampleRate * (float)b) / (float)audioBufferSize;
            float newFreqL = std::exp(freq);
            float newFreqR = std::exp(freq + resolution);
            float binL = newFreqL / resolution;
            float binR = newFreqR / resolution;
            int binLFloor = std::floor(binL);
            float fractionalL = binL - binLFloor;
            int binRCeil = std::ceil(binR);
            float fractionalR = binRCeil - binR;
            int max = binRCeil;
            if (max > out.numBins) max = out.numBins - 1;
            if (binLFloor > out.numBins) break;
            printf("numbins: %d\n", out.numBins);
            for (int nb = binLFloor; nb <= max; nb++) {
              if (nb == binLFloor) {
                specExp[(size_t)nb][0] += (1 - fractionalL) * spec[(size_t)b][0];
                specExp[(size_t)nb][1] += (1 - fractionalL) * spec[(size_t)b][1];
                specExp[(size_t)nb][0] /= 2;
                specExp[(size_t)nb][1] /= 2;
              } else if (nb == binLFloor + 1) {
                specExp[(size_t)nb][0] += fractionalL * spec[(size_t)b][0];
                specExp[(size_t)nb][1] += fractionalL * spec[(size_t)b][1];
                specExp[(size_t)nb][0] /= 2;
                specExp[(size_t)nb][1] /= 2;
              } else if (nb == binRCeil - 1) {
                specExp[(size_t)nb][0] += (1 - fractionalR) * spec[(size_t)b][0];
                specExp[(size_t)nb][1] += (1 - fractionalR) * spec[(size_t)b][1];
              } else if (nb == binRCeil)  {
                specExp[(size_t)nb][0] += (1 - fractionalR) * spec[(size_t)b][0];
                specExp[(size_t)nb][1] += (1 - fractionalR) * spec[(size_t)b][1];
              } else {
                specExp[(size_t)nb][0] = spec[(size_t)b][0];
                specExp[(size_t)nb][1] = spec[(size_t)b][1];
              }
            }
           // printf("frame, bin: %d, %d, dst: %f ", frame, b, dst[(size_t)b]);
        }
        fftwf_execute(reversePlan);

        for (int i = 0; i < fftSize; ++i) {
          outSamples[(size_t)offset + i] = output[(size_t)i] / windowFFT[(size_t)i];
        }

    }


    fftwf_destroy_plan(plan);
    fftwf_destroy_plan(reversePlan);
    return;
}

inline STFTMag computeSTFT_FFTW(
    const std::vector<float>& samples,
    int sampleRate,
    int fftSize = 2048,
    int hopSize = 512
) {
  //printf("samples.size(): %d, fftSize: %d\n", (int)samples.size(), fftSize);
    if ((int)samples.size() < fftSize) throw std::runtime_error("audio too short for fftSize");

    STFTMag out;
    out.sampleRate = sampleRate;
    out.fftSize = fftSize;
    out.hopSize = hopSize;
    out.numBins = fftSize / 2 + 1;
    out.numFrames = ((int)samples.size() - fftSize) / hopSize + 1;
    out.mag.assign((size_t)out.numFrames * (size_t)out.numBins, 0.0f);

    auto windowFFT = makeHann(fftSize);

    std::vector<float> in((size_t)fftSize);
    std::vector<fftwf_complex> spec((size_t)out.numBins);

    fftwf_plan plan = fftwf_plan_dft_r2c_1d(
        fftSize, in.data(), spec.data(), FFTW_MEASURE
    );

    //printf("numbins: %d, numFrames: %d\n", out.numBins, out.numFrames);
    float maxBin = 0.0f;
    float maxVal = -9999999.0f;
    bool search = true;
    bool up = false;
    for (int frame = 0; frame < out.numFrames; ++frame) {
        int offset = frame * hopSize;

        for (int i = 0; i < fftSize; ++i) {
          in[(size_t)i] = samples[(size_t)offset + (size_t)i] * windowFFT[(size_t)i];
          //printf("%f\n", in[(size_t)i]);
        }

        fftwf_execute(plan);

        float* dst = out.mag.data() + (size_t)frame * (size_t)out.numBins;
        for (int b = 0; b < out.numBins; ++b) {
            float re = spec[(size_t)b][0];
            float im = spec[(size_t)b][1];
            dst[(size_t)b] = sqrtf(re * re + im * im);
            //printf("frame, bin: %d, %d, dst: %f ", frame, b, dst[(size_t)b]);
            if (search && dst[(size_t)b] > maxVal) {
              //printf("newTopBin: %f\n", ((float)sampleRate * (float)b) / (float)audioBufferSize);
              //printf("newTopBin: %d\n", b);
              maxBin = (float)b;
              maxVal = dst[(size_t)b];
              up = true;
            } else {
              if (up) {
                maxBin += -1 * (dst[(int)maxBin - 1] / dst[(int)maxBin]) / 2 + (dst[(int)maxBin + 1] / dst[(int)maxBin]) / 2;
                //printf("newPeak: %f\n", maxBin);
                /*
                if (std::abs(lastPeak) >= 1e-6) {
                  if (std::abs((maxBin / 2) - lastPeak) < 2) {
                    maxBin = lastPeak;
                    search = false;
                  }
                }
                if (maxBin > (fftSize * 50) / (float)sampleRate)
                  lastPeak = maxBin;*/
                up = false;
              }
            }
        }
    }
    out.highBin = maxBin;

    fftwf_destroy_plan(plan);
    return out;
}

inline void magToDbInPlace(STFTMag& stft, float floorDb = -80.0f)
{
    for (float& v : stft.mag) {
        float db = 20.0f * log10f(v + 1e-9f);
        if (db < floorDb) db = floorDb;
        v = db;
    }
}

inline void normalize01InPlace(std::vector<float>& a)
{
    float mn = 1e30f, mx = -1e30f;
    for (float v : a) { mn = std::min(mn, v); mx = std::max(mx, v); }
    float range = (mx - mn);
    if (range < 1e-12f) range = 1.0f;
    for (float& v : a) v = (v - mn) / range;
}

struct LogSpec {
    int width = 0;   // time columns (frames or downsampled)
    int height = 0;  // log rows
    std::vector<float> v; // [y*width + x] in [0,1]
};

// helper
inline float binHz(int bin, int sampleRate, int fftSize)
{
    return (float)bin * (float)sampleRate / (float)fftSize;
}

inline LogSpec remapToLogFreq(
    const STFTMag& stft,
    int rows = 512,
    float fMin = 20.0f,
    float fMax = 20000.0f
) {
    LogSpec out;
    out.width  = stft.numFrames;
    out.height = rows;
    out.v.assign((size_t)out.width * (size_t)out.height, 0.0f);

    // Precompute bin edges for each log row:
    // row r covers [f0,f1) where f is exponentially spaced.
    std::vector<int> binLo((size_t)rows), binHi((size_t)rows);

    const float logMin = logf(fMin);
    const float logMax = logf(fMax);

    for (int r = 0; r < rows; ++r) {
        float t0 = (float)r / (float)rows;
        float t1 = (float)(r + 1) / (float)rows;
        float f0 = expf(logMin + (logMax - logMin) * t0);
        float f1 = expf(logMin + (logMax - logMin) * t1);

        int lo = (int)floorf(f0 * stft.fftSize / stft.sampleRate);
        int hi = (int)ceilf (f1 * stft.fftSize / stft.sampleRate);

        lo = std::max(lo, 0);
        hi = std::min(hi, stft.numBins - 1);
        if (hi <= lo) hi = std::min(lo + 1, stft.numBins - 1);

        binLo[(size_t)r] = lo;
        binHi[(size_t)r] = hi;
    }

    // Average magnitude in each band
    for (int x = 0; x < stft.numFrames; ++x) {
        const float* frame = stft.mag.data() + (size_t)x * (size_t)stft.numBins;

        for (int r = 0; r < rows; ++r) {
            int lo = binLo[(size_t)r];
            int hi = binHi[(size_t)r];

            double sum = 0.0;
            int count = 0;
            for (int b = lo; b <= hi; ++b) { sum += frame[(size_t)b]; count++; }

            // IMPORTANT: If you want low freq at bottom, flip r in storage or shader.
            out.v[(size_t)r * (size_t)out.width + (size_t)x] = (float)(sum / std::max(1, count));
        }
    }

    // normalize to 0..1 (assumes input is already dB-ish)
    normalize01InPlace(out.v);
    return out;
}

inline LogSpec downsampleTimeMax(const LogSpec& in, int targetWidth)
{
    if (targetWidth >= in.width) return in;

    LogSpec out;
    out.width  = targetWidth;
    out.height = in.height;
    out.v.assign((size_t)out.width * (size_t)out.height, 0.0f);

    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < targetWidth; ++x) {
            int x0 = (int)((int64_t)x     * in.width / targetWidth);
            int x1 = (int)((int64_t)(x+1) * in.width / targetWidth);
            if (x1 <= x0) x1 = x0 + 1;

            float m = 0.0f;
            for (int xi = x0; xi < x1; ++xi) {
                m = std::max(m, in.v[(size_t)y * (size_t)in.width + (size_t)xi]);
            }
            out.v[(size_t)y * (size_t)out.width + (size_t)x] = m;
        }
    }
    return out;
}

struct GrayImage {
    int w=0, h=0;
    std::vector<uint8_t> px; // size w*h
};

inline GrayImage toR8(const LogSpec& spec, bool flipY = true)
{
    GrayImage img;
    img.w = spec.width;
    img.h = spec.height;
    img.px.resize((size_t)img.w * (size_t)img.h);

    for (int y = 0; y < img.h; ++y) {
        int sy = flipY ? (img.h - 1 - y) : y; // flip so low freq is bottom if desired
        for (int x = 0; x < img.w; ++x) {
            float v = spec.v[(size_t)sy * (size_t)img.w + (size_t)x];
            int iv = (int)lrintf(v * 255.0f);
            iv = std::min(255, std::max(0, iv));
            img.px[(size_t)y * (size_t)img.w + (size_t)x] = (uint8_t)iv;
        }
    }
    return img;
}

inline GrayImage buildSpectrogramTextureR8(
    const std::string& wavPath,
    int fftSize = 2048,
    int hopSize = 512,
    int logRows = 512,
    int targetWidth = 4096,
    float floorDb = -80.0f,
    float fMin = 20.0f,
    float fMax = 20000.0f
) {
    AudioData a = loadWavMono(wavPath);

    STFTMag stft = computeSTFT_FFTW(a.mono, a.sampleRate, fftSize, hopSize);
    magToDbInPlace(stft, floorDb);

    // note: remapToLogFreq expects stft.mag in “some scale”; dB works best.
    LogSpec logSpec = remapToLogFreq(stft, logRows, fMin, fMax);
    LogSpec ds = downsampleTimeMax(logSpec, targetWidth);

    return toR8(ds, /*flipY=*/true);
}


float perfectIntervals[] = {
    1.0,
    3.0 / 2.0, // Fifth
    4.0 / 3.0, // Fourth
    5.0 / 4.0, // Major third
    6.0 / 5.0, // Minor third
    7.0 / 6.0, // Major second
    8.0 / 7.0,
    9.0 / 8.0,
    16.0 / 15.0, // Diatonic semitone
    7.0 / 5.0, // Tritone
    // Inversions
    8.0 / 5.0,
    10.0 / 6.0,
    12.0 /  7.0,
    7.0 / 4.0,
    16.0 / 9.0,
    15.0 / 8.0,
    10.0 / 7.0
};

Ratio perfectIntervalRatios[] {
  {1, 1},
  {3, 2},
  {5, 4},
  {7, 4},
  {9, 8},
  {11, 8},
  {13, 8},
  {15, 8},
  {17, 16},
  {19, 16},
  {21, 16},
  {23, 16},
  {25, 16},
  {27, 16},
  {29, 16},
  {31, 16},
  {4, 3},
  {5, 3},
  {6, 5},
  {7, 5},
  {8, 5},
  {9, 5},
  {7, 6},
  {9, 6},
  {11, 6},
  {8, 7},
  {9, 7},
  {10, 7},
  {11, 7},
  {12, 7},
  {13, 7}
};

float playhead_time = 0.0f;
float loop_end = -1.0f;
bool play_triggered_from_space = false;
bool tuner_triggered = false;

template<typename Stream>
static void writeVarLen(Stream& f, uint32_t value) {
    uint8_t buf[5];
    int pos = 0;
    buf[pos++] = value & 0x7F;
    while ((value >>= 7))
        buf[pos++] = 0x80 | (value & 0x7F);
    for (int i = pos - 1; i >= 0; --i)
        f.put(buf[i]);
}

// --- Write binary song file ---
bool save_song(const std::string& path,
               EventTree& beatEvents,
               EventTree& timeEvents,
               const std::vector<Track>& tracks)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // Magic header
    f.write("SONN", 4);

    char expr[40];
    float v;
    int t, l, p;
    uint32_t id, numB, denB, numR, denR;
    uint32_t eventCount = 0;

    // Write number of time events followed by time Events in order

    Event* current = timeEvents.get_first();

    while (current) {
      eventCount++;
      current = next(current);
    }

    f.write(reinterpret_cast<const char*>(&eventCount), sizeof(eventCount));

    current = timeEvents.get_first();

    while (current) {
      v = current->beat;
      sprintf(expr, "%s", current->string_expr.c_str());
      l = strlen(expr);
      t = current->type;
      f.write(reinterpret_cast<const char*>(&v), sizeof(float));
      f.write(reinterpret_cast<const char*>(&l), sizeof(int));
      f.write(expr, l);
      f.write(reinterpret_cast<const char*>(&t), sizeof(int));
      printf("write: %f, %d, %d\n", v, l, t);
      current = next(current);
    }

    // Write number of beat events followed by beat Events in order

    current = beatEvents.get_first();
    eventCount = 0;
    while (current) {
      eventCount++;
      current = next(current);
    }

    f.write(reinterpret_cast<const char*>(&eventCount), sizeof(eventCount));

    current = beatEvents.get_first();

    while (current) {
      v = current->beat;
      sprintf(expr, "%s", current->string_expr.c_str());
      //sprintf(expr, "3.0\0");
      l = strlen(expr);
      printf("expr: %s, len: %d\n", expr, l);
      t = current->type;
      p = current->profile;
      id = current->id;
      numB = current->num;
      denB = current->den;
      numR = current->ratio.num;
      denR = current->ratio.den;
      f.write(reinterpret_cast<const char*>(&v), sizeof(float));
      f.write(reinterpret_cast<const char*>(&l), sizeof(int));
      f.write(expr, l);
      f.write(reinterpret_cast<const char*>(&t), sizeof(int));
      f.write(reinterpret_cast<const char*>(&id), sizeof(uint32_t));
      f.write(reinterpret_cast<const char*>(&numR), sizeof(uint32_t));
      f.write(reinterpret_cast<const char*>(&denR), sizeof(uint32_t));
      f.write(reinterpret_cast<const char*>(&numB), sizeof(uint32_t));
      f.write(reinterpret_cast<const char*>(&denB), sizeof(uint32_t));
      f.write(reinterpret_cast<const char*>(&p), sizeof(int));
      current = next(current);
    }

    uint32_t profileCount = (uint32_t)tracks.size();
    f.write(reinterpret_cast<char*>(&profileCount), sizeof(profileCount));
    for (const Track& tr : tracks) {
        for (int j = 0; j < 3; j++) {
            const OSCSettings& osc = tr.profile.settings[j];

            // Write enum as fixed-width integer
            int32_t wave = static_cast<int32_t>(osc.waveType);
            f.write(reinterpret_cast<char*>(&wave), sizeof(wave));

            f.write(reinterpret_cast<const char*>(&osc.attack),  sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.decay),   sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.sustain), sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.release), sizeof(float));
            f.write(reinterpret_cast<const char*>(&osc.gain),    sizeof(float));
        }
    }

    // Pd patch per track. Appended as its own tagged section so older
    // files (without it) still load, and older builds ignore it.
    f.write("PDPT", 4);
    for (const Track& tr : tracks) {
        uint32_t len = (uint32_t)tr.pdPatch.size();
        f.write(reinterpret_cast<const char*>(&len), sizeof(len));
        f.write(tr.pdPatch.data(), len);
    }
    // Gain per track, in dB
    f.write("PDGN", 4);
    for (const Track& tr : tracks)
        f.write(reinterpret_cast<const char*>(&tr.gainDb), sizeof(float));

    // Everything else about each track
    f.write("TRKS", 4);
    for (const Track& tr : tracks) {
        int32_t type = static_cast<int32_t>(tr.type);
        f.write(reinterpret_cast<const char*>(&type), sizeof(type));
        uint32_t len = (uint32_t)tr.name.size();
        f.write(reinterpret_cast<const char*>(&len), sizeof(len));
        f.write(tr.name.data(), len);
        f.write(reinterpret_cast<const char*>(&tr.color.r), sizeof(float));
        f.write(reinterpret_cast<const char*>(&tr.color.g), sizeof(float));
        f.write(reinterpret_cast<const char*>(&tr.color.b), sizeof(float));
        uint8_t flags[3] = {tr.mute, tr.solo, tr.armed};
        f.write(reinterpret_cast<const char*>(flags), sizeof flags);
        for (int j = 0; j < 3; j++) {
            f.write(reinterpret_cast<const char*>(&tr.profile.settings[j].freq), sizeof(float));
            f.write(reinterpret_cast<const char*>(&tr.profile.settings[j].hold), sizeof(float));
        }
    }

    f.close();
    return true;
}

float parseDivisor(char* n) {
    float numerator = 0;
    float denominator = 0;
    int s = 0;
    bool found = true;
    while (n[s] != '/') {
        if (s == (int)strlen(n) - 1) {
            found = false;
            s++;
            break;
        }
        s++;
    }
    int i = s - 1;
    int power = 0;
    while (i >= 0) {
        numerator += ((uint8_t)n[i] - 48) * std::pow(10, power);
        power++;
        i--;
    }
    if (found) {
        power = 0;
        int j = strlen(n) - 1;
        while (j > s) {
            denominator += ((uint8_t)n[j] - 48) * std::pow(10, power);
            j--;
            power++;
        }
        printf("res: %f\n", numerator / denominator);
        std::fflush(stdout);
        return numerator / denominator;
    } else {
        return numerator;
    }
}

char buttonLabels[36][32] = {"Export WAV",
    "BPM:          ",
    "Attack:          ",
    "Decay:          ",
    "Gain:            ",
    "Wav name:",
    "Jump size:",
    "Split size:",
    "Export MIDI",
    "Save project",
    "Load project",
    "Change wave",
    "Sustain:          ",
    "Release:          ",
    "OSC1: Sine",
    "OSC2: Sine",
    "Attack:          ",
    "Decay:          ",
    "Sustain:          ",
    "Release:          ",
    "Gain:              ",
    "OSC3: Sine",
    "Attack:          ",
    "Decay:          ",
    "Sustain:          ",
    "Release:          ",
    "Gain:              ",
    "Ref. Freq.:",
    "Hold:            ",
    "Freq:         ",
    "Hold:            ",
    "Freq:       ",
    "Hold:            ",
    "Freq:       ",
    "Harm. Range: ",
    "Alt. H. Range: "
    };
char texts[9][32] = {"file.wav", "              ", "              ", "440", "30", "30", "30", "2", "3"};
char originText[64];
char measureText[64];

struct EditorSettings {
    std::string wavName      = "file.wav";
    std::string jump;
    std::string split;
    std::string refFreq      = "440";
    std::string harmRange    = "2";
    std::string altHarmRange = "3";
};
EditorSettings g_settings;

struct Canvas2D {
    // World-space units
    // x = beats, y = octaves (increasing upward)
    float x0 = 0.0f;       // world x at left edge
    float y0 = 6.0f;       // world y at TOP edge (e.g., show around 6 octaves)
    float sx = 120.0f;     // pixels per beat
    float sy = 80.0f;      // pixels per octave

    bool  snap_to_grid = false;
    bool snap_harmonics = true;
    bool snap_foci = true;
    float snap_x = 0.125f;  // snap to quarter-beats
    float snap_y = 1.0/12.0;  // snap to semitones

    EventTree timeEvents;
    EventTree beatEvents;

    bool placing = false;
    Vec2 anchorW{0,0};
    Ratio anchorR{0,0};
    Event* anchor = (Event*)malloc(sizeof(Event));
    float anchorBeat = 0.0f;
    char anchorExpr[20];
    std::vector<uint32_t> spareIDs;
    uint32_t nextID = 1;
    float split_start = -1.0f;
    int heldProfile = -1;   // track of a note picked up with f / l, -1 = none

    std::vector<Event*> currentEvsAtCursor;

    Vec2 currently_dragged = {0.0f, 0.0f};
    bool dragging = false;
    Event* originEv = nullptr;

    Vec2 hoverW{0,0};
    Ratio hoverR{1,1};
    Vec2 unsnappedHoverW{0,0};
    float anchorBeatExact = 0.0f;

    Vec2 select1{0,0};
    Vec2 select2{0,0};
    Event* select1closest = nullptr;
    Event* select2closest = nullptr;
    bool selectFirst = true;

    Event* createEvExpr(float v, std::string expr, int t) { return new Event(v, expr, t); }

    Event* createEv(float v, int t) { return new Event(v, t); }

    void setBPM(const char* expr, float beat) {
        // Event toInsert = Event(beat, expr, EV_TEMPO);
        Event* toInsert =  new Event(beat, expr, EV_TEMPO);
        if (toInsert->right) {
          printf("HOW?!?\n");
          std::fflush(stdout);
        }
        printf("val: %f\n", ratio_to_octave(toInsert->ratio));
        timeEvents.insert(toInsert);
        timeEvents.setFinalEv();   // accum_beats needs to know which tempo event is last
    }

    void addCanvasRect(VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight, float rx, float ry, float rw, float rh, float r, float g, float b) {
        float x = rx;
        float y = ry;
        float w = rw;
        float h = rh;

        if (x + w < topLeft.x || x > bottomRight.x || y + h < bottomRight.y || y > topLeft.y) {
            return;
        }

        if (x < topLeft.x) {
            x = topLeft.x;
        }
        if (y < bottomRight.y) {
            y = bottomRight.y;
        }
        if (x + w > bottomRight.x) {
            w = std::abs(x  - bottomRight.x);
        } else if (std::abs(x - rx) > 1e-6) {
            w -= std::abs(x - rx);
        }
        if (y + h > topLeft.y) {
            h = std::abs(topLeft.y - y);
        } else if (std::abs(x - ry) > 1e-6) {
            h -= std::abs(y - ry);
        }
        if (w != 0 && h != 0) {
            renderer.addRect(x, y, w, h, r, g, b);
        }
    }

    void addCanvasPercBlip(VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight, float x, float y, float t, float l, float r, float g, float b) {
      float xp = x;
      float yp = y;

      if (x + l < topLeft.x || x > bottomRight.x || y + (t / 2) < bottomRight.y || y > topLeft.y)
        return;

      renderer.addPercBlip(xp, yp, t, l, r, g, b);
    }

    // Inserts divisions - 1 rhythm splits (EV_BEAT) evenly between beats a
    // and b. Each split stores its position within the beat as num/den, built
    // from the splits (if any) at the two ends, so nested splits get the
    // right notches.
    void insertSplit(float a, float b, int divisions) {
      if (divisions < 2 || std::abs(b - a) < 1e-6) return;
      float start = std::min(a, b), end = std::max(a, b);
      float inc = (end - start) / divisions;
      Event* ev1 = findBeatEv(beatEvents.root, start);   // splits at the ends
      Event* ev2 = findBeatEv(beatEvents.root, end);
      int offset = std::floor(end) - std::floor(start);
      int startNum = 0, startDen = 1, endNum = 0, endDen = 1;
      if (ev1) { startNum = ev1->num; startDen = ev1->den; }
      if (ev2) { endNum = ev2->num; endDen = ev2->den; }
      endNum += endDen * offset;
      int diffDen = std::lcm(startDen, endDen);
      int diffNum = (diffDen / endDen) * endNum - (diffDen / startDen) * startNum;
      // Splitting exactly one whole beat: draw like a plain subdivision
      bool wholeBeat = std::abs(std::floor(end) - end) < 1e-6 && std::abs(std::floor(start) - start) < 1e-6 &&
                       std::abs((end - start) - 1.0f) < 1e-6;

      // Counted, not accumulated, so float error can't add a split at `end`
      for (int n = 1; n < divisions; n++) {
        Event* toInsert = new Event(start + n * inc, EV_BEAT);
        toInsert->id = nextID++;   // unique, so it can be removed on its own
        toInsert->beatL = wholeBeat ? 0 : start;
        toInsert->beatR = wholeBeat ? 0 : end;
        int unsimpNum = diffNum * n;
        int unsimpDen = diffDen * divisions;
        int lcm = std::lcm(startDen, unsimpDen);
        unsimpNum = (lcm / unsimpDen) * unsimpNum + (lcm / startDen) * startNum;
        unsimpDen = lcm;
        while (unsimpNum - unsimpDen > 0)
          unsimpNum -= unsimpDen;
        int g = std::gcd(unsimpNum, unsimpDen);
        toInsert->num = g ? unsimpNum / g : unsimpNum;
        toInsert->den = g ? unsimpDen / g : unsimpDen;
        toInsert->primeFactors = getPrimeFactors(toInsert->den);
        toInsert->time = calc_time(&timeEvents, toInsert->beat);
        beatEvents.insert(toInsert);
      }
    }

    // Drops references to events that were removed from beatEvents.
    void forgetEvents(const std::vector<Event*>& removed) {
      auto gone = [&](Event* e) { return e && std::find(removed.begin(), removed.end(), e) != removed.end(); };
      if (gone(originEv)) originEv = nullptr;
      if (gone(select1closest)) select1closest = nullptr;
      if (gone(select2closest)) select2closest = nullptr;
      for (Event* e = beatEvents.get_first(); e; e = next(e))
        if (gone(e->origin)) e->origin = nullptr;
    }

    // ---- Sub-panels ----
    // The bottom of the note window holds two strips that share the notes'
    // time axis: perc lanes (one per perc track, in track order) and below
    // them audio lanes (one per audio track, where clips will go). Sizes are
    // in pixels; each strip scrolls when it has more lanes than fit.
    enum class Region { None, Notes, Perc, Audio };

    struct SubPanel {
        float top = 0, bottom = 0;     // screen y (y-up)
        float laneH = 0;
        float* scroll = nullptr;       // pixels scrolled past the first lane
        std::vector<int> lanes;        // track index per lane

        float height() const { return top - bottom; }
        float laneTop(int k) const { return top + *scroll - k * laneH; }
        float maxScroll() const { return std::max(0.0f, lanes.size() * laneH - height()); }
        // Lane under screen y, snapped to the nearest lane; -1 if there are none
        int laneAt(float y) const {
            if (lanes.empty()) return -1;
            int k = (int)std::floor((top + *scroll - y) / laneH);
            return std::clamp(k, 0, (int)lanes.size() - 1);
        }
    };

    static constexpr float PERC_LANE_H  = 34.0f;   // fits a 30px rhythm line
    static constexpr float AUDIO_LANE_H = 34.0f;
    static constexpr float SUB_DIVIDER  = 2.0f;

    float percPanelH  = 3 * PERC_LANE_H;
    float audioPanelH = 2 * AUDIO_LANE_H;
    float percScroll  = 0.0f;
    float audioScroll = 0.0f;
    float noteWindowH = 0.0f;           // from the last draw, for clamping
    Region hoverRegion = Region::None;  // where the cursor was on the last draw
    std::deque<std::string> laneLabels; // the renderer keeps char*s until the frame is drawn

    // Each strip is at least one lane tall and at most 40% of the window.
    void clampSubPanels() {
        float maxH = noteWindowH * 0.4f;
        percPanelH  = std::clamp(percPanelH,  PERC_LANE_H,  std::max(PERC_LANE_H,  maxH));
        audioPanelH = std::clamp(audioPanelH, AUDIO_LANE_H, std::max(AUDIO_LANE_H, maxH));
    }

    // Up/Down arrows: grow (+1) or shrink (-1) the strip under the cursor by
    // one lane (the perc strip when the cursor isn't over the audio strip).
    void resizeSubPanel(int dir) {
        if (hoverRegion == Region::Audio) audioPanelH += dir * AUDIO_LANE_H;
        else                              percPanelH  += dir * PERC_LANE_H;
        clampSubPanels();
    }

    // A rect clipped to the vertical extent of a panel (and the window's x).
    static void panelRect(VulkanRenderer& r, const SubPanel& p, float x1, float x2,
                          float x, float y, float w, float h, ui::Color c) {
        float l = std::max(x, x1), rr = std::min(x + w, x2);
        float bt = std::max(y, p.bottom), tp = std::min(y + h, p.top);
        if (rr > l && tp > bt) r.addRect(l, bt, rr - l, tp - bt, c.r, c.g, c.b);
    }

    // Background, lanes, beat lines and playhead of one strip.
    void drawSubPanel(VulkanRenderer& r, const SubPanel& p, float x1, float x2,
                      const std::vector<std::pair<float, bool>>& beatXs,
                      float playheadX, float loopX, const char* emptyText) {
        const ui::Color bg = {0.17f, 0.17f, 0.19f};
        panelRect(r, p, x1, x2, x1, p.bottom, x2 - x1, p.height(), bg);

        for (int k = 0; k < (int)p.lanes.size(); k++) {
            float top = p.laneTop(k), bottom = top - p.laneH;
            if (bottom >= p.top || top <= p.bottom) continue;
            if (k % 2) panelRect(r, p, x1, x2, x1, bottom, x2 - x1, p.laneH, {0.20f, 0.20f, 0.22f});
            panelRect(r, p, x1, x2, x1, bottom, x2 - x1, 1, {0.12f, 0.12f, 0.13f});   // separator
            const Track& t = g_tracks[p.lanes[k]];
            panelRect(r, p, x1, x2, x1, bottom, 4, p.laneH, t.color);                // colour tab
        }

        for (auto [x, whole] : beatXs) {
            ui::Color c = whole ? ui::Color{0.36f, 0.36f, 0.45f} : ui::Color{0.26f, 0.26f, 0.31f};
            panelRect(r, p, x1, x2, x, p.bottom, 1, p.height(), c);
        }
        panelRect(r, p, x1, x2, loopX, p.bottom, 1, p.height(), {0.235f, 0.235f, 0.235f});
        panelRect(r, p, x1, x2, playheadX, p.bottom, 1, p.height(), {0.784f, 0.08f, 0.08f});

        // Track names, only on lanes that are fully visible (text isn't clipped)
        const glm::vec4 white(0.85f, 0.85f, 0.85f, 1.0f);
        for (int k = 0; k < (int)p.lanes.size(); k++) {
            float top = p.laneTop(k), bottom = top - p.laneH;
            if (bottom < p.bottom - 0.5f || top > p.top + 0.5f) continue;
            laneLabels.push_back(g_tracks[p.lanes[k]].name);
            r.addText(laneLabels.back().c_str(), x1 + 8, bottom + 3, 13.0f, white);
        }
        if (p.lanes.empty() && p.height() >= 16) {
            r.addText(emptyText, x1 + 8, p.top - 16, 13.0f, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
        }
    }

    // Transform: world -> screen
    Vec2 worldToScreen(Vec2 topLeft, Vec2 w) const {
        // y increases upward in world, downward on screen → invert with (y0 - y)
        float sxp = topLeft.x + (w.x - x0) * sx;
        float syp = topLeft.y + (w.y - y0) * sy;
        return {sxp, syp};
    }
    // Transform: screen -> world
    Vec2 screenToWorld(Vec2 topLeft, Vec2 s) const {
        float wx = x0 + (s.x - topLeft.x) / sx;
        float wy = y0 + (s.y - topLeft.y) / sy;
        return {wx, wy};
    }

    Ratio calculateRoot(std::vector<Ratio> octaves) {
        // Algorithm: take frequencies a, b, c etc. divide a by b, find simplest fraction with
        // Integers, divide a by the numerator (or b by the denominator), then repeat the process
        // with that new number n, c, etc. until you only have one number.
        Ratio root = octaves[0];
        for (int i = 1; i < (int)octaves.size(); i++) {
          Ratio div = divideR(root, octaves[i]);
          root.den *= div.num;
          root = simplifyR(root);
        }
        return root;
    }

    Ratio snapRootHarmonics(Vec2 unsnappedHoverW, Ratio root) {
      Ratio closest = {99999999, 1};
      Ratio current = root;
      while (true) {
        if (std::abs(unsnappedHoverW.y - ratio_to_octave(current)) < std::abs(unsnappedHoverW.y - ratio_to_octave(closest)))
          closest = current;
        else
          break;
        current = addR(current, root);
      }
      return closest;
    }

    Ratio drawFundamental(VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight, float start, float end, std::vector<Event*> currentEvs) {
      bool allConstant = true;
      Ratio root{0,0};
      for  (int i = 0; i < (int)currentEvs.size(); i++) {
        if (!(currentEvs[i]->isConstant())) {
          allConstant = false;
        }
      }

      int height = 0;
      if (allConstant) {
        std::vector<Ratio> octaves;
        for (int i = 0; i < (int)currentEvs.size(); i++) {
          octaves.push_back(currentEvs[i]->ratio);
        }
        root = calculateRoot(octaves);

        Ratio currentFreq = root;
        while (true) {
          Vec2 a = worldToScreen(topLeft, { start, ratio_to_octave(currentFreq) });
          Vec2 b = worldToScreen(topLeft, { end, ratio_to_octave(currentFreq) });
          if (a.y - height < 2) {
            if (height == 0) {
              currentFreq = addR(currentFreq, root);
              continue;
            } else
              break;
          }
          height = a.y;
          if (a.y > topLeft.y)
            break;
          else if (a.y > bottomRight.y) {
            addCanvasRect(renderer, topLeft, bottomRight, a.x, a.y - 1, b.x - a.x, 1, 1.0, 0.0, 1.0);
          }
          currentFreq = addR(currentFreq, root);
        }

      }
      return root;
    }

    // Round a value to nearest step (for snapping)
    static float snap(float v, float step) {
        return step > 0.f ? std::round(v / step) * step : v;
    }

    float snap_beat(const Event* node, float v, float closest) {
        if (!node) return 999999999.0f;
        float current = snap_beat(node->left, v, closest);
        if (node->type == EV_BEAT) {
            if (std::abs(node->beat - v) > std::abs(current - v)) {
                return current;
            } else {
                current = node->beat;
            }
      }
        float rightVal = snap_beat(node->right, v, current);
        if (std::abs(rightVal - v) > std::abs(current - v))
          return current;
        else
          return rightVal;
    }

    float accum_beats(Event* node, float acc, float v, Event* last) {
        if (!node) return acc;
        float current = accum_beats(node->left, acc, v, last);
        if (node->type == EV_TEMPO) {
            // Tempo change event
            if (node->finalEv) {
                // This is the last tempo event, will return
                if (node->beat > v) {
                    if (last && last->isConstant())
                        current += (last->function.evaluate(b, t, 0, 0) / 60.0) * (v - last->beat);
                    else if (last)
                        current += integrate(last->function, last->beat, v);
                } else {
                    if (last && last->isConstant())
                        current += (last->function.evaluate(b, t, 0, 0) / 60.0) * (node->beat - last->beat);
                    else if (last)
                      current += integrate(last->function, last->beat, node->beat);

                    if (node->isConstant())
                        current += (node->function.evaluate(b, t, 0, 0) / 60.0) * (v - node->beat);
                    else if (last)
                        current += integrate(node->function, node->beat, v);
                }
                return current;
            } else {
                // Node isn't the final event
                if (node->beat > v) {
                    // Current time is less than current node's time
                    // Find value up to v and return
                    if (last && last->isConstant())
                        current += (last->function.evaluate(b, t, 0, 0) / 60.0) * (v - last->beat);
                    else if (last)
                        current += integrate(last->function, last->beat, v);
                    return current;
                } else {
                    // Keep going, current node's time is less than current time
                    if (last && last->isConstant())
                        current += (last->function.evaluate(b, t, 0, 0) / 60.0) * (node->beat - last->beat);
                    else if (last)
                        current += integrate(last->function, last->beat, v);
                }
            }
            last = node;
        }
        return accum_beats(node->right, current, v, last);
    }

    void drawNumber(int n, Vec2 p, VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight) {
      int current = n % 10;
      // Draw current;
      switch (current) {
        case 0:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y - 1, 1, 3, 0.0, 0.0, 0.0);
          break;
        case 1:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 1, 5, 0.47, 0.47, 0.63);
          break;
        case 2:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y - 1, 2, 1, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y + 1, 2, 1, 0.0, 0.0, 0.0);
          break;
        case 3:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y + 1, 2, 1, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 1, 2, 1, 0.0, 0.0, 0.0);
          break;
        case 4:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y, 3, 3, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y + 1, 1, 2, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 2, p.y - 2, 1, 2, 0.47, 0.47, 0.63);
          break;
        case 5:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y + 1, 2, 1, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 1, 2, 1, 0.0, 0.0, 0.0);
          break;
        case 6:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y + 1, 2, 1, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y - 1, 1, 1, 0.0, 0.0, 0.0);
          break;
        case 7:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y + 2, 3, 1, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 2, p.y - 2, 1, 5, 0.47, 0.47, 0.63);
          break;
        case 8:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y + 1, 1, 1, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y - 1, 1, 1, 0.0 ,0.0 ,0.0);
          break;
        case 9:
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 2, 3, 5, 0.47, 0.47, 0.63);
          addCanvasRect(renderer, topLeft, bottomRight, p.x + 1, p.y + 1, 1, 1, 0.0, 0.0, 0.0);
          addCanvasRect(renderer, topLeft, bottomRight, p.x, p.y - 1, 2, 1, 0.0, 0.0, 0.0);
          break;
        default:
          break;
      }
      if (n != current)
        drawNumber((n - current) / 10, { p.x - 4, p.y }, renderer, topLeft, bottomRight);
    }

    void drawRhythmLine(Event* ev, Vec2 s0, Vec2 range, VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight) {
        (void)range;
        addCanvasRect(renderer, topLeft, bottomRight, s0.x, s0.y - 15, 1, 30, 0.47, 0.47, 0.63);
        float beat = ev->beat;
        int skipCount = 0;
        int offset = 15;
        printf("s0.x: %f, beat: %f\n", s0.x, beat);
        for (int i = 0; i < (int)ev->primeFactors.size(); i++) {
          if (ev->primeFactors[i] == 0) {
            skipCount++;
          } else {
            printf("i: %d\n", i);
            // Draw the skip marker then the tails
            if (skipCount > 0) {
              printf("skip: %d\n", skipCount);
              addCanvasRect(renderer, topLeft, bottomRight, s0.x, s0.y + offset, 3, 1, 0.47, 0.47, 0.63);
              if (skipCount > 1) {
                std::string st = std::to_string(skipCount);
                drawNumber(skipCount, { s0.x + 4, s0.y + offset}, renderer, topLeft, bottomRight);
              }
              offset -= 6;
              skipCount = 0;
            }
            int offsetX = 3;
            addCanvasRect(renderer, topLeft, bottomRight, s0.x, s0.y + offset, 3, 1, 0.47, 0.47, 0.63);
            for (int j = 0; j < (int)ev->primeFactors[i]; j++) {
              addCanvasRect(renderer, topLeft, bottomRight, s0.x + offsetX, s0.y + offset, 1, 4, 0.47, 0.47, 0.63);
              if (j != (int)ev->primeFactors[i] - 1)
                addCanvasRect(renderer, topLeft, bottomRight, s0.x + offsetX, s0.y + offset, 2, 1, 0.47, 0.47, 0.63);
              offsetX += 2;
            }
            offset -= 5;
          }
        }
    }

    void draw_rhythm_line(Event* beatEv, VulkanRenderer& renderer, Vec2 topLeft, Vec2 bottomRight) {
      Event* current = beatEv;
      std::vector<float> simultaneous;
      while (current && std::abs(current->beat - beatEv->beat) < 1e-6) {
        if (current->type == EV_NOTE_START || current->type == EV_NOTE_END) {
          if (!(current->ratio.num == 0 && current->ratio.den == 0))
            simultaneous.push_back(ratio_to_octave(current->ratio));
          else {
            Event* evi = prev(current);
            while (evi && evi->id != current->id)
              evi = prev(evi);
            if (evi)
              simultaneous.push_back(ratio_to_octave(evi->ratio));
          }
        }
        current = prev(current);
      }
      current = beatEv;
      while (current && std::abs(current->beat - beatEv->beat) < 1e-6) {
        if (current->type == EV_NOTE_START || current->type == EV_NOTE_END) {
          if (!(current->ratio.num == 0 && current->ratio.den == 0))
            simultaneous.push_back(ratio_to_octave(current->ratio));
          else {
            Event* evi = prev(current);
            while (evi->id != current->id)
              evi = prev(evi);
            if (evi)
              simultaneous.push_back(ratio_to_octave(evi->ratio));
          }
        }
        current = next(current);
      }
      if (simultaneous.empty()) {
        Vec2 ap = worldToScreen(topLeft, { beatEv->time, 0.0f });
        ap.y = (topLeft.y + bottomRight.y) / 2.0f;
        drawRhythmLine(beatEv, ap, {beatEv->beatL, beatEv->beatR}, renderer, topLeft, bottomRight);
      } else {
        // Draw it on each of the simultaneous Notes
        for (int i = 0; i < (int)simultaneous.size(); i++) {
          Vec2 ap = worldToScreen(topLeft, { beatEv->time, simultaneous[i]});
          drawRhythmLine(beatEv, ap, {beatEv->beatL, beatEv->beatR}, renderer, topLeft, bottomRight);
        }
      }
    }

    Vec2 prevHover = {0.0f, 0.0f};

    bool containsKey(std::vector<char> keys, char c) {
        for (const auto& k : keys) {
            if (k == c)
                return true;
        }
        return false;
    }

    int numberContained(std::vector<char> keys) {
        for (const auto& k : keys) {
            if (k == '0')
                return 0;
            if (k == '1')
                return 1;
            if (k == '2')
                return 2;
            if (k == '3')
                return 3;
            if (k == '4')
                return 4;
            if (k == '5')
                return 5;
            if (k == '6')
                return 6;
            if (k == '7')
                return 7;
            if (k == '8')
                return 8;
            if (k == '9')
                return 9;
        }
        return -1;
    }

    void draw(VulkanRenderer& renderer, CurrentBox& nw, Vec2 hover, int click, std::vector<char> keys, char*& jump, char*& split, int dx, int dy) {
        // ---- Layout: notes on top, then the perc strip, then the audio strip ----
        noteWindowH = nw.y2 - nw.y1;
        clampSubPanels();
        SubPanel audio, perc;
        audio.bottom = nw.y1;
        audio.top    = nw.y1 + audioPanelH;
        audio.laneH  = AUDIO_LANE_H;
        audio.scroll = &audioScroll;
        audio.lanes  = tracksOfType(TrackType::Audio);
        perc.bottom  = audio.top + SUB_DIVIDER;
        perc.top     = perc.bottom + percPanelH;
        perc.laneH   = PERC_LANE_H;
        perc.scroll  = &percScroll;
        perc.lanes   = tracksOfType(TrackType::Perc);
        float notesBottom = perc.top + SUB_DIVIDER;
        laneLabels.clear();   // the previous frame has been drawn

        // Everything below the sub-panels is the notes area: drawing is clipped to it
        Vec2 topLeft  = { nw.x1, nw.y2};
        Vec2 bottomRight = { nw.x2, notesBottom };

        printf("\nraw hover: %f, %f\n", hover.x, hover.y);

        bool hovered = hover.x > 1e-06 && hover.y > 1e-06;
        hoverRegion = !hovered                 ? Region::None
                    : hover.y >= perc.top      ? Region::Notes
                    : hover.y >= audio.top     ? Region::Perc
                    :                            Region::Audio;
        bool inNotes = hoverRegion == Region::Notes;

        // Mouse wheel over a strip scrolls its lanes rather than the notes
        SubPanel* hoveredPanel = hoverRegion == Region::Perc  ? &perc
                               : hoverRegion == Region::Audio ? &audio : nullptr;
        if (hoveredPanel && dy != 0 && !containsKey(keys, '~')) {
            *hoveredPanel->scroll -= dy;
            dy = 0;
        }
        percScroll  = std::clamp(percScroll,  0.0f, perc.maxScroll());
        audioScroll = std::clamp(audioScroll, 0.0f, audio.maxScroll());

        // --- Panning with RMB drag ---
        if ((hovered && dx != 0) || (dy != 0)) {
            if (containsKey(keys, '~')) {
                printf("hi\n");
                sx += dx;
                sy += dy;
            } else {
                // Convert pixel delta → world delta
                x0 -= dx / sx;
                y0 -= dy / sy; // + because dragging down increases screen y but decreases world y
            }
        }

        if (hovered && click == 2 && std::abs(prevHover.x) > 1e-6) {
            Vec2 d = { hover.x - prevHover.x, hover.y - prevHover.y};
            x0 -= d.x / sx;
            if (inNotes) y0 -= d.y / sy;   // the strips only pan in time
        }

        prevHover = hover;

        // Compute visible world rect
        Vec2 wA = screenToWorld(topLeft,      topLeft);
        //Vec2 wB = screenToWorld(topLeft,      bottomRight);

        // --- Grid lines (integer beats/octaves) ---
        const float gridCol[3]   = { 0.235, 0.235, 0.235};
        const float playheadCol[3] = { 0.784, 0.08, 0.08 };
        //const float noteCol[3] = { 0.353, 0.63, 1.0 };
        //const float selCol[3] = { 1.0, 1.0, 0.2588 };
        //const float focusCol[3] = { 1.0, 0.63, 0.353 };

        // Draw playhead
        Vec2 s0 = worldToScreen(topLeft, { playhead_time, wA.y });
        addCanvasRect(renderer, topLeft, bottomRight, s0.x, topLeft.y, 1, bottomRight.y - topLeft.y, playheadCol[0], playheadCol[1], playheadCol[2]);

        s0 = worldToScreen(topLeft, {0, freq_to_octave(inputFreq)});
        addCanvasRect(renderer, topLeft, bottomRight, topLeft.x, s0.y, bottomRight.x - topLeft.x, 1, gridCol[0], gridCol[1], gridCol[2]);

        // Draw loop end line
        s0 = worldToScreen(topLeft, { loop_end, wA.y });
        addCanvasRect(renderer, topLeft, bottomRight, s0.x, topLeft.y, 1, bottomRight.y - topLeft.y, gridCol[0], gridCol[1], gridCol[2]);

        // First end of a split being made with s
        if (split_start >= 0.0f) {
          s0 = worldToScreen(topLeft, { calc_time(&timeEvents, split_start), wA.y });
          addCanvasRect(renderer, topLeft, bottomRight, s0.x, topLeft.y, 1, bottomRight.y - topLeft.y, 0.8, 0.7, 0.3);
        }

        Vec2 w = screenToWorld(topLeft, hover);
        float currentBeat = accum_beats(timeEvents.root, 0.0f, w.x, NULL);
        float snappedBeat = snap_beat(beatEvents.root, currentBeat, 999999999.0f);

        printf("snappedBeat: %f\n", snappedBeat);

        if (std::abs(currentBeat - std::round(currentBeat)) < std::abs(currentBeat - snappedBeat))
            snappedBeat =  std::round(currentBeat);
        printf("topLeft.x: %f, w.x: %f, currentBeat: %f, snappedBeat: %f\n", topLeft.x, w.x, currentBeat, snappedBeat);

        float snappedTime = calc_time(&timeEvents, snappedBeat);

        Event* currentBeatEv = beatEvents.get_first();

        while (currentBeatEv && (currentBeatEv->type != EV_NOTE_START && currentBeatEv->type != EV_BEAT && currentBeatEv->type != EV_PERC))
          currentBeatEv = next(currentBeatEv);

        Vec2 unsnappedHoverW = screenToWorld(topLeft, hover);
        hoverW = unsnappedHoverW;
        hoverW.x = snappedTime;

        if (empty)
          hoverR = {1, 1};

        Ratio focusAtCursor = {99999999, 99999999};

        currentEvsAtCursor.clear();
        std::vector<Event*> noteStarts;
        Event* prevEv = nullptr;
        int beatRLi = 0;
        float lastNoteEvTime = 0.0f;
        while (currentBeatEv) {
            if (currentBeatEv->type == EV_NOTE_START) {
                if (snap_harmonics && !snap_foci && !noteStarts.empty() && prevEv && (int)noteStarts.size() > 1) {
                  Ratio root = drawFundamental(renderer, topLeft, bottomRight, lastNoteEvTime, currentBeatEv->time, noteStarts);
                  // Snap to harmonics
                  if (unsnappedHoverW.x > lastNoteEvTime && unsnappedHoverW.x < currentBeatEv->time) {
                    hoverR = snapRootHarmonics(unsnappedHoverW, root);
                  }
                }
                lastNoteEvTime = currentBeatEv->time;
                noteStarts.push_back(currentBeatEv);
                if (currentBeatEv->beat < currentBeat)
                    currentEvsAtCursor.push_back(currentBeatEv);
            } else if (currentBeatEv->type == EV_NOTE_END) {
                if (snap_harmonics && !snap_foci && (int)noteStarts.size() > 1 && prevEv) {
                  Ratio root = drawFundamental(renderer, topLeft, bottomRight, lastNoteEvTime, currentBeatEv->time, noteStarts);
                  printf("root ratio: %d/%d\n", root.num, root.den);
                  if (unsnappedHoverW.x > lastNoteEvTime && unsnappedHoverW.x < currentBeatEv->time) {
                    hoverR = snapRootHarmonics(unsnappedHoverW, root);
                  }
                }
                Event* noteEv = nullptr;
                for (int i = 0; i < (int)noteStarts.size(); i++) {
                    if (noteStarts[i]->id == currentBeatEv->id) {
                        noteEv = noteStarts[i];
                        noteStarts.erase(noteStarts.begin() + i);
                        if (currentBeatEv->beat < currentBeat)
                            currentEvsAtCursor.erase(currentEvsAtCursor.begin() + i);
                        break;
                    }
                }
                lastNoteEvTime = currentBeatEv->time;
                //printf("noteEv: %d, ", noteEv != nullptr);
                // printf("value: %f, times: %f, %f!\n", noteEv->function.evaluate(b, t, 0, 0), noteEv->time, currentBeatEv->time);
                Vec2 ap = worldToScreen(topLeft, { noteEv->time, freq_to_octave(ratio_to_freq(noteEv->ratio))});
                //printf("VARLUE: %f\n", noteEv->function.evaluate(b, t, 0, 0));
                Vec2 bp = worldToScreen(topLeft, { currentBeatEv->time, freq_to_octave(ratio_to_freq(noteEv->ratio))});
                // printf("a: {%f, %f}, b: {%f, %f}\n", a.x, a.y, b.x, b.y);
                ui::Color nc = trackColor(noteEv->profile);
                addCanvasRect(renderer, topLeft, bottomRight, ap.x, ap.y - 1, bp.x - ap.x, 2, nc.r, nc.g, nc.b);
                if (std::abs(std::floor(noteEv->beat) - noteEv->beat) < 1e-6) {
                  // Draw rhythm line at note start
                  addCanvasRect(renderer, topLeft, bottomRight, ap.x, ap.y - 15, 1, 30, 0.47, 0.47, 0.63);
                }
                if (std::abs(std::floor(currentBeatEv->beat) - currentBeatEv->beat) < 1e-6) {
                  // Draw rhythm line at note end
                  addCanvasRect(renderer, topLeft, bottomRight, bp.x, ap.y - 15, 1, 30, 0.47, 0.47, 0.63);
                }
            } else if (currentBeatEv->type == EV_BEAT) {
              draw_rhythm_line(currentBeatEv, renderer, topLeft, bottomRight);
            }   // perc blips are drawn in the perc strip (drawSubPanels)
            if (prevEv) {
              // Catch up with regular beat lines
              for (int i = std::floor(prevEv->beat); i < std::ceil(currentBeatEv->beat) + 1; i++) {
                if (i <= beatRLi)
                  continue;
                // Draw line in the middle of the screen
                Vec2 ap = worldToScreen(topLeft, {calc_time(&timeEvents, i), 0.0f});
                addCanvasRect(renderer, topLeft, bottomRight, ap.x, std::floor((topLeft.y + bottomRight.y) / 2) - 15, 1, 30, 0.47, 0.47, 0.63);
              }
              beatRLi = std::ceil(currentBeatEv->beat);
              //if (avoidFirst == 0)
              //  avoidFirst = 1;
            }
            prevEv = currentBeatEv;
            currentBeatEv = next(currentBeatEv);
        }

        currentBeatEv = beatEvents.get_first();
        if (currentBeatEv && currentBeatEv->type != EV_FOCUS)
          currentBeatEv = next_type(currentBeatEv, EV_FOCUS);

        while (currentBeatEv) {
          //printf("FOCUS EVENT: %f, %f, %s, %f\n", currentBeatEv->beat, currentBeatEv->time, currentBeatEv->string_expr.c_str(), currentBeatEv->function.evaluate(b, t, 0, 0));
          currentBeatEv = next_type(currentBeatEv, EV_FOCUS);
        }

        currentBeatEv = beatEvents.get_first();

        if (currentBeatEv && currentBeatEv->type != EV_FOCUS)
          currentBeatEv = next_type(currentBeatEv, EV_FOCUS);

        // Draw foci and related harmonics, snap to focus
        while (currentBeatEv) {
          // for each focus event
          printf("currentFocus: %f, %d/%d\n", currentBeatEv->beat, currentBeatEv->ratio.num, currentBeatEv->ratio.den);
          if (prevEv && !(prevEv->ratio.num == 0 && prevEv->ratio.den == 0)) {
            // foci at 0 represent a break -> no drawing or snapping

            // Draw focus line from previous to current focus draw_events
            Vec2 ap = worldToScreen(topLeft, { prevEv->time, ratio_to_octave(prevEv->ratio) });
            Vec2 bp = worldToScreen(topLeft, { currentBeatEv->time, ratio_to_octave(prevEv->ratio) });
            addCanvasRect(renderer, topLeft, bottomRight, ap.x, ap.y - 1, bp.x - ap.x, 2, 1.0, 0.63, 0.353);

            if (snap_harmonics && snap_foci) {
              // Draw harmonics of focus and snap if hovered within
              // To not draw out of the canvas
              Vec2 bottom = screenToWorld(topLeft, bottomRight);
              Vec2 top = screenToWorld(topLeft, topLeft);

              Ratio current = prevEv->ratio;
              while (ratio_to_octave(current) > bottom.y)
                current = divideR(current, {2, 1});

              Ratio closest = {999999999,1};
              bool hoveredWithin = unsnappedHoverW.x >= prevEv->time && \
                               unsnappedHoverW.x < currentBeatEv->time;
              while (ratio_to_octave(current) < top.y) {
                uint32_t hRange = std::pow(2, std::atoi(g_settings.harmRange.c_str()));
                std::vector<uint32_t> altHRange = extractARange(g_settings.altHarmRange.data());
                float r = 0.73;
                float g = 0.45882;
                float b = 0.807;
                for (uint32_t i = hRange; i < hRange * 2; i++) {
                  Ratio interval = {i, hRange};
                  Ratio currentAdded = multiplyR(current, interval);
                  if (ratio_to_octave(currentAdded) > top.y)
                    break;
                  else if (ratio_to_octave(currentAdded) < bottom.y)
                    continue;
                  Ratio diff = subtractR(currentAdded, prevEv->ratio);
                  if (prevEv->ratio.eval() > currentAdded.eval())
                    diff = subtractR(prevEv->ratio, currentAdded);
                  //printf("currentAdded: %d/%d, prevEv: %d/%d\n", currentAdded.num, currentAdded.den, prevEv->ratio.num, prevEv->ratio.den);
                  if (diff.num != 0) {
                    Vec2 ap = worldToScreen(topLeft, { prevEv->time, ratio_to_octave(currentAdded)});
                    Vec2 bp = worldToScreen(topLeft, { currentBeatEv->time, ratio_to_octave(currentAdded)});
                    int factor = std::log2(simplifyR(interval).den);
                    float cr = r * factor;
                    cr = cr - std::floor(cr);
                    float cg = g * factor;
                    cg = cg - std::floor(cg);
                    addCanvasRect(renderer, topLeft, bottomRight, ap.x, ap.y, bp.x - ap.x, 1, cr, cg, 0.807);
                  }
                  if (hoveredWithin && \
                      std::abs(unsnappedHoverW.y - ratio_to_octave(currentAdded)) < std::abs(unsnappedHoverW.y - ratio_to_octave(closest))) {
                    closest = currentAdded;
                  }
                }
                for (int i = 0; i < (int)altHRange.size(); i++) {
                  Ratio interval = {altHRange[i] + 1, altHRange[i]};
                  while (interval.num < interval.den * 2) {
                    uint32_t simpDen = simplifyR(interval).den;
                    if (simpDen != interval.den) {
                      bool match = false;
                      if (isPow2(simpDen) && simpDen <= hRange)
                        match = true;
                      for (int j = 0; j < i; j++) {
                        if (altHRange[j] == simpDen)
                          match = true;
                      }
                      if (match) {
                        interval.num++;
                        continue;
                      }
                    }
                    Ratio currentAdded = multiplyR(current, interval);
                    if (ratio_to_octave(currentAdded) > top.y)
                      break;
                    else if (ratio_to_octave(currentAdded) < bottom.y) {
                      interval.num++;
                      continue;
                    }
                    Ratio diff = subtractR(currentAdded, prevEv->ratio);
                    if (diff.num != 0) {
                      Vec2 ap = worldToScreen(topLeft, { prevEv->time, ratio_to_octave(currentAdded)});
                      Vec2 bp = worldToScreen(topLeft, { currentBeatEv->time, ratio_to_octave(currentAdded)});
                      float cr = r * altHRange[i];
                      cr = cr - std::floor(cr);
                      float cb = b * altHRange[i];
                      cb = cb - std::floor(cb);
                      addCanvasRect(renderer, topLeft, bottomRight, ap.x, ap.y, bp.x - ap.x, 1, cr, g, cb);
                    }
                    if (hoveredWithin && \
                        std::abs(unsnappedHoverW.y - ratio_to_octave(currentAdded)) < std::abs(unsnappedHoverW.y - ratio_to_octave(closest))) {
                      closest = currentAdded;
                    }
                    interval.num++;
                  }
                }
                current = simplifyR(multiplyR(current, {2, 1}));
              }
              if (hoveredWithin && closest.num != 999999999) {
                focusAtCursor = prevEv->ratio;
                hoverR = closest;
                Ratio simpRat = closest;
                if (simpRat.num > simpRat.den) {
                  while (simpRat.num % 2 == 0 && simpRat.num > simpRat.den * 2) {
                    simpRat.num /= 2;
                  }
                } else {
                  while (simpRat.den % 2 == 0 && simpRat.den > simpRat.num * 2) {
                    simpRat.den /= 2;
                  }
                }
                sprintf(measureText, "%d/%d (%d/%d)", closest.num, closest.den, simpRat.num, simpRat.den);
                renderer.addText(measureText, bottomRight.x - 150, bottomRight.y + 3, 15.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
              }
            }
          } else {
            // TODO: handle non-constant draw_events
          }
          prevEv = currentBeatEv;
          currentBeatEv = next_type(currentBeatEv, EV_FOCUS);
        }

        // draw_events(events.root, renderer, topLeft, bottomRight {snappedBeat, w.y});

        // Hover snapped position (for preview dot / rubber band)
        if (hovered) {
            Vec2 w = unsnappedHoverW;
            printf("w.y: %f, hover: %f\n", w.y, hover.y);
            // w.x = snap_split(w.x);
            w.x = snappedTime;
            if (snap_to_grid) {
                w.y = snap(w.y, snap_y);
            } else if (snap_harmonics) {
              if (snap_foci) {
              }
              else {
                // Snap to fundamental's harmonics
              }
            }
        }

        if (selectFirst && select1closest && select2closest) {
          // Draw measurement
          Ratio rat = simplifyR(divideR(select2closest->ratio, select1closest->ratio));
          Ratio simpRat = rat;
          if (simpRat.num > simpRat.den) {
            while (simpRat.num % 2 == 0 && simpRat.num > simpRat.den * 2) {
              simpRat.num /= 2;
            }
          } else {
            while (simpRat.den % 2 == 0 && simpRat.den > simpRat.num * 2) {
              simpRat.den /= 2;
            }
          }
          sprintf(measureText, "%d/%d (%d/%d)", rat.num, rat.den, simpRat.num, simpRat.den);
          printf("%s\n", measureText);
          renderer.addText(measureText, bottomRight.x - 150, bottomRight.y + 3, 15.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        }

        // Key handling (P to change playhead position, L to change note length, D to delete a note, F to change the note's frequency)
        if (hovered) {
            if (containsKey(keys, 't'))
              tuner_triggered = true;
            if (containsKey(keys, 'p'))
                playhead_time = hoverW.x;
            if (containsKey(keys, 'u')) {
              // Set loop limit
              if (std::abs(loop_end - hoverW.x) < 1e-6)
                loop_end = -1.0f;
              else
                loop_end = hoverW.x;
            }
            if (containsKey(keys, 'h'))
                snap_foci = !snap_foci;
            if (containsKey(keys, 's')) {
              // First press marks one end, second press splits the span
              // between the two into `split` equal divisions
              if (split_start < 0.0f)
                split_start = snappedBeat;
              else {
                insertSplit(split_start, snappedBeat, std::atoi(split));
                split_start = -1.0f;
              }
            }
            if (containsKey(keys, 'x')) {
              // Remove the rhythm split under the cursor
              Event* beatEv = findBeatEv(beatEvents.root, snappedBeat);
              if (beatEv)
                beatEvents.remove(beatEv->beat, beatEv->id);
            }
            if (containsKey(keys, ' '))
                play_triggered_from_space = true;
            if (containsKey(keys, 'v')) {
                // Paste notes
                Event* current = beatEvents.get_first();
                std::vector<Event*> noteStarts;
                float firstSelectedTime = -1.0f;
                while (current) {
                  if (current->type == EV_NOTE_START) {
                    if (current->selected) {
                        printf("new note: %f, %s\n", current->beat, current->string_expr.c_str());
                      if (std::abs(firstSelectedTime + 1.0f) < 1e-6)
                        firstSelectedTime = current->beat;
                      noteStarts.push_back(current);
                    }
                  } else if (current->type == EV_NOTE_END) {
                    for (int i = 0; i < (int)noteStarts.size(); i++) {
                      if (noteStarts[i]->id == current->id) {
                        // Process note
                        Event* e1 = new Event(calc_beat(&timeEvents, hoverW.x) + noteStarts[i]->beat - firstSelectedTime, EV_NOTE_START, noteStarts[i]->ratio);
                        Event* e2 = new Event(calc_beat(&timeEvents, hoverW.x) + current->beat - firstSelectedTime, EV_NOTE_END);
                        e1->profile = e2->profile = noteStarts[i]->profile;
                        if ((int)spareIDs.size() > 0) {
                          e1->id = spareIDs[(int)spareIDs.size() - 1];
                          spareIDs.pop_back();
                          e2->id = e1->id;
                        } else {
                          e1->id = nextID;
                          e2->id = nextID++;
                        }
                        beatEvents.insert(e1);
                        beatEvents.insert(e2);
                        e1->time = calc_time(&timeEvents, e1->beat);
                        e2->time = calc_time(&timeEvents, e2->beat);
                        e1->origin = originEv;
                        beatEvents.inorder();
                        beatEvents.ordered = false;
                        noteStarts.erase(find(noteStarts.begin(), noteStarts.end(), noteStarts[i]));
                        break;
                      }
                    }
                  }
                  current = next_note(current);
                }
            } if (containsKey(keys, 'r')) {
                // Remove selected notes
            }
        }

        // Closest note (note keys) when over the notes, and the blip at the
        // hovered beat in the hovered lane when over the perc strip. Kept
        // apart because the note keys below assume a note: a ratio, a
        // matching NOTE_END, ...
        Event* closest = nullptr;
        Event* closestBlip = nullptr;
        int hoverPercTrack = -1;   // perc track of the lane under the cursor
        if (hoverRegion == Region::Perc && perc.laneAt(hover.y) >= 0) {
          hoverPercTrack = perc.lanes[perc.laneAt(hover.y)];
          Event* e = beatEvents.get_first();
          if (e && e->type != EV_PERC)
            e = next_type(e, EV_PERC);
          while (e) {
            if (e->profile == hoverPercTrack && std::abs(e->beat - snappedBeat) < 1e-6) {
              closestBlip = e;
              break;
            }
            e = next_type(e, EV_PERC);
          }
        } else if (inNotes && (int)currentEvsAtCursor.size() > 0) {
          float dist = 999999999999.0f;
          b = currentBeat;
          for (int i = 0; i < (int)currentEvsAtCursor.size(); i++) {
              if (std::abs(ratio_to_octave(currentEvsAtCursor[i]->ratio) - hoverW.y) < dist) {
                  dist = std::abs(ratio_to_octave(currentEvsAtCursor[i]->ratio) - hoverW.y);
                  closest = currentEvsAtCursor[i];
              }
          }
        }

        // d over a blip deletes it
        if (hovered && closestBlip && containsKey(keys, 'd')) {
          beatEvents.remove(closestBlip->beat, closestBlip->id);
          closestBlip = nullptr;
        }

        if (containsKey(keys, 'e')) {
          if (selectFirst) {
            select1 = hoverW;
            select1closest = closest;
          } else {
            select2 = hoverW;
            select2closest = closest;
          }
          selectFirst = !selectFirst;
        }

        // Draw origin line
        if (closest && closest->origin) {
          Vec2 ap = worldToScreen(topLeft, { closest->time, ratio_to_octave(closest->ratio)});
          Vec2 bp = worldToScreen(topLeft, { closest->time + 2, ratio_to_octave(closest->origin->ratio)});
          addCanvasRect(renderer, topLeft, bottomRight, ap.x, ap.y, 2, bp.y - ap.y, 0.2235, 0.729, 0.196);
          Ratio rat = simplifyR(divideR(closest->ratio, closest->origin->ratio));
          Ratio simpRat = rat;
          if (simpRat.num > simpRat.den) {
            while (simpRat.num % 2 == 0 && simpRat.num > simpRat.den * 2) {
              simpRat.num /= 2;
            }
          } else {
            while (simpRat.den % 2 == 0 && simpRat.den > simpRat.num * 2) {
              simpRat.den /= 2;
            }
          }
          sprintf(originText, "%d/%d (%d/%d)", rat.num, rat.den, simpRat.num, simpRat.den);
          renderer.addText(originText, topLeft.x + 3, bottomRight.y + 3, 15.0f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
        }

        if (hovered && (int)keys.size() != 0 && closest) {
          char closestString[20];
          strcpy(closestString, closest->string_expr.c_str());
          printf("closest: %f, %d, %s\n", closest->beat, closest->id, closestString);

          int number = numberContained(keys);
          int target = number == -1 ? -1 : hotkeyTrack('0' + number);

          if (target != -1 && target < (int)g_tracks.size() && g_tracks[target].type == TrackType::Standard) {
            closest->profile = target;
          }

            if (containsKey(keys, 'q')) {
              beatEvents.inorder();
            }

            if (containsKey(keys, 'a')) {
              closest->selected = !closest->selected;
            }

            if (containsKey(keys, 'j')) {
              // jump the frequency by the amount contained in jump (char* passed to draw())
              uint32_t num, den;
              num = den = 0;
              char * current = jump;
              bool noVal;
              num = getNum(current, noVal);
              current += 1;
              den = getNum(current, noVal);
              Ratio jump = {num, den};
              closest->ratio = simplifyR(multiplyR(jump, closest->ratio));
            }

            if (containsKey(keys, 'd') || containsKey(keys, 'f') || \
                  containsKey(keys, 'l')) {
                // Delete the note because it will be replaced
                beatEvents.inorder();
                Event* noteEnd = next(closest);
                while (noteEnd->id != closest->id) {
                    noteEnd = next(noteEnd);
                }
                // Delete any foci that were on this note
                Event* current = beatEvents.get_first();
                Event* previous = nullptr;
                if (current && current->type != EV_FOCUS)
                  current = next_type(current, EV_FOCUS);
                bool deleted = false;
                while (current) {
                  // We can do this because a focus' string_expr is always
                  // copied from the note that was right clicked on
                  if (current->ratio.num == closest->ratio.num && current->ratio.den == closest->ratio.den) {
                    if (previous && !(previous->ratio.num == 0 && previous->ratio.den == 0)) {
                      current->ratio = {0, 0};
                    } else {
                      beatEvents.remove(current->beat, current->id);
                      deleted = true;
                    }
                  }
                  if (!deleted)
                    previous = current;
                  else
                    deleted = false;
                  current = next_type(current, EV_FOCUS);
                }

                if (!containsKey(keys, 'd'))
                  heldProfile = closest->profile;
                if (containsKey(keys, 'l')) {
                  placing = true;
                  if (std::abs(hoverW.x - closest->time) < std::abs(hoverW.x - noteEnd->time)) {
                    anchorW.x = noteEnd->time;
                    b = noteEnd->beat;
                    anchorR = closest->ratio;
                    strcpy(anchorExpr, closestString);
                    anchorBeat = noteEnd->beat;
                    anchorBeatExact= noteEnd->beat;
                  } else {
                    anchorW.x = closest->time;
                    b = closest->beat;
                    anchorR = closest->ratio;
                    strcpy(anchorExpr, closestString);
                    anchorBeat = closest->beat;
                    anchorBeatExact= closest->beat;
                  }
                } else if (containsKey(keys, 'f')) {
                  currently_dragged.x = closest->beat;
                  currently_dragged.y = noteEnd->beat;
                  dragging = true;
                }
                beatEvents.remove(closest->beat, closest->id);
                beatEvents.remove(noteEnd->beat, noteEnd->id);
                //beatEvents.remove(current->beat, current->id);
                beatEvents.inorder();
            }
        }

        // TODO: Convert time to beats by using tempo events.

        // Canvas axes are time (seconds), frequency (octaves)
        // Beat divisions are stored in units of beats
        // Notes are stored in units of beats
        // Tempo changes are stored in units of seconds
        // Have to constantly convert world x coordinate into beats for hover snap, should be a relatively simple calculation

        if (inNotes && click == 2 && !placing && (int)currentEvsAtCursor.size() > 0) {
            float dist = 999999999999.0f;
            b = currentBeat;
            Event* closest;
            for (int i = 0; i < (int)currentEvsAtCursor.size(); i++) {
                if (std::abs(ratio_to_octave(currentEvsAtCursor[i]->ratio) - hoverW.y) < dist) {
                    dist = std::abs(ratio_to_octave(currentEvsAtCursor[i]->ratio) - hoverW.y);
                    closest = currentEvsAtCursor[i];
                }
            }

            printf("closest beat: %f\n", closest->beat);

            Event* closestOff = next(closest);
            while (!(closestOff->type == EV_NOTE_END && closestOff->id == closest->id))
                closestOff = next(closestOff);

            printf("closestOff->beat: %f\n", closestOff->beat);

            int insert = 0;
            int fociLength = 0; // This is i
            bool focus_after = true;
            bool focus_before = true;
            std::vector<Event> newFoci;
            Event* insertEv = nullptr;
            Event* currentFocus = beatEvents.get_first();
            if (currentFocus && currentFocus->type != EV_FOCUS)
              currentFocus = next_type(currentFocus, EV_FOCUS);
            Event* prev = nullptr;
            Event* prevPrev = nullptr;
            while (currentFocus) {
                if (currentFocus->beat > currentBeat && !insertEv) {
                    // This index is the next focus change after the click point
                    insert = fociLength;
                    insertEv = currentFocus;
                    if (currentFocus->ratio.num == 0 && currentFocus->ratio.den == 0)
                        focus_after = false;
                    if (fociLength <= 1 || (prevPrev && prevPrev->ratio.num == 0 && prevPrev->ratio.den == 0))
                        focus_before = false;
                } else if (!next_type(currentFocus, EV_FOCUS) && !insertEv) {
                    insert = fociLength + 1;
                    focus_after = false;
                }
                if (!insertEv) {
                    prevPrev = prev;
                    prev = currentFocus;
                }
                fociLength++;
                currentFocus = next_type(currentFocus, EV_FOCUS);
            }
            printf("fociLength: %d insert: %d\n", fociLength, insert);
            // printf("insert: %d\n", insert);
            // printf("focus_before: %d, focus_after: %d\n", focus_before, focus_after);
            std::fflush(stdout);
            bool removePrevious = false;
            bool removeNext = false;
            Event* start = new Event(0.0f, EV_FOCUS, {0, 0});
            Event* end = new Event(0.0f, EV_FOCUS, {0, 0});

            /*if (prevPrev && prevPrev->isConstant())
                printf("prevPrev: %f, %f\n", prevPrev->beat, prevPrev->function.evaluate(b, t, 0, 0));
            if (prev && prev->isConstant())
                printf("prev: %f, %f\n", prev->beat, prev->function.evaluate(b, t, 0, 0));
            if (insertEv && insertEv->isConstant())
                printf("current: %f, %f\n", insertEv->beat, insertEv->function.evaluate(b, t, 0, 0));*/

            bool no_prev_or_closest_starts_after_prev = !prev || closest->beat > prev->beat;
            bool no_foci_or_after_foci_or_closest_ends_before_next = fociLength == 0 || insert == fociLength || (insertEv && closestOff->beat < insertEv->beat);

            bool closest_starts_at_prev = insert > 0 && prev && std::abs(closest->beat - prev->beat) < 1e-6;
            bool closest_ends_at_next_type_change = insert > 0 && insert <= fociLength - 1 && std::abs(closestOff->beat - insertEv->beat) < 1e-6;

            printf("npcsap: %d, cebnfc: %d, csap: %d, ceantc: %d, fb: %d, fa: %d\n", no_prev_or_closest_starts_after_prev, no_foci_or_after_foci_or_closest_ends_before_next, closest_starts_at_prev, closest_ends_at_next_type_change, focus_before, focus_after);

            Ratio prevRat;
            if (prev)
              prevRat = prev->ratio;
            if (no_prev_or_closest_starts_after_prev) {
                printf("A");
                start->beat = closest->beat;
                start->time = calc_time(&timeEvents, start->beat);
                start->ratio = closest->ratio;
            } else if (closest_starts_at_prev) {
                printf("B");
                prev->ratio = closest->ratio;
            } else if (!focus_before) { // From here onwards we will not extend to the start of closest
                if (insert > 1 && closest->beat < prevPrev->beat) {
                    // Change insert - 2 to be the current frequency instead of zero, then delete insert - 1
                    prevPrev->ratio = closest->ratio;
                    removePrevious = true;
                } else {
                    prev->beat = closest->beat;
                    prev->time = calc_time(&timeEvents, prev->beat);
                    prev->ratio = closest->ratio;
                }
            } else { // focus_before is true
                if (prevPrev && !(closest->ratio.num == 0 && closest->ratio.den == 0) && !(prevPrev->ratio.num == 0 && prevPrev->ratio.den == 0) && !(prevPrev->ratio.num == closest->ratio.num && prevPrev->ratio.den == closest->ratio.den)) {
                    prev->ratio = closest->ratio;
                } else {
                    printf("IT\n");
                    removePrevious = true;
                }
            }

            if (no_foci_or_after_foci_or_closest_ends_before_next) {
                printf("C");
                end->beat = closestOff->beat;
                end->time = calc_time(&timeEvents, end->beat);
                if (insert > 0 && prev) {
                    end->ratio = prevRat;
                }
            } else if (closest_ends_at_next_type_change) {
                printf("D");
            } else if (!focus_after) {
                // Still must check if the one after the next happens before the end of the note
                if (next_type(insertEv, EV_FOCUS) && next_type(insertEv, EV_FOCUS)->beat < closestOff->beat) {
                    removeNext = true;
                } else {
                    insertEv->beat = closestOff->beat;
                    insertEv->time = calc_time(&timeEvents, insertEv->beat);
                    insertEv->ratio = {0, 0};
                }
            } else {
                if (insertEv) {
                    if (removePrevious && prevPrev) {
                        currentFocus = prevPrev;
                    } else if (no_prev_or_closest_starts_after_prev) {
                        currentFocus = start;
                    } else {
                        currentFocus = prev;
                    }
                    if (!(currentFocus->ratio.num == 0 && currentFocus->ratio.den == 0) && !(insertEv->ratio.num == 0 && insertEv->ratio.den == 0) && !(insertEv->ratio.num == currentFocus->ratio.num && insertEv->ratio.den == currentFocus->ratio.den)) {
                        removeNext = true;
                    }
                }
            }

            bool focus_exists = insert > 0 && (!prev || (start->beat == prev->beat && start->ratio.eval() - prev->ratio.eval() < 1e-6)) && end->beat == currentFocus->beat && end->ratio.eval() - currentFocus->ratio.eval() < 1e-6;
            // printf("start: (%f, %f), end: (%f, %f)\n", start.time, start.octave, end.time, end.octave));
            if (!focus_exists) {
                if (removePrevious)
                    beatEvents.remove(prev->beat, 0);
                if (removeNext)
                    beatEvents.remove(insertEv->beat, 0);

                if (no_prev_or_closest_starts_after_prev && no_foci_or_after_foci_or_closest_ends_before_next) {
                    printf("foci start: %s, end: %s\n", start->string_expr.c_str(), end->string_expr.c_str());
                    beatEvents.insert(start);
                    start->time = calc_time(&timeEvents, start->beat);
                    beatEvents.insert(end);
                    end->time = calc_time(&timeEvents, end->beat);
                } else if (no_prev_or_closest_starts_after_prev) {
                    beatEvents.insert(start);
                    start->time = calc_time(&timeEvents, start->beat);
                } else if (no_foci_or_after_foci_or_closest_ends_before_next) {
                    beatEvents.insert(end);
                    end->time = calc_time(&timeEvents, end->beat);
                }
            }
        }

        // Rubber-band preview (after first click)
        if (placing && hovered) {
            float x0w = anchorW.x;
            float x1w = hoverW.x;
            float y   = ratio_to_octave(anchorR); // octave fixed from first click
            Vec2 a = worldToScreen(topLeft, { x0w, y });
            Vec2 b = worldToScreen(topLeft, { x1w, y });

            // addCanvasRect(renderer, topLeft, bottomRight, {a.x, a.y}, {b.x, a.y}, noteCol);
            Vec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
            Vec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
            // printf("preview: (%f, %f, %f, %f)\n", rmin.x, rmin.y, rmax.x - rmin.x, rmax.y - rmin.y);
            ui::Color nc = trackColor(heldProfile >= 0 ? heldProfile : noteTargetTrack());
            addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, 2, nc.r, nc.g, nc.b);
            // dl->AddRect(rmin, rmax, IM_COL32(80,80,200,180), 6.0f, 0, 2.0f);
        }

        // Light-red preview dot (when Snap is on and hovered)
        if ((snap_to_grid || snap_harmonics) && inNotes) {
            Vec2 pos = {hoverW.x, ratio_to_octave(hoverR)};
            printf("pos: %f, %f\n", pos.x, pos.y);
            Vec2 sd = worldToScreen(topLeft, pos);
            renderer.addCircle(sd.x, sd.y, 5.0f, 0.8f, 0.2f, 0.2f);
            if (dragging) {
                float y   = ratio_to_octave(hoverR);
                // rectangle from (x0w,y±h/2) to (x1w,y±h/2); choose fixed height (e.g., 0.6 octave)
                float halfH = 1.0/24.0;
                Vec2 a = worldToScreen(topLeft, { calc_time(&timeEvents, currently_dragged.x), y + halfH });
                Vec2 b = worldToScreen(topLeft, { calc_time(&timeEvents, currently_dragged.y), y - halfH });
                // ensure correct corners
                Vec2 rmin{ std::min(a.x,b.x), std::min(a.y,b.y) };
                Vec2 rmax{ std::max(a.x,b.x), std::max(a.y,b.y) };
                addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, rmax.y - rmin.y, 0.35, 0.63, 1.0);
                addCanvasRect(renderer, topLeft, bottomRight, rmin.x, rmin.y, rmax.x - rmin.x, rmax.y - rmin.y, 0.12, 0.24, 0.39);
            }
        }

        // Input: left-click to place/finish
        if (hovered && click == 1) {
          // A note picked up with f / l is put back even if another kind of
          // track has been selected since.
          int noteTrack = heldProfile >= 0 ? heldProfile : noteTargetTrack();
          if (inNotes && (placing || dragging || noteTrack >= 0)) {
            if (!placing) {
                // First click: start note (snap already applied above)
                if (focusAtCursor.num != 99999999 && focusAtCursor.den != 99999999)
                  originEv = findNote(beatEvents.get_first(), focusAtCursor);
                if (dragging) {
                  sprintf(anchorExpr, "%f", hoverW.y);
                  Event* e1 = new Event(currently_dragged.x, EV_NOTE_START, hoverR);
                  Event* e2 = new Event(currently_dragged.y, EV_NOTE_END);
                  e1->profile = e2->profile = noteTrack;
                  heldProfile = -1;
                  if ((int)spareIDs.size() > 0) {
                    e1->id = spareIDs[(int)spareIDs.size() - 1];
                    spareIDs.pop_back();
                    e2->id = e1->id;
                  } else {
                    e1->id = nextID;
                    e2->id = nextID++;
                  }
                  beatEvents.insert(e1);
                  beatEvents.insert(e2);
                  e1->time = calc_time(&timeEvents, e1->beat);
                  e2->time = calc_time(&timeEvents, e2->beat);
                  e1->origin = originEv;
                  beatEvents.inorder();
                  beatEvents.ordered = false;
                  dragging = false;
                  originEv = nullptr;
                  empty = false;
                } else {
                  placing = true;
                  anchorW = hoverW;
                  anchorR = hoverR;
                  sprintf(anchorExpr, "%f", hoverW.y);
                  printf("anchorExpr: %s\n", anchorExpr);
                  anchorBeat = snappedBeat;
                  anchorBeatExact= currentBeat;
                }
            } else {
                // Second click: finish note
                float a = anchorW.x, b = hoverW.x;
                if (a != b) {
                    if (snap_harmonics) {
                        // Check whether there's any focus at the anchor
                    }
                    Event* e1;
                    Event* e2;
                    if (anchorBeat > snappedBeat) {
                        e1 = new Event(snappedBeat, EV_NOTE_START, anchorR);
                        e2 = new Event(anchorBeat, EV_NOTE_END);
                    } else {
                        e1 = new Event(anchorBeat, EV_NOTE_START, anchorR);
                        e2 = new Event(snappedBeat, EV_NOTE_END);
                    }
                    e1->profile = e2->profile = noteTrack;
                    if ((int)spareIDs.size() > 0) {
                        e1->id = spareIDs[(int)spareIDs.size() - 1];
                        spareIDs.pop_back();
                        e2->id = e1->id;
                    } else {
                        e1->id = nextID;
                        e2->id = nextID++;
                    }
                    beatEvents.insert(e1);
                    printf("INSERTED\n");
                    beatEvents.insert(e2);
                    e1->time = calc_time(&timeEvents, e1->beat);
                    e2->time = calc_time(&timeEvents, e2->beat);
                    e1->origin = originEv;
                    //printf("e1->function: %f\n", e1->function.evaluate(b, t, 0, 0));
                    printf("e1 time: %f, e2 time: %f\n", e1->time, e2->time);
                    beatEvents.inorder();
                    beatEvents.ordered = false;
                    originEv = nullptr;
                    empty = false;
                }
                placing = false;
                heldProfile = -1;
            }
          } else if (hoverPercTrack >= 0 && !placing && !dragging && !closestBlip) {
            // The blip's height is its track's lane; the stored value is only
            // used by MIDI export (as an octave, 4 = C4).
            {
              Event* ev = new Event(snappedBeat, "4", EV_PERC);
              ev->profile = hoverPercTrack;
              // Unique id, so the blip can be removed without touching other
              // id-0 events (beats, foci) at the same beat
              if ((int)spareIDs.size() > 0) {
                ev->id = spareIDs.back();
                spareIDs.pop_back();
              } else {
                ev->id = nextID++;
              }
              ev->time = calc_time(&timeEvents, ev->beat);
              beatEvents.insert(ev);
              beatEvents.inorder();
            }
          }
        }

        drawSubPanels(renderer, perc, audio, nw.x1, nw.x2, topLeft);

        // Hover marker in the perc strip: where a click puts a blip
        if (hoverPercTrack >= 0 && !placing && !dragging) {
          int k = perc.laneAt(hover.y);
          float cy = perc.laneTop(k) - PERC_LANE_H / 2;
          float cx = worldToScreen(topLeft, {snappedTime, 0.0f}).x;
          if (cy > perc.bottom && cy < perc.top && cx >= nw.x1 && cx <= nw.x2)
            renderer.addCircle(cx, cy, 4.0f, 0.8f, 0.2f, 0.2f);
        }
    }

    // Draws the perc and audio strips: lanes, the rhythm splits (EV_BEAT
    // events and whole beats) so placement lines up with the notes, the
    // playhead, and the perc blips.
    void drawSubPanels(VulkanRenderer& renderer, const SubPanel& perc, const SubPanel& audio,
                       float x1, float x2, Vec2 topLeft) {
        // Divider bars between notes / perc / audio
        renderer.addRect(x1, perc.top, x2 - x1, SUB_DIVIDER, 0.35f, 0.35f, 0.38f);
        renderer.addRect(x1, audio.top, x2 - x1, SUB_DIVIDER, 0.35f, 0.35f, 0.38f);

        // Beat lines in the visible time range: (x, is a whole beat)
        std::vector<std::pair<float, bool>> beatXs;
        float t0 = std::max(0.0f, screenToWorld(topLeft, {x1, 0.0f}).x);
        float t1 = screenToWorld(topLeft, {x2, 0.0f}).x;
        if (t1 > t0) {
          int b0 = (int)std::floor(calc_beat(&timeEvents, t0));
          int b1 = (int)std::ceil(calc_beat(&timeEvents, t1));
          for (int i = std::max(b0, 0); i <= b1 && i - b0 < 2000; i++)
            beatXs.push_back({worldToScreen(topLeft, {calc_time(&timeEvents, (float)i), 0.0f}).x, true});
        }
        Event* e = beatEvents.get_first();
        if (e && e->type != EV_BEAT) e = next_type(e, EV_BEAT);
        for (; e; e = next_type(e, EV_BEAT)) {
          float x = worldToScreen(topLeft, {e->time, 0.0f}).x;
          if (x >= x1 && x <= x2) beatXs.push_back({x, false});
        }

        float playheadX = worldToScreen(topLeft, {playhead_time, 0.0f}).x;
        float loopX     = worldToScreen(topLeft, {loop_end, 0.0f}).x;
        drawPercPanel(renderer, perc, x1, x2, topLeft, beatXs, playheadX, loopX);
        drawSubPanel(renderer, audio, x1, x2, beatXs, playheadX, loopX, "No audio tracks");
    }

    // The perc strip: a small canvas like the notes area (a little lighter),
    // with a thin line per perc track carrying its blips and the same rhythm
    // lines (with notches) the notes area draws.
    void drawPercPanel(VulkanRenderer& renderer, const SubPanel& perc, float x1, float x2, Vec2 topLeft,
                       const std::vector<std::pair<float, bool>>& beatXs, float playheadX, float loopX) {
        panelRect(renderer, perc, x1, x2, x1, perc.bottom, x2 - x1, perc.height(), {0.06f, 0.06f, 0.07f});

        // Clip bounds for the canvas helpers (y-up: "top left" has the larger y)
        Vec2 ptl = {x1, perc.top}, pbr = {x2, perc.bottom};
        const ui::Color rhythm = {0.47f, 0.47f, 0.63f};

        for (int k = 0; k < (int)perc.lanes.size(); k++) {
          float cy = perc.laneTop(k) - PERC_LANE_H / 2;
          if (cy - PERC_LANE_H / 2 >= perc.top || cy + PERC_LANE_H / 2 <= perc.bottom) continue;

          ui::Color c = trackColor(perc.lanes[k]);
          panelRect(renderer, perc, x1, x2, x1, cy, x2 - x1, 1, {c.r * 0.45f, c.g * 0.45f, c.b * 0.45f});

          // Whole beats: plain rhythm lines
          for (auto [x, whole] : beatXs)
            if (whole) addCanvasRect(renderer, ptl, pbr, x, cy - 15, 1, 30, rhythm.r, rhythm.g, rhythm.b);
          // Splits: rhythm lines with their notches
          Event* e = beatEvents.get_first();
          if (e && e->type != EV_BEAT) e = next_type(e, EV_BEAT);
          for (; e; e = next_type(e, EV_BEAT)) {
            float x = worldToScreen(topLeft, {e->time, 0.0f}).x;
            if (x >= x1 && x <= x2)
              drawRhythmLine(e, {x, cy}, {e->beatL, e->beatR}, renderer, ptl, pbr);
          }
        }

        panelRect(renderer, perc, x1, x2, loopX, perc.bottom, 1, perc.height(), {0.235f, 0.235f, 0.235f});
        panelRect(renderer, perc, x1, x2, playheadX, perc.bottom, 1, perc.height(), {0.784f, 0.08f, 0.08f});
        if (split_start >= 0.0f) {
          float sx0 = worldToScreen(topLeft, {calc_time(&timeEvents, split_start), 0.0f}).x;
          panelRect(renderer, perc, x1, x2, sx0, perc.bottom, 1, perc.height(), {0.8f, 0.7f, 0.3f});
        }
        if (perc.lanes.empty() && perc.height() >= 16)
          renderer.addText("No perc tracks", x1 + 8, perc.top - 16, 13.0f, glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));

        // Blips, on their track's line
        const float blipH = 10.0f, blipW = 8.0f;
        Event* e = beatEvents.get_first();
        if (e && e->type != EV_PERC) e = next_type(e, EV_PERC);
        for (; e; e = next_type(e, EV_PERC)) {
          auto lane = std::find(perc.lanes.begin(), perc.lanes.end(), e->profile);
          if (lane == perc.lanes.end()) continue;
          float cy = perc.laneTop((int)(lane - perc.lanes.begin())) - PERC_LANE_H / 2;
          float x = worldToScreen(topLeft, {e->time, 0.0f}).x;
          if (cy - blipH / 2 < perc.bottom || cy + blipH / 2 > perc.top) continue;
          if (x < x1 || x + blipW > x2) continue;
          ui::Color c = trackColor(e->profile);
          renderer.addPercBlip(x, cy, blipH, blipW, c.r, c.g, c.b);
        }
    }
};


// --- Load binary song file ---
bool load_song(const std::string& path,
               Canvas2D* canvas,
               std::vector<Track>& tracks, uint32_t* nextID)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    char magic[4];
    f.read(magic, 4);
    if (std::strncmp(magic, "SONN", 4) != 0) {
        throw std::runtime_error("Invalid file format");
    }

    canvas->timeEvents.remove_all();
    canvas->beatEvents.remove_all();

    uint32_t eventCount;
    float v;
    char expr[40];
    int t, l, p;
    uint32_t id, numR, denR, numB, denB;
    uint32_t next_id = 0;
    // Read time events

    f.read(reinterpret_cast<char*>(&eventCount), sizeof(eventCount));

    for (int i = 0; i < (int)eventCount; i++) {
      f.read(reinterpret_cast<char*>(&v), sizeof(float));
      f.read(reinterpret_cast<char*>(&l), sizeof(int));
      f.read(expr, l);
      expr[l] = '\0';
      printf("timeEvent: %f, %s\n", v, expr);
      f.read(reinterpret_cast<char*>(&t), sizeof(t));
      Event* toInsert;
      if (l != 0)
        toInsert = canvas->createEvExpr(v, expr, t);
      else
        toInsert = canvas->createEv(v, t);
      canvas->timeEvents.insert(toInsert);
    }

    canvas->timeEvents.reparent_tree();
    canvas->timeEvents.setFinalEv();
    printf("read timeEvents:");
    canvas->timeEvents.inorder();

    // Read beat events

    printf("ready to read beatEvents");
    std::fflush(stdout);

    f.read(reinterpret_cast<char*>(&eventCount), sizeof(eventCount));
    printf("beatEvents size: %d\n", eventCount);
    if (eventCount > 0)
      empty = false;
    std::fflush(stdout);

    for (int i = 0; i < (int)eventCount; i++) {
      printf("event %d\n", i);
      std::fflush(stdout);
      f.read(reinterpret_cast<char*>(&v), sizeof(float));
      f.read(reinterpret_cast<char*>(&l), sizeof(int));
      f.read(expr, l);
      expr[l] = '\0';
      f.read(reinterpret_cast<char*>(&t), sizeof(t));
      f.read(reinterpret_cast<char*>(&id), sizeof(uint32_t));
      f.read(reinterpret_cast<char*>(&numR), sizeof(uint32_t));
      f.read(reinterpret_cast<char*>(&denR), sizeof(uint32_t));
      f.read(reinterpret_cast<char*>(&numB), sizeof(uint32_t));
      f.read(reinterpret_cast<char*>(&denB), sizeof(uint32_t));
      f.read(reinterpret_cast<char*>(&p), sizeof(int));
      printf("read: %f, %s, %d, %d, %d\n", v, expr, l, t, id);
      Event* toInsert;
      if (l != 0)
        toInsert = canvas->createEvExpr(v, expr, t);
      else
        toInsert = canvas->createEv(v, t);
      toInsert->id = id;
      printf("id: %d, toInsert->id: %d\n", id, toInsert->id);
      if (toInsert->id > next_id) {
        next_id = toInsert->id;
      }
      toInsert->num = numB;
      toInsert->den = denB;
      if (toInsert->den != 0)
        toInsert->primeFactors = getPrimeFactors(denB);
      toInsert->ratio = {numR, denR};
      toInsert->time = calc_time(&canvas->timeEvents, v);
      toInsert->profile = p;
      std::fflush(stdout);
      canvas->beatEvents.insert(toInsert);
    }
    *nextID = next_id + 1;
    canvas->beatEvents.reparent_tree();

    printf("read beatEvent\n");
    std::fflush(stdout);

    uint32_t profileCount;
    f.read(reinterpret_cast<char*>(&profileCount), sizeof(profileCount));

    if (!f || profileCount == 0 || profileCount > (uint32_t)MAX_TRACKS) {
        return false;
    }

    std::vector<Track> loaded;
    for (uint32_t i = 0; i < profileCount; i++)
        loaded.push_back(makeTrack(TrackType::Standard, i, i));

    for (Track& tr : loaded) {
        for (int j = 0; j < 3; j++) {
            OSCSettings* osc = &(tr.profile.settings[j]);

            int32_t wave;
            f.read(reinterpret_cast<char*>(&wave), sizeof(wave));
            osc->waveType = static_cast<WaveformType>(wave);

            f.read(reinterpret_cast<char*>(&osc->attack),  sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->decay),   sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->sustain), sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->release), sizeof(float));
            f.read(reinterpret_cast<char*>(&osc->gain),    sizeof(float));
        }
    }

    // Optional tagged sections. Files saved before Pd support have none, and
    // files saved before tracks have no TRKS.
    bool hasTracks = false;
    char tag[4];
    while (f.read(tag, 4)) {
        if (std::strncmp(tag, "PDPT", 4) == 0) {
            for (Track& tr : loaded) {
                uint32_t len = 0;
                if (!f.read(reinterpret_cast<char*>(&len), sizeof(len))) break;
                tr.pdPatch.assign(len, '\0');
                f.read(tr.pdPatch.data(), len);
            }
        } else if (std::strncmp(tag, "PDGN", 4) == 0) {
            for (Track& tr : loaded)
                f.read(reinterpret_cast<char*>(&tr.gainDb), sizeof(float));
        } else if (std::strncmp(tag, "TRKS", 4) == 0) {
            for (Track& tr : loaded) {
                int32_t type = 0;
                uint32_t len = 0;
                f.read(reinterpret_cast<char*>(&type), sizeof(type));
                f.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (!f || len > 1024) return false;
                tr.type = static_cast<TrackType>(std::clamp(type, 0, 2));
                tr.name.assign(len, '\0');
                f.read(tr.name.data(), len);
                f.read(reinterpret_cast<char*>(&tr.color.r), sizeof(float));
                f.read(reinterpret_cast<char*>(&tr.color.g), sizeof(float));
                f.read(reinterpret_cast<char*>(&tr.color.b), sizeof(float));
                uint8_t flags[3] = {};
                f.read(reinterpret_cast<char*>(flags), sizeof flags);
                tr.mute = flags[0]; tr.solo = flags[1]; tr.armed = flags[2];
                for (int j = 0; j < 3; j++) {
                    f.read(reinterpret_cast<char*>(&tr.profile.settings[j].freq), sizeof(float));
                    f.read(reinterpret_cast<char*>(&tr.profile.settings[j].hold), sizeof(float));
                }
            }
            hasTracks = true;
        } else {
            break;
        }
    }

    if (!hasTracks && (int)loaded.size() < MAX_TRACKS) {
        // Pre-track file: the profiles become standard tracks, and the perc
        // blips (which had no profile of their own) move to a new perc track.
        int percIndex = (int)loaded.size();
        loaded.push_back(makeTrack(TrackType::Perc, percIndex, 0));
        loaded.back().pdPatch = percPatchName(1);
        for (Event* e = canvas->beatEvents.get_first(); e; e = next(e))
            if (e->type == EV_PERC) e->profile = percIndex;
    }

    // Events pointing at a track that doesn't exist go to the first one, and
    // perc blips and rhythm splits from older files get ids of their own
    for (Event* e = canvas->beatEvents.get_first(); e; e = next(e)) {
        if (e->profile < 0 || e->profile >= (int)loaded.size()) e->profile = 0;
        if ((e->type == EV_PERC || e->type == EV_BEAT) && e->id == 0) e->id = (*nextID)++;
    }

    tracks = std::move(loaded);
    f.close();
    return true;
}

// Draws a frame `m` pixels wide around the note window (nw is y-up: y1 = bottom, y2 = top)
void drawMargin(VulkanRenderer& renderer, CurrentBox& nw, float m = 5.0f) {
    const float c = 0.07f;
    const float w = nw.x2 - nw.x1, h = nw.y2 - nw.y1;
    renderer.addRect(nw.x1 - m, nw.y2,     w + 2 * m, m, c, c, c);  // top
    renderer.addRect(nw.x1 - m, nw.y1 - m, w + 2 * m, m, c, c, c);  // bottom
    renderer.addRect(nw.x1 - m, nw.y1,     m, h,         c, c, c);  // left
    renderer.addRect(nw.x2,     nw.y1,     m, h,         c, c, c);  // right
}

static Canvas2D gBeatOctCanvas;

static const char* waveName(WaveformType w) {
    switch (w) {
        case WaveformType::Sine:     return "Sine";
        case WaveformType::Square:   return "Square";
        case WaveformType::Saw:      return "Saw";
        case WaveformType::Triangle: return "Triangle";
        default:                     return "Noise";
    }
}

static WaveformType nextWave(WaveformType w) {
    switch (w) {
        case WaveformType::Sine:     return WaveformType::Square;
        case WaveformType::Square:   return WaveformType::Saw;
        case WaveformType::Saw:      return WaveformType::Triangle;
        case WaveformType::Triangle: return WaveformType::Noise;
        default:                     return WaveformType::Sine;
    }
}

// Track panel geometry, in pixels
constexpr float TRACK_PANEL_W  = 230.0f;
constexpr float TRACK_ROW_H    = 46.0f;
constexpr float TRACK_HEADER_H = 22.0f;
constexpr float TRACK_FOOTER_H = 26.0f;

// Everything the control panel edits lives here. Sliders hold pointers into
// `profile` and into g_tracks, so an App must not be moved or copied after
// buildUI(), and the track rows are rebuilt (tracksChanged) whenever g_tracks
// is resized or replaced.
struct App {
    AudioEngine& engine;
    ui::UI       ui;
    ui::Widget*  noteWindow = nullptr;
    ui::Widget*  trackList  = nullptr;   // holds one row per track
    bool         tracksChanged = true;   // rebuild the track rows before the next draw
    float        trackScroll = 0.0f;     // pixels of rows scrolled off the top
    std::vector<ui::Widget*> nameBoxes;  // per row: text box used while renaming
    int          renaming = -1;          // track whose name is being edited
    std::string  nameText;               // the name being typed
    int          deleteArmed = -1;       // track whose delete button was clicked once
    int          deleteArmedBefore = -1; // deleteArmed as it was before the current click

    Profile     profile;                 // synth settings of the current track
    float       bpm = 110.0f;
    std::string freqText[3];             // percussion freq text boxes
    std::string pdPatchText;             // Pd patch name for the current track

    explicit App(AudioEngine& e) : engine(e) {}
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    Track& track() { return g_tracks[g_currentTrack]; }

    void sendProfile() {
        track().profile = profile;
        engine.setProfile(profile, g_currentTrack);
    }

    // Makes `index` the current track; every bound widget updates automatically.
    void selectTrack(int index) {
        if (index < 0 || index >= (int)g_tracks.size()) return;
        g_currentTrack = index;
        profile = track().profile;
        for (int k = 0; k < 3; k++) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", profile.settings[k].freq);
            freqText[k] = buf;
        }
        pdPatchText = track().pdPatch;
        scrollToTrack(index);
    }

    void addTrack(TrackType type) {
        if ((int)g_tracks.size() >= MAX_TRACKS) return;
        int ofType = (int)std::count_if(g_tracks.begin(), g_tracks.end(),
                                        [type](const Track& t) { return t.type == type; });
        int index = (int)g_tracks.size();
        Track t = makeTrack(type, index, ofType);
        if (type == TrackType::Perc) t.pdPatch = freePercPatchName(g_tracks);
        g_tracks.push_back(t);
        engine.setProfile(t.profile, index);
        loadPatch(index);
        applyTrackGains();
        tracksChanged = true;
        selectTrack(index);
    }

    // Swaps two tracks: their rows, their notes and blips, and their Pd slots.
    void swapTracks(int a, int b) {
        if (a == b) return;
        std::swap(g_tracks[a], g_tracks[b]);
        auto remap = [a, b](int& t) { if (t == a) t = b; else if (t == b) t = a; };
        for (Event* e = gBeatOctCanvas.beatEvents.get_first(); e; e = next(e))
            remap(e->profile);
        remap(g_currentTrack);
        remap(gBeatOctCanvas.heldProfile);
        engine.pd.swapSlots(a, b);
        engine.setProfile(g_tracks[a].profile, a);
        engine.setProfile(g_tracks[b].profile, b);
        tracksChanged = true;
    }

    // Moves the current track up (-1) or down (+1) the list.
    void moveTrack(int dir) {
        int a = g_currentTrack, b = a + dir;
        if (b < 0 || b >= (int)g_tracks.size()) return;
        swapTracks(a, b);
        applyTrackGains();
        selectTrack(b);
    }

    // Deletes a track along with its notes and blips. The last track stays.
    void deleteTrack(int i) {
        int n = (int)g_tracks.size();
        if (n <= 1 || i < 0 || i >= n) return;
        Canvas2D& c = gBeatOctCanvas;

        std::vector<Event*> removed;
        for (Event* e = c.beatEvents.get_first(); e; e = next(e)) {
            if (e->profile != i) continue;
            if (e->type == EV_PERC) {
                removed.push_back(e);
            } else if (e->type == EV_NOTE_START) {
                removed.push_back(e);
                Event* end = next(e);
                while (end && !(end->type == EV_NOTE_END && end->id == e->id)) end = next(end);
                if (end) removed.push_back(end);
            }
        }
        for (Event* e : removed) c.beatEvents.remove(e->beat, e->id);
        c.forgetEvents(removed);
        if (c.heldProfile == i) {   // a note of this track was picked up with f / l
            c.heldProfile = -1;
            c.placing = c.dragging = false;
        }

        int select = g_currentTrack > i ? g_currentTrack - 1 : g_currentTrack;
        for (int k = i; k < n - 1; k++) swapTracks(k, k + 1);   // move it to the end...
        g_tracks.pop_back();                                     // ...and drop it
        engine.pd.setPatch(n - 1, "");
        applyTrackGains();
        tracksChanged = true;
        selectTrack(std::min(select, n - 2));
    }

    void startRename(int i) {
        if (i < 0 || i >= (int)nameBoxes.size()) return;
        renaming = i;
        nameText = g_tracks[i].name;
        ui.focused = nameBoxes[i];
    }

    void finishRename() {
        if (renaming >= 0 && renaming < (int)g_tracks.size() && !nameText.empty())
            g_tracks[renaming].name = nameText;
        renaming = -1;
    }

    // Loads track i's Pd patch, first writing the starter perc patch for a
    // perc track whose file doesn't exist yet.
    bool loadPatch(int i) {
        const Track& t = g_tracks[i];
        if (t.type == TrackType::Perc && !t.pdPatch.empty())
            engine.pd.ensurePercPatch(t.pdPatch, percVariant(t.pdPatch));
        return engine.pd.setPatch(i, t.pdPatch);
    }

    // Mute and solo are applied through each track's Pd gain: while any track
    // is soloed only soloed tracks are heard, and a muted track never is.
    void applyTrackGains() {
        bool anySolo = std::any_of(g_tracks.begin(), g_tracks.end(),
                                   [](const Track& t) { return t.solo; });
        for (int i = 0; i < (int)g_tracks.size(); i++) {
            const Track& t = g_tracks[i];
            bool audible = !t.mute && (!anySolo || t.solo);
            engine.pd.setGain(i, audible ? t.gainDb : PdHost::MIN_DB);
        }
    }

    // ---- Track list scrolling (only whole rows are shown) ----
    int visibleRows() const {
        return trackList ? (int)std::floor(trackList->screen.h / TRACK_ROW_H) : 0;
    }

    void scrollTracks(int rows) {
        int maxFirst = std::max(0, (int)g_tracks.size() - visibleRows());
        int first = std::clamp((int)std::lround(trackScroll / TRACK_ROW_H) + rows, 0, maxFirst);
        trackScroll = first * TRACK_ROW_H;
    }

    void scrollToTrack(int index) {
        int rows = visibleRows();
        if (rows <= 0) return;   // not laid out yet
        int first = (int)std::lround(trackScroll / TRACK_ROW_H);
        if (index < first)              first = index;
        else if (index >= first + rows) first = index - rows + 1;
        trackScroll = first * TRACK_ROW_H;
    }

    // Stores the typed patch name in the current track and loads it.
    void commitPdPatch() {
        if (track().type == TrackType::Audio) return;
        track().pdPatch = pdPatchText;
        loadPatch(g_currentTrack);
    }

    void editPdPatch() {
        commitPdPatch();
        engine.pd.openEditor(g_currentTrack, pdPatchText);
    }

    // Pushes every track's settings to the engine, and unloads the Pd slots
    // of tracks that no longer exist.
    void syncEngine() {
        int loaded = 0, failed = 0;
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (i < (int)g_tracks.size()) {
                const Track& t = g_tracks[i];
                engine.setProfile(t.profile, i);
                bool ok = loadPatch(i);
                if (!t.pdPatch.empty()) (ok ? loaded : failed)++;
            } else {
                engine.pd.setPatch(i, "");
            }
        }
        applyTrackGains();
        engine.pd.status = std::to_string(loaded) + " Pd patch(es) loaded" +
                           (failed ? ", " + std::to_string(failed) + " missing" : "");
    }

    void exportWav() {
        float length = 0.0f;
        for (Event* e = gBeatOctCanvas.beatEvents.get_first(); e; e = next_note(e))
            length = e->time;
        if (length > 0.0f) {
            engine.setEvents(&gBeatOctCanvas.beatEvents);
            SDL_Delay(50);
            engine.exportWav(g_settings.wavName.data(), playhead_time, length);
        }
    }

    void saveProject() {
        save_song("session.son", gBeatOctCanvas.beatEvents, gBeatOctCanvas.timeEvents, g_tracks);
    }

    void loadProject() {
        load_song("session.son", &gBeatOctCanvas, g_tracks, &gBeatOctCanvas.nextID);
        g_tracks.reserve(MAX_TRACKS);
        for (Track& t : g_tracks)   // perc tracks always start with a patch
            if (t.type == TrackType::Perc && t.pdPatch.empty()) t.pdPatch = freePercPatchName(g_tracks);
        gBeatOctCanvas.beatEvents.inorder();
        gBeatOctCanvas.beatEvents.find_loop();
        syncEngine();
        tracksChanged = true;
        trackScroll = 0.0f;
        selectTrack(std::min(g_currentTrack, (int)g_tracks.size() - 1));   // refresh the panel
    }
};

// One oscillator column. The three columns differ only in x and index.
static void addOscColumn(ui::Widget& cw, App& app, int k, float x) {
    using namespace ui;
    OSCSettings& s = app.profile.settings[k];
    auto send     = [&app](Widget&) { app.sendProfile(); };
    auto percOnly = [] { return currentTrackType() == TrackType::Perc; };
    std::string name = "OSC" + std::to_string(k + 1);

    cw.add(button({x, 10, 120, 15}, "", [&app, k](Widget&) {
        auto& wt = app.profile.settings[k].waveType;
        wt = nextWave(wt);
        app.sendProfile();
    }))->labelFn = [&app, k, name] {
        return name + ": " + waveName(app.profile.settings[k].waveType);
    };

    cw.add(slider({x,  30, 120, 15}, "Attack",  &s.attack,  0, 3, 1, send));
    cw.add(slider({x,  50, 120, 15}, "Decay",   &s.decay,   0, 3, 1, send));
    cw.add(slider({x,  70, 120, 15}, "Sustain", &s.sustain, 0, 1, 1, send));
    cw.add(slider({x,  90, 120, 15}, "Release", &s.release, 0, 3, 1, send));
    cw.add(slider({x, 110, 120, 15}, "Gain",    &s.gain,    0, 1, 5, send));
    cw.add(slider({x, 130, 120, 15}, "Hold",    &s.hold,    0, 1, 3, send))
        ->visibleFn = percOnly;

    cw.add(textBox({x + 50, 150, 70, 15}, "Freq:", &app.freqText[k], [&app, k](Widget&) {
        if (app.freqText[k].empty()) return;
        app.profile.settings[k].freq = (float)std::atof(app.freqText[k].c_str());
        app.sendProfile();
    }, /*labelLeft=*/true, {0.8f, 0.7f, 0.3f}))->visibleFn = percOnly;
}

static std::string gainLabel(float db) {
    if (db <= PdHost::MIN_DB) return "Gain: -inf dB";
    char buf[32];
    std::snprintf(buf, sizeof buf, "Gain: %+.1f dB", db);
    return buf;
}

// Small toggle button on a track row (mute / solo / record).
static void addTrackToggle(ui::Widget& row, App& app, float x, const char* label,
                           bool Track::*flag, ui::Color on, int i) {
    using namespace ui;
    Widget* w = row.add(button({x, 3, 19, 16}, label, [&app, flag, i](Widget&) {
        g_tracks[i].*flag = !(g_tracks[i].*flag);
        app.applyTrackGains();
    }));
    w->textColor = {1, 1, 1};
    w->colorFn = [flag, on, i] {
        return g_tracks[i].*flag ? on : Color{0.30f, 0.30f, 0.33f};
    };
}

// (Re)creates one row per track. Called before drawing whenever the track
// list changed, never from inside a widget callback (it frees the widgets).
static void rebuildTrackRows(App& app) {
    using namespace ui;
    Widget* list = app.trackList;
    if (app.renaming >= 0) {   // the box being typed into is about to go
        app.ui.focused = nullptr;
        app.renaming = -1;
    }
    list->children.clear();
    app.nameBoxes.clear();
    app.tracksChanged = false;

    const float rowW = TRACK_PANEL_W - 8.0f;
    for (int i = 0; i < (int)g_tracks.size(); i++) {
        auto select = [&app, i](Widget&) { app.selectTrack(i); };

        // The row itself is a button, so clicking anywhere on it selects the track
        Widget* row = list->add(button({}, "", select));
        row->layoutFn = [&app, i](const Rect& p) {
            return Rect{p.x + 4, p.y + i * TRACK_ROW_H - app.trackScroll + 2,
                        p.w - 8, TRACK_ROW_H - 4};
        };
        row->visibleFn = [&app, list, i] {
            float top = i * TRACK_ROW_H - app.trackScroll;
            return top >= -0.5f && top + TRACK_ROW_H <= list->screen.h + 0.5f;
        };
        row->colorFn = [i] {
            return i == g_currentTrack ? Color{0.25f, 0.25f, 0.30f} : Color{0.14f, 0.14f, 0.16f};
        };

        // Colour strip down the left edge
        row->add(button({0, 0, 6, TRACK_ROW_H - 4}, "", select))
            ->colorFn = [i] { return g_tracks[i].color; };

        // "<hotkey>  <name>". Double-click to rename.
        Widget* name = row->add(button({10, 3, rowW - 100, 16}, "", select));
        name->fill = false;
        name->textColor = {1, 1, 1};
        name->labelFn = [&app, i] {
            if (app.deleteArmed == i) return std::string("Delete? Click x again");
            std::string key = i < 9 ? std::to_string(i + 1) : i == 9 ? "0" : " ";
            return key + "  " + g_tracks[i].name;
        };
        name->visibleFn = [&app, i] { return app.renaming != i; };
        name->onDoubleClick = [&app, i](Widget&) { app.startRename(i); };
        row->onDoubleClick = name->onDoubleClick;

        Widget* box = row->add(textBox({10, 3, rowW - 100, 16}, "", &app.nameText));
        box->maxLen = 16;
        box->textColor = {1, 1, 1};
        box->colorFn = [] { return Color{0.14f, 0.14f, 0.16f}; };
        box->visibleFn = [&app, i] { return app.renaming == i; };
        box->onCommit = [&app](Widget&) { app.finishRename(); };
        app.nameBoxes.push_back(box);

        addTrackToggle(*row, app, rowW - 88, "M", &Track::mute, {0.85f, 0.55f, 0.15f}, i);
        addTrackToggle(*row, app, rowW - 66, "S", &Track::solo, {0.25f, 0.55f, 0.85f}, i);
        if (g_tracks[i].type == TrackType::Audio)
            addTrackToggle(*row, app, rowW - 44, "R", &Track::armed, {0.85f, 0.15f, 0.15f}, i);

        // Delete: the first click arms it, a second click deletes
        Widget* del = row->add(button({rowW - 22, 3, 19, 16}, "x", [&app, i](Widget&) {
            if (app.deleteArmedBefore == i) app.deleteTrack(i);
            else app.deleteArmed = i;
        }));
        del->textColor = {1, 1, 1};
        del->colorFn = [&app, i] {
            return app.deleteArmed == i ? Color{0.85f, 0.15f, 0.15f} : Color{0.30f, 0.30f, 0.33f};
        };

        Widget* gain = row->add(slider({10, 23, rowW - 16, 15}, "Gain", &g_tracks[i].gainDb,
                                       PdHost::MIN_DB, 12.0f, 1.0f,
                                       [&app](Widget&) { app.applyTrackGains(); }));
        gain->labelFn = [i] { return gainLabel(g_tracks[i].gainDb); };
    }
}

static void buildUI(App& app) {
    using namespace ui;
    Widget& root = app.ui.root;

    constexpr float MENU_BAR_H = 22.0f;

    // Everything below the menu bar. An invisible container, so the old
    // 75% / 25% split now applies to the space left after the menu bar.
    Widget* content = root.add(panelFn([](const Rect& p) {
        return Rect{p.x, p.y + MENU_BAR_H, p.w, p.h - MENU_BAR_H};
    }, {}, /*fill=*/false));

    // Area under the control panel: track panel on the left, note window on the right
    auto below = [](const Rect& p) {
        float top = p.y + p.h * CW_HEIGHT;
        return Rect{p.x, top, p.w, p.y + p.h - top};
    };

    // Note window. Canvas2D draws inside its screen rect.
    // Added first so the control panel draws on top of it, as before.
    app.noteWindow = content->add(panelFn([below](const Rect& p) {
        Rect r = below(p);
        return Rect{r.x + TRACK_PANEL_W, r.y, r.w - TRACK_PANEL_W, r.h};
    }, {0, 0, 0}));
    app.noteWindow->kind  = Kind::Canvas;
    app.noteWindow->inset = 5.0f;

    // Track panel
    Widget* tracks = content->add(panelFn([below](const Rect& p) {
        Rect r = below(p);
        return Rect{r.x, r.y, TRACK_PANEL_W, r.h};
    }, {0.08f, 0.08f, 0.09f}));

    Widget* title = tracks->add(button({8, 3, 100, 16}, "Tracks", {}));
    title->fill = false;
    title->textColor = {0.8f, 0.8f, 0.8f};

    app.trackList = tracks->add(panelFn([](const Rect& p) {
        return Rect{p.x, p.y + TRACK_HEADER_H, p.w, p.h - TRACK_HEADER_H - TRACK_FOOTER_H};
    }, {}, /*fill=*/false));

    const struct { const char* label; TrackType type; } adds[] = {
        {"+ Standard", TrackType::Standard},
        {"+ Audio",    TrackType::Audio},
        {"+ Perc",     TrackType::Perc},
    };
    for (int k = 0; k < 3; k++) {
        TrackType type = adds[k].type;
        Widget* b = tracks->add(button({}, adds[k].label,
                                       [&app, type](Widget&) { app.addTrack(type); },
                                       {0.20f, 0.20f, 0.22f}));
        b->textColor = {1, 1, 1};
        b->layoutFn = [k](const Rect& p) {
            float w = (p.w - 16) / 3;
            return Rect{p.x + 4 + k * (w + 4), p.y + p.h - TRACK_FOOTER_H + 4, w, 18};
        };
    }

    // Top: grey border panel, with the dark control window inset inside it
    Widget* border = content->add(panel({0.0f, 0.0f, 1.0f, CW_HEIGHT},
                                        {0.15f, 0.15f, 0.15f}, 2.0f, /*relative=*/true));
    Widget* cw = border->add(panel({0, 0, 1, 1}, {0.01f, 0.03f, 0.07f}, 3.0f, true));
 
    // ---- Menu bar (added after `content` so it's on top) ----
    Widget* menuBar = root.add(panelFn([](const Rect& p) {
        return Rect{p.x, p.y, p.w, MENU_BAR_H};
    }, {0.10f, 0.10f, 0.10f}));
 
    // File menu. Item positions are relative to the "File" header.
    Widget* file = menuBar->add(menu({4, 2, 40, 18}, "File"));
    file->add(menuItem({0, 22, 150, 18}, "Export WAV",   [&app](Widget&) { app.exportWav(); }));
    file->add(menuItem({0, 42, 150, 18}, "Save project", [&app](Widget&) { app.saveProject(); }));
    file->add(menuItem({0, 62, 150, 18}, "Load project", [&app](Widget&) { app.loadProject(); }));
    file->add(textBox({0, 102, 150, 18}, "Wav name:", &g_settings.wavName,
                      {}, false, {0.85f, 0.85f, 0.85f}));
    file->add(menuItem({0, 122, 150, 18}, "Export MIDI", [](Widget&) {
    export_midi("export.mid", gBeatOctCanvas.beatEvents, gBeatOctCanvas.timeEvents);
}));

    // ---- Control panel, left column ----
    cw->add(slider({10, 10, 120, 15}, "BPM", &app.bpm, 20, 200, 1,
                   [&app](Widget&) { app.engine.setTempo(app.bpm); }))
        ->labelFn = [&app] { return "BPM: " + std::to_string((int)app.bpm); };

    cw->add(textBox({10, 45, 120, 15}, "Ref. Freq.:", &g_settings.refFreq, [&app](Widget&) {
        if (g_settings.refFreq.empty()) return;
        ref = (float)std::atof(g_settings.refFreq.c_str());
        app.engine.setFrequency(ref);
    }));
 
    cw->add(textBox({140, 25, 120, 15}, "Jump size:", &g_settings.jump, [&app](Widget&) {
        std::string tmp = g_settings.jump;
        char* p = tmp.data();
        bool noVal = false;
        int num = getNum(p, noVal);
        p += 1;
        int den = getNum(p, noVal);
        if (den != 0) app.engine.setJump((float)num / (float)den);
    }));
    cw->add(textBox({140, 60, 120, 15}, "Split size:", &g_settings.split));

    cw->add(textBox({270, 25, 120, 15}, "Harm. Range: ",   &g_settings.harmRange));
    cw->add(textBox({270, 60, 120, 15}, "Alt. H. Range: ", &g_settings.altHarmRange));

    // Pd patch for the current track (not audio tracks)
    auto standardOnly = [] { return currentTrackType() != TrackType::Audio; };
    Widget* pdBox = cw->add(textBox({400, 25, 150, 15}, "Pd patch:", &app.pdPatchText));
    pdBox->maxLen = 63;
    pdBox->onCommit = [&app](Widget&) { app.commitPdPatch(); };
    pdBox->visibleFn = standardOnly;
    cw->add(button({400, 45, 150, 15}, "Edit in Pd", [&app](Widget&) { app.editPdPatch(); }))
        ->visibleFn = standardOnly;
    Widget* pdStatus = cw->add(button({560, 25, 300, 15}, "", {}));   // read-only status line
    pdStatus->fill = false;
    pdStatus->textColor = {0.7f, 0.7f, 0.7f};
    pdStatus->labelFn = [&app] { return app.engine.pd.status; };
    pdStatus->visibleFn = standardOnly;

    // Oscillator columns
    //for (int k = 0; k < 3; k++) addOscColumn(*cw, app, k, 140.0f + 130.0f * k);
}

int main(int, char**)
{
    computeComplexityOrder(20);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return 1;
    }
 
    SDL_Window* window = SDL_CreateWindow(
        "Sonnote",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
 
    VulkanRenderer renderer = VulkanRenderer(window);
 
    AudioEngine engine;
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine." << std::endl;
        return 1;
    }
    std::cout << "Audio engine started." << std::endl;
 
    gBeatOctCanvas.setBPM("110", 0.0f);
 
    engine.setFrequency(440.0f);
    engine.setGain(0.4f);
    engine.setWaveform(WaveformType::Sine);
    engine.setTempo(120.0f);
    engine.setADSR({0.2f, 0.2f, 0.7f, 0.025f});   // same values the old buttons produced
 
    App app(engine);
    buildUI(app);
    g_tracks.reserve(MAX_TRACKS);
    for (int i = 0; i < 4; i++) app.addTrack(TrackType::Standard);
    app.addTrack(TrackType::Perc);
    app.syncEngine();
    app.selectTrack(0);
 
    renderer.createSpectrogramPipeline();
 
    bool running = true;
    bool playing = false;
    bool rerender = true;
    bool ctrl = false;
    int click = 0;
    int dx = 0, dy = 0;
    int sens = 15;
    bool windowActive = true;
    Vec2 hover = {-1.0f, -1.0f};
    std::vector<char> keys;
    float inputFreqSum = 0.0f;
    int inputFreqi = 0;
    STFTMag res;
 
    while (running) {
        if (newAudioIn) {
            std::vector<float> v(audioIn, audioIn + audioBufferSize);
            res = computeSTFT_FFTW(v, engine.sampleRate, audioBufferSize);
            float resFreq = (engine.sampleRate * res.highBin) / audioBufferSize;
            if (!isnan(resFreq)) {
                inputFreqSum += resFreq;
                inputFreqi++;
                if (inputFreqi == 3) {
                    inputFreq = inputFreqSum / 3;
                    rerender = true;
                    inputFreqi = 0;
                    inputFreqSum = 0;
                }
            }
            newAudioOut = true;
            newAudioIn = false;
        }
 
        SDL_Event e;
        keys.clear();
        if (ctrl) keys.push_back('~');
        dx = dy = 0;
 
        while (SDL_PollEvent(&e)) {
            rerender = true;
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
 
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) windowActive = true;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)   windowActive = false;
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) renderer.onResize(window);
                if (e.window.event == SDL_WINDOWEVENT_CLOSE &&
                    e.window.windowID == SDL_GetWindowID(window)) running = false;
                break;
 
            case SDL_MOUSEBUTTONDOWN:
               if (e.button.button == SDL_BUTTON_LEFT) {
                 // Any click disarms a track's delete button; clicking it again
                 // right away (see rebuildTrackRows) deletes the track.
                 app.deleteArmedBefore = app.deleteArmed;
                 app.deleteArmed = -1;
                 if (!app.ui.mouseDown((float)e.button.x, (float)e.button.y, e.button.clicks))
                   click = 1;
               } else if (e.button.button == SDL_BUTTON_RIGHT) {
                 if (app.ui.openMenu) app.ui.closeMenu();
                 else click = 2;
               }
               break;
 
            case SDL_MOUSEBUTTONUP:
                click = 0;
                break;
 
            case SDL_KEYDOWN: {
                SDL_Keycode sym = e.key.keysym.sym;
                char key = sym;
                if (key == SDLK_ESCAPE) {
                  if (app.ui.focused) app.ui.blur();
                  else if (app.ui.openMenu) app.ui.closeMenu();
                  else running = false;
                  break;
                }
                if (sym == SDLK_LCTRL) { ctrl = true; break; }

                // Up / Down grow / shrink the perc or audio strip under the
                // cursor; with Shift they move the current track instead.
                // (Arrow keycodes don't fit in a char, so keep them away from
                // the text boxes and keys.)
                if (sym == SDLK_UP || sym == SDLK_DOWN || sym == SDLK_LEFT || sym == SDLK_RIGHT) {
                  app.deleteArmed = -1;
                  if (!app.ui.focused && (sym == SDLK_UP || sym == SDLK_DOWN)) {
                    int dir = sym == SDLK_UP ? 1 : -1;
                    if (e.key.keysym.mod & KMOD_SHIFT) app.moveTrack(-dir);
                    else gBeatOctCanvas.resizeSubPanel(dir);
                  }
                  break;
                }

                // Typing into a text box shouldn't also trigger shortcuts
                char typed = key;
                if ((e.key.keysym.mod & KMOD_SHIFT) && typed >= 'a' && typed <= 'z') typed -= 'a' - 'A';
                if (app.ui.keyDown(typed)) break;
 
                keys.push_back(key);
                if (hotkeyTrack(key) != -1) {
                    app.selectTrack(hotkeyTrack(key));
                } else if (key == 'n') {
                    engine.toggleFreeze();
                }
                break;
            }
 
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_LCTRL) ctrl = false;
                break;
 
            case SDL_MOUSEMOTION: {
                int dw, dh;
                SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
                float mx = (float)e.motion.x, my = (float)e.motion.y;
                if (!app.ui.openMenu && app.noteWindow->screen.contains(mx, my))
                    hover = {mx, (float)dh - my};       // canvas expects y-up pixels
                else
                    hover = {-1.0f, -1.0f};
                break;
            }
 
            case SDL_MOUSEWHEEL: {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                if (!app.ui.openMenu && app.trackList->screen.contains((float)mx, (float)my)) {
                    int steps = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? e.wheel.y : -e.wheel.y;
                    app.scrollTracks(steps);
                    break;
                }
                dx = e.wheel.x * sens;
                dy = e.wheel.y * sens;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) { dx = -dx; dy = -dy; }
                break;
            }
            }
        }
 
        if (play_triggered_from_space) {
            play_triggered_from_space = false;
            if (!playing) {
                engine.setEvents(&(gBeatOctCanvas.beatEvents));
                engine.resetSampleCounter(calc_beat(&gBeatOctCanvas.timeEvents, playhead_time),
                                          calc_beat(&gBeatOctCanvas.timeEvents, loop_end));
                engine.setPlayState(true);
            } else {
                engine.setPlayState(false);
            }
            playing = !playing;
        }
 
        if (tuner_triggered) {
            engine.toggleTuner();
            tuner_triggered = false;
        }

        {   // reload patches whose Pd editor window was closed
            std::string before = engine.pd.status;
            engine.pd.poll();
            if (engine.pd.status != before) rerender = true;
        }
 
        if (windowActive && rerender) {
            renderer.vertices.clear();
            int dw, dh;
            SDL_Vulkan_GetDrawableSize(window, &dw, &dh);
 
            if (app.tracksChanged) rebuildTrackRows(app);
            app.ui.layout((float)dw, (float)dh);
            app.ui.draw(renderer, (float)dh);
 
            const ui::Rect& s = app.noteWindow->screen;
            printf("nws: {%f, %f, %f, %f}\n", s.x, s.y, s.w, s.h);
            CurrentBox screenNw{ s.x, (float)dh - (s.y + s.h), s.x + s.w, (float)dh - s.y };
 
            char* jump  = g_settings.jump.data();
            char* split = g_settings.split.data();
            gBeatOctCanvas.draw(renderer, screenNw, hover, click, keys, jump, split, dx, dy);
            drawMargin(renderer, screenNw);
            renderer.nextLayer();
            app.ui.drawOverlay(renderer, (float)dh);   // dropdowns on top of everything
 
            renderer.updateVertexData();
            renderer.drawFrame(std::floor(screenNw.x1), std::floor(screenNw.y1),
                               std::floor(screenNw.x2 - screenNw.x1),
                               std::floor(screenNw.y2 - screenNw.y1));
            rerender = false;
        } else {
            SDL_Delay(50);
        }
    }
 
    engine.stop();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
