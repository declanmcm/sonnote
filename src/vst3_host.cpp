#include "vst3_host.hpp"

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <poll.h>

// Last: Xlib's macros (None, Bool, Status, ...) would clash with the SDK
#include <X11/Xlib.h>
#include <X11/Xutil.h>

using namespace Steinberg;
using namespace Steinberg::Vst;
namespace fs = std::filesystem;

namespace vst3 {

const char* tuningName(TuningMode m) {
    switch (m) {
        case TuningMode::Mpe48: return "MPE +-48";
        case TuningMode::Mpe2:  return "MPE +-2";
        default:                return "Per-note";
    }
}

// ======================================================
// ================== MODULES AND SCAN ==================
// ======================================================

// The host context every plugin is initialised with
static HostApplication* hostContext() {
    static HostApplication* host = [] {
        auto* h = new HostApplication;
        PluginContextFactory::instance().setPluginContext(h);
        return h;
    }();
    return host;
}

// Loaded .vst3 modules, kept for the life of the app (a module can't be
// unloaded while any of its plugins exists, so they never are).
static std::map<std::string, VST3::Hosting::Module::Ptr>& modules() {
    static std::map<std::string, VST3::Hosting::Module::Ptr> m;
    return m;
}

static VST3::Hosting::Module::Ptr loadModule(const std::string& path, std::string& error) {
    auto& m = modules();
    auto it = m.find(path);
    if (it != m.end()) return it->second;
    auto mod = VST3::Hosting::Module::create(path, error);
    if (mod) m[path] = mod;
    return mod;
}

static bool isInstrumentClass(const VST3::Hosting::ClassInfo& c) {
    return c.subCategoriesString().find(PlugType::kInstrument) != std::string::npos;
}

const std::vector<PluginInfo>& plugins() {
    static std::vector<PluginInfo> list;
    static bool scanned = false;
    if (scanned) return list;
    scanned = true;
    for (const std::string& path : VST3::Hosting::Module::getModulePaths()) {
        std::string error;
        auto mod = loadModule(path, error);
        if (!mod) {
            std::fprintf(stderr, "[vst3] skipping %s: %s\n", path.c_str(), error.c_str());
            continue;
        }
        for (const auto& c : mod->getFactory().classInfos())
            if (c.category() == kVstAudioEffectClass)
                list.push_back({path, c.name(), isInstrumentClass(c)});
    }
    return list;
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ======================================================
// =============== RUN LOOP AND WINDOWS =================
// ======================================================
// Plugins on Linux ask the host's IPlugFrame for an IRunLoop and register
// file descriptors and timers with it, to be called on the main thread.

namespace {

struct FdEntry    { Linux::IEventHandler* handler; int fd; };
struct TimerEntry {
    Linux::ITimerHandler* handler;
    std::chrono::milliseconds interval;
    std::chrono::steady_clock::time_point next;
};
std::vector<FdEntry>    g_fds;
std::vector<TimerEntry> g_timers;

Display* g_display = nullptr;
Atom     g_wmDelete = 0;

Display* display() {
    if (!g_display) {
        g_display = XOpenDisplay(nullptr);   // XWayland under Wayland
        if (g_display) g_wmDelete = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    }
    return g_display;
}

std::map<::Window, Plugin::Impl*> g_editors;   // open editor windows

} // namespace

// One plugin's window: the IPlugFrame its view talks to, which also serves
// as the run loop.
class Frame : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame, Linux::IRunLoop>> {
public:
    Plugin::Impl* owner = nullptr;

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override;

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override {
        if (!handler) return kInvalidArgument;
        g_fds.push_back({handler, fd});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
        g_fds.erase(std::remove_if(g_fds.begin(), g_fds.end(),
                                   [handler](const FdEntry& e) { return e.handler == handler; }),
                    g_fds.end());
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override {
        if (!handler) return kInvalidArgument;
        auto interval = std::chrono::milliseconds(std::max<Linux::TimerInterval>(ms, 1));
        g_timers.push_back({handler, interval, std::chrono::steady_clock::now() + interval});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
        g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(),
                                      [handler](const TimerEntry& e) { return e.handler == handler; }),
                       g_timers.end());
        return kResultTrue;
    }
};

// Edits from the plugin's window, which go to its processor
class ComponentHandler : public U::ImplementsNonDestroyable<U::Directly<IComponentHandler>> {
public:
    Plugin::Impl* owner = nullptr;
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};

// ======================================================
// ======================= PLUGIN =======================
// ======================================================

struct Plugin::Impl {
    std::string name, query;
    bool instrument = false;
    double sampleRate = 48000.0;
    int maxBlock = 1024;
    ProcessSetup setup{};

