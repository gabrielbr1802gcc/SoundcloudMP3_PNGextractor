#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
using NativeString = std::wstring;
#else
using NativeString = std::string;
#endif

#ifdef _WIN32
using NativeChar = wchar_t;
static std::wostream &nout() { return std::wcout; }
#else
using NativeChar = char;
static std::ostream &nout() { return std::cout; }
#endif

static NativeString toNativeSimple(const std::string &s)
{
#ifdef _WIN32
    return NativeString(s.begin(), s.end());
#else
    return s;
#endif
}

// Forward declaration (used by isLikelyDirectoryPath)
static bool pathEndsWithSlash(const fs::path &p);

static bool nativeEqualsAscii(const NativeString &a, const char *ascii)
{
#ifdef _WIN32
    const NativeString b = toNativeSimple(std::string(ascii));
    return a == b;
#else
    return a == ascii;
#endif
}

static bool nativeIEqualsAscii(const NativeString &a, const char *ascii)
{
    // Case-insensitive compare for ASCII-only tokens.
    NativeString b = toNativeSimple(std::string(ascii));
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        NativeChar ca = a[i];
        NativeChar cb = b[i];
        if (ca >= NativeChar('A') && ca <= NativeChar('Z')) ca = NativeChar(ca - NativeChar('A') + NativeChar('a'));
        if (cb >= NativeChar('A') && cb <= NativeChar('Z')) cb = NativeChar(cb - NativeChar('A') + NativeChar('a'));
        if (ca != cb) return false;
    }
    return true;
}

static bool isYes(const NativeString &s)
{
    if (s.empty()) return false;
    // Accept: s/S/y/Y
    const NativeChar c = s[0];
    return c == NativeChar('s') || c == NativeChar('S') || c == NativeChar('y') || c == NativeChar('Y');
}

struct OutputOptions {
    fs::path out;                 // empty = default
    NativeString namePattern;     // empty = default "{stem}"
    bool overwrite = false;       // false = never overwrite
};

static NativeString applyStemPattern(const NativeString &pattern, const fs::path &mp3Path)
{
    NativeString out = pattern;
    const NativeString token = toNativeSimple("{stem}");
    const NativeString repl = mp3Path.stem().native();

    if (token.empty()) return out;

    size_t pos = 0;
    while ((pos = out.find(token, pos)) != NativeString::npos) {
        out.replace(pos, token.size(), repl);
        pos += repl.size();
    }

    return out;
}

static bool isLikelyDirectoryPath(const fs::path &p)
{
    // A path ending in slash is treated as directory.
    if (pathEndsWithSlash(p)) return true;
    // If it exists and is a directory, it's a directory.
    std::error_code ec;
    if (!p.empty() && fs::exists(p, ec) && fs::is_directory(p, ec)) return true;
    return false;
}

static bool isMp3Extension(const fs::path &p)
{
    NativeString ext = p.extension().native();
    if (ext.empty()) return false;
    for (auto &ch : ext) {
        if (ch >= NativeChar('A') && ch <= NativeChar('Z')) ch = NativeChar(ch - NativeChar('A') + NativeChar('a'));
    }
    return ext == toNativeSimple(".mp3");
}

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

static NativeString trimSpacesNative(const NativeString &s)
{
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;

    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;

    return s.substr(a, b - a);
}

static fs::path cleanPath(const NativeString &raw)
{
    NativeString t = trimSpacesNative(raw);

    if (t.size() >= 2) {
        const auto first = t.front();
        const auto last = t.back();

        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            t = t.substr(1, t.size() - 2);
        }
    }

    return fs::path(trimSpacesNative(t));
}

static NativeString asciiToNative(const std::string &s)
{
#ifdef _WIN32
    return NativeString(s.begin(), s.end());
#else
    return s;
#endif
}

static bool pathEndsWithSlash(const fs::path &p)
{
    const auto s = p.native();
    if (s.empty()) return false;

    const auto c = s.back();
    return c == '/' || c == '\\';
}

