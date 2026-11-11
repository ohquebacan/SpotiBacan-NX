// SpotiBacan: el token de la Web API para el descargador (ver downloader.cpp).
#include "spotify_api.hpp"

std::string downloaderWebToken() { return SpotifyApi::instance().webAccessToken(); }
