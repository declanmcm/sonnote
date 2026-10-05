#pragma once
// Minimal retained-mode widget tree.
//
//   UI::root
//    ├─ Panel (content area, below the menu bar)
//    │   ├─ Canvas (note window)
//    │   └─ Panel (control border)
//    │       └─ Panel (control window)
//    │           └─ Slider / Button / TextBox ...
//    └─ Panel (menu bar)
//        └─ Menu "File"            <- click to open
//            └─ Button / TextBox   <- dropdown items, drawn on top of everything
//
// Layout coordinates are y-DOWN (origin top-left), in pixels, relative to the
// parent's top-left corner. draw() flips to the renderer's y-up space.

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

struct Color { float r = 0, g = 0, b = 0; };

enum class Kind { Panel, Canvas, Button, Slider, TextBox, Menu };

struct Widget {
    Kind kind = Kind::Panel;

    // --- Layout ---
    Rect  local;              // relative to parent (pixels, or fractions if `relative`)
    bool  relative = false;   // local is a fraction of the parent's size
    float inset    = 0.0f;    // shrink by this many pixels on every side
    // Custom layout: given the parent's screen rect, return this widget's rect.
    // Overrides `local`/`relative`; use it for mixed fixed/fractional sizes.
    std::function<Rect(const Rect& parent)> layoutFn;
    Rect  screen;             // computed by layout(): absolute pixels, y-down

    // --- Appearance ---
    Color color;
    Color textColor;          // label colour for Button / Slider / Menu
    bool  fill = true;        // false = invisible container
    std::string label;
    std::function<std::string()> labelFn;   // dynamic label; overrides `label`
    std::function<Color()>       colorFn;   // dynamic fill colour; overrides `color`
    std::function<bool()>        visibleFn; // empty = always visible

    // --- Slider: reads and writes *target directly, mapped to [lo, hi] ---
    float* target = nullptr;
    float  lo = 0.0f, hi = 1.0f;

    // --- TextBox: edits *text directly ---
    std::string* text = nullptr;
    bool   labelLeft = false;  // label to the left instead of above
    size_t maxLen = 15;

    // --- Behaviour ---
    std::function<void(Widget&)> onClick;   // Button
    std::function<void(Widget&)> onChange;  // Slider, TextBox
    std::function<void(Widget&)> onCommit;  // TextBox: editing finished (Enter or focus lost)

    // --- Tree ---
    Widget* parent = nullptr;
    std::vector<std::unique_ptr<Widget>> children;  // for a Menu: its dropdown items

    // Keeps the label string alive for as long as the renderer might use it.
    std::string drawnLabel;

    Widget* add(std::unique_ptr<Widget> child) {
        child->parent = this;
        children.push_back(std::move(child));
        return children.back().get();
    }

    bool visible() const { return !visibleFn || visibleFn(); }
    std::string currentLabel() const { return labelFn ? labelFn() : label; }

    float normalized() const {
        if (!target || hi == lo) return 0.0f;
        return std::clamp((*target - lo) / (hi - lo), 0.0f, 1.0f);
    }
    void setNormalized(float t) {
        if (target) *target = lo + std::clamp(t, 0.0f, 1.0f) * (hi - lo);
    }
};

// ---------------------------------------------------------------- factories

inline std::unique_ptr<Widget> panel(Rect r, Color c, float inset = 0.0f,
                                     bool relative = false, Kind kind = Kind::Panel) {
    auto w = std::make_unique<Widget>();
    w->kind = kind; w->local = r; w->color = c;
    w->inset = inset; w->relative = relative;
    return w;
}

// A panel positioned by a function of its parent's rect.
inline std::unique_ptr<Widget> panelFn(std::function<Rect(const Rect&)> fn, Color c,
                                       bool fill = true) {
    auto w = std::make_unique<Widget>();
    w->layoutFn = std::move(fn); w->color = c; w->fill = fill;
    return w;
}

inline std::unique_ptr<Widget> button(Rect r, std::string label,
                                      std::function<void(Widget&)> onClick,
                                      Color c = {0.8f, 0.7f, 0.3f}) {
    auto w = std::make_unique<Widget>();
    w->kind = Kind::Button; w->local = r; w->color = c;
    w->label = std::move(label); w->onClick = std::move(onClick);
    return w;
}

