/*
 * SpotiBacan · view_downloads.cpp
 * Descargas como una lista, igual que en SpotiBacan 3DS: arriba lo que se esta bajando (la cancion actual con su barra
 * de progreso y las que esperan), abajo todo lo guardado en la SD en una sola lista que se reproduce desde la tarjeta,
 * sin internet. Antes era el explorador de archivos y habia que ir carpeta por carpeta.
 */
#include "downloader.hpp"
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <strings.h>
#include <unordered_map>

#include "app.hpp"
#include "icons.inc"

namespace {

/** sdmc:/music/SpotiBacan/<Artista>/<Album>/<NN - Titulo>.ogg → la cancion, hasta que lleguen sus etiquetas. */
Track trackFromPath(const std::string& full) {
    Track t;
    t.path = full;
    const size_t s3 = full.find_last_of('/');
    t.folder = full.substr(0, s3);
    std::string name = full.substr(s3 + 1);
    if (name.size() > 4) name.resize(name.size() - 4);
    if (name.size() > 5 && isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) && name.compare(2, 3, " - ") == 0) {
        t.trackNo = atoi(name.c_str());
        name = name.substr(5);
    }
    t.title = name;
    const size_t s2 = full.find_last_of('/', s3 - 1);
    const size_t s1 = s2 == std::string::npos ? std::string::npos : full.find_last_of('/', s2 - 1);
    if (s2 != std::string::npos) t.album = full.substr(s2 + 1, s3 - s2 - 1);
    if (s1 != std::string::npos) t.artist = full.substr(s1 + 1, s2 - s1 - 1);
    t.albumArtist = t.artist;
    return t;
}

}  // namespace

std::vector<int> App::downloadedTracks() {
    static uint64_t gen = ~0ull;
    static std::vector<int> cached;
    const uint64_t g2 = Downloader::instance().generation();
    if (g2 == gen) return cached;
    gen = g2;
    cached.clear();
    bool added = false;
    for (const std::string& p : Downloader::instance().savedList()) {
        auto it = pathIndex.find(p);
        if (it == pathIndex.end()) {
            lib.tracks.push_back(trackFromPath(p));
            pathIndex[p] = int(lib.tracks.size()) - 1;
            cached.push_back(int(lib.tracks.size()) - 1);
            added = true;
        } else {
            cached.push_back(it->second);
        }
    }
    if (added) {
        rebuildIndex();
        meta.syncLibrary(&lib, store.path("tags.tsv"));   // etiquetas y caratula desde el propio .ogg
    }
    return cached;
}

void App::playLocal(const std::vector<int>& tracks, int start, bool shuffle) {
    if (tracks.empty()) return;
    // lo de la SD lo toca el reproductor de la app: el servicio de fondo se pausa y se suelta hasta volver a Spotify
    if (player.remote()) {
        bg.pause(time);
        player.setRemote(nullptr);
    }
    localPlayback_ = true;
    playTracks(tracks, start, tr("Downloads", "Descargas"), shuffle);
}

