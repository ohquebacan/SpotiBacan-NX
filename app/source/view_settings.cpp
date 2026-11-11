/*
 * Gatefold NX · view_settings.cpp
 * #view-settings as Gatefold 1.4 lays it out: a "GATEFOLD / Settings" header, a
 * "Find a setting" field and the categories down the left; on the right the
 * chosen category's header (icon tile + title) over rounded cards of rows.
 * Rows put the label (and a hint, wrapped, never cut) on the left and the
 * control on the right: segmented choices, toggles, sliders, buttons.
 */
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <functional>

#include "app.hpp"
#include "spotify_api.hpp"
#include "icons.inc"
#include "plicons.inc"

#include "version.hpp"

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}

struct Row {
    std::string label, value, hint;   // value: shown after the label, like Gatefold's "Saturation 100%"
    float ctlW = 0;                   // the control's width, so the text knows how much room it has
    std::function<void(float rx, float cy)> control;
    float minH = 56;
    // a full-width block instead of label + control (the theme picker, the About header)
    float blockH = 0;
    std::function<void(float x, float y, float w)> block;
    Id focus = 0;   // its control, for showSetting (buttons; other rows are only scrolled to and lit)
    bool lifted = false;   // picked up with Move (a sidebar tab): drawn as the queue's held row
};
struct Card { std::string kicker; std::vector<Row> rows; };
struct Section { const char* name; const char* icon; std::vector<Card> cards; };

}  // namespace

/** Gatefold's .seg: a pill holding the options, the chosen one raised. Right-aligned at rx; returns its width. */
float App::segmented(Id id, float rx, float cy, const std::vector<std::string>& opts, int cur, int* picked, unsigned disabled) {
    const Font f{ css::Sm, Weight::SemiBold };
    float w = 6;
    for (const auto& o : opts) w += g.measure(o, f) + 26;
    const float h = 36, x0 = rx - w;
    g.roundRect(x0, cy - h / 2, w, h, 12, pal.text, 0.05f);
    g.roundStroke(x0, cy - h / 2, w, h, 12, 1, pal.text, 0.09f);
    float x = x0 + 3;
    for (int i = 0; i < int(opts.size()); i++) {
        const float ow = g.measure(opts[i], f) + 26;
        const Id oid = id + 0x9e3779b97f4a7c15ull * uint64_t(i + 1);
        const bool off = disabled & (1u << i);   // greyed: drawn, but not focusable or pressable
        if (!off && ui.item(oid, { x, cy - h / 2 + 3, ow, h - 6 }, 9) && picked) *picked = i;
        const bool on = i == cur, hot = !off && (ui.pressed(oid) || (ui.focused(oid) && ui.showFocusRing()));
        if (on) g.roundRect(x, cy - h / 2 + 3, ow, h - 6, 9, pal.text, 0.13f);
        else if (hot) g.roundRect(x, cy - h / 2 + 3, ow, h - 6, 9, pal.text, 0.065f);
        g.textC(opts[i], f, x + ow / 2, cy, on ? pal.text : pal.text2, off ? 0.35f : 1.f, Align::Center);
        x += ow;
    }
    return w;
}

std::string App::eqPresetName(int preset) const {
    if (preset >= 0 && preset < GFEQ_PRESETS) return kGfEqPresets[preset].name;
    if (preset >= GFEQ_PRESETS && preset - GFEQ_PRESETS < int(store.eqUser.size())) return store.eqUser[size_t(preset - GFEQ_PRESETS)].name;
    return "Custom";
}

