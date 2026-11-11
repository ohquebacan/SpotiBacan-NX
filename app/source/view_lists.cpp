/*
 * Gatefold NX · view_lists.cpp
 * Track lists: #view-playlist (play queue, Liked, folder playlists, search),
 * #view-album, and the Folders browser that Gatefold NX adds.
 */
#include "downloader.hpp"
#include "logger.hpp"
#include "words.hpp"
#include <algorithm>
#include <cmath>
#include <strings.h>
#include <sys/stat.h>

#include "app.hpp"
#include "icons.inc"

namespace {
#define RowH (g_bigLayout ? 48.f : 40.f)   // SpotiBacan: filas mas altas en la vista Grande
}

void App::drawTrackList(float x, float y, float w, float h) {
    const View v = route.view;
    const std::vector<int> tracks = listTracks(v, route.ref, route.query);

    std::string kick = "Playlist", title, sub, playName;
    const char* placeholder = ICON_LIST;
    if (v == View::Queue) {
        // the queue wears the kind and icon of the playlist it came from, as that playlist's own page
        // (Automix: the user, 2026-10-29; Smart and the rest: 2026-10-29, session 6)
        if (const int src = queuePlaylist(); src >= 0) {
            kick = playlistKind(src);
            placeholder = playlistIcon(src);
        } else if (q.name == "Liked" || q.name == "Tus me gusta") placeholder = ICON_HEART;
        title = q.name.empty() ? tr("Nothing queued", "Nada en la cola")
                               : (q.name == "Liked" || q.name == "Tus me gusta") ? tr("Liked Songs", "Tus me gusta") : q.name;
        playName = q.name;
    } else if (v == View::Search) {
        kick = tr("Search", "Busqueda");
        title = "“" + route.query + "”";
        playName = std::string(tr("Search: ", "Busqueda: ")) + route.query;
        placeholder = ICON_SEARCH;
    } else if (route.ref < 0) {
        title = tr("Liked Songs", "Tus me gusta");
        playName = "Liked";
        placeholder = ICON_HEART;
    } else if (route.ref < int(store.playlists.size())) {
        const FolderPlaylist& p = store.playlists[route.ref];
        kick = playlistKind(route.ref);
        title = (p.name == "Tus me gusta" || p.name == "Liked") ? tr("Liked Songs", "Tus me gusta") : p.name;
        playName = p.name;
        placeholder = playlistIcon(route.ref);
    }
    sub = fmtTotal(tracks);
    // SpotiBacan: las canciones de Spotify de esta lista, para «Descargar», y cuantas ya estan en la SD. Se recalcula
    // solo cuando cambia la lista o lo guardado (antes se miraba la SD por cada cancion en cada cuadro: con una lista de
    // 1300 canciones, 1300 consultas a la tarjeta 60 veces por segundo, y la app iba a tirones).
    static struct { View v = View::Home; int ref = -2; size_t n = 0; int first = -1; uint64_t gen = ~0ull; int have = 0; std::vector<DlJob> jobs; } dl;
    if (v != View::Queue && v != View::Search) {
        const uint64_t gen = Downloader::instance().generation();
        const int first = tracks.empty() ? -1 : tracks.front();
        if (dl.v != v || dl.ref != route.ref || dl.n != tracks.size() || dl.first != first || dl.gen != gen) {
            dl.v = v; dl.ref = route.ref; dl.n = tracks.size(); dl.first = first; dl.gen = gen;
            dl.have = 0;
            dl.jobs.clear();
            for (int ti : tracks) {
                if (ti < 0 || ti >= int(lib.tracks.size())) continue;
                const Track& t = lib.tracks[ti];
                if (t.path.rfind("spotify:track:", 0) != 0) continue;
                DlJob j{ t.path, t.title, t.albumArtist.empty() ? t.artist : t.albumArtist, t.album, t.trackNo };
                if (Downloader::instance().isDownloaded(j)) ++dl.have;
                else dl.jobs.push_back(std::move(j));
            }
        }
        const Downloader::Status ds = Downloader::instance().status();
        if (ds.active) {
            char pc[48];
            snprintf(pc, sizeof pc, "  ·  descargando (%d en cola, %d%%)", ds.pending + 1, int(ds.progress * 100));
            sub += pc;
        } else if (dl.have > 0) {
            sub += "  ·  " + std::to_string(dl.have) + (dl.have == 1 ? " descargada" : " descargadas");
        }
        if (ds.keyDenied > 0 && !ds.active) sub += "  ·  Spotify nego la clave de audio (probablemente por la cuenta)";
    }
    if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size())) {   // an Automix playlist building up
        const FolderPlaylist& p = store.playlists[route.ref];
        if (p.automix && p.waiting && !p.manual) {
            int more = 0;
            for (const auto& path : p.tracks) more += pathIndex.count(path) && !automixReady(path) ? 1 : 0;
            if (more > 0) sub += "  ·  " + std::to_string(more) + (more == 1 ? tr(" more song being analysed", " cancion mas analizandose") : tr(" more songs being analysed", " canciones mas analizandose"));
        }
        // a Mood playlist while the library's moods are measured: as an Automix playlist building up
        if (isMoodPlaylist(p) && moodTotal_ > moodDone_) {
            const int more = moodTotal_ - moodDone_;
            sub += "  ·  " + std::to_string(more) + (more == 1 ? tr(" more song being analysed", " cancion mas analizandose") : tr(" more songs being analysed", " canciones mas analizandose"));
        }
    }
    if (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && !store.playlists[route.ref].own()) {
        const FolderPlaylist& p = store.playlists[route.ref];
        sub = (p.smart() ? smartSummary(p.path) : p.path) + "  ·  " + sub;
    }

    ScrollArea& sa = area(v == View::Search ? "search" : "list", int(v) * 1000 + route.ref);
    const float headH = 18 + 120 + 14;
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;
    // Room between the rows for the joins (a playlist's page, not the grid): Automix playlists, and the queue while it plays
    // one, its joins the same (the user, 2026-10-30: "the temporary automix playlist should also show them")
    const bool mixable = (v == View::List && route.ref >= 0 && route.ref < int(store.playlists.size()) && store.playlists[route.ref].automix) ||
                         (v == View::Queue && automixQueue());
    // no Grid there (the user, 2026-10-30: "in Automix playlists this view makes no sense": tiles have no order to join): shown
    // with covers instead, your choice for other playlists kept
    const ListMode mode = mixable && ListMode(store.viewPlaylist) == ListMode::Grid ? ListMode::Covers : ListMode(store.viewPlaylist);
    // the joins always shown: no Mix button to hide them (the user, 2026-10-30: it read as turning the mixing off)
    const float joinGap = mixable && mode != ListMode::Grid ? 46 : 0;

    // .pl-head: cover, kicker, title, sub, actions
    const float cx = x + 28, cyTop = oy + 18;
    if (!tracks.empty()) {
        coverQuad(tracks, cx, cyTop, 120, 12);   // .pl-cover.quad (no halo, as on the other pages)
    } else {
        g.roundRect(cx, cyTop, 120, 120, 12, pal.surface2);
        g.icon(placeholder, cx + 60, cyTop + 60, 40, pal.text3, 0.7f);
    }
    const float tx = cx + 120 + 22, tw = w - (tx - x) - 28;
    if (kick == "Automix playlist") automixKicker(kick, tx, cyTop + 8);   // with a little magic (widgets.cpp)
    else kicker(kick, tx, cyTop + 8, pal.text3);
    g.text(title, { css::Xxl, Weight::Bold }, tx, cyTop + 16, pal.text, 1, Align::Left, int(tw));
    g.text(sub, { css::Sm }, tx, cyTop + 58, pal.text3, 1, Align::Left, int(tw));
    float bx = tx;
    const float by = cyTop + 120 - css::BtnH;
    bool clicked = false;
    // The page's playlist, as it was when the frame began: Export and Open folder go to another page part way through
    // (route.ref is then the picker's −1 or a folder's index), and the buttons after them read the playlist again
    const int ref = route.ref;
    const bool ownPage = v == View::List && ref >= 0 && ref < int(store.playlists.size());
    // The actions after Play and Shuffle shrink to round icon buttons when their labels would run into the views
    // (a folder playlist with Games has seven); the playlist's icon button is always one.
    bool compact = false;
    // An Automix playlist plays in its own order: no Shuffle on its page, nor on the Playlist page while one plays (the user,
    // 2026-10-29; App::holdOrderForAutomix)
    const bool noShuffle = (v == View::Queue && automixQueue()) || (ownPage && store.playlists[ref].automix);
    {
        const Font bf{ css::Sm, Weight::SemiBold };
        auto bw = [&](const char* label) { return 16 + 24 + g.measure(label, bf) + 16 + 8.f; };
        float need = bw("Play") + (noShuffle ? 0 : bw("Shuffle"));
        if (v == View::Queue) need += bw("Current track");
        else if (v == View::Search) need += bw("New search");
        else if (ownPage && !store.playlists[ref].folder()) need += bw("Delete") + bw("Rename") + (store.playlists[ref].automix ? bw("Order again") : 0);
        else if (ref >= 0) need += bw("Open folder") + bw("Delete");
        if (v == View::List && bgShown()) need += bw("Games");
        if (v != View::Search) need += bw("Export");
        need += bw("Descargar");
        if (ownPage) need += 34 + 8;
        compact = tx + need > x + w - 28 - (6 + 38 * 4) - 16;
    }
    // A secondary action: labelled, or (compact) round with its icon only
    auto action = [&](const char* key, const char* icon, const char* label, bool toggled = false, bool danger = false) {
        if (!compact) {
            bool c = false;
            bx += button(uid(key), icon, label, bx, by, false, &c, toggled, danger) + 8;
            return c;
        }
        const Id id = uid(key);
        hint(id, label);   // round with its icon only: its name when focused (App::drawHint)
        const bool c = ui.item(id, { bx, by, css::BtnH, css::BtnH }, css::BtnH / 2);
        const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());
        g.roundRect(bx, by, css::BtnH, css::BtnH, css::BtnH / 2, toggled ? pal.accent : pal.text, toggled ? 0.18f : hot ? 0.11f : 0.065f);
        g.roundStroke(bx, by, css::BtnH, css::BtnH, css::BtnH / 2, 1, toggled ? pal.accent : pal.text, toggled ? 0.5f : 0.15f);
        g.icon(icon, bx + css::BtnH / 2, by + css::BtnH / 2, 16, danger ? dangerInk() : toggled ? pal.accentInk : pal.text);
        bx += css::BtnH + 8;
        return c;
    };
    bx += button(uid("pl.play"), ICON_PLAY, tr("Play", "Reproducir"), bx, by, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked && !tracks.empty()) { if (v == View::Queue) playAt(0); else playTracks(tracks, 0, playName); }
    if (!noShuffle) {
        bx += button(uid("pl.shuffle"), ICON_SHUFFLE, tr("Shuffle", "Aleatorio"), bx, by, false, &clicked) + 8;
        if (clicked && !tracks.empty()) playTracks(tracks, 0, playName, true);
    }
    if (v != View::Queue && v != View::Search && !dl.jobs.empty() && action("pl.download", ICON_DOWNLOAD, tr("Download", "Descargar"))) {
        const int n = Downloader::instance().enqueue(dl.jobs);
        LOG("Descargar esta lista: %d canciones nuevas en cola", n);
    }
    if (v == View::Queue) {
        clicked = action("pl.jump", ICON_DISC, tr("Current track", "Cancion actual"));
        const int cur = q.pos;   // the list is in play order
        if (clicked && cur >= 0) {
            // Gatefold's "Current track": the list glides there (the row need not exist yet, it is
            // only drawn once on screen) and the focus lands on it when it arrives.
            sa.target = headH + listRowY(listLayout(tracks, w, mode), cur) - (h - RowH) / 2;
            sa.clamp();
            listJump = uid("pl.r", cur);
        }
    } else if (v == View::Search) {
        if (action("pl.search", ICON_SEARCH, tr("New search", "Nueva busqueda"))) openSearch();
    } else if (v == View::List && ref >= 0 && ref < int(store.playlists.size()) && !store.playlists[ref].folder()) {
        if (store.playlists[ref].automix && action("pl.order", ICON_LINK, tr("Order again", "Ordenar de nuevo"), store.playlists[ref].manual)) orderAutomixList(ref, true);
    }

    // the four views, right of the actions (.pl-modes)
    {
        int m = int(mode);
        viewSwitch("pl.view", x + w - 28, by + css::BtnH / 2, m, mixable ? 1u << int(ListMode::Grid) : 0u);
        if (m != int(mode)) store.viewPlaylist = m;
    }

    // the tracks (no panel behind them: the background shows through, as everywhere here)
    const float ly = oy + headH;
    if (tracks.empty()) {
        listColumns(x, ly, w, mode == ListMode::Grid ? ListMode::List : mode, false);
        if (v == View::List && ref >= 0 && ref < int(store.playlists.size()) && player.remote()) {
            fetchPlaylistOnDemand(ref);
            const char* msg = (onDemandLoadingRef_ == ref)
                                  ? tr("Loading tracks...", "Cargando canciones...")
                                  : tr("This playlist has no tracks", "Esta playlist no tiene canciones");
            g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, ly + 90, pal.text2, 1, Align::Center);
        } else {
            const char* msg = v == View::Search ? tr("No tracks match", "No coinciden canciones") : v == View::Queue ? tr("Play something from your library", "Reproduce algo de tu biblioteca")
                            : ref < 0 ? tr("Tracks you like appear here", "Las canciones que te gustan aparecen aqui")
                            : ref < int(store.playlists.size()) && store.playlists[ref].smart() ? tr("Nothing matches yet", "Aun no coincide nada")
                                                                                                             : tr("This playlist has no tracks", "Esta playlist no tiene canciones");
            g.textC(msg, { css::Lg, Weight::SemiBold }, x + w / 2, ly + 90, pal.text2, 1, Align::Center);
        }
    }
    g.clip(x, y, w, h);
    bodyGrab = v == View::Queue && grab.kind == Grab::Queue && !grab.upNext ? grab.pos
             : v == View::List && grab.kind == Grab::List && grab.ref == route.ref ? grab.pos : -1;
    const float bodyH = drawTrackBody("pl", tracks, x, ly, w, y, h, mode, false, [this, v, tracks, playName](int i) {
        if (v == View::Queue) playAt(i);   // i is the position in play order
        else playTracks(tracks, i, playName);
    }, joinGap);
    bodyGrab = -1;
    // Mix: after each track (but the last) a line down to the next, and on it a pill saying the two are joined by Automix
    // (Spotify's "Auto"). Only a sign for 2.0 (the user, 2026-10-29: "all in on Auto"; editing a join later perhaps): it
    // opens nothing and takes no focus. (The join editor, view_join.cpp, stays in the code, out of reach.)
    if (joinGap > 0) {
        const std::vector<ListEntry> layout = listLayout(tracks, w, mode, joinGap);
        const bool covers = mode == ListMode::Covers;
        const float lx = covers ? x + 14 + 16 + 44 + 14 + (60 - 4 - 16) / 2.f : x + 14 + 16 + 30;   // the cover's middle; by the number
        const Font lf{ css::Sm, Weight::SemiBold };
        for (const ListEntry& e : layout) {
            if (e.kind != ListEntry::Row || e.index + 1 >= int(tracks.size())) continue;
            const float g0 = ly + e.y + (covers ? e.h - 4 : e.h), g1 = ly + e.y + e.h + joinGap;
            if (g1 < y - 4 || g0 > y + h + 4) continue;
            const char* label = automix::name(automix::Preset::Auto);
            const float ph = 30, pt = (g0 + g1) / 2 - ph / 2, pl = lx - 15, pw = 4 + 22 + 10 + g.measure(label, lf) + 12;   // (no chevron: nothing to open)
            // the two lines linking the tracks, above and below the pill
            g.rect(lx - 1, g0 + 3, 2, pt - g0 - 3, pal.text, 0.22f);
            g.rect(lx - 1, pt + ph, 2, g1 - (pt + ph) - 3, pal.text, 0.22f);
            g.roundRect(pl, pt, pw, ph, 10, pal.surface2);
            g.roundRect(pl, pt, pw, ph, 10, pal.text, 0.05f);
            g.roundRect(pl + 4, pt + 4, 22, 22, 6, pal.accent, 1.f);
            g.icon(ICON_SPARKLE, pl + 15, pt + 15, 14, pal.onAccent);
            g.textC(label, lf, pl + 36, pt + ph / 2, pal.text, 1);
        }
    }
    if (v == View::Queue) {
        if (bodyFocus >= 0) hoverQueue = bodyFocus;
        if (grab.pos >= 0 && grab.kind == Grab::Queue && !grab.upNext) grab.cols = bodyCols;
    }
    sa.content = headH + bodyH;
    g.unclip();
    if (listJump && ui.hasItem(listJump) && std::fabs(sa.y - sa.target) < RowH) {
        ui.focus(listJump);
        listJump = 0;
    }
    ui.popArea();
}

