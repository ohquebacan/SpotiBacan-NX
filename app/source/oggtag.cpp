// SpotiBacan · oggtag.cpp (ver oggtag.hpp)
#include "oggtag.hpp"

#include <ogg/ogg.h>
#include <mbedtls/base64.h>

#include <cstdio>
#include <cstring>

namespace oggtag {
namespace {

void le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i)));
}

/** El vendor del paquete de comentarios original ("\x03vorbis" + largo + texto); vacio si no se entiende. */
std::string vendorOf(const std::vector<uint8_t>& p) {
    if (p.size() < 11 || p[0] != 3 || memcmp(p.data() + 1, "vorbis", 6) != 0) return {};
    const uint32_t n = p[7] | (p[8] << 8) | (p[9] << 16) | (uint32_t(p[10]) << 24);
    if (11 + size_t(n) > p.size()) return {};
    return std::string(reinterpret_cast<const char*>(p.data() + 11), n);
}

/** Bloque PICTURE de FLAC (tipo 3, portada) en base64: lo que lee METADATA_BLOCK_PICTURE. */
std::string pictureBlock(const std::vector<uint8_t>& jpeg) {
    std::vector<uint8_t> b;
    const char* mime = "image/jpeg";
    be32(b, 3);
    be32(b, uint32_t(strlen(mime)));
    b.insert(b.end(), mime, mime + strlen(mime));
    be32(b, 0);   // sin descripcion
    be32(b, 0); be32(b, 0); be32(b, 0); be32(b, 0);   // ancho, alto, profundidad, colores: desconocidos (se permiten en 0)
    be32(b, uint32_t(jpeg.size()));
    b.insert(b.end(), jpeg.begin(), jpeg.end());
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, b.data(), b.size());
    std::string out(olen, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out.size(), &olen, b.data(), b.size()) != 0) return {};
    out.resize(olen);
    return out;
}

bool writePage(FILE* f, const ogg_page& og) {
    return fwrite(og.header, 1, og.header_len, f) == size_t(og.header_len) &&
           fwrite(og.body, 1, og.body_len, f) == size_t(og.body_len);
}

}  // namespace

bool retag(const std::string& in, const std::string& out, const Comments& comments, const std::vector<uint8_t>& jpeg) {
    FILE* fi = fopen(in.c_str(), "rb");
    if (!fi) return false;
    FILE* fo = fopen(out.c_str(), "wb");
    if (!fo) { fclose(fi); return false; }
    setvbuf(fo, nullptr, _IOFBF, 256 * 1024);

    ogg_sync_state oy;
    ogg_stream_state is, os;
    ogg_sync_init(&oy);
    bool streamInit = false, outInit = false, ok = true;
    std::vector<std::vector<uint8_t>> headers;
    int headerPages = 0, newPages = 0;
    bool headersDone = false;
    ogg_page og;

    auto readMore = [&]() -> bool {
        char* buf = ogg_sync_buffer(&oy, 64 * 1024);
        const size_t n = fread(buf, 1, 64 * 1024, fi);
        ogg_sync_wrote(&oy, long(n));
        return n > 0;
    };

    while (ok) {
        const int r = ogg_sync_pageout(&oy, &og);
        if (r == 0) {
            if (!readMore()) break;
            continue;
        }
        if (r < 0) continue;   // basura antes de una pagina: se salta

        if (!headersDone) {
            if (!streamInit) {
                ogg_stream_init(&is, ogg_page_serialno(&og));
                streamInit = true;
            }
            ++headerPages;
            ogg_stream_pagein(&is, &og);
            ogg_packet op;
            while (headers.size() < 3 && ogg_stream_packetout(&is, &op) == 1)
                headers.emplace_back(op.packet, op.packet + op.bytes);
            if (headers.size() < 3) continue;
            if (headers[0].size() < 7 || headers[0][0] != 1 || memcmp(headers[0].data() + 1, "vorbis", 6) != 0) { ok = false; break; }

            // el paquete de comentarios nuevo
            std::vector<uint8_t> c = { 3, 'v', 'o', 'r', 'b', 'i', 's' };
            std::string vendor = vendorOf(headers[1]);
            if (vendor.empty()) vendor = "SpotiBacan";
            le32(c, uint32_t(vendor.size()));
            c.insert(c.end(), vendor.begin(), vendor.end());
            Comments all = comments;
            if (!jpeg.empty()) {
                std::string pic = pictureBlock(jpeg);
                if (!pic.empty()) all.emplace_back("METADATA_BLOCK_PICTURE", std::move(pic));
            }
            le32(c, uint32_t(all.size()));
            for (const auto& [k, v] : all) {
                const std::string e = k + "=" + v;
                le32(c, uint32_t(e.size()));
                c.insert(c.end(), e.begin(), e.end());
            }
            c.push_back(1);   // bit de cierre
            headers[1] = std::move(c);

            ogg_stream_init(&os, ogg_page_serialno(&og));
            outInit = true;
            for (int i = 0; i < 3; ++i) {
                ogg_packet p{};
                p.packet = headers[i].data();
                p.bytes = long(headers[i].size());
                p.b_o_s = i == 0;
                p.packetno = i;
                p.granulepos = 0;
                ogg_stream_packetin(&os, &p);
                if (i == 0) {   // la primera cabecera va sola en su pagina
                    while (ogg_stream_flush(&os, &og)) { ok = ok && writePage(fo, og); ++newPages; }
                }
            }
            while (ogg_stream_flush(&os, &og)) { ok = ok && writePage(fo, og); ++newPages; }
            headersDone = true;
            continue;
        }

        // pagina de audio: misma, con el numero corrido y el CRC de nuevo
        const long seq = ogg_page_pageno(&og) - headerPages + newPages;
        for (int i = 0; i < 4; ++i) og.header[18 + i] = uint8_t(uint32_t(seq) >> (8 * i));
        ogg_page_checksum_set(&og);
        ok = writePage(fo, og);
    }

    if (streamInit) ogg_stream_clear(&is);
    if (outInit) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    fclose(fi);
    const bool closed = fclose(fo) == 0;
    if (!ok || !closed || !headersDone) {
        remove(out.c_str());
        return false;
    }
    return true;
}

}  // namespace oggtag
