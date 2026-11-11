/*
 * SpotiBacan · oggtag.hpp
 * Escribe las etiquetas (titulo, artista, album, numero) y la caratula en un Ogg Vorbis ya descargado.
 *
 * Reemplaza solo el paquete de comentarios: los tres paquetes de cabecera se vuelven a paginar y las paginas de audio se
 * copian tal cual, con su numero de pagina corrido y el CRC recalculado (Vorbis exige que el audio empiece en una pagina
 * nueva, asi que las cabeceras nunca comparten pagina con el audio).
 */
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oggtag {

using Comments = std::vector<std::pair<std::string, std::string>>;   // ("TITLE", "...") etc.

/** in → out con esos comentarios (y la caratula JPEG, si hay). false si el archivo no es un Ogg Vorbis legible. */
bool retag(const std::string& in, const std::string& out, const Comments& comments, const std::vector<uint8_t>& jpeg);

}  // namespace oggtag