/** A folder playlist's Delete (its page, or the Folders page's "In playlists"): the same card as your own playlists'. */
/** The first launch (the user, 2026-10-27: always asked): the card's Music folder offered, with the guided tour or without
 *  (the user, 2026-10-28), the picker under the card for another. No Cancel: Another folder, B or × leave the picker. */
void App::welcomeCard(const std::string&) {
    // Suppressed for NXtify
}

void App::confirmRemoveFolderPlaylist(int ref, int count) {
    if (ref < 0 || ref >= int(store.playlists.size())) return;
    const FolderPlaylist& p = store.playlists[ref];
    confirm(ICON_FOLDER, "Delete", p.name, "Folder playlist · " + std::to_string(count) + (count == 1 ? " track" : " tracks"),
            "Delete this playlist?", "It leaves the sidebar. The folder and its tracks stay " GF_ON_CARD "; Folders can add it again.",
            "Delete", [this, ref, name = p.name, path = p.path] {
                if (ref >= int(store.playlists.size()) || store.playlists[ref].name != name || store.playlists[ref].path != path) return;
                erasePlaylist(ref);
                toast("Deleted the playlist “" + name + "”");
            });
}

void App::drawAlbum(float x, float y, float w, float h) {
    const int a = routeAlbum();
    if (a < 0) { viewHead(tr("Album", "Album"), tr("Not in the library", "No esta en la biblioteca"), tr("Its tags changed or the library was updated.", "Sus etiquetas cambiaron o la biblioteca se actualizo."), x, y, w); return; }
    const Album& al = lib.albums[a];

    ScrollArea& sa = area("album", a);
    const float heroH = 26 + 220 + 22;
    sa.view = { x, y, w, h };
    sa.content = heroH + 30 + 8 + al.tracks.size() * RowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    // .album-hero::before (the album's accent, fading down) and the cover's halo are left out: on the console the
    // tint covered only the content area and clashed with the sidebar and player bar. The app's background stays.
    const float cx = x + 30, cyTop = oy + 26;
    coverBox(al.tracks[0], cx, cyTop, 220, 12);

    // h1 + .album-meta + .album-actions, bottom-aligned with the cover
    const float tx = cx + 220 + 28, tw = w - (tx - x) - 30;
    const Font h1{ 46, Weight::ExtraBold };
    const std::vector<std::string> lines = g.wrap(al.title, h1, int(tw), 2);
    float ty = cyTop + 220 - 34 - 16 - 22 - 8 - lines.size() * 48 - 6 - 18;
    kicker(tr("Album", "Album"), tx, ty + 8, pal.text3);
    ty += 18 + 6;
    for (const auto& l : lines) { g.text(l, h1, tx - 2, ty - 4, pal.text, 1, Align::Left, int(tw)); ty += 48; }
    ty += 8;
    float mx = tx;
    if (!al.artist.empty()) mx += g.text(al.artist, { css::Base, Weight::SemiBold }, mx, ty, pal.text);
    std::string rest = (al.year.empty() ? "" : " · " + al.year) + " · " + fmtTotal(al.tracks);
    g.text(rest, { css::Base }, mx, ty, pal.text2, 1, Align::Left, int(tx + tw - mx));
    ty += 22 + 16;

    float bx = tx;
    bool clicked = false;
    const std::string name = al.title;
    bx += button(uid("al.play"), ICON_PLAY, tr("Play", "Reproducir"), bx, ty, true, &clicked, false, false, DefaultFocus) + 8;
    if (clicked) playTracks(al.tracks, 0, name);
    bx += button(uid("al.shuffle"), ICON_SHUFFLE, tr("Shuffle", "Aleatorio"), bx, ty, false, &clicked) + 8;
    if (clicked) playTracks(al.tracks, 0, name, true);
    bx += button(uid("al.artist"), ICON_MIC, tr("Artist", "Artista"), bx, ty, false, &clicked) + 8;
    if (clicked) openArtist(al.artist);

    const float ly = oy + heroH;
    listHead(x, ly, w, false);
    g.clip(x, std::max(y, ly + 30), w, y + h - std::max(y, ly + 30));
    const float top = ly + 30 + 8;
    for (int i = 0; i < int(al.tracks.size()); i++) {
        const float ry = top + i * RowH;
        if (ry + RowH < y - RowH || ry > y + h + RowH) continue;
        if (trackRow(uid("row", i), al.tracks[i], i + 1, x + 14, ry, w - 28, RowH, false)) playTracks(al.tracks, i, name);
    }
    g.unclip();
    ui.popArea();
}

