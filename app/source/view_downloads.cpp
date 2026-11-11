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

    // lo guardado
    kicker(tr("SAVED", "GUARDADAS"), px, cy + 6, pal.text3);
    cy += 22;
    if (saved.empty()) {
        g.text(tr("Nothing yet. In any playlist, tap Download, or the arrow next to a song.",
                  "Todavia nada. En cualquier lista toca Descargar, o la flecha junto a una cancion."), { css::Base }, px, cy + 10, pal.text2, 1,
               Align::Left, int(pw));
        cy += 60;
    } else {
        const float rh = g_bigLayout ? 70.f : 60.f;   // con portada, como las listas
        for (size_t i = 0; i < saved.size(); ++i) {
            if (cy + rh > y - rh && cy < y + h + rh) {
                if (coverRow(uid("dl.row", int(i)), saved[i], int(i) + 1, px - 14, cy, pw + 28, rh, false)) playLocal(saved, int(i), false);
            }
            cy += rh;
        }
    }
    sa.content = cy - oy + 40;
    ui.popArea();
}
