/*
 * Songate · view_support.cpp
 * Settings › Support's sheets (the user, 2026-10-30): the report's form, filled with the options the app gives, sent from the
 * console to Songate's reports on GitHub (report.hpp: a repository of its own); and Atmosphère's crash reports. The card's
 * reads (the crash reports, the console's versions) and the sending run on workers; nothing of it on the main thread.
 */
#include "words.hpp"
#include <algorithm>
#include <ctime>

#include "app.hpp"
#include "cores.hpp"
#include "icons.inc"
#include "plicons.inc"

namespace {

const char* const kAreas[] = { "Now playing", "Library and playlists", "Automix", "Mood playlists", "Canvas and lyrics",
                               "Sound and equalizer",
#ifndef GF_WINDOWED
                               "Background player and overlay",
#endif
                               "Updates and settings", "Something else" };
constexpr int kAreaCount = int(std::size(kAreas));
const char* const kOften[] = { "Always", "Sometimes", "Once" };

std::string thousands(size_t n) {
    std::string s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

}  // namespace

/** Reports are off until allowed here: what a report sends, where it goes and who can see it, said before anything is sent
 *  (the user, 2026-10-30: "a toggle like Updates for Support, with the privacy prompt too"). */
void App::allowReports() {
    confirm(ICON_HELP, tr("Support", "Soporte"), tr("Send reports from SpotiBacán?", "Enviar reportes desde SpotiBacán?"), "GitHub · ohquebacan/SpotiBacan-NX",
            tr("Allow SpotiBacán to send reports?", "Permitir que SpotiBacán envie reportes?"),
            tr("SpotiBacán sends a report only when you fill in the form and press Send, to the SpotiBacán repository on GitHub (ohquebacan/SpotiBacan-NX). "
               "With what you write it sends SpotiBacán's version, your console's version, the playing track's format, how many tracks you have, and, if you leave them on, SpotiBacán's last messages. "
               "Never your files' names. You can turn this off at any time.",
               "SpotiBacán envia un reporte solo cuando completas el formulario y presionas Enviar, al repositorio de SpotiBacán en GitHub (ohquebacan/SpotiBacan-NX). "
               "Con lo que escribas se envia la version de SpotiBacán, la version del sistema, el formato de reproduccion, la cantidad de canciones y los ultimos mensajes del registro. "
               "Puedes desactivar esto en cualquier momento."),
            tr("Allow", "Permitir"), [this] {
                store.reports = true;
                persistSoon(0.3f);
                toast(tr("Reports allowed: sent only when you press Send", "Reportes permitidos: se envian solo al presionar Enviar"));
            });
    sheet.askSafe = true;
    sheet.askYesIcon = ICON_CHECK;
}

void App::openReport(int kind) {
    if (sheet.preview) SDL_DestroyTexture(sheet.preview);
    const Id back = ui.focusedId();
    sheet = Sheet{};
    sheet.page = Sheet::Report;
    sheet.repKind = kind;
    sheet.returnFocus = back;
    ui.focusDefault();
    supportScan();
}

void App::supportScan() {
    if (supportAsked_ || supportThread_.joinable()) return;
    supportAsked_ = true;
    supportReady_ = false;
    supportThread_ = std::thread([this] {
        cores::moveHere(cores::FileOps);
        supportSys_ = report::system();
        supportCrashes_ = report::crashes(20);
        supportReady_ = true;
    });
}

void App::supportTick() {
    if (supportThread_.joinable() && supportReady_) supportThread_.join();
    if (reportThread_.joinable() && reportDone_) {
        reportThread_.join();
        if (reportOk_) {
            reportSentAt_ = time;
            toast(reportNumber_ > 0 ? std::string(tr("Sent, thank you: report #", "Enviado, gracias: reporte #")) + std::to_string(reportNumber_) : std::string(tr("Sent, thank you", "Enviado, gracias")));
            if (sheet.page == Sheet::Report) closeSheet();
        } else {
            toast(std::string(tr("Not sent: ", "No enviado: ")) + reportErr_);
        }
    }
    // the app's log, saved now and then by the writer thread (store.writeFile), never from here
    if (logDirty_ && time - logSavedAt_ > 30) {
        std::string t;
        for (const auto& l : logLines_) { t += l; t += '\n'; }
        store.writeFile("log.txt", std::move(t));
        logDirty_ = false;
        logSavedAt_ = time;
    }
}

std::string App::reportBody() const {
    const bool bug = sheet.repKind == 0;
    std::string b = std::string("**") + (bug ? "Bug" : "Idea") + "** · " + (sheet.repArea >= 0 ? kAreas[sheet.repArea] : "?");
    if (bug) b += std::string(" · ") + kOften[std::clamp(sheet.repOften, 0, 2)];
    b += "\n\n### " + std::string(bug ? "What happened" : "The idea") + "\n\n" + (sheet.repWhat.empty() ? "(not given)" : sheet.repWhat) + "\n";
    if (bug) b += "\n### Steps\n\n" + (sheet.repSteps.empty() ? std::string("(not given)") : sheet.repSteps) + "\n";
    // the app's part: nothing of the user's files but the playing one's format
    b += "\n### " GF_CAP_THIS_CONSOLE "\n\n| | |\n|---|---|\n| SpotiBacán | " + Updater::thisVersion() + " · protocol " + std::to_string(Updater::thisProtocol()) + " |\n";
    b += "{{SYSTEM}}";
#ifndef GF_WINDOWED
    b += "| Background player | " + std::string(player.remote() ? "connected" : "not connected") + " |\n";
#endif
    if (now.track >= 0 && !now.info.codec.empty()) {
        std::string f = now.info.codec;
        if (now.info.sampleRate > 0) { char r[24]; snprintf(r, sizeof r, " · %.1f kHz", now.info.sampleRate / 1000.0); f += r; }
        if (now.info.bitsPerSample > 0) f += " · " + std::to_string(now.info.bitsPerSample) + " bit";
        else if (now.info.bitrateKbps > 0) f += " · " + std::to_string(now.info.bitrateKbps) + " kbps";
        b += "| Playing | " + f + " |\n";
    }
    b += "| Library | " + thousands(lib.tracks.size()) + " tracks |\n";
    b += "{{CRASHES}}";
    if (sheet.repLog && !logLines_.empty()) {
        const size_t n = std::min<size_t>(40, logLines_.size());
        b += "\n### SpotiBacán's log (" + (n == 1 ? std::string("its last message") : "the last " + std::to_string(n) + " messages") + ")\n\n```\n";
        for (size_t i = logLines_.size() > 40 ? logLines_.size() - 40 : 0; i < logLines_.size(); i++) b += logLines_[i] + "\n";
        b += "```\n";
    }
    b += "\n*Sent from SpotiBacán's Settings › Support.*\n";
    return b;
}

void App::reportHeader(const char*& icon, std::string& kick, std::string& title, std::string& sub) {
    const bool bug = sheet.repKind == 0;
    icon = bug ? ICON_BUG : PLI_BULB;
    kick = tr("Support", "Soporte");
    title = bug ? tr("Report a bug", "Reportar un error") : tr("Suggest a feature", "Sugerir una funcion");
    sub = report::available() ? tr("Sent to SpotiBacán on GitHub (ohquebacan/SpotiBacan-NX), with what is listed below", "Enviado a SpotiBacán en GitHub (ohquebacan/SpotiBacan-NX), con lo indicado abajo") : tr("Open an issue at github.com/ohquebacan/SpotiBacan-NX/issues", "Abre un issue en github.com/ohquebacan/SpotiBacan-NX/issues");
}

void App::reportRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    const bool bug = sheet.repKind == 0;
    Row kind{ Row::Setting, nullptr, tr("It is", "Tipo") };
    kind.opts = { tr("A bug", "Un error"), tr("An idea", "Una idea") };
    kind.cur = sheet.repKind;
    kind.pick = [this](int p) { sheet.repKind = p; };
    kind.h = 56;
    rows.push_back(kind);
    rows.push_back({ Row::Name, nullptr, tr("Title", "Titulo"), "", sheet.repTitle.empty() ? tr("Add a title", "Agregar un titulo") : sheet.repTitle, [this] {
                         std::string t;
                         if (keyboard(tr("A short title", "Un titulo breve"), sheet.repTitle, t)) {
                             while (!t.empty() && t.back() == ' ') t.pop_back();
                             sheet.repTitle = t;
                         }
                     } });
    rows.back().h = 48;
    rows.push_back({ Row::Name, nullptr, bug ? tr("What happened", "Que ocurrio") : tr("The idea", "La idea"), "", sheet.repWhat.empty() ? tr("Tell us", "Cuentanos") : sheet.repWhat, [this, bug] {
                         std::string t;
                         if (keyboard(bug ? tr("What happened, and what you expected", "Que ocurrio y que esperabas") : tr("Your idea", "Tu idea"), sheet.repWhat, t)) sheet.repWhat = t;
                     } });
    rows.back().h = 48;
    if (bug) {
        rows.push_back({ Row::Name, nullptr, tr("Steps", "Pasos"), "", sheet.repSteps.empty() ? tr("How to make it happen", "Como reproducirlo") : sheet.repSteps, [this] {
                             std::string t;
                             if (keyboard(tr("The steps that make it happen", "Los pasos para reproducirlo"), sheet.repSteps, t)) sheet.repSteps = t;
                         } });
        rows.back().h = 48;
        Row often{ Row::Setting, nullptr, tr("How often", "Con que frecuencia") };
        often.opts = { tr("Always", "Siempre"), tr("Sometimes", "A veces"), tr("Once", "Una vez") };
        often.cur = sheet.repOften;
        often.pick = [this](int p) { sheet.repOften = p; };
        often.h = 56;
        rows.push_back(often);
    }
    rows.push_back({ Row::Group, nullptr, tr("Which part of SpotiBacán", "Que parte de SpotiBacán") });
    rows.back().h = 38;
    const char* const areaLabels[] = {
        tr("Now playing", "Esta sonando"),
        tr("Library and playlists", "Biblioteca y playlists"),
        "Automix",
        tr("Mood playlists", "Playlists por estado de animo"),
        tr("Canvas and lyrics", "Lienzo y letras"),
        tr("Sound and equalizer", "Sonido y ecualizador"),
#ifndef GF_WINDOWED
        tr("Background player and overlay", "Reproductor en segundo plano y overlay"),
#endif
        tr("Settings", "Configuracion"),
        tr("Something else", "Otra cosa")
    };
    for (int a = 0; a < kAreaCount; a++) {
        rows.push_back({ Row::Choice, nullptr, areaLabels[a], "", "", [this, a] { sheet.repArea = a; } });
        rows.back().h = 42;
        rows.back().on = sheet.repArea == a;
    }
    rows.push_back({ Row::Group, nullptr, tr("Sent with it", "Enviado con el reporte") });
    rows.back().h = 38;
#ifndef GF_WINDOWED
    // the crash reports of the last 30 days (none: nothing to send)
    int recent = 0;
    if (supportAsked_ && !supportThread_.joinable())
        for (const auto& c : supportCrashes_) recent += c.at > (long long)std::time(nullptr) - 30LL * 86400 ? 1 : 0;
    Row crashes{ Row::Toggle, nullptr, tr("Crash reports", "Reportes de errores"),
                 recent ? std::to_string(recent) + tr(" from Atmosphère in the last 30 days: when, which program, the error", " de Atmosphère en los ultimos 30 dias: fecha, programa y error") : tr("None from Atmosphère in the last 30 days", "Ninguno de Atmosphère en los ultimos 30 dias"),
                 "", [this] { sheet.repCrashes = !sheet.repCrashes; } };
    crashes.on = sheet.repCrashes && recent > 0;
    if (!recent) crashes.act = nullptr;
    crashes.h = 62;
    rows.push_back(crashes);
#endif
    Row log{ Row::Toggle, nullptr, tr("SpotiBacán's log", "Registro de SpotiBacán"), tr("Its last 40 messages, the ones shown at the bottom of the screen", "Sus ultimos 40 mensajes mostrados en la parte inferior"), "",
             [this] { sheet.repLog = !sheet.repLog; } };
    log.on = sheet.repLog;
    log.h = 62;
    rows.push_back(log);
    Row text{ Row::Text, ICON_INFO, "", tr("Always: SpotiBacán's version, your console's version, the playing file's format and how many tracks you have. Never your files' names.",
                                           "Siempre: la version de SpotiBacán, la version de la consola, el formato de reproduccion y cuantas canciones tienes.") };
    text.h = 2 * 19.f + 14;
    rows.push_back(text);
}