void App::drawDownloads(float x, float y, float w, float h) {
    ScrollArea& sa = area("downloads");
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;
    const float px = x + 28, pw = w - 56;

    const std::vector<int> saved = downloadedTracks();
    DlJob cur;
    std::vector<DlJob> pending;
    Downloader::instance().queueSnapshot(cur, pending);
    const Downloader::Status ds = Downloader::instance().status();

    // cabecera
    float cy = oy + 18;
    kicker(tr("ON YOUR SD CARD", "EN TU SD"), px, cy + 8, pal.accent);
    g.text(tr("Downloads", "Descargas"), { css::Xxl, Weight::Bold }, px, cy + 18, pal.text);
    char sub[160];
    snprintf(sub, sizeof sub, tr("%zu songs · they play without internet", "%zu canciones · suenan sin internet"), saved.size());
    std::string subS = sub;
    if (ds.active) subS += std::string("  ·  ") + tr("downloading ", "bajando ") + std::to_string(pending.size() + 1);
    g.text(subS, { css::Sm }, px, cy + 18 + 46, pal.text3, 1, Align::Left, int(pw));
    cy += 18 + 46 + 30;
    {
        float bx = px;
        bool c = false;
        bx += button(uid("dl.play"), ICON_PLAY, tr("Play", "Reproducir"), bx, cy, true, &c) + 8;
        if (c && !saved.empty()) playLocal(saved, 0, false);
        bx += button(uid("dl.shuffle"), ICON_SHUFFLE, tr("Shuffle", "Aleatorio"), bx, cy, false, &c) + 8;
        if (c && !saved.empty()) playLocal(saved, 0, true);
        if (ds.active || !pending.empty()) {
            bx += button(uid("dl.cancel"), ICON_X, tr("Stop downloads", "Detener descargas"), bx, cy, false, &c, false, true) + 8;
            if (c) { Downloader::instance().cancelAll(); toast(tr("Downloads stopped", "Descargas detenidas")); }
        }
    }
    cy += css::BtnH + 22;

    // la cola
    if (!cur.uri.empty() || !pending.empty()) {
        kicker(tr("DOWNLOADING", "BAJANDO"), px, cy + 6, pal.text3);
        cy += 20;
        if (!cur.uri.empty()) {
            const float rh = 64;
            g.roundRect(px, cy, pw, rh, 14, pal.accent, 0.12f);
            g.icon(ICON_DOWNLOAD, px + 30, cy + rh / 2, 20, pal.accentInk);
            g.text(cur.title, { css::Base, Weight::SemiBold }, px + 58, cy + 10, pal.text, 1, Align::Left, int(pw - 140));
            g.text(cur.artist, { css::Sm }, px + 58, cy + 10 + css::Base + 4, pal.text3, 1, Align::Left, int(pw - 140));
            g.textC(std::to_string(int(ds.progress * 100)) + "%", { css::Base, Weight::SemiBold }, px + pw - 18, cy + rh / 2, pal.accentInk, 1, Align::Right);
            g.roundRect(px + 14, cy + rh - 8, pw - 28, 3, 1.5f, pal.text, 0.12f);
            g.roundRect(px + 14, cy + rh - 8, (pw - 28) * std::clamp(ds.progress, 0.f, 1.f), 3, 1.5f, pal.accent);
            cy += rh + 6;
        }
        const size_t shown = std::min<size_t>(pending.size(), 50);
        for (size_t i = 0; i < shown; ++i) {
            const float rh = 46;
            if (cy + rh > y - 60 && cy < y + h + 60) {
                g.icon(ICON_HOURGLASS, px + 30, cy + rh / 2, 16, pal.text3);
                g.textC(pending[i].title, { css::Base }, px + 58, cy + rh / 2 - 9, pal.text2, 1, Align::Left, int(pw - 80));
                g.textC(pending[i].artist, { css::Sm }, px + 58, cy + rh / 2 + 11, pal.text3, 1, Align::Left, int(pw - 80));
            }
            cy += rh;
        }
        if (pending.size() > shown) {
            g.text(std::to_string(pending.size() - shown) + tr(" more in the queue", " mas en la cola"), { css::Sm }, px + 58, cy + 4, pal.text3);
            cy += 28;
        }
        if (ds.keyDenied > 0) {
            g.text(tr("Spotify refused the audio key for some songs (likely the account)",
                      "Spotify nego la clave de audio de algunas canciones (probablemente por la cuenta)"), { css::Sm }, px, cy + 6, pal.text3);
            cy += 28;
        }
        cy += 16;
    }

    // lo guardado: Canciones, Artistas o Albumes; un artista o un album abierto muestra solo lo suyo
    static int tab = 0;
    const std::string& q = route.query;
    const bool inArtist = q.rfind("artist:", 0) == 0, inAlbum = q.rfind("album:", 0) == 0;
    auto artistOf = [&](int ti) { const Track& t = lib.tracks[ti]; return t.albumArtist.empty() ? t.artist : t.albumArtist; };
    std::vector<int> shown;
    if (inArtist || inAlbum) {
        const std::string key = q.substr(inArtist ? 7 : 6);
        for (int ti : saved) {
            if (inArtist ? artistOf(ti) == key : (artistOf(ti) + '\x1f' + lib.tracks[ti].album) == key) shown.push_back(ti);
        }
        bool c = false;
        button(uid("dl.back"), ICON_BACK, tr("All downloads", "Todas las descargas"), px, cy, false, &c);
        if (c) navigate(View::Downloads, -1, "", false);
        const std::string name = inArtist ? key : key.substr(key.find('\x1f') + 1);
        g.textC(name, { css::Lg, Weight::Bold }, px + 260, cy + css::BtnH / 2, pal.text, 1, Align::Left, int(pw - 270));
        cy += css::BtnH + 16;
    } else {
        // pestanas grandes, como pastillas
        const char* tabs[3] = { tr("Songs", "Canciones"), tr("Artists", "Artistas"), tr("Albums", "Albumes") };
        float tx = px;
        const Font tf{ css::Base, Weight::SemiBold };
        for (int i = 0; i < 3; i++) {
            const float tw = float(g.measure(tabs[i], tf)) + 40, th = css::BtnH;
            const Id id = uid("dl.tab", i);
            if (ui.item(id, { tx, cy, tw, th }, th / 2)) tab = i;
            const bool on = tab == i, hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
            g.roundRect(tx, cy, tw, th, th / 2, on ? pal.accent : pal.text, on ? 0.9f : hot ? 0.14f : 0.07f);
            g.textC(tabs[i], tf, tx + tw / 2, cy + th / 2, on ? pal.onAccent : pal.text, 1, Align::Center);
            tx += tw + 8;
        }
        cy += css::BtnH + 16;
    }

    const float rh = g_bigLayout ? 70.f : 60.f;
    if (saved.empty()) {
        g.text(tr("Nothing yet. In any playlist, tap Download, or the arrow next to a song.",
                  "Todavia nada. En cualquier lista toca Descargar, o la flecha junto a una cancion."), { css::Base }, px, cy + 10, pal.text2, 1,
               Align::Left, int(pw));
        cy += 60;
    } else if (inArtist || inAlbum || tab == 0) {
        const std::vector<int>& list = (inArtist || inAlbum) ? shown : saved;
        for (size_t i = 0; i < list.size(); ++i) {
            if (cy + rh > y - rh && cy < y + h + rh) {
                if (coverRow(uid("dl.row", int(i)), list[i], int(i) + 1, px - 14, cy, pw + 28, rh, false)) playLocal(list, int(i), false);
            }
            cy += rh;
        }
    } else {
        // artistas o albumes, con cuantas canciones tiene cada uno (ordenados por nombre)
        struct Group { std::string key, name, sub; int first = -1, count = 0; };
        std::vector<Group> groups;
        std::unordered_map<std::string, size_t> at;
        for (int ti : saved) {
            const std::string art = artistOf(ti);
            const std::string key = tab == 1 ? art : art + '\x1f' + lib.tracks[ti].album;
            auto it = at.find(key);
            if (it == at.end()) {
                at[key] = groups.size();
                groups.push_back({ key, tab == 1 ? art : lib.tracks[ti].album, tab == 1 ? std::string() : art, ti, 1 });
            } else groups[it->second].count++;
        }
        std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
        for (size_t i = 0; i < groups.size(); ++i) {
            const Group& gr = groups[i];
            if (cy + rh > y - rh && cy < y + h + rh) {
                const Id id = uid(tab == 1 ? "dl.artist" : "dl.album", int(i));
                const bool hit = ui.item(id, { px - 14, cy, pw + 28, rh - 4 }, 10);
                const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
                if (hot) g.roundRect(px - 14, cy, pw + 28, rh - 4, 10, pal.text, ui.pressed(id) ? 0.11f : 0.065f);
                const float cov = rh - 20, ccy = cy + (rh - 4) / 2;
                if (tab == 1) {   // artista: circulo con la portada de una de sus canciones
                    coverBox(gr.first, px + 4, ccy - cov / 2, cov, cov / 2);
                } else coverBox(gr.first, px + 4, ccy - cov / 2, cov, 7);
                const float tx = px + 4 + cov + 16;
                g.text(gr.name, { css::Base, Weight::SemiBold }, tx, ccy - (gr.sub.empty() ? 10 : 20), pal.text, 1, Align::Left, int(pw - cov - 160));
                if (!gr.sub.empty()) g.text(gr.sub, { css::Sm }, tx, ccy + 2, pal.text3, 1, Align::Left, int(pw - cov - 160));
                g.textC(std::to_string(gr.count) + (gr.count == 1 ? tr(" song", " cancion") : tr(" songs", " canciones")), { css::Sm }, px + pw - 30, ccy,
                        pal.text3, 1, Align::Right);
                g.icon(ICON_CHEV, px + pw - 8, ccy, 16, pal.text3);
                if (hit) navigate(View::Downloads, -1, (tab == 1 ? "artist:" : "album:") + gr.key);
            }
            cy += rh;
        }
    }
    sa.content = cy - oy + 40;
    ui.popArea();
}