    IPtr<PlugProvider>    provider;
    IPtr<IComponent>      component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;
    ComponentHandler      handler;

    // Audio buses: every bus gets buffers (the spec wants them even for
    // inactive buses); only the main ones are used.
    struct Bus {
        std::vector<std::vector<float>> channels;
        std::vector<float*> ptrs;
    };
    std::vector<Bus> ins, outs;
    std::vector<AudioBusBuffers> inBufs, outBufs;

    EventList        events{512};
    ParameterChanges paramsIn, paramsOut;
    ProcessContext   context{};

    // Edits from the window: main thread -> audio thread
    struct Edit { ParamID id; ParamValue value; };
    static constexpr size_t EDITS = 1024;
    std::array<Edit, EDITS> edits{};
    std::atomic<size_t> editWrite{0}, editRead{0};
    // ...and kept for the main thread too, until takeEdits()
    std::vector<ParamEdit> mainEdits;
    std::map<ParamID, std::string> units;   // per parameter, for paramText()

    // Values set by setParameter(): audio thread -> main thread, for the controller
    std::array<Edit, EDITS> toCtrl{};
    std::atomic<size_t> ctrlWrite{0}, ctrlRead{0};

    // Notes playing, so note-offs can repeat the note-on's channel and key
    struct Note { uint32_t id = 0; int16 channel = 0; int16 pitch = 0; bool active = false; };
    std::array<Note, 128> notes{};
    std::atomic<int> tuning{0};
    std::array<ParamID, 16> bendParam{};   // pitch bend per MIDI channel, kNoParamId = unmapped
    int nextChannel = 1;

    // Editor
    IPtr<IPlugView> view;
    Frame frame;
    ::Window window = 0;

    // ---- Audio thread ----
    void pushEvent(const Event& e) { events.addEvent(const_cast<Event&>(e)); }

    void sendNoteOff(int offset, Note& n) {
        Event e{};
        e.busIndex = 0;
        e.sampleOffset = offset;
        e.type = Event::kNoteOffEvent;
        e.noteOff.channel = n.channel;
        e.noteOff.pitch = n.pitch;
        e.noteOff.velocity = 0.0f;
        e.noteOff.noteId = (int32)n.id;
        e.noteOff.tuning = 0.0f;
        pushEvent(e);
        n.active = false;
    }

    // A channel (1-15, MIDI channels 2-16) for an MPE note: the next one
    // with no note on it, or the next one anyway if all are busy.
    int16 mpeChannel() {
        for (int k = 0; k < 15; k++) {
            int ch = 1 + (nextChannel - 1 + k) % 15;
            bool busy = std::any_of(notes.begin(), notes.end(),
                                    [ch](const Note& n) { return n.active && n.channel == ch; });
            if (!busy) { nextChannel = 1 + ch % 15; return (int16)ch; }
        }
        int ch = nextChannel;
        nextChannel = 1 + ch % 15;
        return (int16)ch;
    }

    void setParam(ParamID id, int offset, ParamValue v) {
        int32 qi = 0, pi = 0;
        if (IParamValueQueue* q = paramsIn.addParameterData(id, qi)) q->addPoint(offset, v, pi);
    }

    // ---- Main thread ----
    void closeEditor() {
        if (!view) return;
        view->removed();
        view->setFrame(nullptr);
        view = nullptr;
        if (window && g_display) {
            g_editors.erase(window);
            XDestroyWindow(g_display, window);
            XFlush(g_display);
        }
        window = 0;
    }
};

tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value) {
    Plugin::Impl& d = *owner;
    size_t w = d.editWrite.load(std::memory_order_relaxed);
    size_t next = (w + 1) % Plugin::Impl::EDITS;
    if (next == d.editRead.load(std::memory_order_acquire)) return kResultFalse;   // full
    d.edits[w] = {id, value};
    d.editWrite.store(next, std::memory_order_release);
    if (d.mainEdits.size() < 4096) d.mainEdits.push_back({id, value});
    return kResultOk;
}