void App::saveEqPreset(int into) {
    std::string name = "My preset " + std::to_string(store.eqUser.size() + 1);
    if (into >= int(store.eqUser.size())) into = -1;
    if (into >= 0) name = store.eqUser[size_t(into)].name;   // updated where it is, under its name
    else if (!keyboard("Name the preset", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (char& c : name) if (c == '\t') c = ' ';
    if (name.empty()) return;
    if (name.size() > 63) name.resize(63);
    GfEqUser u{};
    snprintf(u.name, sizeof u.name, "%s", name.c_str());
    u.preamp = store.eq.preamp;
    u.bass = store.eq.bass;
    u.treble = store.eq.treble;
    for (int b = 0; b < GFEQ_BANDS; b++) u.bands[b] = store.eq.bands[b];
    int k = into;
    if (k < 0)
        for (int i = 0; i < int(store.eqUser.size()); i++) if (name == store.eqUser[size_t(i)].name) k = i;   // the same name: replaced
    if (k < 0) {
        if (store.eqUser.size() >= GFEQ_USER_MAX) { toast("That is a hundred presets already: delete one first"); return; }
        store.eqUser.push_back(u);
        k = int(store.eqUser.size()) - 1;
    } else {
        store.eqUser[size_t(k)] = u;
    }
    store.saveEqPresets();
    GfbgEq e = store.eq;
    e.preset = int8_t(GFEQ_PRESETS + k);
    e.edited = 0;
    setEq(e);
    ui.focus(uid("set.eqpreset"));   // the Save row goes with the change: without this the focus fell back to Find a setting
    toast(into >= 0 ? "Updated the preset “" + name + "”" : "Saved the preset “" + name + "”");
}

void App::renameEqPreset(int k) {
    if (k < 0 || k >= int(store.eqUser.size())) return;
    std::string name = store.eqUser[size_t(k)].name;
    if (!keyboard("Rename the preset", name, name)) return;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (char& c : name) if (c == '\t') c = ' ';
    if (name.empty()) return;
    snprintf(store.eqUser[size_t(k)].name, sizeof store.eqUser[size_t(k)].name, "%s", name.c_str());
    store.saveEqPresets();
}

void App::deleteEqPreset(int k) {
    if (k < 0 || k >= int(store.eqUser.size())) return;
    const std::string name = store.eqUser[size_t(k)].name;
    confirm(ICON_EQ, "Equalizer", name, "Your preset", "Delete this preset?", "It leaves the preset list. The sound playing now stays as it is, as Custom.",
            "Delete", [this, k, name] {
                if (k >= int(store.eqUser.size()) || name != store.eqUser[size_t(k)].name) return;
                store.eqUser.erase(store.eqUser.begin() + k);
                store.saveEqPresets();
                GfbgEq e = store.eq;
                if (e.preset == GFEQ_PRESETS + k) gfEqMakeCustom(&e);      // the one on: its sound stays, as Custom
                else if (e.preset > GFEQ_PRESETS + k) e.preset--;           // the ones after it move up
                setEq(e);
                ui.focus(uid("set.eqpreset"));   // its rows go with it
                toast("Deleted the preset “" + name + "”");
            });
}

/** The equalizer buttons' short press (player bar, Canvas). */
void App::toggleEq() {
    GfbgEq e = store.eq;
    e.on = !e.on;
    setEq(e);
    toast(e.on ? "Equalizer on: " + eqPresetName(e.preset) : std::string("Equalizer off"));
}

/** A long press on an equalizer button: Settings → Equalizer, its switch focused. */
void App::openEqualizer() {
    settingsCat = SetEqualizer;
    settingsQuery.clear();
    area("settings.pane").target = area("settings.pane").y = 0;
    navigate(View::Settings);
    settingsJump = "Equalizer";   // drawSettings focuses its switch and lights the row, as showSetting does
    settingsJumpAt = time;
}

void App::openFindSetting() {
    char out[128] = {};
    const plat::TextResult r = plat::askText(tr("Settings", "Configuracion"), tr("Find a setting", "Buscar una opcion"), settingsQuery.c_str(), out, sizeof out);
    if (r == plat::TextResult::Unavailable) toast(tr("The keyboard is not available in applet mode", "El teclado no esta disponible en modo applet"));
    if (r != plat::TextResult::Done) return;
    settingsQuery = out;   // an empty answer clears the search
    area("settings.pane").target = area("settings.pane").y = 0;
}

void App::drawSettings(float x, float y, float w, float h) {
    const Font labelF{ css::Md, Weight::SemiBold }, hintF{ css::Sm }, valueF{ css::Md };
    // A category chosen last frame: its page was drawn then, so its first setting is known now
    if (settingsFocusFirst) {
        settingsFocusFirst = false;
        if (const Id first = ui.firstIn(&area("settings.pane"))) ui.focus(first);
    }

    // ------------------------------------------------------------ the rows, by category
    auto toggleRow = [&](const std::string& label, const std::string& hint, const char* key, bool& v) {
        return Row{ label, "", hint, 40, [this, key, &v](float rx, float cy) {
                       if (toggle(uid(key), rx - 40, cy, v)) { v = !v; persistSoon(0.3f); }
                   } };
    };
    auto sliderRow = [&](const std::string& label, const std::string& value, const std::string& hint, const char* key, float& v,
                         std::function<void()> changed = nullptr) {
        return Row{ label, value, hint, 180, [this, key, &v, changed](float rx, float cy) {
                       float nv = v;
                       slider(uid(key), rx - 180, cy, 180, v, &nv);
                       if (nv != v) { v = nv; persistSoon(0.8f); if (changed) changed(); }
                   } };
    };
    auto segRow = [&](const std::string& label, const std::string& hint, const char* key, std::vector<std::string> opts, int cur,
                      std::function<void(int)> pick, unsigned disabled = 0) {
        float sw = 6;
        for (const auto& o : opts) sw += g.measure(o, { css::Sm, Weight::SemiBold }) + 26;
        return Row{ label, "", hint, sw, [this, key, opts, cur, pick, disabled](float rx, float cy) {
                       int p = -1;
                       segmented(uid(key), rx, cy, opts, cur, &p, disabled);
                       if (p >= 0 && p != cur) { pick(p); persistSoon(0.3f); }
                   } };
    };
    auto buttonRow = [&](const std::string& label, const std::string& hint, const char* key, const char* icon, const std::string& text,
                         std::function<void()> act, bool danger = false, float minW = 0) {
        const float bw = std::max(minW, float(g.measure(text, { css::Sm, Weight::SemiBold }) + 32 + (icon ? 24 : 0)));
        Row r{ label, "", hint, bw, [this, key, icon, text, act, bw, danger](float rx, float cy) {
                  bool c = false;
                  button(uid(key), icon, text, rx - bw, cy - 17, false, &c, false, danger, 0, bw);
                  if (c) act();
              } };
        r.focus = uid(key);
        return r;
    };
    // Two buttons on one row (the equalizer's Save / Save new): the first is the row's focus
    struct Btn { const char* key; const char* icon; std::string text; std::function<void()> act; bool danger = false; };
    auto buttonsRow = [&](const std::string& label, const std::string& hint, std::vector<Btn> btns) {
        float total = 0;
        std::vector<float> ws;
        for (const Btn& b : btns) {
            ws.push_back(g.measure(b.text, { css::Sm, Weight::SemiBold }) + 32 + (b.icon ? 24 : 0));
            total += ws.back() + (total > 0 ? 8 : 0);
        }
        Row r{ label, "", hint, total, [this, btns, ws, total](float rx, float cy) {
                  float bx = rx - total;
                  for (size_t i = 0; i < btns.size(); i++) {
                      bool c = false;
                      button(uid(btns[i].key), btns[i].icon, btns[i].text, bx, cy - 17, false, &c, false, btns[i].danger);
                      if (c) btns[i].act();
                      bx += ws[i] + 8;
                  }
              } };
        if (!btns.empty()) r.focus = uid(btns[0].key);
        return r;
    };
    auto infoRow = [&](const std::string& label, const std::string& hint, const std::string& value = {}) {
        // Values are counts that change (tags read, covers ready): fixed-width digits, so they do not jitter.
        const float vw = value.empty() ? 0 : g.measureNum(value, valueF);
        return Row{ label, "", hint, vw, value.empty() ? std::function<void(float, float)>() : [this, value](float rx, float cy) {
                                                              g.textNum(value, { css::Md }, rx, cy - g.lineHeight({ css::Md }) / 2.f, pal.text2, 1,
                                                                        Align::Right);
                                                          } };
    };
    auto pct = [](float v) { return std::to_string(int(std::lround(v * 100))) + "%"; };
    auto oneDecimal = [](double v) { char b[16]; snprintf(b, sizeof b, "%.1f", v); return std::string(b); };

    std::vector<Section> sections;
    static float satShown, trShown;   // Saturation 0..100% and Colour transition 0..3 s, as the slider's 0..1
    satShown = store.saturation;   // 0..100%: past 100% the palette changes no further
    trShown = store.colourTransition / 3.f;

    // Appearance
    std::vector<Row> sidebarRows;
    {
        // Settings → Appearance → Sidebar: the tabs in the sidebar's order (store.tabOrder), each shown or hidden with its
        // switch (Store::hiddenTabs; Now playing always shows) and put in order as a queue row is: Move picks it up, the
        // D-pad moves it, A or B puts it down (App::grab). The button keeps its id per tab, so the focus moves with it.
        const Font bf{ css::Sm, Weight::SemiBold };
        const char* moveTxt = tr("Move", "Mover");
        const float mw = g.measure(moveTxt, bf) + 32 + 24;
        const float rw = 34;   // Remove: a round icon button
        int lines = 0;
        for (int i = 0; i < int(store.tabOrder.size()); i++) {
            const int tab = store.tabOrder[size_t(i)];
            const unsigned bit = Store::tabBit(tab);
            const bool held = grab.kind == Grab::Tabs && grab.pos == i;
            if (Store::isLine(tab)) {   // one of your lines: Move it, or Remove it
                lines++;
                // The same columns as the tabs' rows: Move where their Move is, the cross centred where their switch is
                Row r{ tr("Line", "Linea"), "", held ? tr("The D-pad moves it, A puts it down", "La cruz lo mueve, A lo suelta") : tr("A thin line in the sidebar between the tabs around it", "Una linea delgada en la barra lateral entre las pestañas"),
                       40 + 16 + mw, [this, tab, i, mw, rw, moveTxt](float rx, float cy) {
                           bool c = false;
                           {   // Remove: round, the red cross only (as the playlist page's compact Delete)
                               const float cx = rx - 20 - rw / 2;
                               const Id did = uid("set.tabdel", tab);
                               c = ui.item(did, { cx, cy - 17, 34, 34 }, 17);
                               const bool hot = ui.pressed(did) || (ui.focused(did) && ui.showFocusRing());
                               g.roundRect(cx, cy - 17, 34, 34, 17, pal.text, hot ? 0.11f : 0.065f);
                               g.roundStroke(cx, cy - 17, 34, 34, 17, 1, pal.text, 0.15f);
                               g.icon(ICON_X, cx + 17, cy, 16, dangerInk());
                           }
                           if (c) {
                               if (i < int(store.tabOrder.size()) && store.tabOrder[size_t(i)] == tab) store.tabOrder.erase(store.tabOrder.begin() + i);
                               persistSoon(0.3f);
                               return;
                           }
                           button(uid("set.tabmove", tab), ICON_SORT_DOWN, moveTxt, rx - 40 - 16 - mw, cy - 17, false, &c);
                           if (c) {
                               grab = Grab{};
                               grab.kind = Grab::Tabs;
                               grab.pos = i;
                               toast(tr("Move it with the D-pad, then press A", "Muevelo con la cruz y presiona A"));
                           }
                       } };
                r.lifted = held;
                r.minH = 48;
                sidebarRows.push_back(r);
                continue;
            }
            const char* hint = held                    ? tr("The D-pad moves it, A puts it down", "La cruz lo mueve, A lo suelta")
                             : tab == Store::TabNow    ? tr("Always shown. Settings stays at the bottom", "Siempre visible. Configuracion permanece abajo")
                             : "";
            Row r{ Store::tabName(tab), "", hint, 40 + 16 + mw, [this, tab, bit, i, mw, moveTxt](float rx, float cy) {
                      if (bit) {
                          const bool shown = !(store.hiddenTabs & bit);
                          if (toggle(uid("set.tab", int(bit)), rx - 40, cy, shown)) {
                              store.hiddenTabs ^= bit;
                              persistSoon(0.3f);
                          }
                      }
                      bool c = false;
                      button(uid("set.tabmove", tab), ICON_SORT_DOWN, moveTxt, rx - 40 - 16 - mw, cy - 17, false, &c);
                      if (c) {
                          grab = Grab{};
                          grab.kind = Grab::Tabs;
                          grab.pos = i;
                          toast(tr("Move it with the D-pad, then press A", "Muevelo con la cruz y presiona A"));
                      }
                  } };
            r.lifted = held;
            sidebarRows.push_back(r);
        }
        // A new line goes in at the end, picked up, so it is placed straight away
        if (lines < Store::MaxLines) {
            Row add = buttonRow(tr("Add a line", "Agregar una linea"), tr("Separates the tabs in groups, as you like (up to four)", "Separa las pestañas en grupos a tu gusto (hasta cuatro)"), "set.tabaddline", ICON_PLUS, tr("Add", "Agregar"), [this] {
                int id = Store::TabLine;
                while (std::find(store.tabOrder.begin(), store.tabOrder.end(), id) != store.tabOrder.end()) id++;
                store.tabOrder.push_back(id);
                persistSoon(0.3f);
                grab = Grab{};
                grab.kind = Grab::Tabs;
                grab.pos = int(store.tabOrder.size()) - 1;
                toast(tr("Move it with the D-pad, then press A", "Muevelo con la cruz y presiona A"));
            });
            sidebarRows.push_back(add);
        } else {
            sidebarRows.push_back(infoRow(tr("Add a line", "Agregar una linea"), tr("Four lines at most: remove one to add another", "Maximo cuatro lineas: elimina una para agregar otra")));
        }
    }
    {
        // Interface colours: Gatefold's cards in one row (From the cover, Dark, Light, Custom). Grey is left out at the
        // user's request, to keep the page shorter; a saved Grey reads as Dark (Store::load).
        Row themes;
        themes.blockH = 150;
        themes.block = [this](float bx, float by, float bw) {
            struct Opt { const char* name; int id; Palette p; };
            const Palette custom = store.theme == 4 ? target : customPalette();
            const Opt opts[4] = { { tr("From the cover", "De la portada"), 0, coverPal }, { tr("Dark", "Oscuro"), 1, paletteFixed(FixedTheme::Dark) },
                                  { tr("Light", "Claro"), 3, paletteFixed(FixedTheme::Light) }, { tr("Custom", "Personalizado"), 4, custom } };
            const float gap = 12, cw = (bw - gap * 3) / 4, ch = 150;
            for (int i = 0; i < 4; i++) {
                const float cx = bx + i * (cw + gap), cy = by;
                const Id id = uid("set.theme", opts[i].id);
                if (ui.item(id, { cx, cy, cw, ch }, 14)) { store.theme = opts[i].id; applyTheme(); persistSoon(0.3f); }
                const bool on = store.theme == opts[i].id, hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                g.roundRect(cx, cy, cw, ch, 14, pal.text, hot ? 0.08f : 0.04f);
                g.roundStroke(cx, cy, cw, ch, 14, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.09f);
                // .theme-card .pv: a tiny window in that palette — sidebar strip, two text lines, an accent pill
                const Palette& p = opts[i].p;
                const float px = cx + 10, py = cy + 10, pw = cw - 20, ph = ch - 50;
                g.roundRect(px, py, pw, ph, 9, p.bg);
                if (i == 0) {
                    g.glow(px + pw * 0.2f, py + ph * 0.3f, pw * 0.6f, p.blobs[0], 0.9f);
                    g.glow(px + pw * 0.85f, py + ph * 0.8f, pw * 0.5f, p.blobs[2], 0.9f);
                    g.glow(px + pw * 0.7f, py + ph * 0.1f, pw * 0.35f, p.accent, 0.45f);
                }
                g.roundRect(px, py, pw * 0.24f, ph, 9, p.surface, 0.85f);
                const float tx = px + pw * 0.34f;
                g.roundRect(tx, py + ph * 0.24f, pw * 0.48f, 7, 3.5f, p.text);
                g.roundRect(tx, py + ph * 0.42f, pw * 0.32f, 6, 3, p.text3);
                g.roundRect(tx, py + ph * 0.62f, pw * 0.24f, 14, 7, p.accent);
                g.roundStroke(px, py, pw, ph, 9, 1, p.dark ? Rgb{ 1, 1, 1 } : Rgb{ 0, 0, 0 }, 0.06f);
                float lx = cx + 14;
                if (on) { g.circle(lx + 4, cy + ch - 20, 4, pal.accent); lx += 14; }
                g.textC(opts[i].name, { css::Base, Weight::SemiBold }, lx, cy + ch - 20, on ? pal.text : pal.text2, 1, Align::Left,
                        int(cx + cw - 10 - lx));
            }
        };
        const std::vector<std::string> brightNames = { tr("Dark", "Oscuro"), tr("Light", "Claro") };
        // Custom has its own: Dark, Light, or the background exactly as picked
        const std::vector<std::string> customBrightNames = { tr("Dark", "Oscuro"), tr("Light", "Claro"), tr("As picked", "Tal cual") };
        const Row brightness =
            store.theme == 4
                ? segRow(tr("Brightness", "Brillo"), tr("Your colours on a dark or a light background, like a cover's; or your background, moved only as far as text needs to stay readable", "Tus colores sobre un fondo oscuro o claro; o tu fondo ajustado para que el texto sea legible"),
                         "set.cbright", customBrightNames, store.customMode, [this](int i) { store.customMode = i; applyTheme(); })
                : segRow(tr("Brightness", "Brillo"),
                         tr("From the cover, on a dark or a light background", "De la portada, sobre un fondo oscuro o claro"),
                         "set.bright", brightNames, store.brightness, [this](int i) { store.brightness = i; applyTheme(); });
        const Row saturation = sliderRow(tr("Saturation", "Saturacion"), std::to_string(int(std::lround(store.saturation * 100))) + "%", tr("How strongly the cover tints the interface", "Intensidad con la que la portada tine la interfaz"),
                                         "set.sat", satShown, [this] { store.saturation = std::round(satShown * 100) / 100; applyTheme(); });
        char trV[16];
        snprintf(trV, sizeof trV, "%.1f s", double(store.colourTransition));
        const Row transition = sliderRow(tr("Colour transition", "Transicion de color"), trV, tr("How long the colours take to follow a new cover", "Tiempo que tardan los colores en cambiar a una nueva portada"), "set.tr", trShown,
                                         [this] { store.colourTransition = std::round(trShown * 3 * 10) / 10; });
        // Cover palette: the eight colours read from the cover playing (.swatches)
        Row swatches{ tr("Cover palette", "Paleta de la portada"), "", coverPal.swatchCount ? "" : tr("Play something to see its colours", "Reproduce algo para ver sus colores"), 8 * 34.f, [this](float rx, float cy) {
                         const int n = coverPal.swatchCount;
                         for (int i = 0; i < n; i++) {
                             const float sx = rx - (n - i) * 34;
                             g.roundRect(sx, cy - 15, 28, 30, 8, coverPal.swatches[i].c);
                             g.roundStroke(sx, cy - 15, 28, 30, 8, 1, pal.text, 0.12f);
                         }
                     } };
        // Custom: your background and two accents, each from the cover playing or a few of Gatefold's own
        std::vector<Row> customRows;
        if (store.theme == 4) {
            // The last swatch opens the colour picker; it shows your own colour, ringed, when it is none of the others.
            auto chooser = [this](const char* label, const char* hint, const char* key, int* field, bool allowAuto, Rgb shown) {
                std::vector<int> choices;
                if (allowAuto) choices.push_back(-1);
                for (int i = 0; i < coverPal.swatchCount; i++) {
                    const Rgb& c = coverPal.swatches[i].c;
                    choices.push_back(int(std::lround(c.r * 255)) << 16 | int(std::lround(c.g * 255)) << 8 | int(std::lround(c.b * 255)));
                }
                for (int v : { 0x16181f, 0xf4f1ea, 0x8fb4ff, 0xff5a6a, 0x4cd6a0, 0xffc857 }) choices.push_back(v);
                const float cw = (choices.size() + 1) * 30.f;
                return Row{ label, "", hint, cw, [this, label, key, field, choices, shown](float rx, float cy) {
                               const int n = int(choices.size()) + 1;
                               for (int i = 0; i < int(choices.size()); i++) {
                                   const float sx = rx - (n - i) * 30;
                                   const Id id = uid(key, i);
                                   if (ui.item(id, { sx, cy - 13, 26, 26 }, 13)) { *field = choices[i]; applyTheme(); persistSoon(0.3f); }
                                   const int v = choices[i];
                                   if (v < 0) {
                                       g.circle(sx + 13, cy, 12, pal.text, 0.1f);
                                       g.textC("A", { css::Xs, Weight::Bold }, sx + 13, cy, pal.text2, 1, Align::Center);
                                   } else {
                                       g.circle(sx + 13, cy, 12, Rgb{ ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f });
                                   }
                                   const bool on = *field == v, hot = ui.focused(id) && ui.showFocusRing();
                                   if (on || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, on ? 2.f : 1.f, on ? pal.accent : pal.text, on ? 1.f : 0.4f);
                               }
                               // the picker: a colour wheel, or your own colour in the middle of it
                               const float sx = rx - 30;
                               const Id id = uid(key, 99);
                               if (ui.item(id, { sx, cy - 13, 26, 26 }, 13))
                                   openColourPicker(field, label, *field >= 0 ? Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f } : shown);
                               const bool own = *field >= 0 && std::find(choices.begin(), choices.end(), *field) == choices.end();
                               if (SDL_Texture* wheel = wheelTexture()) g.imageStretched(wheel, sx + 1, cy - 12, 24, 24, 0);
                               if (own) {
                                   g.circle(sx + 13, cy, 8.5f, { 0, 0, 0 }, 0.35f);
                                   g.circle(sx + 13, cy, 7, Rgb{ ((*field >> 16) & 255) / 255.f, ((*field >> 8) & 255) / 255.f, (*field & 255) / 255.f });
                               }
                               const bool hot = ui.focused(id) && ui.showFocusRing();
                               if (own || hot) g.roundStroke(sx - 2, cy - 15, 30, 30, 15, own ? 2.f : 1.f, own ? pal.accent : pal.text, own ? 1.f : 0.4f);
                           } };
            };
            customRows.push_back(chooser(tr("Background", "Fondo"), tr("Behind every page and the bars", "Detras de cada pagina y las barras"), "set.cbg", &store.customBg, true, pal.bg));
            customRows.push_back(chooser(tr("Accent", "Acento"), tr("Buttons, sliders and what is playing", "Botones, deslizadores y lo que esta sonando"), "set.cacc", &store.customAccent, false, pal.accent));
            customRows.push_back(chooser(tr("Second accent", "Segundo acento"), tr("Where gradients start: the seek bar, the spectrum", "Donde inician los degradados: la barra de progreso y el espectro"), "set.cacc2", &store.customAccent2,
                                         true, pal.accent2));
            customRows.push_back(infoRow(tr("Your colours", "Tus colores"), tr("Kept readable: text and an accent too close to the background move only as far as they must. "
                                                                               "A uses the cover's own; the colour wheel picks any colour.",
                                                                               "Siempre legibles: el texto y el acento se ajustan si son muy parecidos al fondo. A usa el de la portada; el circulo elige cualquier color.")));
        }
        // Brightness only where it does something: From the cover and Custom (Dark and Light are their own)
        std::vector<Row> colourRows = { themes };
        if (store.theme == 0 || store.theme == 4) colourRows.push_back(brightness);
        for (const Row& r : { saturation, transition, swatches }) colourRows.push_back(r);
        std::vector<Card> cards = { { tr("Interface colours", "Colores de la interfaz"), colourRows } };
        if (!customRows.empty()) cards.push_back({ tr("Custom", "Personalizado"), customRows });   // only while Custom is chosen
        cards.push_back({ tr("Layout", "Diseño e idioma"),
                          { segRow(tr("View", "Vista"), tr("Large: big buttons, side rail and info pills. Classic: the compact original",
                                                             "Grande: botones grandes, riel lateral y pastillas de informacion. Clasica: la original, mas compacta"),
                                   "set.layout", { tr("Large", "Grande"), tr("Classic", "Clasica") }, store.bigLayout ? 0 : 1,
                                   [this](int i) { store.bigLayout = i == 0; g_bigLayout = store.bigLayout; css::applyLayout(store.bigLayout); persistSoon(0.3f); }),
                            segRow(tr("Language", "Idioma"), tr("Interface language", "Idioma de la interfaz"), "set.lang",
                                   { "English", "Español" }, store.lang, [this](int i) { store.lang = i; g_lang = i; persistSoon(0.3f); }),
                            toggleRow(tr("Collapse the sidebar", "Contraer la barra lateral"), tr("Icons only, more room for everything else", "Solo iconos, mas espacio para todo lo demas"), "set.side", store.sidebarCollapsed),
                            toggleRow(tr("Background motion", "Movimiento de fondo"), tr("The slowly drifting colours behind every view and the Canvas", "Los colores suaves en movimiento detras de cada vista y del Canvas"), "set.motion",
                                      store.motion) } });
        cards.push_back({ tr("Sidebar", "Barra lateral"), sidebarRows });
#ifdef GF_WINDOWED
        unsigned batteryNow = 0;
        if (plat::batteryLevel(batteryNow))   // a desktop has none
#endif
        cards.push_back({ tr("Title bar", "Barra superior"),
                          { toggleRow(tr("Battery percentage", "Porcentaje de bateria"), tr("Beside the battery, top right", "Junto a la bateria, arriba a la derecha"), "set.batpct", store.batteryPercent),
                            toggleRow(tr("Battery time", "Tiempo de bateria"), tr("How long to empty, or to full while charging; shown after a minute, steadier as it goes", "Tiempo restante o para carga completa; se muestra tras un minuto"),
                                      "set.batest", store.batteryEstimate) } });
        sections.push_back({ tr("Appearance", "Apariencia"), ICON_SPARKLE, cards });
    }

    // Playback
    {
        // The background player (sysmodule): music on the HOME menu and in games, and its pause-for-apps.
        std::vector<Row> bgRows;
#ifdef GF_WINDOWED
        bgRows.push_back(toggleRow(tr("Keep playing when the window closes", "Seguir reproduciendo al cerrar la ventana"), tr("SpotiBacán stays by the clock, playing; its icon brings it back. Off: closing the window quits", "SpotiBacán sigue reproduciendo en segundo plano"),
                                   "set.bg", store.background));
#else
        const bool on = player.remote();
        bgRows.push_back(infoRow(tr("Background playback", "Reproduccion en segundo plano"),
                                 on ? tr("Connected to the SpotiBacán sysmodule. Music plays continuously on the HOME menu and during games.",
                                         "Conectado al sysmodule de SpotiBacán. La musica se reproduce de forma continua en el menu HOME y durante los juegos.")
                                    : tr("Disconnected. Make sure the sysmodule is running.",
                                         "Desconectado. Asegurate de que el sysmodule este iniciado.")));
        bgRows.push_back(infoRow(tr("In-game controls", "Controles en juegos"),
                                 tr("Use the Tesla overlay (L + D-pad Down + R3) or your linked mobile/PC app to pause, skip tracks and change volume.",
                                    "Usa el overlay de Tesla (L + Cruz abajo + R3) o tu aplicacion vinculada en el telefono o PC para pausar, cambiar de cancion y volumen.")));
#endif
        bgRows.push_back(toggleRow(tr("Save while playing", "Guardar mientras suena"),
                                   tr("Each Spotify song that plays with SpotiBacán open is saved to the SD card, to listen without internet",
                                      "Cada cancion de Spotify que suena con SpotiBacán abierto se guarda en la SD, para escucharla sin internet"),
                                   "set.autosave", store.autoSave));
        // Measured: the count beside the label; Stop / Resume while the library is not all measured.
        const size_t measured = std::min(levels.count(), lib.tracks.size());
        const std::string measuredHint =
            store.levelMode == 0 ? tr("Starts once levelling is on, with the track playing first", "Inicia cuando la nivelacion esta activa, empezando por la cancion actual")
            : store.stopLevels       ? tr("Stopped. Only the track playing and the next ones are measured until you resume", "Detenido. Solo se miden la cancion actual y las siguientes hasta reanudar")
            : levels.secondsPerTrack() > 0 ? std::string(tr("In the background while SpotiBacán is open, about ", "En segundo plano mientras SpotiBacán esta abierto, aprox. ")) + oneDecimal(levels.secondsPerTrack()) + tr(" s a track", " s por cancion")
                                     : tr("In the background while SpotiBacán is open, the track playing first", "En segundo plano mientras SpotiBacán esta abierto, empezando por la cancion actual");
        Row measuredRow = store.levelMode && (store.stopLevels || measured < lib.tracks.size())
                              ? buttonRow(tr("Measured", "Medidas"), measuredHint, "set.lvlstop", store.stopLevels ? ICON_PLAY : ICON_X,
                                          store.stopLevels ? tr("Resume", "Reanudar") : tr("Stop", "Detener"),
                                          [this] { setJobStopped(Stoppable::Levels, !store.stopLevels); })
                              : infoRow(tr("Measured", "Medidas"), measuredHint);
        measuredRow.value = std::to_string(measured) + tr(" of ", " de ") + std::to_string(lib.tracks.size());
        const std::vector<std::string> orders = { tr("Default", "Normal"), tr("Repeat list", "Repetir lista"), tr("Repeat track", "Repetir cancion"), tr("Shuffle", "Aleatorio") };
        sections.push_back(
            { tr("Playback", "Reproduccion"), ICON_PLAY,
              { { "", { segRow(tr("Order", "Orden"), tr("Also the first button in the player bar", "Tambien es el primer boton de la barra de reproduccion"), "set.order", orders, int(store.order),
                               [this](int i) { setOrder(Order(i)); }, automixQueue() ? 1u << 3 : 0u),
                        Row{ tr("Stop after the current track", "Detener al terminar la cancion actual"), "", tr("Then playback stops until you start it again", "La reproduccion se detiene hasta que la inicies de nuevo"), 40,
                             [this](float rx, float cy) {
                                 if (toggle(uid("set.sac"), rx - 40, cy, stopAfterCurrent)) stopAfterCurrent = !stopAfterCurrent;
                             } },
                        Row{ tr("Volume", "Volumen"), pct(volumeShown), "", 180,
                             [this](float rx, float cy) {
                                 float v = volumeShown;
                                 slider(uid("set.vol"), rx - 180, cy, 180, volumeShown, &v);
                                 if (v != volumeShown) { volumeShown = v; player.setVolume(v); }
                             } } } },
                { tr("Volume levelling", "Nivelacion de volumen"),
                  { segRow(tr("Volume levelling", "Nivelacion de volumen"), tr("Every track about as loud as the next. Measured here; the files are not changed", "Todas las canciones suenan a un volumen similar"),
                           "set.level", { tr("Off", "Apagado"), tr("Track", "Cancion"), tr("Album", "Album"), "Auto" }, store.levelMode,
                           [this](int i) { store.levelMode = i; persistSoon(0.3f); }),
                    segRow(tr("Target loudness", "Volumen objetivo"), tr("Quiet tracks are raised only as far as their peaks allow", "Las canciones bajas se suben solo hasta donde permitan sus picos"), "set.lvltarget",
                           { "−18", "−14", "−11 LUFS" }, store.levelTarget <= -16 ? 0 : store.levelTarget <= -12 ? 1 : 2,
                           [this](int i) {
                               static const int t[] = { -18, -14, -11 };
                               store.levelTarget = t[i];
                               persistSoon(0.3f);
                           },
                           store.levelMode ? 0u : 0x7u),
                    measuredRow } },
                { tr("Startup", "Inicio"), { toggleRow(tr("Resume where I left off", "Reanudar donde lo deje"), tr("The queue comes back paused at the same place in the same track", "La cola regresa en pausa en el mismo punto de la misma cancion"),
                                         "set.resume", store.resume) } },
                { tr("Sleep", "Temporizador"),
                  { segRow(tr("Sleep timer", "Temporizador de apagado"), tr("The music fades out and pauses; also the moon in the player bar", "La musica baja de volumen gradualmente y se pausa; tambien es el icono de luna en la barra"), "set.sleep",
                           { tr("Off", "Apagado"), "15", "30", "45", "60", "90 min" }, sleepLeft() > 0 ? sleepChoice : 0,
                           [this](int i) {
                               static const int mins[] = { 0, 15, 30, 45, 60, 90 };
                               setSleep(mins[i] * 60.0, i);
                           }),
                    segRow(tr("Then sleep the console", "Luego suspender la consola"), tr("When the timer ends with SpotiBacán open, a card counts down; Stay awake keeps it on", "Cuando termina el temporizador con SpotiBacán abierto, muestra una cuenta regresiva"),
                           "set.sleepcon", { tr("Off", "Apagado"), "10 s", "30 s", "1 min" },
                           store.sleepConsole <= 0 ? 0 : store.sleepConsole <= 10 ? 1 : store.sleepConsole <= 30 ? 2 : 3,
                           [this](int i) {
                               static const int secs[] = { 0, 10, 30, 60 };
                               store.sleepConsole = secs[i];
                               persistSoon(0.3f);
                           }),
                    toggleRow(tr("Keep the console awake", "Mantener la consola despierta"), tr("While SpotiBacán is open the screen does not dim and the console does not sleep by itself. After a sleep timer ends, it may again.", "Mientras SpotiBacán este abierto la pantalla no se atenua ni entra en modo de espera sola."),
                              "set.awake", store.keepAwake),
                    segRow(tr("Dim the screen", "Atenuar la pantalla"), store.keepAwake ? tr("After this long without input; any button brings it back", "Despues de este tiempo sin tocar botones; cualquier boton la reactiva")
                                                             : tr("With Keep the console awake on; otherwise the console dims by itself", "Con Mantener la consola despierta activo; de lo contrario se atenua sola"),
                           "set.dim", { tr("Off", "Apagado"), "1", "2", "5", "10 min" },
                           store.dimAfter <= 0 ? 0 : store.dimAfter <= 1 ? 1 : store.dimAfter <= 2 ? 2 : store.dimAfter <= 5 ? 3 : 4,
                           [this](int i) {
                               static const int mins[] = { 0, 1, 2, 5, 10 };
                               store.dimAfter = mins[i];
                               persistSoon(0.3f);
                           },
                           store.keepAwake ? 0u : 0x1Fu) } },
                { tr("Background player", "Reproductor en segundo plano"), bgRows },
                { tr("Linked Account", "Cuenta vinculada"),
                  { buttonRow(tr("Log out", "Cerrar sesion"),
                              tr("Unlinks the current account from this Nintendo Switch and shows the instructions to sign in again",
                                 "Desvincula la cuenta actual de esta Nintendo Switch y muestra las instrucciones para iniciar sesion de nuevo"),
                              "set.pblogout", ICON_X, tr("Log out", "Cerrar sesion"), [this] { confirmLogoutSpotify(); }, true) } } } });
    }

    // Linked Account
    {
        const bool hasCreds = SpotifyApi::instance().hasCredentials();
        const std::string user = SpotifyApi::instance().username();
        std::vector<Card> cards;
        cards.push_back({ tr("Session", "Sesion"),
                          { infoRow(tr("Linked account", "Cuenta vinculada"),
                                    hasCreds ? tr("Session credentials active on this console", "Credenciales de sesion activas en esta consola")
                                             : tr("No account is currently linked", "No hay ninguna cuenta vinculada actualmente"),
                                    !user.empty() ? user : (hasCreds ? std::string(tr("Connected", "Conectado")) : std::string(tr("Not signed in", "Sin sesion")))),
                            buttonRow(tr("Log out", "Cerrar sesion"),
                                      tr("Clears saved credentials on the console and returns to the sign-in instructions screen",
                                         "Elimina las credenciales guardadas en la consola y vuelve a la pantalla con las instrucciones para abrir una nueva sesion"),
                                      "set.logout", ICON_X, tr("Log out", "Cerrar sesion"), [this] { confirmLogoutSpotify(); }, true) } });
        sections.push_back({ tr("Linked Account", "Cuenta vinculada"), ICON_SPOTIFY, cards });
    }

    // Lyrics
    sections.push_back({ tr("Lyrics", "Letras"), ICON_QUOTE,
                         { { "", { toggleRow(tr("Lyrics in the Canvas", "Letras en el Canvas"), tr("Also the quote button in the Canvas", "Tambien es el boton de comillas en el Canvas"), "set.cvlyr", store.canvasLyrics),
                                   toggleRow(tr("Timing control", "Control de sincronizacion"), tr("Earlier · 0.0 s · Later above synced lyrics, on Now playing and in the Canvas: each track keeps its own. Timings already set still apply when it is off", "Antes · 0.0 s · Despues sobre las letras sincronizadas en Esta sonando y en el Canvas: cada cancion guarda su propio ajuste"),
                                             "set.lyrtime", store.lyricTiming),
                                   infoRow(tr("Lyrics provider", "Proveedor de letras"),
                                           tr("Synced lyrics are fetched online via LRCLIB and synchronized with playback.",
                                              "Las letras sincronizadas se obtienen en linea mediante LRCLIB y se sincronizan con la reproduccion.")) } } } });

    // Canvas
    {
        const std::vector<std::string> layouts = { tr("Cover", "Portada"), tr("Lyrics", "Letras"), "Minimal" };
        // Idle: when the controls fade, and how long the progress line takes to grow into the spectrum.
        char idleV[16], vizV[16];
        snprintf(idleV, sizeof idleV, "%.1f s", double(store.canvasIdleAfter));
        snprintf(vizV, sizeof vizV, "%.1f s", double(store.canvasVizTime));
        const Row idleAfterRow{ tr("Controls fade after", "Ocultar controles tras"), idleV, tr("Without a button or a touch", "Sin presionar botones ni tocar la pantalla"), 180, [this](float rx, float cy) {
                                   const float cur = (store.canvasIdleAfter - 2) / 13;   // 2..15 s
                                   float nv = cur;
                                   slider(uid("set.cvidle"), rx - 180, cy, 180, cur, &nv);
                                   if (nv != cur) { store.canvasIdleAfter = std::round((2 + nv * 13) * 2) / 2; persistSoon(0.8f); }
                               } };
        const Row vizTimeRow{ tr("Spectrum rise", "Aparicion del espectro"), vizV, tr("How long the progress line takes to become the spectrum, and back", "Tiempo que tarda la barra de progreso en convertirse en el espectro"), 180,
                              [this](float rx, float cy) {
                                  const float cur = (store.canvasVizTime - 0.2f) / 2.8f;   // 0.2..3 s
                                  float nv = cur;
                                  slider(uid("set.cvviz"), rx - 180, cy, 180, cur, &nv);
                                  if (nv != cur) { store.canvasVizTime = std::round((0.2f + nv * 2.8f) * 10) / 10; persistSoon(0.8f); }
                              } };
        sections.push_back(
            { "Canvas", ICON_CANVAS,
              { { "", { segRow(tr("Layout", "Diseño"),
                               canvasLayoutsOff() == 0b011 ? tr("This track has no lyrics, so the Canvas shows Minimal until one that has them", "Esta cancion no tiene letras, por lo que el Canvas muestra Minimal")
                                                           : tr("Also at the bottom of the Canvas", "Tambien en la parte inferior del Canvas"),
                               "set.cvlayout", layouts, canvasLayoutShown(), [this](int i) { store.canvasLayout = i; }, canvasLayoutsOff()),
                        toggleRow(tr("Minimal when there are no lyrics", "Minimal cuando no hay letras"), tr("Your layout comes back with the next track that has lyrics", "Tu diseño vuelve con la siguiente cancion que tenga letras"),
                                  "set.cvauto", store.canvasAutoMinimal) } },
                { tr("Cover motion", "Movimiento de portada"),
                  { toggleRow(tr("Float", "Flotar"), tr("The cover rises and settles slowly", "La portada sube y baja suavemente"), "set.cvfloat", store.canvasFloat),
                    toggleRow(tr("Slow zoom", "Zoom suave"), tr("A gentle Ken Burns zoom across the cover", "Un suave acercamiento sobre la portada"), "set.cvzoom", store.canvasZoom) } },
                { tr("When idle", "En inactividad"),
                  { idleAfterRow, vizTimeRow } } } });
    }

    // Visualizer
    {
        static const std::vector<std::string> bands = { "32", "56", "80" };
        const int bandIdx = store.vizBands <= 40 ? 0 : store.vizBands <= 64 ? 1 : 2;
        static const std::vector<std::string> cvBands = { "32", "56", "80", "112" };
        const int cvBandIdx = store.canvasBars <= 40 ? 0 : store.canvasBars <= 64 ? 1 : store.canvasBars <= 96 ? 2 : 3;
        sections.push_back(
            { tr("Visualizer", "Visualizador"), ICON_WAVE,
              { { tr("Spectrum", "Espectro"),
                  { segRow(tr("Style", "Estilo"), tr("How the Canvas spectrum looks; both take the cover's colours", "Como se ve el espectro del Canvas; los dos toman los colores del disco"),
                           "set.vstyle", { tr("LED dots", "Puntos LED"), tr("Bars", "Barras") }, store.vizStyle == 1 ? 0 : 1,
                           [this](int i) { store.vizStyle = i == 0 ? 1 : 0; persistSoon(0.3f); }),
                    sliderRow(tr("Sensitivity", "Sensibilidad"), pct(store.vizSensitivity), tr("Higher for quiet recordings, lower if the bars hit the top", "Mas alto para canciones suaves, mas bajo si las barras tocan el tope"),
                              "set.vsens", store.vizSensitivity),
                    sliderRow(tr("Smoothing", "Suavizado"), pct(store.vizSmoothing), tr("Higher is calmer, lower follows every hit", "Mas alto es mas suave, mas bajo sigue cada golpe"), "set.vsmooth",
                              store.vizSmoothing),
                    segRow(tr("Bars in the Canvas", "Barras en el Canvas"), tr("The spectrum the Canvas grows when it is idle", "El espectro que muestra el Canvas cuando esta inactivo"), "set.cvbands", cvBands, cvBandIdx,
                           [this](int i) { store.canvasBars = i == 0 ? 32 : i == 1 ? 56 : i == 2 ? 80 : 112; }),
                    toggleRow(tr("Peak caps", "Picos"), tr("The thin marks that fall slowly above the bars", "Las marcas delgadas que caen lentamente sobre las barras"), "set.vpeaks", store.vizPeaks) } },
                { tr("Cover", "Portada"),
                  { sliderRow(tr("Bass pulse", "Pulso de graves"), store.coverPulse < 0.01f ? tr("Off", "Apagado") : pct(store.coverPulse),
                              tr("How much the cover breathes with the bass. 100% is the default, 0 turns it off", "Cuanto late la portada con los graves. 100% por defecto, 0 lo apaga"), "set.vpulse",
                              store.coverPulse),
                    toggleRow(tr("Pulse in the Canvas too", "Pulso tambien en el Canvas"), tr("The full-screen cover breathes with the same strength", "La portada en pantalla completa late con la misma intensidad"), "set.vcvpulse",
                              store.canvasPulse) } } } });
    }

    // Controls
    {
        std::vector<Row> keys;
#ifdef GF_WINDOWED
        const std::pair<const char*, const char*> map[] = { { "Enter  ·  pad A", tr("Select", "Seleccionar") },           { "Esc  ·  pad B", tr("Back", "Atras") },
                                                            { "Space  ·  pad X", tr("Play / pause", "Reproducir / pausar") },    { "Ctrl F  ·  pad Y", tr("Search", "Buscar") },
                                                            { "Ctrl ← →  ·  LB RB", tr("Previous / next track", "Cancion anterior / siguiente") },
                                                            { "Shift ← →  ·  LT RT", tr("Seek 10 seconds", "Adelantar / retroceder 10 s") },
                                                            { "C  ·  View", tr("Canvas (full screen)", "Canvas (pantalla completa)") },
                                                            { "F11  ·  Alt Enter", tr("Full screen", "Pantalla completa") },
                                                            { "Right-click  ·  Menu", tr("Track menu", "Menu de la cancion") },
                                                            { "Arrows  ·  D-pad", tr("Move around", "Navegar") },
                                                            { "Wheel  ·  right stick", tr("Scroll", "Desplazar") },
                                                            { "R3", tr("Recenter pointer", "Recentrar cursor") } };
#else
        const std::pair<const char*, const char*> map[] = { { "A", tr("Select", "Seleccionar") },           { "B", tr("Back", "Atras") },        { "X", tr("Play / pause", "Reproducir / pausar") },
                                                            { "Y", tr("Search", "Buscar") },           { "L  R", tr("Previous / next track", "Cancion anterior / siguiente") },
                                                            { "ZL  ZR", tr("Seek 10 seconds", "Adelantar / retroceder 10 s") }, { "−", tr("Canvas (full screen)", "Canvas (pantalla completa)") },
                                                            { "+", tr("Track menu", "Menu de la cancion") },
                                                            { "HOME", tr("Leave SpotiBacán (close it from the HOME menu)", "Salir de SpotiBacán (cerrar desde el menu HOME)") },
                                                            { tr("Right stick", "Stick derecho"), tr("Scroll", "Desplazar") },
                                                            { "R3", tr("Recenter pointer", "Recentrar cursor") } };
#endif
        for (auto& [k, l] : map) {
            const Font f{ css::Sm, Weight::SemiBold };
            const float kw = g.measure(k, f) + 18;
            Row r{ l, "", "", kw, [this, k = std::string(k), kw, f](float rx, float cy) {
                      g.roundRect(rx - kw, cy - 13, kw, 26, 7, pal.text, 0.065f);
                      g.roundStroke(rx - kw, cy - 13, kw, 26, 7, 1, pal.text, 0.15f);
                      g.textC(k, f, rx - kw / 2, cy, pal.text2, 1, Align::Center);
                  } };
            r.minH = 44;
            keys.push_back(r);
        }
        const Row speedRow = sliderRow(tr("Pointer speed", "Velocidad del puntero"), pct(store.pointerSpeed * 2),
                                       tr("The left stick moves a pointer; A taps under it", "El stick izquierdo mueve un puntero; A selecciona bajo el"), "set.ptrspeed", store.pointerSpeed);
        const Row motionToggleRow = toggleRow(tr("Motion pointer control", "Control por movimiento"),
                                              tr("Aim with the right Joy-Con when Joy-Cons are detached", "Apunta con el Joy-Con derecho al desacoplar los mandos"),
                                              "set.motionpointer", store.motionPointer);
        const Row motionSpeedRow = sliderRow(tr("Motion pointer speed", "Velocidad del puntero por movimiento"), pct(store.motionSpeed * 2),
                                             tr("Sensitivity when aiming with the right Joy-Con", "Sensibilidad al apuntar con el Joy-Con derecho"), "set.motionspeed", store.motionSpeed);
        const Row accelRow = toggleRow(tr("Pointer acceleration", "Aceleracion del puntero"), tr("Slow for small moves, fast when the stick is pushed all the way", "Lento para movimientos cortos, rapido al inclinar el stick al maximo"),
                                       "set.ptraccel", store.pointerAccel);
        std::vector<Row> pointerRows = { speedRow, motionToggleRow };
        if (store.motionPointer) pointerRows.push_back(motionSpeedRow);
        pointerRows.push_back(accelRow);
        sections.push_back({ tr("Controls", "Controles"), ICON_SLIDERS,
                             { { tr("Buttons", "Botones"), keys },
                               { tr("Pointer", "Puntero"), pointerRows },
                               { tr("Touch", "Pantalla tactil"), { infoRow(tr("Touch works everywhere", "La pantalla tactil funciona en toda la app"), tr("Tap, drag lists with momentum, drag the seek bar and the volume", "Toca, desliza listas con inercia, arrastra la barra de progreso y el volumen")) } } } });
    }

    // Updates
    {
        std::vector<Card> cards;

        // Pending restart card if an update is waiting to finish
        if (updater.installed.appPending || updater.justInstalled) {
            cards.push_back({
                tr("Update ready", "Actualizacion lista"),
                { buttonRow(
                    tr("Restart SpotiBacán", "Reiniciar SpotiBacán"),
                    tr("Restart now to complete installing the update", "Reinicia ahora para completar la instalacion"),
                    "set.updrelaunch",
                    ICON_REFRESH,
                    tr("Restart now", "Reiniciar ahora"),
                    [this] { finishUpdate(); }
                ) }
            });
        }

        // Automatic checking & manual check
        std::vector<Row> checkRows;
        std::string checkHint;
        if (updater.busy()) {
            checkHint = updateStatus();
        } else if (updater.checkedAt > 0) {
            const time_t t = (time_t)updater.checkedAt;
            char b[32];
            struct tm* tm = localtime(&t);
            if (tm) strftime(b, sizeof(b), "%Y-%m-%d %H:%M", tm);
            else snprintf(b, sizeof(b), "OK");
            checkHint = tr("Last checked: ", "Ultima comprobacion: ") + std::string(b);
        } else {
            checkHint = tr("Check GitHub for newer versions of SpotiBacán", "Comprobar en GitHub si hay versiones mas recientes de SpotiBacán");
        }

        checkRows.push_back(buttonRow(
            tr("Check for updates", "Buscar actualizaciones"),
            checkHint,
            "set.updcheck",
            ICON_DOWNLOAD,
            updater.busy() ? tr("Checking...", "Buscando...") : tr("Check now", "Buscar ahora"),
            [this] { checkUpdates(false); }
        ));

        checkRows.push_back(toggleRow(
            tr("Check when SpotiBacán starts", "Buscar al iniciar SpotiBacán"),
            tr("Automatically check for updates on startup", "Buscar actualizaciones automaticamente al abrir la aplicacion"),
            "set.updstart",
            store.updateAtStart
        ));

        checkRows.push_back(toggleRow(
            tr("Beta versions", "Versiones beta"),
            tr("Include preview and test releases", "Incluir versiones preliminares y de prueba"),
            "set.updbeta",
            store.betas
        ));

        checkRows.push_back(infoRow(
            tr("Installed version", "Version instalada"),
            "",
            Updater::thisVersion()
        ));

        cards.push_back({ tr("System updates", "Actualizaciones del sistema"), checkRows });

        // Releases list
        std::vector<Row> relRows;
        if (updater.releases.empty()) {
            relRows.push_back(infoRow(
                tr("No releases loaded", "No hay versiones cargadas"),
                tr("Press 'Buscar ahora' above to query GitHub", "Presiona 'Buscar ahora' arriba para consultar GitHub"),
                ""
            ));
        } else {
            for (size_t i = 0; i < updater.releases.size(); i++) {
                const auto& r = updater.releases[i];
                const int cmp = Updater::compare(r.version, Updater::thisVersion());
                const std::string status = (cmp > 0) ? tr("New", "Nueva") : (cmp == 0) ? tr("Current", "Actual") : tr("Old", "Anterior");
                const std::string label = Updater::releaseName(r.version) + " (" + status + ")";
                std::string sub = r.date;
                if (r.zipSize > 0) {
                    char mb[32];
                    snprintf(mb, sizeof(mb), " · %.1f MB", double(r.zipSize) / (1024.0 * 1024.0));
                    sub += mb;
                }
                const char* icon = (cmp > 0) ? ICON_DOWNLOAD : (cmp == 0) ? ICON_CHECK : ICON_HISTORY;
                const std::string btnText = (cmp > 0) ? tr("Update", "Actualizar") : tr("Details", "Detalles");

                const float bw = std::max(120.f, float(g.measure(btnText, { css::Sm, Weight::SemiBold }) + 32 + 24));
                const Id rid = uid("set.updrel", int64_t(i));
                Row rrow{ label, "", sub, bw, [this, rid, icon, btnText, i, bw](float rx, float cy) {
                    bool c = false;
                    button(rid, icon, btnText, rx - bw, cy - 17, false, &c, false, false, 0, bw);
                    if (c) openRelease(int(i));
                } };
                rrow.focus = rid;
                relRows.push_back(rrow);
            }
        }
        cards.push_back({ tr("Available versions", "Versiones disponibles"), relRows });

        sections.push_back({ tr("Updates", "Actualizaciones"), ICON_DOWNLOAD, cards });
    }

    // Support: a report from the console to NXtify on GitHub
    {
        std::vector<Card> cards;
        std::vector<Row> rep;
        rep.push_back(infoRow(tr("Repository", "Repositorio"), "github.com/ohquebacan/SpotiBacan-NX", "SpotiBacán"));
        if (report::available()) {
            Row onRow{ tr("Reports", "Reportes"), "", store.reports ? tr("Sent only when you press Send, to SpotiBacán on GitHub, and nowhere else", "Se envian solo al presionar Enviar, al repositorio de SpotiBacán en GitHub")
                                                     : tr("Off: SpotiBacán sends nothing. On, you can report a bug or suggest a feature from here", "Apagado: SpotiBacán no envia nada. Activalo para reportar un error o sugerir una funcion"),
                       40, [this](float rx, float cy) {
                           if (toggle(uid("set.repon"), rx - 40, cy, store.reports)) {
                               if (store.reports) {
                                   store.reports = false;
                                   persistSoon(0.3f);
                                   toast(tr("Reports off: SpotiBacán sends nothing", "Reportes desactivados: SpotiBacán no envia nada"));
                               } else {
                                   allowReports();
                               }
                           }
                       } };
            onRow.focus = uid("set.repon");
            rep.push_back(onRow);
            if (store.reports) {
                rep.push_back(buttonRow(tr("Report a bug", "Reportar un error"), tr("A short form: what happened, and where", "Un breve formulario: que ocurrio y donde"), "set.repbug", ICON_BUG, tr("Report", "Reportar"),
                                        [this] { openReport(0); }));
                rep.push_back(buttonRow(tr("Suggest a feature", "Sugerir una funcion"), tr("Something SpotiBacán could do, sent the same way", "Algo que SpotiBacán podria hacer, enviado de la misma forma"), "set.repidea", PLI_BULB, tr("Suggest", "Sugerir"),
                                        [this] { openReport(1); }));
            }
        } else {
            rep.push_back(infoRow(tr("Report issues on GitHub", "Reportar problemas en GitHub"), tr("Open an issue on GitHub: ", "Abre un issue en GitHub: ") + std::string(ISSUES_URL)));
        }
        cards.push_back({ tr("Report", "Reportar"), rep });
        // read on a worker the first time this page shows (supportScan): never the card on the main thread
        const bool read = supportAsked_ && !supportThread_.joinable();
#ifndef GF_WINDOWED
        std::vector<Row> cr;
        if (!read) cr.push_back(infoRow(tr("Atmosphère's crash reports", "Reportes de cierre de Atmosphère"), tr("Reading them…", "Leyendo…")));
        else if (supportCrashes_.empty()) cr.push_back(infoRow(tr("Atmosphère's crash reports", "Reportes de cierre de Atmosphère"), tr("None on this console", "Ninguno en esta consola")));
        else
            cr.push_back(buttonRow(tr("Atmosphère's crash reports", "Reportes de cierre de Atmosphère"), std::to_string(supportCrashes_.size()) + (supportCrashes_.size() == 1 ? tr(" report", " reporte") : tr(" reports", " reportes")) +
                                       tr(", the latest ", ", el mas reciente ") + supportCrashes_.front().when, "set.crashes", ICON_BUG, tr("See them", "Verlos"), [this] {
                                           if (sheet.preview) SDL_DestroyTexture(sheet.preview);
                                           const Id back = ui.focusedId();
                                           sheet = Sheet{};
                                           sheet.page = Sheet::Crashes;
                                           sheet.returnFocus = back;
                                           ui.focusDefault();
                                       }));
        cards.push_back({ tr("Crash reports", "Reportes de errores"), cr });
#endif
        std::string playing = tr("Nothing", "Nada");
        if (now.track >= 0 && !now.info.codec.empty()) {
            playing = now.info.codec;
            if (now.info.sampleRate > 0) { char r[24]; snprintf(r, sizeof r, " · %.1f kHz", now.info.sampleRate / 1000.0); playing += r; }
        }
        cards.push_back({ tr("Sent with a report", "Enviado con un reporte"),
                          { infoRow("SpotiBacán", "", Updater::thisVersion()),
#ifdef GF_WINDOWED
                            infoRow(tr("System", "Sistema"), "", read ? supportSys_.firmware : std::string("…")),
#else
                            infoRow("Firmware", "", read ? supportSys_.firmware : std::string("…")),
                            infoRow("Atmosphère", "", read ? supportSys_.atmosphere : std::string("…")),
                            infoRow(tr("Background player", "Reproductor en segundo plano"), "", player.remote() ? tr("Connected", "Conectado") : tr("Not connected", "No conectado")),
#endif
                            infoRow(tr("Playing", "Reproduciendo"), tr("Its format only, never its name", "Solo su formato, nunca su nombre"), playing),
                            infoRow(tr("Library", "Biblioteca"), "", std::to_string(lib.tracks.size()) + tr(" tracks", " canciones")) } });
        sections.push_back({ tr("Support", "Soporte"), ICON_HELP, cards });
    }

    // About
    {
        Row head;
        head.blockH = 76;
        head.block = [this](float bx, float by, float) {
            g.roundRect(bx, by + 8, 60, 60, 16, { 0.118f, 0.843f, 0.376f }, 0.16f);
            g.glow(bx + 30, by + 38, 30, { 0.118f, 0.843f, 0.376f }, 0.45f);
            g.icon(ICON_SPOTIFY, bx + 30, by + 38, 36, { 0.118f, 0.843f, 0.376f });
            g.text(tr("SpotiBacán for " GF_PLATFORM, "SpotiBacán para " GF_PLATFORM), { css::Xl, Weight::Bold }, bx + 76, by + 12, pal.text);
            g.text(tr("Version " GF_VERSION " · by noesio582-boop", "Version " GF_VERSION " · por noesio582-boop"), { css::Base }, bx + 76, by + 46, pal.text2);
        };
        sections.push_back(
            { tr("About", "Acerca de"), ICON_INFO,
              { { "", { head } },
                { tr("Credits", "Creditos"),
                  { infoRow("noesio582-boop", tr("Project author and maintainer. Switch integration and updates.", "Autor y mantenedor del proyecto. Integracion en Switch y actualizaciones.")),
                    infoRow("SpotiBacán", tr("Cloud music streaming client and UI application for Nintendo Switch, powered by cspot and librespot. GPL 3.0", "Cliente de musica en la nube e interfaz para Nintendo Switch, impulsado por cspot y librespot. Licencia GPL 3.0")),
                    infoRow("AndryTheBeast", tr("Creator of the Songate Nintendo Switch homebrew interface. GPL 3.0", "Creador de la interfaz homebrew Songate para Nintendo Switch. Licencia GPL 3.0")),
                    infoRow("HiroshiYamauchi", tr("Original Switch streaming client and background service architecture. GPL 3.0", "Cliente original de streaming en Switch y arquitectura de servicio en segundo plano. Licencia GPL 3.0")),
                    infoRow("Gatefold", tr("Original interface design by DEVIROYrus for foobar2000. MIT licence", "Diseño original de interfaz por DEVIROYrus para foobar2000. Licencia MIT")),
                    infoRow("Inter", tr("The typeface. (c) The Inter Project Authors, SIL Open Font Licence", "La tipografia. (c) The Inter Project Authors, Licencia SIL Open Font")),
                    infoRow("FFmpeg", tr("Audio streaming decoders: Vorbis, AAC and Opus. LGPL 2.1", "Decodificadores de audio: Vorbis, AAC y Opus. LGPL 2.1")),
                    infoRow("SDL 2", tr("Graphics, sound, fonts and images: SDL, SDL_mixer, SDL_ttf, SDL_image. zlib licence", "Graficos, sonido, fuentes e imagenes: SDL, SDL_mixer, SDL_ttf, SDL_image. Licencia zlib")),
                    infoRow("Opus, Tremor", tr("Audio codecs. BSD and LGPL licences", "Codecs de audio. Licencias BSD y LGPL")),
                    infoRow("libcurl & zlib", tr("Network over the console's secure connection. curl and zlib licences", "Red mediante la conexion segura de la consola. Licencias curl y zlib"))
#ifndef GF_WINDOWED
                    , infoRow("libnx & devkitPro", tr("The Nintendo Switch homebrew toolchain. ISC licence", "El toolchain homebrew de Nintendo Switch. Licencia ISC")),
                    infoRow(tr("Background player", "Reproductor en segundo plano"), tr("SpotiBacán background sysmodule (0x4200000000535754) and Tesla overlay (libultrahand). GPL 3.0", "Sysmodule en segundo plano de SpotiBacán (0x4200000000535754) y overlay Tesla (libultrahand). GPL 3.0"))
#endif
                  } } } });
    }
    settingsCat = std::clamp(settingsCat, 0, int(sections.size()) - 1);
    if (settingsCat != settingsCatSeen_) {
        settingsCatSeen_ = settingsCat;
        if (sections[size_t(settingsCat)].icon == ICON_HELP) supportScan();   // its reads, on a worker
    }

    // ------------------------------------------------------------ left column
    const float lx = x + 24, lw = 214;
    float cy = y + 24;
    kicker("SpotiBacán", lx, cy + 6, pal.text3, 0.14f);
    cy += 16;
    g.text(tr("Settings", "Configuracion"), { css::Xxl, Weight::ExtraBold }, lx - 1, cy, pal.text);
    cy += 50;
    // .st-search: opens the console keyboard; what it finds replaces the category on the right
    {
        const Id sid = uid("set.find");
        if (ui.item(sid, { lx, cy, lw, 38 }, 10)) openFindSetting();
        const bool hot = ui.focused(sid) && ui.showFocusRing();
        g.roundRect(lx, cy, lw, 38, 10, pal.text, hot ? 0.09f : 0.05f);
        g.roundStroke(lx, cy, lw, 38, 10, 1, pal.text, 0.12f);
        g.icon(ICON_SEARCH, lx + 20, cy + 19, 16, pal.text3);
        g.textC(settingsQuery.empty() ? tr("Find a setting", "Buscar una opcion") : settingsQuery, { css::Base }, lx + 38, cy + 19, settingsQuery.empty() ? pal.text3 : pal.text,
                1, Align::Left, int(lw - 50));
        cy += 38 + 12;
    }
    // Eleven categories and more sit a little closer; past what fits (Support, a dev build's Debug: About went under the
    // player bar, the user, 2026-10-30) the list scrolls, the focused category kept in view (Ui, as every scrolled list)
    const float step = sections.size() > 10 ? 36 : 40, catH = step - 2;
    ScrollArea& ca = area("settings.cats");
    const float listTop = cy;
    ca.view = { lx - 8, listTop, lw + 16, y + h - listTop };
    ca.content = sections.size() * step + 8;
    ca.clamp();
    ui.pushArea(&ca);
    g.clip(lx - 8, listTop, lw + 16, y + h - listTop);
    cy -= ca.y;
    for (int i = 0; i < int(sections.size()); i++) {
        const Id id = uid("set.cat", i);
        const bool on = settingsQuery.empty() && settingsCat == i;
        if (ui.item(id, { lx, cy, lw, catH }, 10, on ? DefaultFocus : 0)) {
            settingsCat = i;
            settingsQuery.clear();
            area("settings.pane").target = area("settings.pane").y = 0;
            settingsFocusFirst = true;   // the page's first setting takes the focus (the user: as an artist's page does)
        }
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        if (on) g.roundRect(lx, cy, lw, catH, 10, pal.accent, 0.14f);
        else if (hot) g.roundRect(lx, cy, lw, catH, 10, pal.text, 0.065f);
        g.icon(sections[i].icon, lx + 12 + 9, cy + catH / 2, 18, on ? pal.accentInk : pal.text2);
        g.textC(sections[i].name, { css::Base, Weight::SemiBold }, lx + 12 + 18 + 12, cy + catH / 2, on ? pal.accentInk : hot ? pal.text : pal.text2, 1,
                Align::Left, int(lw - 50));
        cy += step;
    }
    g.unclip();
    ui.popArea();

    // ------------------------------------------------------------ right pane
    const float px = lx + lw + 28, pw = x + w - 24 - px;
    settingsPaneX = px - 8;
    ScrollArea& sa = area("settings.pane");
    sa.view = { px - 8, y, x + w - (px - 8), h };
    ui.pushArea(&sa);
    g.clip(px - 8, y, x + w - (px - 8), h);
    float py = y + 22 - sa.y;

    // What to show: the category, or every row the search matches, grouped under its category.
    std::vector<Card> cards;
    std::string title = sections[settingsCat].name;
    const char* icon = sections[settingsCat].icon;
    if (!settingsQuery.empty()) {
        const std::string q = lowerAscii(settingsQuery);
        for (const auto& s : sections) {
            Card hit{ s.name, {} };
            for (const auto& c : s.cards)
                for (const auto& r : c.rows)
                    if (!r.block && lowerAscii(r.label + " " + r.hint + " " + c.kicker + " " + s.name).find(q) != std::string::npos)
                        hit.rows.push_back(r);
            if (!hit.rows.empty()) cards.push_back(hit);
        }
        title = "“" + settingsQuery + "”";
        icon = ICON_SEARCH;
    } else {
        cards = sections[settingsCat].cards;
    }

    // .st-head: a 44 px tile with the icon, the title beside it
    g.roundRect(px, py, 44, 44, 12, pal.accent, 0.16f);
    g.icon(icon, px + 22, py + 22, 22, pal.accentInk);
    g.textC(title, { 30, Weight::ExtraBold }, px + 44 + 16, py + 22, pal.text, 1, Align::Left, int(pw - 60));
    py += 44 + 20;
    if (cards.empty()) {
        g.text(tr("Nothing matches. Press the search field to try other words.", "No hay coincidencias. Presiona el buscador para probar otras palabras."), { css::Base }, px, py + 8, pal.text3, 1, Align::Left, int(pw));
        py += 40;
    }

    // .st-group cards
    const float pad = 20, innerW = pw - pad * 2;
    settingsRows.clear();
    for (const Card& c : cards) {
        struct Laid { const Row* r; std::vector<std::string> hint; float h, labelW; };
        std::vector<Laid> laid;
        float ch = c.kicker.empty() ? 6 : 42;
        for (const Row& r : c.rows) {
            Laid l{ &r, {}, 0, 0 };
            if (r.block) {
                l.h = r.blockH + 16;
            } else {
                l.labelW = innerW - (r.ctlW > 0 ? r.ctlW + 24 : 0);
                if (!r.hint.empty()) l.hint = g.wrap(r.hint, hintF, int(l.labelW), 4);
                const float textH = 21 + (l.hint.empty() ? 0 : 3 + l.hint.size() * 18.f);
                l.h = std::max(r.minH, textH + 28);
            }
            ch += l.h;
            laid.push_back(l);
        }
        ch += 6;
        // .st-group: surface at 55%, 1 px line, 18 px radius
        g.roundRect(px, py, pw, ch, 18, pal.surface, 0.55f);
        g.roundStroke(px, py, pw, ch, 18, 1, pal.text, 0.09f);
        float ry = py + (c.kicker.empty() ? 6 : 42);
        if (!c.kicker.empty()) kicker(c.kicker, px + pad, py + 26, pal.text3, 0.12f);
        for (size_t i = 0; i < laid.size(); i++) {
            const Laid& l = laid[i];
            const Row& r = *l.r;
            if (i > 0) g.rect(px + pad, ry, innerW, 1, pal.text, 0.09f);
            if (r.lifted) {   // held to be moved: lifted, the accent around it (the queue's Move)
                g.roundRect(px + 8, ry + 4, pw - 16, l.h - 8, 12, pal.accent, 0.14f);
                g.roundStroke(px + 8, ry + 4, pw - 16, l.h - 8, 12, 2, pal.accent, 0.95f);
            }
            // showSetting: for its first frames the page scrolls to the row and focuses its control; the row is lit
            // in the accent and fades over a second and a half.
            if (settingsQuery.empty() && !settingsJump.empty() && r.label == settingsJump) {
                const double since = time - settingsJumpAt;
                if (since < 0.15) {
                    sa.target = ry - y + sa.y - 90;
                    sa.clamp();
                    if (r.focus) ui.focus(r.focus);
                }
                const float lit = float(std::clamp(1.0 - (since - 0.5) / 1.2, 0.0, 1.0));
                if (lit > 0) g.roundRect(px + 8, ry + 4, pw - 16, l.h - 8, 12, pal.accent, 0.16f * lit);
                else settingsJump.clear();
            }
            settingsRows.push_back({ px + 8, ry + 4, pw - 16, l.h - 8 });   // as showSetting lights it
            {   // every row registers, even scrolled out of view: the D-pad scrolls by moving focus onto it
                if (r.block) {
                    r.block(px + pad, ry + 8, innerW);
                } else {
                    const float textH = 21 + (l.hint.empty() ? 0 : 3 + l.hint.size() * 18.f);
                    float ty = ry + (l.h - textH) / 2;
                    const int lw2 = g.text(r.label, labelF, px + pad, ty, pal.text, 1, Align::Left, int(l.labelW));
                    if (!r.value.empty()) g.textNum(r.value, valueF, px + pad + lw2 + 7, ty, pal.text2);
                    ty += 24;
                    for (const auto& hl : l.hint) { g.text(hl, hintF, px + pad, ty, pal.text3); ty += 18; }
                    if (r.control) r.control(px + pw - pad, ry + l.h / 2);
                    else ui.item(uid("set.row", int64_t(std::hash<std::string>{}(r.label))), { px + 8, ry + 4, pw - 16, l.h - 8 }, 10);
                }
            }
            ry += l.h;
        }
        py += ch + 16;
    }
    sa.content = py + sa.y - y + 24;
    g.unclip();
    ui.popArea();
}
