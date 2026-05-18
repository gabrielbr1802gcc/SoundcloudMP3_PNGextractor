
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint32_t readU32BE(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint32_t readSynchSafe32(const uint8_t *p)
{
    // 4 bytes, 7 bits per byte
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) |
           uint32_t(p[3] & 0x7F);
}

static std::vector<uint8_t> unsyncDecode(const std::vector<uint8_t> &in)
{
    // Remove 0x00 inserido apos 0xFF (ID3v2 unsynchronisation)
    std::vector<uint8_t> out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const uint8_t b = in[i];
        out.push_back(b);
        if (b == 0xFF && i + 1 < in.size() && in[i + 1] == 0x00) {
            ++i;
        }
    }
    return out;
}

static std::string trimSpaces(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

static std::string stripOuterQuotes(const std::string &s)
{
    std::string t = trimSpaces(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        t = t.substr(1, t.size() - 2);
    }
    return trimSpaces(t);
}

static std::string hexPrefix(const std::vector<uint8_t> &v, size_t n)
{
    const char *hex = "0123456789ABCDEF";
    std::string s;
    const size_t m = (v.size() < n) ? v.size() : n;
    s.reserve(m * 3);
    for (size_t i = 0; i < m; ++i) {
        const uint8_t b = v[i];
        s.push_back(hex[(b >> 4) & 0xF]);
        s.push_back(hex[b & 0xF]);
        if (i + 1 < m) s.push_back(' ');
    }
    return s;
}

static std::string hexSuffix(const std::vector<uint8_t> &v, size_t n)
{
    if (v.empty()) return std::string();
    const size_t start = (v.size() > n) ? (v.size() - n) : 0;
    std::vector<uint8_t> tail(v.begin() + start, v.end());
    return hexPrefix(tail, tail.size());
}

struct ApicCandidate {
    std::vector<uint8_t> img;
    std::string mime;
    size_t storedPayloadBytes = 0;
    bool magicKnown = false;
};

static std::string detectExtFromMagic(const std::vector<uint8_t> &img)
{
    if (img.size() >= 3 && img[0] == 0xFF && img[1] == 0xD8 && img[2] == 0xFF) return ".jpg";
    if (img.size() >= 8 && img[0] == 0x89 && img[1] == 0x50 && img[2] == 0x4E && img[3] == 0x47 && img[4] == 0x0D &&
        img[5] == 0x0A && img[6] == 0x1A && img[7] == 0x0A)
        return ".png";
    if (img.size() >= 6 && img[0] == 'G' && img[1] == 'I' && img[2] == 'F' && img[3] == '8' && (img[4] == '7' || img[4] == '9') &&
        img[5] == 'a')
        return ".gif";
    if (img.size() >= 12 && img[0] == 'R' && img[1] == 'I' && img[2] == 'F' && img[3] == 'F' && img[8] == 'W' && img[9] == 'E' &&
        img[10] == 'B' && img[11] == 'P')
        return ".webp";
    if (img.size() >= 2 && img[0] == 'B' && img[1] == 'M') return ".bmp";
    return std::string();
}

static bool looksLikeFrameIdV23V24(const std::vector<uint8_t> &tag, size_t off)
{
    if (off + 4 > tag.size()) return false;
    bool anyNonZero = false;
    for (size_t i = 0; i < 4; ++i) {
        const uint8_t c = tag[off + i];
        if (c != 0) anyNonZero = true;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return anyNonZero;
}

static std::string extFromMimeOrFmt(const std::string &mimeOrFmt)
{
    std::string m;
    m.reserve(mimeOrFmt.size());
    for (char c : mimeOrFmt) {
        if (c >= 'A' && c <= 'Z') m.push_back(char(c - 'A' + 'a'));
        else m.push_back(c);
    }

    if (m.find("png") != std::string::npos) return ".png";
    if (m.find("jpg") != std::string::npos) return ".jpg";
    if (m.find("jpeg") != std::string::npos) return ".jpg";
    return ".bin";
}

static bool hasDot(const std::string &s)
{
    for (char c : s) {
        if (c == '.') return true;
        if (c == '/' || c == '\\') return false;
    }
    return false;
}

static bool endsWithSlash(const std::string &s)
{
    if (s.empty()) return false;
    const char c = s.back();
    return c == '/' || c == '\\';
}

static void splitDirFile(const std::string &path, std::string &dirOut, std::string &fileOut)
{
    const size_t p1 = path.find_last_of('/');
    const size_t p2 = path.find_last_of('\\');
    size_t p = std::string::npos;
    if (p1 != std::string::npos && p2 != std::string::npos) p = (p1 > p2) ? p1 : p2;
    else if (p1 != std::string::npos) p = p1;
    else p = p2;

    if (p == std::string::npos) {
        dirOut.clear();
        fileOut = path;
        return;
    }

    dirOut = path.substr(0, p + 1);
    fileOut = path.substr(p + 1);
}

static std::string stripExtension(const std::string &filename)
{
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) return filename;
    return filename.substr(0, dot);
}

static int extractCoverFromMp3(const std::string &mp3PathRaw, const std::string &outArgRaw, bool debug)
{
    const std::string mp3Path = stripOuterQuotes(mp3PathRaw);
    const std::string outArg = stripOuterQuotes(outArgRaw);

    std::ifstream in(mp3Path, std::ios::binary);
    if (!in) {
        std::cout << "Nao foi possivel abrir o arquivo\n";
        return 1;
    }

    uint8_t header[10];
    in.read(reinterpret_cast<char *>(header), 10);
    if (in.gcount() != 10 || header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        std::cout << "Sem ID3v2 no inicio do arquivo\n";
        return 1;
    }

    const uint8_t verMajor = header[3];
    const uint8_t verRev = header[4];
    const uint8_t flags = header[5];
    const uint32_t tagSize = readSynchSafe32(&header[6]);

    const bool headerUnsync = (flags & 0x80) != 0;

    if (debug) {
        std::cout << "[debug] ID3v2: 2." << int(verMajor) << "." << int(verRev) << " flags=0x" << std::hex << int(flags) << std::dec
                  << " tagSize=" << tagSize << "\n";
    }

    if (tagSize == 0) {
        std::cout << "ID3v2 vazio\n";
        return 1;
    }

    std::vector<uint8_t> tag(tagSize);
    in.read(reinterpret_cast<char *>(tag.data()), tag.size());
    if (size_t(in.gcount()) != tag.size()) {
        std::cout << "Arquivo truncado\n";
        return 1;
    }

    size_t pos = 0;

    // Pular extended header se existir
    if (flags & 0x40) {
        if (tag.size() < 4) {
            std::cout << "ID3v2 com header estendido invalido\n";
            return 1;
        }

        uint32_t extSizeField = 0;
        if (verMajor == 3) {
            extSizeField = readU32BE(&tag[pos]);
        } else if (verMajor == 4) {
            extSizeField = readSynchSafe32(&tag[pos]);
        } else {
            // v2.2 e outros: nao suportado aqui
            std::cout << "Versao ID3v2 nao suportada: 2." << int(verMajor) << "\n";
            return 1;
        }

        if (extSizeField > tag.size()) {
            std::cout << "Extended header maior que a tag\n";
            return 1;
        }

        // Ambiguidade comum: alguns arquivos tratam o size como incluindo os 4 bytes;
        // outros como excluindo. Tentamos os 2 jeitos e escolhemos o que parece
        // alinhar no comeco de um frame valido.
        const size_t candA = pos + size_t(extSizeField);
        const size_t candB = pos + size_t(extSizeField) + 4;

        bool okA = looksLikeFrameIdV23V24(tag, candA);
        bool okB = looksLikeFrameIdV23V24(tag, candB);
        size_t chosen = candA;
        if (!okA && okB) chosen = candB;
        else if (okA && okB) chosen = (candA < candB) ? candA : candB;
        else if (!okA && !okB) chosen = candA;

        if (chosen > tag.size()) {
            std::cout << "Extended header invalido\n";
            return 1;
        }

        if (debug) {
            std::cout << "[debug] extHeader sizeField=" << extSizeField << " candA=" << candA << " okA=" << (okA ? 1 : 0)
                      << " candB=" << candB << " okB=" << (okB ? 1 : 0) << " chosen=" << chosen << "\n";
        }

        pos = chosen;
    }

    std::string mp3Dir, mp3File;
    splitDirFile(mp3Path, mp3Dir, mp3File);
    const std::string baseName = stripExtension(mp3File);

    auto buildOutName = [&](const std::string &mimeOrFmt) -> std::string {
        const std::string ext = extFromMimeOrFmt(mimeOrFmt);
        if (outArg.empty()) {
            // Padrao: salva ao lado do mp3 e com nome unico
            return mp3Dir + baseName + "_cover" + ext;
        }

        // Se o usuario passou uma pasta terminando com / ou \, salva dentro dela.
        if (endsWithSlash(outArg)) {
            return outArg + baseName + "_cover" + ext;
        }

        // Se tem ponto, tratamos como caminho completo de arquivo.
        if (hasDot(outArg)) {
            return outArg;
        }

        // Caso contrario, e um "nome base" (sem extensao)
        return outArg + ext;
    };

    auto writeCover = [&](const std::vector<uint8_t> &img, const std::string &mimeOrFmt) -> int {
        // Se o MIME/format estiver estranho, tenta detectar pelo "magic" real.
        std::string outName = buildOutName(mimeOrFmt);
        const std::string magicExt = detectExtFromMagic(img);
        if (!magicExt.empty() && hasDot(outName)) {
            // Se o usuario escolheu a extensao manualmente, nao mexe.
        } else if (!magicExt.empty()) {
            // Ajusta a extensao padrao baseada no magic quando nao foi especificada uma extensao.
            if (outArg.empty()) {
                outName = mp3Dir + baseName + "_cover" + magicExt;
            } else if (endsWithSlash(outArg)) {
                outName = outArg + baseName + "_cover" + magicExt;
            } else if (!hasDot(outArg)) {
                outName = outArg + magicExt;
            }
        }

        std::ofstream out(outName, std::ios::binary);
        if (!out) {
            std::cout << "Nao foi possivel criar o arquivo de saida\n";
            return 1;
        }

        out.write(reinterpret_cast<const char *>(img.data()), img.size());
        out.close();

        std::cout << "Capa salva como " << outName << " (" << img.size() << " bytes)\n";
        if (debug) {
            const bool looksJpg = (img.size() >= 2 && img[0] == 0xFF && img[1] == 0xD8);
            const bool endsJpg = (img.size() >= 2 && img[img.size() - 2] == 0xFF && img[img.size() - 1] == 0xD9);
            const bool looksPng = (img.size() >= 8 && img[0] == 0x89 && img[1] == 0x50 && img[2] == 0x4E && img[3] == 0x47);

            std::cout << "[debug] mime/fmt='" << mimeOrFmt << "' magicExt='" << (magicExt.empty() ? "?" : magicExt) << "' firstBytes="
                      << hexPrefix(img, 16) << "\n";
            std::cout << "[debug] lastBytes=" << hexSuffix(img, 16) << "\n";
            std::cout << "[debug] looksJpg=" << (looksJpg ? 1 : 0) << " endsJpg=" << (endsJpg ? 1 : 0) << " looksPng=" << (looksPng ? 1 : 0)
                      << "\n";
        }
        return 0;
    };

    auto isMagicKnown = [&](const std::vector<uint8_t> &img) -> bool {
        return !detectExtFromMagic(img).empty();
    };

    auto chooseBetter = [&](const ApicCandidate &a, const ApicCandidate &b) -> bool {
        // true se a melhor que b
        if (a.magicKnown != b.magicKnown) return a.magicKnown; // prefira magic conhecido
        if (a.img.size() != b.img.size()) return a.img.size() > b.img.size();
        return a.storedPayloadBytes > b.storedPayloadBytes;
    };

    auto updateBest = [&](ApicCandidate &best, bool &hasBest, ApicCandidate &&cand) {
        if (cand.img.empty()) return;
        if (!hasBest || chooseBetter(cand, best)) {
            best = std::move(cand);
            hasBest = true;
        }
    };

    auto parseApicPayload = [&](const std::vector<uint8_t> &payloadIn, bool v24Local, uint8_t flagFormatLocal, uint8_t flagStatusLocal,
                                size_t storedPayloadBytes) -> ApicCandidate {
        std::vector<uint8_t> payload = payloadIn;

        // Unsync: em v2.3 pode vir no header e vale para todos frames. Em v2.4 pode vir por-frame.
        const bool tagUnsync = headerUnsync;
        const bool frameUnsync = v24Local && ((flagFormatLocal & 0x02) != 0);
        if (tagUnsync || frameUnsync) {
            payload = unsyncDecode(payload);
        }

        size_t payloadPos = 0;

        // grouping identity: 1 byte no inicio do frame
        if (v24Local) {
            if ((flagFormatLocal & 0x40) != 0) payloadPos += 1;
        } else {
            if ((flagFormatLocal & 0x20) != 0) payloadPos += 1;
        }

        // v2.4: data length indicator
        if (v24Local && ((flagFormatLocal & 0x01) != 0)) {
            if (payload.size() < payloadPos + 4) return {};
            payloadPos += 4;
        }

        // Rejeita compressao/criptografia
        if (v24Local) {
            const bool compressed = (flagFormatLocal & 0x08) != 0;
            const bool encrypted = (flagFormatLocal & 0x04) != 0;
            if (compressed || encrypted) {
                if (debug) {
                    std::cout << "[debug] APIC compressed/encrypted v2.4 status=0x" << std::hex << int(flagStatusLocal) << " format=0x"
                              << int(flagFormatLocal) << std::dec << "\n";
                }
                return {};
            }
        } else {
            const bool compressed = (flagFormatLocal & 0x80) != 0;
            const bool encrypted = (flagFormatLocal & 0x40) != 0;
            if (compressed || encrypted) {
                if (debug) {
                    std::cout << "[debug] APIC compressed/encrypted v2.3 status=0x" << std::hex << int(flagStatusLocal) << " format=0x"
                              << int(flagFormatLocal) << std::dec << "\n";
                }
                return {};
            }
        }

        size_t p = payloadPos;
        if (p >= payload.size()) return {};

        const uint8_t encoding = payload[p++];

        std::string mime;
        while (p < payload.size() && payload[p] != 0x00) {
            mime.push_back(char(payload[p]));
            ++p;
        }
        if (p >= payload.size()) return {};
        ++p; // \0
        if (p >= payload.size()) return {};

        p += 1; // picture type

        // descricao
        if (encoding == 0 || encoding == 3) {
            while (p < payload.size() && payload[p] != 0x00) ++p;
            if (p < payload.size()) ++p;
        } else {
            while (p + 1 < payload.size()) {
                if (payload[p] == 0x00 && payload[p + 1] == 0x00) {
                    p += 2;
                    break;
                }
                p += 2;
            }
        }
        if (p >= payload.size()) return {};

        ApicCandidate cand;
        cand.mime = mime;
        cand.storedPayloadBytes = storedPayloadBytes;
        cand.img.assign(payload.begin() + p, payload.end());
        cand.magicKnown = isMagicKnown(cand.img);

        if (debug) {
            std::cout << "[debug] APIC mime='" << mime << "' encoding=" << int(encoding) << " storedPayloadBytes=" << storedPayloadBytes
                      << " decodedPayloadBytes=" << payload.size() << " imgBytes=" << cand.img.size() << " magicKnown="
                      << (cand.magicKnown ? 1 : 0) << "\n";
        }

        return cand;
    };

    // Frames
    if (verMajor == 2) {
        // ID3v2.2: frames com 6 bytes de header
        ApicCandidate best;
        bool hasBest = false;
        while (pos + 6 <= tag.size()) {
            if (tag[pos] == 0 && tag[pos + 1] == 0 && tag[pos + 2] == 0) break; // padding

            const std::string id(reinterpret_cast<const char *>(&tag[pos]), 3);
            const uint32_t size = (uint32_t(tag[pos + 3]) << 16) | (uint32_t(tag[pos + 4]) << 8) | uint32_t(tag[pos + 5]);
            pos += 6;
            if (size == 0 || pos + size > tag.size()) break;

            if (id == "PIC" && size >= 5) {
                size_t p = pos;
                const uint8_t enc = tag[p++];

                const std::string fmt(reinterpret_cast<const char *>(&tag[p]), 3);
                p += 3;
                p += 1; // picture type

                // Descricao: terminador depende do encoding
                if (enc == 0 || enc == 3) {
                    // ISO-8859-1 / UTF-8: termina em 0x00
                    while (p < pos + size && tag[p] != 0x00) ++p;
                    if (p < pos + size && tag[p] == 0x00) ++p;
                } else {
                    // UTF-16 / UTF-16BE: termina em 0x00 0x00
                    while (p + 1 < pos + size) {
                        if (tag[p] == 0x00 && tag[p + 1] == 0x00) {
                            p += 2;
                            break;
                        }
                        p += 2;
                    }
                }

                if (p < pos + size) {
                    ApicCandidate cand;
                    cand.mime = fmt;
                    cand.storedPayloadBytes = size;
                    cand.img.assign(tag.begin() + p, tag.begin() + (pos + size));
                    cand.magicKnown = isMagicKnown(cand.img);
                    updateBest(best, hasBest, std::move(cand));
                }
            }

            pos += size;
        }

        if (hasBest) return writeCover(best.img, best.mime);

        std::cout << "Sem imagem embutida\n";
        return 1;
    }

    if (verMajor != 3 && verMajor != 4) {
        std::cout << "Versao ID3v2 nao suportada: 2." << int(verMajor) << "\n";
        return 1;
    }

    const bool v24 = (verMajor == 4);
    ApicCandidate best;
    bool hasBest = false;

    auto scanFrames = [&](size_t startPos) {
        size_t ppos = startPos;
        while (ppos + 10 <= tag.size()) {
            if (tag[ppos] == 0 && tag[ppos + 1] == 0 && tag[ppos + 2] == 0 && tag[ppos + 3] == 0) break;

            if (!looksLikeFrameIdV23V24(tag, ppos)) break;

            const std::string id(reinterpret_cast<const char *>(&tag[ppos]), 4);
            const uint32_t frameSize = v24 ? readSynchSafe32(&tag[ppos + 4]) : readU32BE(&tag[ppos + 4]);
            const uint8_t flagStatus = tag[ppos + 8];
            const uint8_t flagFormat = tag[ppos + 9];

            const size_t payloadStart = ppos + 10;
            const size_t nextPos = payloadStart + frameSize;
            if (frameSize == 0 || nextPos > tag.size()) break;

            if (debug) {
                std::cout << "[frame] id=" << id << " size=" << frameSize << " hdrPos=" << ppos << " payloadPos=" << payloadStart
                          << " nextPos=" << nextPos << " status=0x" << std::hex << int(flagStatus) << " format=0x" << int(flagFormat)
                          << std::dec << "\n";
            }

            if (id == "APIC") {
                std::vector<uint8_t> payload(tag.begin() + payloadStart, tag.begin() + nextPos);
                ApicCandidate cand = parseApicPayload(payload, v24, flagFormat, flagStatus, frameSize);
                updateBest(best, hasBest, std::move(cand));
            }

            ppos = nextPos;
        }
    };

    scanFrames(pos);

    // Fallback: varredura bruta por "APIC" caso a tag esteja inconsistente.
    if (!hasBest) {
        if (debug) std::cout << "[debug] fallback brute-scan for APIC\n";
        for (size_t i = pos; i + 10 <= tag.size(); ++i) {
            if (tag[i] != 'A' || tag[i + 1] != 'P' || tag[i + 2] != 'I' || tag[i + 3] != 'C') continue;
            // tenta interpretar como frame header
            const uint32_t frameSize = v24 ? readSynchSafe32(&tag[i + 4]) : readU32BE(&tag[i + 4]);
            const uint8_t flagStatus = tag[i + 8];
            const uint8_t flagFormat = tag[i + 9];
            const size_t payloadStart = i + 10;
            const size_t nextPos = payloadStart + frameSize;
            if (frameSize == 0 || nextPos > tag.size()) continue;

            std::vector<uint8_t> payload(tag.begin() + payloadStart, tag.begin() + nextPos);
            ApicCandidate cand = parseApicPayload(payload, v24, flagFormat, flagStatus, frameSize);
            updateBest(best, hasBest, std::move(cand));
        }
    }

    if (hasBest) return writeCover(best.img, best.mime);

    std::cout << "Sem imagem embutida\n";
    return 1;
}

int main(int argc, char *argv[])
{
    bool debug = false;
    int argi = 1;
    if (argc >= 2 && std::string(argv[1]) == "--debug") {
        debug = true;
        argi = 2;
    }

    // Modo interativo: quando nao veio mp3 nos argumentos.
    // Ex: Run normal (sem args) OU Run com "--debug".
    if (argc <= argi) {
        std::string mp3;
        std::string out;
        std::cout << "Cole o caminho do MP3 e aperte Enter:\n> ";
        std::getline(std::cin, mp3);
        if (mp3.empty()) {
            std::cout << "Uso: thumb_extrac [--debug] arquivo.mp3 [saida]\n";
            return 1;
        }

        std::cout << "Saida (Enter para padrao):\n> ";
        std::getline(std::cin, out);
        return extractCoverFromMp3(mp3, out, debug);
    }

    const std::string mp3Path = argv[argi];
    const std::string outArg = (argc >= argi + 2) ? argv[argi + 1] : std::string();
    return extractCoverFromMp3(mp3Path, outArg, debug);
}