// Label is generated as "<name>: <value * displayScale>".
inline std::unique_ptr<Widget> slider(Rect r, std::string name, float* target,
                                      float lo, float hi, float displayScale = 1.0f,
                                      std::function<void(Widget&)> onChange = {},
                                      Color c = {0.8f, 0.7f, 0.3f}) {
    auto w = std::make_unique<Widget>();
    w->kind = Kind::Slider; w->local = r; w->color = c;
    w->target = target; w->lo = lo; w->hi = hi;
    w->onChange = std::move(onChange);
    w->labelFn = [name = std::move(name), target, displayScale] {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s: %.3f", name.c_str(), *target * displayScale);
        return std::string(buf);
    };
    return w;
}

inline std::unique_ptr<Widget> textBox(Rect r, std::string label, std::string* text,
                                       std::function<void(Widget&)> onChange = {},
                                       bool labelLeft = false,
                                       Color c = {0.2f, 0.2f, 0.2f}) {
    auto w = std::make_unique<Widget>();
    w->kind = Kind::TextBox; w->local = r; w->color = c;
    w->label = std::move(label); w->text = text;
    w->onChange = std::move(onChange); w->labelLeft = labelLeft;
    return w;
}

// Menu-bar header ("File"). Add dropdown items as its children, positioned
// relative to the header's top-left (so the first item usually sits at y = h).
inline std::unique_ptr<Widget> menu(Rect r, std::string label) {
    auto w = std::make_unique<Widget>();
    w->kind = Kind::Menu; w->local = r;
    w->color = {0.10f, 0.10f, 0.10f}; w->textColor = {1, 1, 1};
    w->label = std::move(label);
    return w;
}

inline std::unique_ptr<Widget> menuItem(Rect r, std::string label,
                                        std::function<void(Widget&)> onClick) {
    auto w = button(r, std::move(label), std::move(onClick), {0.20f, 0.20f, 0.22f});
    w->textColor = {1, 1, 1};
    return w;
}

// ---------------------------------------------------------------- UI

class UI {
public:
    Widget  root;
    Widget* focused  = nullptr;  // text box currently being typed into
    Widget* openMenu = nullptr;  // menu whose dropdown is showing

    void layout(float width, float height) {
        root.screen = {0, 0, width, height};
        for (auto& c : root.children) layoutNode(*c, root.screen);
    }

    // Ends text editing, firing the box's onCommit.
    void blur() {
        Widget* w = focused;
        focused = nullptr;
        if (w && w->onCommit) w->onCommit(*w);
    }

    void closeMenu() {
        if (focused && focused->parent == openMenu) blur();
        openMenu = nullptr;
    }

    // x, y in window pixels, y-down. Returns true if the UI used the click,
    // so the caller shouldn't also pass it to the canvas.
    bool mouseDown(float x, float y) {
        if (openMenu) {
            // While a dropdown is open, it gets first pick of every click.
            for (auto it = openMenu->children.rbegin(); it != openMenu->children.rend(); ++it) {
                if ((*it)->visible() && (*it)->screen.contains(x, y)) {
                    blur();
                    return activate(**it, x);
                }
            }
            // Clicked anywhere else: just close the menu and swallow the click.
            closeMenu();
            return true;
        }

        blur();
        Widget* w = hitTest(root, x, y);
        return w ? activate(*w, x) : false;
    }

    // Returns true if the key went into a focused text box.
    bool keyDown(int key) {
        if (!focused || !focused->text) return false;
        if (key == '\r' || key == '\n') {           // Enter: finish editing
            if (focused->parent == openMenu) closeMenu();
            blur();
            return true;
        }
        std::string& t = *focused->text;
        bool changed = false;
        if (key >= 32 && key <= 126) {
            if (t.size() < focused->maxLen) { t.push_back((char)key); changed = true; }
        } else if (key == 8 || key == 127) {
            if (!t.empty()) { t.pop_back(); changed = true; }
        }
        if (changed && focused->onChange) focused->onChange(*focused);
        return true;
    }

    // Draws the tree, except open dropdowns.
    template <class Renderer>
    void draw(Renderer& r, float screenHeight) {
        for (auto& c : root.children) drawNode(r, *c, screenHeight);
    }