tresult PLUGIN_API Frame::resizeView(IPlugView* view, ViewRect* r) {
    if (!view || !r || !owner || !owner->window || !g_display) return kInvalidArgument;
    XResizeWindow(g_display, owner->window, (unsigned)std::max(1, r->getWidth()),
                  (unsigned)std::max(1, r->getHeight()));
    XFlush(g_display);
    view->onSize(r);
    return kResultTrue;
}

Plugin::Plugin(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}

Plugin::~Plugin() {
    d->closeEditor();
    if (d->processor) d->processor->setProcessing(false);
    if (d->component) d->component->setActive(false);
    if (d->controller) d->controller->setComponentHandler(nullptr);
    d->processor = nullptr;
    d->component = nullptr;
    d->controller = nullptr;
    d->provider = nullptr;   // terminates the plugin
}

const std::string& Plugin::name() const  { return d->name; }
const std::string& Plugin::query() const { return d->query; }
bool Plugin::isInstrument() const        { return d->instrument; }

std::unique_ptr<Plugin> Plugin::load(const std::string& query, bool instrument,
                                     double sampleRate, int maxBlock, std::string& error) {
    hostContext();

    // Find the class: by path, by class name or by bundle name
    VST3::Hosting::Module::Ptr mod;
    std::vector<VST3::Hosting::ClassInfo> candidates;
    auto addClasses = [&](const VST3::Hosting::Module::Ptr& m, const std::string& name) {
        for (const auto& c : m->getFactory().classInfos())
            if (c.category() == kVstAudioEffectClass && (name.empty() || lower(c.name()) == name))
                candidates.push_back(c);
    };
    if (query.find('/') != std::string::npos) {
        mod = loadModule(fs::absolute(query).lexically_normal().string(), error);
        if (!mod) return nullptr;
        addClasses(mod, "");
    } else {
        std::string q = lower(query);
        for (const PluginInfo& p : plugins()) {
            if (lower(p.name) != q && lower(fs::path(p.path).stem().string()) != q) continue;
            mod = loadModule(p.path, error);
            if (!mod) continue;
            addClasses(mod, lower(p.name) == q ? q : "");
            if (!candidates.empty()) break;
        }
    }
    if (!mod || candidates.empty()) {
        error = "No VST3 plugin called \"" + query + "\"";
        return nullptr;
    }
    // Prefer the requested kind, but take the other if that's all there is
    auto pick = std::find_if(candidates.begin(), candidates.end(),
                             [instrument](const auto& c) { return isInstrumentClass(c) == instrument; });
    const VST3::Hosting::ClassInfo& info = pick != candidates.end() ? *pick : candidates.front();

    auto d = std::make_unique<Impl>();
    d->name = info.name();
    d->query = query;
    d->instrument = isInstrumentClass(info);
    d->sampleRate = sampleRate;
    d->maxBlock = maxBlock;
    d->handler.owner = d.get();
    d->frame.owner = d.get();
    d->bendParam.fill(kNoParamId);

    d->provider = owned(new PlugProvider(mod->getFactory(), info, true));
    if (!d->provider->initialize() || !d->provider->getComponentPtr()) {
        error = "Couldn't start " + d->name;
        return nullptr;
    }
    d->component = d->provider->getComponentPtr();
    d->controller = d->provider->getControllerPtr();
    d->processor = U::cast<IAudioProcessor>(d->component);
    if (!d->processor) {
        error = d->name + " doesn't process audio";
        return nullptr;
    }
    if (d->controller) d->controller->setComponentHandler(&d->handler);

    // Buses: the main output (and, for effects, the main input) in stereo,
    // everything else left as the plugin has it but switched off
    IComponent& comp = *d->component;
    IAudioProcessor& proc = *d->processor;
    int32 nIn = comp.getBusCount(kAudio, kInput), nOut = comp.getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> inArr(nIn), outArr(nOut);
    for (int32 i = 0; i < nIn; i++)  proc.getBusArrangement(kInput, i, inArr[i]);
    for (int32 i = 0; i < nOut; i++) proc.getBusArrangement(kOutput, i, outArr[i]);
    if (nIn > 0)  inArr[0] = SpeakerArr::kStereo;
    if (nOut > 0) outArr[0] = SpeakerArr::kStereo;
    proc.setBusArrangements(inArr.data(), nIn, outArr.data(), nOut);   // the plugin may refuse
    for (int32 i = 0; i < nIn; i++)
        comp.activateBus(kAudio, kInput, i, i == 0 && !d->instrument);
    for (int32 i = 0; i < nOut; i++)
        comp.activateBus(kAudio, kOutput, i, i == 0);
    if (comp.getBusCount(kEvent, kInput) > 0) comp.activateBus(kEvent, kInput, 0, true);

    auto makeBuses = [&](BusDirection dir, int32 count, std::vector<Impl::Bus>& buses,
                         std::vector<AudioBusBuffers>& bufs) {
        buses.resize(count);
        bufs.resize(count);
        for (int32 i = 0; i < count; i++) {
            SpeakerArrangement arr = 0;
            proc.getBusArrangement(dir, i, arr);
            int ch = std::max(1, SpeakerArr::getChannelCount(arr));
            buses[i].channels.assign(ch, std::vector<float>(maxBlock, 0.0f));
            buses[i].ptrs.resize(ch);
            for (int c = 0; c < ch; c++) buses[i].ptrs[c] = buses[i].channels[c].data();
            bufs[i] = {};
            bufs[i].numChannels = ch;
            bufs[i].channelBuffers32 = buses[i].ptrs.data();
        }
    };
    makeBuses(kInput, nIn, d->ins, d->inBufs);
    makeBuses(kOutput, nOut, d->outs, d->outBufs);

    d->setup = {kRealtime, kSample32, maxBlock, sampleRate};
    if (proc.setupProcessing(d->setup) != kResultOk) {
        error = d->name + " refused the sample rate / block size";
        return nullptr;
    }
    comp.setActive(true);
    proc.setProcessing(true);

    if (d->controller) {
        // The controller starts from the processor's settings
        MemoryStream state;
        if (comp.getState(&state) == kResultOk) {
            state.seek(0, IBStream::kIBSeekSet, nullptr);
            d->controller->setComponentState(&state);
        }
        // Pitch bend per channel, for MPE
        if (auto mm = U::cast<IMidiMapping>(d->controller)) {
            for (int16 ch = 0; ch < 16; ch++) {
                ParamID id = kNoParamId;
                if (mm->getMidiControllerAssignment(0, ch, ControllerNumbers::kPitchBend, id) == kResultTrue)
                    d->bendParam[ch] = id;
            }
        }
        d->paramsIn.setMaxParameters(d->controller->getParameterCount() + 32);
    } else {
        d->paramsIn.setMaxParameters(64);
    }
    d->paramsOut.setMaxParameters(64);

    return std::unique_ptr<Plugin>(new Plugin(std::move(d)));
}