void App::reportFooter(float x, float w, float fy) {
    const Font bf{ css::Sm, Weight::SemiBold };
    const bool sending = reportThread_.joinable();
    const char* label = sending ? tr("Sending…", "Enviando…") : tr("Send", "Enviar");
    const char* cancelTxt = tr("Cancel", "Cancelar");
    const float sw = g.measure(label, bf) + 32 + 24, kw = g.measure(cancelTxt, bf) + 32;
    bool c = false;
    button(uid("rp.send"), ICON_CHECK, label, x + w - 24 - sw, fy + 7, true, &c, false, false, DefaultFocus, sw);
    if (c && !sending) {
        const double wait = 300 - (time - reportSentAt_);
        if (!report::available()) toast(tr("This build cannot send reports", "Esta compilacion no puede enviar reportes"));
        else if (!store.reports) { closeSheet(); allowReports(); }   // (switched off meanwhile: asked again)
        else if (sheet.repTitle.empty()) toast(tr("Add a title first", "Agrega un titulo primero"));
        else if (sheet.repArea < 0) toast(tr("Choose the part of SpotiBacán it is about", "Elige de que parte de SpotiBacán trata"));
        else if (wait > 0) toast(std::string(tr("One report every five minutes: try again in ", "Un reporte cada cinco minutos: intenta en ")) + std::to_string(int(wait / 60) + 1) + " min");
        else {
            const bool bug = sheet.repKind == 0;
            const std::string title = std::string(bug ? "[Bug] " : "[Idea] ") + sheet.repTitle;
            const std::string body = reportBody();
            const bool withCrashes = sheet.repCrashes;
            reportDone_ = false;
            reportThread_ = std::thread([this, title, body, withCrashes, bug] {
                cores::moveHere(cores::Net);
                const report::System s = report::system();
                std::string b = body;
#ifdef GF_WINDOWED
                const std::string sys = "| System | " + s.firmware + " |\n";
#else
                const std::string sys = "| Firmware | " + s.firmware + " |\n| Atmosphère | " + s.atmosphere + " |\n| Mode | " +
                                        (s.docked ? "docked" : "handheld") + " |\n";
#endif
                b.replace(b.find("{{SYSTEM}}"), 10, sys);
                std::string cr;
                if (withCrashes) {
                    const long long since = (long long)std::time(nullptr) - 30LL * 86400;
                    for (const auto& c : report::crashes(10))
                        if (c.at > since) cr += "- " + c.when + " · " + c.program + " · " + c.result + "\n";
                    if (!cr.empty()) cr = "\n### Crash reports (Atmosphère, the last 30 days)\n\n" + cr;
                }
                b.replace(b.find("{{CRASHES}}"), 11, cr);
                reportOk_ = report::send(title, b, bug ? "bug" : "enhancement", reportNumber_, reportErr_);
                reportDone_ = true;
            });
        }
    }
    button(uid("rp.cancel"), nullptr, cancelTxt, x + w - 24 - sw - 8 - kw, fy + 7, false, &c);
    if (c) closeSheet();
}

void App::crashRows(std::vector<SheetRow>& rows) {
    using Row = SheetRow;
    if (!supportAsked_ || supportThread_.joinable()) {
        rows.push_back({ Row::Text, ICON_HOURGLASS, "", tr("Reading them…", "Leyendo…") });
        rows.back().h = 19 + 14;
        return;
    }
    if (supportCrashes_.empty()) {
        rows.push_back({ Row::Text, ICON_CHECK, "", tr("None on this console: Atmosphère has not recorded a crash.", "Ninguno en esta consola: Atmosphère no ha registrado cierres.") });
        rows.back().h = 19 + 14;
        return;
    }
    for (const auto& c : supportCrashes_) {
        rows.push_back({ Row::Action, c.fatal ? ICON_X : ICON_BUG, c.program, c.when + "  ·  " + c.result, "", nullptr });
        rows.back().h = 58;
    }
}
