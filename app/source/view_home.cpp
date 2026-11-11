/*
 * NXSpotify · view_home.cpp
 * Inicio: Canciones recomendadas y feed de Spotify.
 */
#include "app.hpp"
#include "icons.inc"

void App::drawHome(float x, float y, float w, float h) {
    ScrollArea& sa = area("home");
    sa.view = { x, y, w, h };
    ui.pushArea(&sa);
    const float oy = y - sa.y;

    BgBrowsePage page;
    bg.pollBrowse(page);

    float cy = oy + 18 + 8;
    kicker("SpotiBacán", x + 28, cy, pal.accent);
    cy += 10;
    const std::string title = (page.title.empty() || page.title == "Inicio") ? tr("Home", "Inicio") : page.title;
    g.text(title, { css::Xxl, Weight::Bold }, x + 28, cy, pal.text, 1, Align::Left, int(w - 56));
    cy += 38;
    const std::string sub = (page.subtitle.empty() || page.subtitle == "Canciones recomendadas de Spotify" || page.subtitle == "Canciones recomendadas")
                                ? tr("Recommended songs", "Canciones recomendadas")
                                : page.subtitle;
    g.text(sub, { css::Sm }, x + 28, cy + 4, pal.text3, 1, Align::Left, int(w - 56));
    cy += 22 + 16;

    if (page.depth > 0) {
        bool backClicked = false;
        button(uid("home.back"), ICON_BACK, tr("Back", "Atras"), x + 28, cy, false, &backClicked);
        if (backClicked) bg.browseBack(page.generation);
        cy += 42;
    }

    g.clip(x, y, w, h);

    if (page.state == 1) { // Loading
        g.textC(tr("Loading content...", "Cargando contenido..."), { css::Base }, x + w / 2, cy + 50, pal.text2, 0.8f, Align::Center);
        g.icon(ICON_SPARKLE, x + w / 2, cy + 90, 28, pal.accent);
        cy += 140;
    } else if (page.state == 3) { // Error
        std::string errMsg = page.message.empty() ? tr("Could not connect to SpotiBacán", "No se pudo conectar con SpotiBacán") : page.message;
        g.textC(errMsg, { css::Base }, x + w / 2, cy + 50, pal.text2, 0.8f, Align::Center);
        bool retryClicked = false;
        button(uid("home.retry"), ICON_REFRESH, tr("Retry", "Reintentar"), x + w / 2 - 60, cy + 85, true, &retryClicked);
        if (retryClicked) bg.browseHome();
        cy += 140;
    } else if (page.items.empty()) {
        g.textC(tr("No recommended items right now", "No hay elementos recomendados en este momento"), { css::Base }, x + w / 2, cy + 50, pal.text3, 0.8f, Align::Center);
        bool refreshClicked = false;
        button(uid("home.refresh"), ICON_REFRESH, tr("Refresh", "Actualizar"), x + w / 2 - 60, cy + 85, true, &refreshClicked);
        if (refreshClicked) bg.browseHome();
        cy += 140;
    } else {
        const float rowH = 56.f;
        const float rw = w - 56;
        for (size_t i = 0; i < page.items.size(); ++i) {
            const auto& item = page.items[i];
            const float ry = cy + i * (rowH + 6);
            if (ry + rowH < y - 40 || ry > y + h + 40) continue;

            const Id id = uid("home.item", int(i));
            const bool hit = ui.item(id, { x + 28, ry, rw, rowH }, 10);
            const bool hot = ui.pressed(id) || (ui.focused(id) && ui.showFocusRing());

            if (hot) g.roundRect(x + 28, ry, rw, rowH, 10, pal.text, ui.pressed(id) ? 0.12f : 0.07f);
            else g.roundRect(x + 28, ry, rw, rowH, 10, pal.surface, 0.5f);

            const char* ic = (item.kind == 1) ? ICON_MUSIC : ((item.kind == 0) ? ICON_SPARKLE : ICON_DISC);
            g.roundRect(x + 36, ry + 8, 40, 40, 8, pal.surface2);
            g.icon(ic, x + 56, ry + 28, 20, hot ? pal.accent : pal.text2);

            g.text(item.name, { css::Base, Weight::SemiBold }, x + 88, ry + 8, hot ? pal.accentInk : pal.text, 1, Align::Left, int(rw - 170));
            if (!item.subtitle.empty()) {
                g.text(item.subtitle, { css::Sm }, x + 88, ry + 30, pal.text3, 1, Align::Left, int(rw - 170));
            }

            if (item.kind == 1 && item.durationMs > 0) {
                g.textC(fmtTime(double(item.durationMs) / 1000.0), { css::Xs }, x + 28 + rw - 20, ry + rowH / 2, pal.text3, 1, Align::Right);
            } else if (item.kind != 1) {
                g.icon(ICON_CHEV, x + 28 + rw - 24, ry + rowH / 2, 16, pal.text3);
            }

            if (hit) {
                if (item.kind == 1) {
                    bg.browsePlay(page.generation, item.index);
                } else {
                    bg.browseOpen(page.generation, item.index);
                }
            }
        }
        cy += page.items.size() * (rowH + 6);
    }

    g.unclip();
    ui.popArea();
    sa.content = cy - oy + 40;
}