void App::drawFolders(float, float, float, float) {}
float App::drawSubfolders(const char*, const FolderNode&, float, float, float, float, float, ListMode) { return 0; }
void App::addSubfoldersAsPlaylists(int) {}

/**
 * Browsing the SD card's folders: for the music folder ("Use this folder"); as View::PickPlaylist, for a playlist
 * file to play or import (+ → Import a playlist file…: .m3u and .m3u8 files are listed after the folders); as
 * View::PickExport, for the folder an exported playlist goes in ("Export here"; the files there are shown, not picked).
 */
void App::drawPicker(float x, float y, float w, float h) {
    const bool exporting = route.view == View::PickExport;
    const bool files = route.view == View::PickPlaylist || exporting;
    const View self = route.view;
    std::string path = route.query.empty() ? std::string(plat::root()) : route.query;
    const bool isRoot = plat::isRoot(path);
    auto join = [](const std::string& a, const std::string& b) { return plat::join(a, b); };
    auto parent = [&](const std::string& p) { return plat::parentDir(p); };
    // Listing the card is cheap but not free: keep the last listing.
    static std::string listedPath;
    static bool listedFiles = false;
    static unsigned listedGen = ~0u;
    static std::vector<std::string> dirs, lists;
    if (listedPath != path || listedFiles != files || listedGen != pickerGen) {
        listedGen = pickerGen;
        dirs = listDirs(path);
        lists = files ? listPlaylistFiles(path) : std::vector<std::string>();
        listedPath = path;
        listedFiles = files;
    }
    const int nDirs = int(dirs.size()), nRows = nDirs + int(lists.size());

    ScrollArea& sa = area(exporting ? "pickex" : files ? "pickpl" : "picker", int(uid(path) & 0x7fffffff));
    const float headH = 18 + 8 + 10 + 38 + 22 + 16 + 34 + 16;
    const float rowH = 52;
    sa.view = { x, y, w, h };
    sa.content = headH + nRows * rowH + 24;
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    float cy = oy + 18 + 8;
    const bool choosing = !files && folderFor >= 0;   // Settings → Storage and reset: a browser's start folder
    kicker(exporting ? "Export “" + exportName + "” to" : files ? "Import a playlist file" : choosing ? folderPickName() : "Music folder", x + 28, cy, pal.text3);
    cy += 10;
    g.text(isRoot ? GF_CARD_NAME : baseName(path), { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    std::string sub = (path == "/" ? std::string(GF_CARD_NAME) : path) + "  ·  " + std::to_string(dirs.size()) + (dirs.size() == 1 ? " folder" : " folders");
    if (files) sub += lists.empty() ? std::string("  ·  no playlist files")
                                    : "  ·  " + std::to_string(lists.size()) + (lists.size() == 1 ? " playlist file" : " playlist files");
    else if (choosing) sub += folderPickField().empty() ? std::string("  ·  now its own default") : "  ·  now " + folderPickField();
    else if (!store.musicRoot.empty()) sub += "  ·  now reading " + store.musicRoot;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    float bx = x + 28;
    bool clicked = false;
    if (choosing) {
        bx += button(uid("pk.use"), ICON_CHECK, "Choose this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
        // Backups inside Gatefold's own settings folder would be copied into themselves, and cleared by a fresh start
        const std::string cfg = Store::Dir;
        if (clicked && folderFor == 4 && strncasecmp(path.c_str(), cfg.c_str(), cfg.size()) == 0 && (path.size() == cfg.size() || path[cfg.size()] == '/')) {
            toast("Backups cannot go in SpotiBacán's own settings folder");
            clicked = false;
        }
        if (clicked) {
            ui.popArea();
            folderPickField() = path;
            const std::string what = folderPickName();
            folderFor = -1;
            persistSoon(0.3f);
            back();
            toast(what + ": " + (isRoot ? std::string(plat::rootName()) : path));
            return;
        }
    } else if (!files) {
        bx += button(uid("pk.use"), ICON_FOLDER, "Use this folder", bx, cy, true, &clicked, false, false, isRoot ? 0 : DefaultFocus) + 8;
        if (clicked) { ui.popArea(); useMusicFolder(path); return; }
    }
    if (exporting) {
        bx += button(uid("pk.export"), ICON_DOWNLOAD, "Export here", bx, cy, true, &clicked, false, false, DefaultFocus) + 8;
        if (clicked) { ui.popArea(); exportPlaylist(path); return; }
        bx += button(uid("pk.mkdir"), ICON_PLUS, "New folder", bx, cy, false, &clicked) + 8;
        if (clicked) {
            std::string name;
            if (keyboard("New folder", "", name)) {
                while (!name.empty() && name.back() == ' ') name.pop_back();
                while (!name.empty() && name[0] == ' ') name.erase(0, 1);
                const std::string made = join(path, name);
                struct stat st;
                if (name.empty()) {
                    // nothing typed: nothing made
                } else if (name[0] == '.' || name.find_first_of("/\\:*?\"<>|") != std::string::npos) {
                    toast("A folder name cannot start with a dot or hold / \\ : * ? \" < > |");
                } else if (stat(made.c_str(), &st) == 0) {
                    toast("“" + name + "” is already here");
                } else if (mkdir(made.c_str(), 0777) != 0) {
                    toast("Could not make the folder “" + name + "”");
                } else {
                    listedPath.clear();   // list it again
                    ui.popArea();
                    navigate(self, -1, made, false);
                    toast("Made the folder “" + name + "”");
                    return;
                }
            }
        }
    }
    if (!isRoot) {
        bx += button(uid("pk.up"), ICON_BACK, "Up", bx, cy, false, &clicked, false, false, files && !exporting && nRows == 0 ? DefaultFocus : 0) + 8;
        if (clicked) { navigate(self, -1, parent(path), false); ui.focus(uid("pk.dir", int(uid(path) & 0x7fffffff))); }
    }
    if (files || choosing || !store.musicRoot.empty()) {
        bx += button(uid("pk.cancel"), ICON_X, "Cancel", bx, cy, false, &clicked, false, false, files && !exporting && isRoot && nRows == 0 ? DefaultFocus : 0) + 8;
        if (clicked) {
            ui.popArea();
            folderFor = -1;
            if (files) leavePlaylistPicker();
            else back();
            return;
        }
    }
    cy += 34 + 16;

    g.clip(x, y, w, h);
    for (int i = 0; i < nRows; i++) {
        const float ry = cy + i * rowH, rx = x + 14, rw = w - 28;
        if (ry + rowH < y - rowH || ry > y + h + rowH) continue;
        const bool isFile = i >= nDirs;
        const std::string& name = isFile ? lists[size_t(i - nDirs)] : dirs[size_t(i)];
        const std::string child = join(path, name);
        const Id id = uid("pk.dir", int(uid(child) & 0x7fffffff));
        // exporting: the files already there are shown (a name taken is asked about), not picked
        const bool pickable = !(exporting && isFile);
        if (pickable && ui.item(id, { rx, ry, rw, rowH - 4 }, 10, (isRoot || files) && !exporting && i == 0 ? DefaultFocus : 0)) {
            if (isFile) openPlaylistFile(child);
            else navigate(self, -1, child, !files && !choosing);   // the playlist browsers and a chosen folder: one entry in the history
        }
        if (ui.pressed(id) || (ui.focused(id) && ui.showFocusRing())) g.roundRect(rx, ry, rw, rowH - 4, 10, pal.text, 0.065f);
        const float mid = ry + (rowH - 4) / 2;
        if (isFile) {
            // a playlist file: the accent tile with the list icon, its name without the extension, then the extension
            const size_t dot = name.find_last_of('.');
            g.roundRect(rx + 16, mid - 18, 36, 36, 8, pal.accent, pickable ? 0.16f : 0.08f);
            g.icon(ICON_LIST, rx + 34, mid, 18, pickable ? pal.accentInk : pal.text3);
            g.textC(name.substr(0, dot), { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, pickable ? pal.text : pal.text3, 1, Align::Left, int(rw - 220));
            g.textC("Playlist file · ." + name.substr(dot + 1), { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        } else {
            const bool current = !files && strcasecmp(child.c_str(), (choosing ? folderPickField() : store.musicRoot).c_str()) == 0;
            g.roundRect(rx + 16, mid - 18, 36, 36, 8, current ? pal.accent : pal.surface2, current ? 0.25f : 1.f);
            g.icon(ICON_FOLDER, rx + 34, mid, 18, current ? pal.accentInk : pal.text2);
            g.textC(name, { css::Base, Weight::SemiBold }, rx + 16 + 36 + 14, mid, current ? pal.accentInk : pal.text, 1, Align::Left, int(rw - 160));
            if (current) g.textC(choosing ? "chosen now" : "current music folder", { css::Sm }, rx + rw - 44, mid, pal.text3, 1, Align::Right);
        }
        if (pickable) g.icon(ICON_CHEV, rx + rw - 24, mid, 18, pal.text3);
    }
    if (nRows == 0)
        g.textC(files ? "No folders or playlist files here" : "No folders here", { css::Lg, Weight::SemiBold }, x + w / 2, cy + 60, pal.text2, 1, Align::Center);
    g.unclip();
    ui.popArea();
}