// ---- State: [u32 size][component state][u32 size][controller state] ----

std::vector<uint8_t> Plugin::saveState() {
    MemoryStream cs, ks;
    d->component->getState(&cs);
    if (d->controller) d->controller->getState(&ks);
    std::vector<uint8_t> out;
    auto put = [&out](const MemoryStream& s) {
        uint32_t n = (uint32_t)s.getSize();
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&n);
        out.insert(out.end(), p, p + 4);
        out.insert(out.end(), s.getData(), s.getData() + n);
    };
    put(cs);
    put(ks);
    return out;
}

bool Plugin::loadState(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    auto take = [&](std::vector<char>& out) {
        uint32_t n = 0;
        if (pos + 4 > in.size()) return false;
        std::memcpy(&n, in.data() + pos, 4);
        pos += 4;
        if (pos + n > in.size()) return false;
        out.assign(in.begin() + pos, in.begin() + pos + n);
        pos += n;
        return true;
    };
    std::vector<char> comp, ctrl;
    if (!take(comp) || !take(ctrl)) return false;

    if (!comp.empty()) {
        MemoryStream cs(comp.data(), (TSize)comp.size());
        if (d->component->setState(&cs) != kResultOk) return false;
        if (d->controller) {
            cs.seek(0, IBStream::kIBSeekSet, nullptr);
            d->controller->setComponentState(&cs);
        }
    }
    if (!ctrl.empty() && d->controller) {
        MemoryStream ks(ctrl.data(), (TSize)ctrl.size());
        d->controller->setState(&ks);
    }
    return true;
}