    // Draws the open dropdown. Call this LAST, after anything else that draws
    // (e.g. the canvas), so the dropdown sits on top.
    template <class Renderer>
    void drawOverlay(Renderer& r, float H) {
        if (!openMenu) return;

        // Backdrop around all visible items
        bool any = false;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        for (auto& c : openMenu->children) {
            if (!c->visible()) continue;
            const Rect& s = c->screen;
            if (!any) { x0 = s.x; y0 = s.y; x1 = s.x + s.w; y1 = s.y + s.h; any = true; }
            x0 = std::min(x0, s.x);       y0 = std::min(y0, s.y);
            x1 = std::max(x1, s.x + s.w); y1 = std::max(y1, s.y + s.h);
        }
        if (!any) return;
        const float pad = 3.0f;
        y0 = std::min(y0, openMenu->screen.y + openMenu->screen.h);  // join up with header
        r.addRect(x0 - pad, H - (y1 + pad), (x1 - x0) + 2 * pad, (y1 - y0) + pad,
                  0.12f, 0.12f, 0.13f);

        for (auto& c : openMenu->children) drawNode(r, *c, H);
    }

private:
    bool activate(Widget& w, float x) {
        switch (w.kind) {
            case Kind::Menu:
                openMenu = &w;
                return true;
            case Kind::Button:
                if (openMenu) closeMenu();          // picking an item closes the menu
                if (w.onClick) w.onClick(w);
                return true;
            case Kind::Slider:
                w.setNormalized((x - w.screen.x) / w.screen.w);
                if (w.onChange) w.onChange(w);
                return true;
            case Kind::TextBox:
                focused = &w;                       // menu stays open while typing
                return true;
            default:
                return false;   // panels and the canvas don't consume clicks
        }
    }

    static void layoutNode(Widget& w, const Rect& p) {
        Rect s;
        if (w.layoutFn)      s = w.layoutFn(p);
        else if (w.relative) s = {p.x + w.local.x * p.w, p.y + w.local.y * p.h,
                                  w.local.w * p.w, w.local.h * p.h};
        else                 s = {p.x + w.local.x, p.y + w.local.y, w.local.w, w.local.h};
        s.x += w.inset; s.y += w.inset;
        s.w -= 2 * w.inset; s.h -= 2 * w.inset;
        w.screen = s;
        for (auto& c : w.children) layoutNode(*c, w.screen);
    }

    // Topmost first: children in reverse order, before the widget itself.
    // Children are tested even if they hang outside their parent (no clipping).
    // A menu's dropdown items are only reachable while it's open (see mouseDown).
    static Widget* hitTest(Widget& w, float x, float y) {
        if (!w.visible()) return nullptr;
        if (w.kind != Kind::Menu)
            for (auto it = w.children.rbegin(); it != w.children.rend(); ++it)
                if (Widget* hit = hitTest(**it, x, y)) return hit;
        return w.screen.contains(x, y) ? &w : nullptr;
    }

    template <class Renderer>
    void drawNode(Renderer& r, Widget& w, float H) {
        
        if (!w.visible()) return;
        const Rect& s = w.screen;
        const float y = H - (s.y + s.h);              // renderer is y-up
        const glm::vec4 white(1.0f), black(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec4 text(w.textColor.r, w.textColor.g, w.textColor.b, 1.0f);

        Color c = w.colorFn ? w.colorFn() : w.color;
        if (&w == focused)  c = {0.4f, 0.4f, 0.8f};
        if (&w == openMenu) c = {0.25f, 0.25f, 0.30f};
        if (w.fill) r.addRect(s.x, y, s.w, s.h, c.r, c.g, c.b);

        switch (w.kind) {
            case Kind::Slider:
                r.addRect(s.x + s.w * w.normalized() - 1, y, 2, s.h, 0.8f, 0.0f, 0.0f);
                [[fallthrough]];
            case Kind::Button:
            case Kind::Menu:
                w.drawnLabel = w.currentLabel();
                r.addText(w.drawnLabel.data(), s.x + 2, y + 2, 15.0f, text);
                break;
            case Kind::TextBox:
                w.drawnLabel = w.currentLabel();
                if (w.labelLeft) r.addText(w.drawnLabel.data(), s.x - 50, y + 2, 15.0f, white);
                else             r.addText(w.drawnLabel.data(), s.x + 2, y + 19, 15.0f, white);
                if (w.text) r.addText(w.text->data(), s.x + 2, y + 2, 15.0f, black);
                break;
            default:
                break;
        }

        if (w.kind == Kind::Menu) return;   // dropdown items are drawn by drawOverlay()
        for (auto& ch : w.children) drawNode(r, *ch, H);
    }
};

} // namespace ui