static fs::path appendAsciiToPath(const fs::path &base, const std::string &suffix)
{
    return fs::path(base.native() + asciiToNative(suffix));
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

static int extractCoverFromMp3(const fs::path &mp3Path, const OutputOptions &opt, bool debug)
{
    std::ifstream in(mp3Path, std::ios::binary);
    if (!in) {
        nout() << toNativeSimple("Nao foi possivel abrir o arquivo") << toNativeSimple("\n");
        return 1;
    }

    uint8_t header[10];
    in.read(reinterpret_cast<char *>(header), 10);
    if (in.gcount() != 10 || header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        nout() << toNativeSimple("Sem ID3v2 no inicio do arquivo") << toNativeSimple("\n");
        return 1;
    }

    const uint8_t verMajor = header[3];
    const uint8_t verRev = header[4];
    const uint8_t flags = header[5];
    const uint32_t tagSize = readSynchSafe32(&header[6]);

    const bool headerUnsync = (flags & 0x80) != 0;

    if (debug) {
        nout() << toNativeSimple("[debug] ID3v2: 2.") << int(verMajor) << toNativeSimple(".") << int(verRev) << toNativeSimple(" flags=0x")
               << std::hex << int(flags) << std::dec << toNativeSimple(" tagSize=") << tagSize << toNativeSimple("\n");
    }

    if (tagSize == 0) {
        nout() << toNativeSimple("ID3v2 vazio") << toNativeSimple("\n");
        return 1;
    }

    std::vector<uint8_t> tag(tagSize);
    in.read(reinterpret_cast<char *>(tag.data()), tag.size());
    if (size_t(in.gcount()) != tag.size()) {
        nout() << toNativeSimple("Arquivo truncado") << toNativeSimple("\n");
        return 1;
    }

    size_t pos = 0;

    // Pular extended header se existir
    if (flags & 0x40) {
        if (tag.size() < 4) {
            nout() << toNativeSimple("ID3v2 com header estendido invalido") << toNativeSimple("\n");
            return 1;
        }

        uint32_t extSizeField = 0;
        if (verMajor == 3) {
            extSizeField = readU32BE(&tag[pos]);
        } else if (verMajor == 4) {
            extSizeField = readSynchSafe32(&tag[pos]);
        } else {
            nout() << toNativeSimple("Versao ID3v2 nao suportada: 2.") << int(verMajor) << toNativeSimple("\n");
            return 1;
        }

        if (extSizeField > tag.size()) {
            nout() << toNativeSimple("Extended header maior que a tag") << toNativeSimple("\n");
            return 1;
        }

        const size_t candA = pos + size_t(extSizeField);
        const size_t candB = pos + size_t(extSizeField) + 4;

        bool okA = looksLikeFrameIdV23V24(tag, candA);
        bool okB = looksLikeFrameIdV23V24(tag, candB);
        size_t chosen = candA;
        if (!okA && okB) chosen = candB;
        else if (okA && okB) chosen = (candA < candB) ? candA : candB;
        else if (!okA && !okB) chosen = candA;

        if (chosen > tag.size()) {
            nout() << toNativeSimple("Extended header invalido") << toNativeSimple("\n");
            return 1;
        }

        if (debug) {
            nout() << toNativeSimple("[debug] extHeader sizeField=") << extSizeField << toNativeSimple(" candA=") << candA
                   << toNativeSimple(" okA=") << (okA ? 1 : 0) << toNativeSimple(" candB=") << candB << toNativeSimple(" okB=")
                   << (okB ? 1 : 0) << toNativeSimple(" chosen=") << chosen << toNativeSimple("\n");
        }

        pos = chosen;
    }

    auto buildOutName = [&](const std::string &ext) -> fs::path {
        const NativeString pattern = opt.namePattern.empty() ? toNativeSimple("{stem}") : opt.namePattern;
        const NativeString baseName = applyStemPattern(pattern, mp3Path);

        // Exact file path (user-specified, has extension)
        if (!opt.out.empty() && opt.out.has_extension() && !isLikelyDirectoryPath(opt.out)) {
            return opt.out;
        }

        // Folder destination
        if (opt.out.empty()) {
            return mp3Path.parent_path() / appendAsciiToPath(fs::path(baseName), ext);
        }

        if (isLikelyDirectoryPath(opt.out)) {
            return opt.out / appendAsciiToPath(fs::path(baseName), ext);
        }

        // Back-compat: treat as base path/name.
        return appendAsciiToPath(opt.out, ext);
    };

    auto writeCover = [&](const std::vector<uint8_t> &img, const std::string &mimeOrFmt) -> int {
        // Se o MIME/format estiver estranho, tenta detectar pelo "magic" real.
        const std::string extByMime = extFromMimeOrFmt(mimeOrFmt);
        fs::path outName = buildOutName(extByMime);
        const std::string magicExt = detectExtFromMagic(img);

        const bool exactFile = (!opt.out.empty() && opt.out.has_extension() && !isLikelyDirectoryPath(opt.out));

        // So ajusta a extensao automaticamente se o usuario nao passou um arquivo com extensao.
        if (!magicExt.empty() && !exactFile) {
            outName = buildOutName(magicExt);
        }

        // Overwrite protection
        {
            std::error_code ec;
            if (fs::exists(outName, ec) && !opt.overwrite) {
                nout() << toNativeSimple("Output file already exists (won't overwrite): ") << outName.native() << toNativeSimple("\n");
                nout() << toNativeSimple("Tip: use --overwrite or pick a different name.") << toNativeSimple("\n");
                return 2; // skipped
            }
        }

        std::ofstream out(outName, std::ios::binary);
        if (!out) {
            nout() << toNativeSimple("Nao foi possivel criar o arquivo de saida") << toNativeSimple("\n");
            return 1;
        }

        out.write(reinterpret_cast<const char *>(img.data()), img.size());
        out.close();

        nout() << toNativeSimple("Capa salva como ") << outName.native() << toNativeSimple(" (") << img.size() << toNativeSimple(" bytes)\n");
        if (debug) {
            const bool looksJpg = (img.size() >= 2 && img[0] == 0xFF && img[1] == 0xD8);
            const bool endsJpg = (img.size() >= 2 && img[img.size() - 2] == 0xFF && img[img.size() - 1] == 0xD9);
            const bool looksPng = (img.size() >= 8 && img[0] == 0x89 && img[1] == 0x50 && img[2] == 0x4E && img[3] == 0x47);

            nout() << toNativeSimple("[debug] mime/fmt='") << toNativeSimple(mimeOrFmt) << toNativeSimple("' magicExt='")
                   << toNativeSimple(magicExt.empty() ? "?" : magicExt) << toNativeSimple("' firstBytes=")
                   << toNativeSimple(hexPrefix(img, 16)) << toNativeSimple("\n");
            nout() << toNativeSimple("[debug] lastBytes=") << toNativeSimple(hexSuffix(img, 16)) << toNativeSimple("\n");
            nout() << toNativeSimple("[debug] looksJpg=") << (looksJpg ? 1 : 0) << toNativeSimple(" endsJpg=") << (endsJpg ? 1 : 0)
                   << toNativeSimple(" looksPng=") << (looksPng ? 1 : 0) << toNativeSimple("\n");
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
                    nout() << toNativeSimple("[debug] APIC compressed/encrypted v2.4 status=0x") << std::hex << int(flagStatusLocal)
                           << toNativeSimple(" format=0x") << int(flagFormatLocal) << std::dec << toNativeSimple("\n");
                }
                return {};
            }
        } else {
            const bool compressed = (flagFormatLocal & 0x80) != 0;
            const bool encrypted = (flagFormatLocal & 0x40) != 0;
            if (compressed || encrypted) {
                if (debug) {
                    nout() << toNativeSimple("[debug] APIC compressed/encrypted v2.3 status=0x") << std::hex << int(flagStatusLocal)
                           << toNativeSimple(" format=0x") << int(flagFormatLocal) << std::dec << toNativeSimple("\n");
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
            nout() << toNativeSimple("[debug] APIC mime='") << toNativeSimple(mime) << toNativeSimple("' encoding=") << int(encoding)
                   << toNativeSimple(" storedPayloadBytes=") << storedPayloadBytes << toNativeSimple(" decodedPayloadBytes=") << payload.size()
                   << toNativeSimple(" imgBytes=") << cand.img.size() << toNativeSimple(" magicKnown=") << (cand.magicKnown ? 1 : 0)
                   << toNativeSimple("\n");
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

        nout() << toNativeSimple("Sem imagem embutida") << toNativeSimple("\n");
        return 1;
    }

    if (verMajor != 3 && verMajor != 4) {
        nout() << toNativeSimple("Versao ID3v2 nao suportada: 2.") << int(verMajor) << toNativeSimple("\n");
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
                nout() << toNativeSimple("[frame] id=") << toNativeSimple(id) << toNativeSimple(" size=") << frameSize
                       << toNativeSimple(" hdrPos=") << ppos << toNativeSimple(" payloadPos=") << payloadStart
                       << toNativeSimple(" nextPos=") << nextPos << toNativeSimple(" status=0x") << std::hex << int(flagStatus)
                       << toNativeSimple(" format=0x") << int(flagFormat) << std::dec << toNativeSimple("\n");
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
        if (debug) nout() << toNativeSimple("[debug] fallback brute-scan for APIC\n");
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

    nout() << toNativeSimple("Sem imagem embutida") << toNativeSimple("\n");
    return 1;
}

static void printHelp()
{
    nout() << toNativeSimple("Usage:\n");
    nout() << toNativeSimple("  thumb_extrac [--debug] file.mp3 [output]\n");
    nout() << toNativeSimple("  thumb_extrac single <file.mp3> [--out <path>] [--name <name|pattern>] [--overwrite] [--debug]\n");
    nout() << toNativeSimple("  thumb_extrac batch <folder> [--out-dir <folder>] [--pattern <pattern>] [--overwrite] [--debug]\n\n");
    nout() << toNativeSimple("Output (single):\n");
    nout() << toNativeSimple("  --out can be: an existing folder, a path ending with /\\, or a full file path with extension.\n");
    nout() << toNativeSimple("  --name / --pattern support {stem} (MP3 filename without extension).\n");
    nout() << toNativeSimple("Examples:\n");
    nout() << toNativeSimple("  thumb_extrac single \"C:\\Music\\x.mp3\" --name \"{stem} - cover\"\n");
    nout() << toNativeSimple("  thumb_extrac batch  \"C:\\Music\\\" --out-dir \"D:\\Covers\\\" --pattern \"{stem}\"\n");
}

static int runSingle(const fs::path &mp3Path, const OutputOptions &opt, bool debug)
{
    if (mp3Path.empty()) {
        nout() << toNativeSimple("Invalid MP3 file\n");
        return 1;
    }

    return extractCoverFromMp3(mp3Path, opt, debug);
}

static int runBatch(const fs::path &folder, const fs::path &outDir, const NativeString &pattern, bool overwrite, bool debug)
{
    std::error_code ec;
    if (folder.empty() || !fs::exists(folder, ec) || !fs::is_directory(folder, ec)) {
        nout() << toNativeSimple("Invalid folder: ") << folder.native() << toNativeSimple("\n");
        return 1;
    }

    size_t total = 0;
    size_t extracted = 0;
    size_t skipped = 0;
    size_t errors = 0;

    for (const auto &entry : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const fs::path p = entry.path();
        if (!isMp3Extension(p)) continue;

        ++total;

        OutputOptions opt;
        opt.out = outDir;
        opt.namePattern = pattern;
        opt.overwrite = overwrite;

        const int rc = extractCoverFromMp3(p, opt, debug);
        if (rc == 0) ++extracted;
        else if (rc == 2) ++skipped;
        else ++errors;
    }

        nout() << toNativeSimple("\nBatch done. Total MP3=") << total << toNativeSimple(" extracted=") << extracted
            << toNativeSimple(" skipped=") << skipped << toNativeSimple(" errors=") << errors << toNativeSimple("\n");

    return (errors == 0) ? 0 : 1;
}

static NativeString promptLine(const std::string &msg)
{
    nout() << toNativeSimple(msg);
    nout() << toNativeSimple("\n> ");
    NativeString s;
#ifdef _WIN32
    std::getline(std::wcin, s);
#else
    std::getline(std::cin, s);
#endif
    return s;
}

static int runInteractive(bool debug)
{
    nout() << toNativeSimple("Select mode:\n");
    nout() << toNativeSimple("  1) Single (one MP3)\n");
    nout() << toNativeSimple("  2) Batch (folder with MP3 files)\n");

    NativeString mode = promptLine("Mode (1/2)");
    mode = trimSpacesNative(mode);

    if (nativeEqualsAscii(mode, "1")) {
        NativeString mp3Raw = promptLine("Paste the MP3 path");
        fs::path mp3Path = cleanPath(mp3Raw);

        NativeString outRaw = promptLine("Output (Enter for default; can be folder or file)");
        fs::path out = cleanPath(outRaw);

        OutputOptions opt;
        opt.out = out;
        // Use MP3 filename by default (no prompt)
        opt.namePattern = NativeString();

        // overwrite prompt only if output already exists AND we can infer final name after extraction.
        // To keep UX simple, we just ask once.
        NativeString ow = promptLine("If output exists, overwrite? (y/n) [n]");
        opt.overwrite = isYes(trimSpacesNative(ow));

        return runSingle(mp3Path, opt, debug);
    }

    if (nativeEqualsAscii(mode, "2")) {
        fs::path folder = cleanPath(promptLine("Paste the folder path (MP3 files inside)"));
        fs::path outDir = cleanPath(promptLine("Output folder (Enter = save next to each MP3)"));

        // Default pattern uses MP3 filename (no prompt)
        NativeString pattern;

        NativeString ow = promptLine("If output exists, overwrite? (y/n) [n]");
        const bool overwrite = isYes(trimSpacesNative(ow));

        return runBatch(folder, outDir, pattern, overwrite, debug);
    }

    nout() << toNativeSimple("Invalid mode\n");
    return 1;
}

static int runCli(const std::vector<NativeString> &args)
{
    bool debug = false;
    bool overwrite = false;

    if (args.size() >= 2 && (nativeEqualsAscii(args[1], "--help") || nativeEqualsAscii(args[1], "-h"))) {
        printHelp();
        return 0;
    }

    // No args => interactive menu
    if (args.size() <= 1) {
        return runInteractive(false);
    }

    size_t i = 1;
    if (i < args.size() && nativeEqualsAscii(args[i], "--debug")) {
        debug = true;
        ++i;
    }

    if (i >= args.size()) {
        return runInteractive(debug);
    }

    // New-style commands: single/batch
    if (nativeIEqualsAscii(args[i], "single")) {
        ++i;
        if (i >= args.size()) {
            printHelp();
            return 1;
        }

        fs::path mp3Path = cleanPath(args[i++]);
        OutputOptions opt;

        while (i < args.size()) {
            if (nativeEqualsAscii(args[i], "--debug")) {
                debug = true;
                ++i;
            } else if (nativeEqualsAscii(args[i], "--overwrite")) {
                overwrite = true;
                ++i;
            } else if (nativeEqualsAscii(args[i], "--out")) {
                if (i + 1 >= args.size()) return 1;
                opt.out = cleanPath(args[i + 1]);
                i += 2;
            } else if (nativeEqualsAscii(args[i], "--name")) {
                if (i + 1 >= args.size()) return 1;
                opt.namePattern = trimSpacesNative(args[i + 1]);
                i += 2;
            } else {
                // unknown arg
                printHelp();
                return 1;
            }
        }

        opt.overwrite = overwrite;
        return runSingle(mp3Path, opt, debug);
    }

    if (nativeIEqualsAscii(args[i], "batch")) {
        ++i;
        if (i >= args.size()) {
            printHelp();
            return 1;
        }

        fs::path folder = cleanPath(args[i++]);
        fs::path outDir;
        NativeString pattern;

        while (i < args.size()) {
            if (nativeEqualsAscii(args[i], "--debug")) {
                debug = true;
                ++i;
            } else if (nativeEqualsAscii(args[i], "--overwrite")) {
                overwrite = true;
                ++i;
            } else if (nativeEqualsAscii(args[i], "--out-dir")) {
                if (i + 1 >= args.size()) return 1;
                outDir = cleanPath(args[i + 1]);
                i += 2;
            } else if (nativeEqualsAscii(args[i], "--pattern")) {
                if (i + 1 >= args.size()) return 1;
                pattern = trimSpacesNative(args[i + 1]);
                i += 2;
            } else {
                printHelp();
                return 1;
            }
        }

        return runBatch(folder, outDir, pattern, overwrite, debug);
    }

    // Old-style positional mode: [--debug] mp3 [saida]
    {
        fs::path mp3Path = cleanPath(args[i++]);
        fs::path out = (i < args.size()) ? cleanPath(args[i++]) : fs::path();
        OutputOptions opt;
        opt.out = out;
        opt.overwrite = false;
        return runSingle(mp3Path, opt, debug);
    }
}

#ifdef _WIN32

static std::vector<std::wstring> getWindowsArgsUnicode()
{
    int argcW = 0;
    LPWSTR *argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);

    std::vector<std::wstring> args;
    if (!argvW || argcW <= 0) {
        return args;
    }

    args.reserve(size_t(argcW));
    for (int i = 0; i < argcW; ++i) {
        args.emplace_back(argvW[i] ? argvW[i] : L"");
    }
    LocalFree(argvW);
    return args;
}

int main(int /*argc*/, char ** /*argv*/)
{
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stdout), _O_U16TEXT);

    std::vector<NativeString> args = getWindowsArgsUnicode();
    if (args.empty()) {
        // Fallback (should be rare)
        args.emplace_back(L"thumb_extrac");
    }

    return runCli(args);
}

#else

int main(int argc, char *argv[])
{
    std::vector<NativeString> args;
    args.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return runCli(args);
}

#endif