void Plugin::setOffline(bool offline) {
    int32 mode = offline ? kOffline : kRealtime;
    if (d->setup.processMode == mode) return;
    d->processor->setProcessing(false);
    d->component->setActive(false);
    d->setup.processMode = mode;
    d->processor->setupProcessing(d->setup);
    d->component->setActive(true);
    d->processor->setProcessing(true);
}

void Plugin::setTuning(TuningMode m) { d->tuning = (int)m; }

// ---- Editor ----

bool Plugin::openEditor() {
    if (d->view) {   // already open: bring it forward
        if (g_display && d->window) { XRaiseWindow(g_display, d->window); XFlush(g_display); }
        return true;
    }
    if (!d->controller) return false;
    Display* dpy = display();
    if (!dpy) return false;
    IPtr<IPlugView> view = owned(d->controller->createView(ViewType::kEditor));
    if (!view || view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue)
        return false;

    ViewRect r{};
    view->getSize(&r);
    unsigned w = (unsigned)std::max(r.getWidth(), 100), h = (unsigned)std::max(r.getHeight(), 100);
    ::Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, w, h, 0, 0, 0);
    XStoreName(dpy, win, d->name.c_str());
    XSetWMProtocols(dpy, win, &g_wmDelete, 1);
    XSelectInput(dpy, win, StructureNotifyMask);
    if (view->canResize() != kResultTrue) {   // fixed size
        XSizeHints* hints = XAllocSizeHints();
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = (int)w;
        hints->min_height = hints->max_height = (int)h;
        XSetWMNormalHints(dpy, win, hints);
        XFree(hints);
    }
    XMapWindow(dpy, win);
    XSync(dpy, False);

    d->window = win;
    d->view = view;
    view->setFrame(&d->frame);
    if (view->attached(reinterpret_cast<void*>(win), kPlatformTypeX11EmbedWindowID) != kResultOk) {
        view->setFrame(nullptr);
        d->view = nullptr;
        d->window = 0;
        XDestroyWindow(dpy, win);
        XFlush(dpy);
        return false;
    }
    g_editors[win] = d.get();
    return true;
}

void Plugin::closeEditor()     { d->closeEditor(); }
bool Plugin::editorOpen() const { return (bool)d->view; }

// ---- Parameters ----

std::vector<ParamInfo> Plugin::parameters() {
    std::vector<ParamInfo> out;
    if (!d->controller) return out;
    int32 n = d->controller->getParameterCount();
    for (int32 i = 0; i < n; i++) {
        ParameterInfo pi{};
        if (d->controller->getParameterInfo(i, pi) != kResultOk) continue;
        if (!(pi.flags & ParameterInfo::kCanAutomate) ||
            (pi.flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden))) continue;
        out.push_back({pi.id, Steinberg::Vst::StringConvert::convert(pi.title)});
    }
    return out;
}

std::string Plugin::paramName(uint32_t id) {
    if (!d->controller) return "";
    int32 n = d->controller->getParameterCount();
    for (int32 i = 0; i < n; i++) {
        ParameterInfo pi{};
        if (d->controller->getParameterInfo(i, pi) == kResultOk && pi.id == id)
            return Steinberg::Vst::StringConvert::convert(pi.title);
    }
    return "";
}

double Plugin::paramValue(uint32_t id) {
    return d->controller ? d->controller->getParamNormalized(id) : 0.0;
}

std::string Plugin::paramText(uint32_t id, double v) {
    if (!d->controller) return "";
    String128 s{};
    if (d->controller->getParamStringByValue(id, v, s) != kResultOk) return "";
    std::string text = Steinberg::Vst::StringConvert::convert(s);
    if (d->units.empty()) {   // looked up once: this is called on every redraw
        int32 n = d->controller->getParameterCount();
        for (int32 i = 0; i < n; i++) {
            ParameterInfo pi{};
            if (d->controller->getParameterInfo(i, pi) == kResultOk)
                d->units[pi.id] = Steinberg::Vst::StringConvert::convert(pi.units);
        }
    }
    auto u = d->units.find(id);
    if (u != d->units.end() && !u->second.empty()) text += " " + u->second;
    return text;
}

void Plugin::takeEdits(std::vector<ParamEdit>& out) {
    out.insert(out.end(), d->mainEdits.begin(), d->mainEdits.end());
    d->mainEdits.clear();
}

void Plugin::syncController() {
    size_t r = d->ctrlRead.load(std::memory_order_relaxed);
    size_t w = d->ctrlWrite.load(std::memory_order_acquire);
    if (r == w) return;
    // Only the latest value of each parameter matters
    std::map<ParamID, ParamValue> latest;
    for (; r != w; r = (r + 1) % Impl::EDITS) latest[d->toCtrl[r].id] = d->toCtrl[r].value;
    d->ctrlRead.store(r, std::memory_order_release);
    if (d->controller)
        for (auto [id, v] : latest) d->controller->setParamNormalized(id, v);
}

// ---- Audio thread ----

void Plugin::noteOn(int offset, uint32_t id, float freq, float velocity) {
    if (freq <= 0.0f) return;
    double key = 69.0 + 12.0 * std::log2(freq / 440.0);
    int16 pitch = (int16)std::clamp<long>(std::lround(key), 0, 127);
    double cents = (key - pitch) * 100.0;

    auto slot = std::find_if(d->notes.begin(), d->notes.end(), [](const Impl::Note& n) { return !n.active; });
    if (slot == d->notes.end()) {   // all in use: end the first
        slot = d->notes.begin();
        d->sendNoteOff(offset, *slot);
    }

    TuningMode mode = (TuningMode)d->tuning.load(std::memory_order_relaxed);
    int16 channel = 0;
    float tuning = (float)cents;
    if (mode != TuningMode::PerNote) {
        int16 ch = d->mpeChannel();
        if (d->bendParam[ch] != kNoParamId) {
            double range = mode == TuningMode::Mpe48 ? 48.0 : 2.0;
            double bend = std::clamp(cents / 100.0 / range, -1.0, 1.0);
            d->setParam(d->bendParam[ch], offset, 0.5 + 0.5 * bend);
            channel = ch;
            tuning = 0.0f;   // the bend carries it
        }
    }

    Event e{};
    e.busIndex = 0;
    e.sampleOffset = offset;
    e.type = Event::kNoteOnEvent;
    e.noteOn.channel = channel;
    e.noteOn.pitch = pitch;
    e.noteOn.tuning = tuning;
    e.noteOn.velocity = std::clamp(velocity, 0.0f, 1.0f);
    e.noteOn.length = 0;
    e.noteOn.noteId = (int32)id;
    d->pushEvent(e);
    *slot = {id, channel, pitch, true};
}

void Plugin::noteOff(int offset, uint32_t id) {
    for (Impl::Note& n : d->notes)
        if (n.active && n.id == id) d->sendNoteOff(offset, n);
}

void Plugin::allNotesOff(int offset) {
    for (Impl::Note& n : d->notes)
        if (n.active) d->sendNoteOff(offset, n);
}

void Plugin::setParameter(int offset, uint32_t id, double value) {
    d->setParam(id, offset, std::clamp(value, 0.0, 1.0));
    size_t w = d->ctrlWrite.load(std::memory_order_relaxed);
    size_t next = (w + 1) % Impl::EDITS;
    if (next == d->ctrlRead.load(std::memory_order_acquire)) return;   // window just lags behind
    d->toCtrl[w] = {id, value};
    d->ctrlWrite.store(next, std::memory_order_release);
}

void Plugin::process(const float* inL, const float* inR, float* outL, float* outR, int n,
                     const Transport& t) {
    Impl& p = *d;
    n = std::min(n, p.maxBlock);

    // Edits made in the window since the last block
    size_t r = p.editRead.load(std::memory_order_relaxed);
    size_t w = p.editWrite.load(std::memory_order_acquire);
    for (; r != w; r = (r + 1) % Impl::EDITS) p.setParam(p.edits[r].id, 0, p.edits[r].value);
    p.editRead.store(r, std::memory_order_release);

    // Main input (effects); every other input stays silent
    for (size_t b = 0; b < p.ins.size(); b++) {
        auto& chans = p.ins[b].channels;
        for (size_t c = 0; c < chans.size(); c++) {
            const float* src = (b == 0 && !p.instrument) ? (c == 0 ? inL : inR) : nullptr;
            if (src) std::copy(src, src + n, chans[c].data());
            else     std::fill_n(chans[c].data(), n, 0.0f);
        }
        p.inBufs[b].silenceFlags = 0;
    }
    for (auto& bus : p.outs)
        for (auto& ch : bus.channels) std::fill_n(ch.data(), n, 0.0f);
    for (auto& b : p.outBufs) b.silenceFlags = 0;

    ProcessContext& ctx = p.context;
    ctx = {};
    ctx.state = ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid |
                ProcessContext::kTimeSigValid | (t.playing ? ProcessContext::kPlaying : 0);
    ctx.sampleRate = p.sampleRate;
    ctx.projectTimeSamples = t.samplePos;
    ctx.projectTimeMusic = t.beat;
    ctx.tempo = t.tempo;
    ctx.timeSigNumerator = 4;
    ctx.timeSigDenominator = 4;

    ProcessData data;
    data.processMode = p.setup.processMode;
    data.symbolicSampleSize = kSample32;
    data.numSamples = n;
    data.numInputs = (int32)p.inBufs.size();
    data.numOutputs = (int32)p.outBufs.size();
    data.inputs = p.inBufs.empty() ? nullptr : p.inBufs.data();
    data.outputs = p.outBufs.empty() ? nullptr : p.outBufs.data();
    data.inputParameterChanges = &p.paramsIn;
    data.outputParameterChanges = &p.paramsOut;
    data.inputEvents = &p.events;
    data.processContext = &ctx;

    bool ok = p.processor->process(data) == kResultOk;

    p.events.clear();
    p.paramsIn.clearQueue();
    p.paramsOut.clearQueue();

    if (!ok || p.outs.empty()) return;
    const auto& out = p.outs[0].channels;
    const float* l = out[0].data();
    const float* rr = out.size() > 1 ? out[1].data() : l;
    for (int i = 0; i < n; i++) {
        outL[i] += l[i];
        outR[i] += rr[i];
    }
}

// ======================================================
// ======================== IDLE ========================
// ======================================================

void idle() {
    // Plugin windows: closing, and resizing by the user
    if (g_display) {
        while (XPending(g_display)) {
            XEvent ev;
            XNextEvent(g_display, &ev);
            auto it = g_editors.find(ev.xany.window);
            if (it == g_editors.end()) continue;
            Plugin::Impl* p = it->second;
            if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == g_wmDelete) {
                p->closeEditor();
            } else if (ev.type == ConfigureNotify && p->view && p->view->canResize() == kResultTrue) {
                ViewRect r{0, 0, ev.xconfigure.width, ev.xconfigure.height};
                ViewRect cur{};
                p->view->getSize(&cur);
                if (cur.getWidth() != r.getWidth() || cur.getHeight() != r.getHeight()) {
                    p->view->checkSizeConstraint(&r);
                    p->view->onSize(&r);
                }
            }
        }
    }

    // File descriptors the plugins asked to be told about. Copied first:
    // a handler may unregister itself (or others) when called.
    if (!g_fds.empty()) {
        std::vector<FdEntry> fds = g_fds;
        std::vector<pollfd> pfds;
        for (const FdEntry& e : fds) pfds.push_back({e.fd, POLLIN, 0});
        if (poll(pfds.data(), pfds.size(), 0) > 0) {
            for (size_t i = 0; i < fds.size(); i++) {
                if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                bool still = std::any_of(g_fds.begin(), g_fds.end(), [&](const FdEntry& e) {
                    return e.handler == fds[i].handler && e.fd == fds[i].fd;
                });
                if (still) fds[i].handler->onFDIsSet(fds[i].fd);
            }
        }
    }

    // Timers that are due
    auto now = std::chrono::steady_clock::now();
    std::vector<Linux::ITimerHandler*> due;
    for (TimerEntry& t : g_timers) {
        if (now < t.next) continue;
        due.push_back(t.handler);
        t.next = now + t.interval;
    }
    for (Linux::ITimerHandler* h : due) {
        bool still = std::any_of(g_timers.begin(), g_timers.end(),
                                 [h](const TimerEntry& t) { return t.handler == h; });
        if (still) h->onTimer();
    }
}

bool editorsOpen() { return !g_editors.empty(); }

void closeAllEditors() {
    while (!g_editors.empty()) g_editors.begin()->second->closeEditor();
}

} // namespace vst3
