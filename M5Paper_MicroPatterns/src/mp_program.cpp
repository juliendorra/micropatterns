#include "mp_program.h"
#include <string.h>

// ---------------------------------------------------------------------------
// CRC-32 with a 16-entry nibble table: two lookups per byte, 64 bytes of
// table. It runs over a ~15 KB source once per sync and over a program body
// once per load, so neither a 1 KB table nor the bitwise loop is the right
// trade on the watch.
// ---------------------------------------------------------------------------
static const uint32_t kCrcNibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t mp_crc32(const uint8_t* data, size_t len, uint32_t seed) {
    uint32_t crc = ~seed;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ kCrcNibble[crc & 0x0F];
        crc = (crc >> 4) ^ kCrcNibble[crc & 0x0F];
    }
    return ~crc;
}

void mp_program_fingerprint(MpProgram& program, const uint8_t* source, size_t len) {
    program.sourceLength = (uint32_t)len;
    program.sourceCrc = mp_crc32(source, len);
}

void MpProgram::clear() {
    code.clear();
    exprs.clear();
    assets.clear();
    varNames.clear();
    maxExprStack = 0;
    sourceLength = 0;
    sourceCrc = 0;
}

size_t MpProgram::byteSize() const {
    size_t n = code.size() * sizeof(MpInstr) + exprs.size() * sizeof(MpOperand);
    for (const MicroPatternsAsset& a : assets) {
        n += sizeof(MicroPatternsAsset) + a.data.size() + a.name.length() + a.originalName.length();
    }
    for (const String& v : varNames) n += sizeof(String) + v.length();
    return n;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------
namespace {

const char kMagic[4] = {'M', 'P', 'C', '1'};

struct Writer {
    std::vector<uint8_t>& out;
    explicit Writer(std::vector<uint8_t>& o) : out(o) {}
    void u8(uint8_t v)   { out.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)v); u16((uint16_t)(v >> 16)); }
    void raw(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        out.insert(out.end(), b, b + n);
    }
    // u8 length prefix; names longer than 255 are truncated (the parser never
    // produces one, but the format must not be able to lie about a length).
    void str8(const String& s) {
        size_t n = s.length();
        if (n > 255) n = 255;
        u8((uint8_t)n);
        if (n) raw(s.c_str(), n);
    }
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    Reader(const uint8_t* data, size_t len) : p(data), end(data + len) {}
    bool need(size_t n) { if ((size_t)(end - p) < n) { ok = false; return false; } return true; }
    uint8_t  u8()  { if (!need(1)) return 0; return *p++; }
    uint16_t u16() { uint16_t lo = u8(); uint16_t hi = u8(); return (uint16_t)(lo | (hi << 8)); }
    uint32_t u32() { uint32_t lo = u16(); uint32_t hi = u16(); return lo | (hi << 16); }
    bool raw(void* dst, size_t n) { if (!need(n)) return false; memcpy(dst, p, n); p += n; return true; }
    bool str8(String& s) {
        uint8_t n = u8();
        if (!need(n)) return false;
        s = String();
        s.reserve(n);
        for (uint8_t i = 0; i < n; ++i) s += (char)p[i];
        p += n;
        return true;
    }
};

// Little-endian raw copies of MpInstr / MpOperand rely on every target sharing
// the layout the static_asserts pin. int32_t fields are stored in host order,
// which is little-endian on every target this runs on; a big-endian host would
// need per-field writes here.
static_assert(sizeof(int32_t) == 4, "int32_t");

} // namespace

namespace {

size_t assetPixelBytes(const MicroPatternsAsset& a) {
    return ((size_t)a.width * (size_t)a.height + 7) / 8;
}
size_t cappedLen(const String& s) { return s.length() > 255 ? 255 : s.length(); }

size_t bodySizeOf(const MpProgram& p) {
    size_t n = 4 + p.code.size() * sizeof(MpInstr)
             + 4 + p.exprs.size() * sizeof(MpOperand)
             + 2 + 2;
    for (const MicroPatternsAsset& a : p.assets) {
        n += 1 + cappedLen(a.name) + 1 + cappedLen(a.originalName) + 2 + 2 + assetPixelBytes(a);
    }
    n += 2;
    for (const String& v : p.varNames) n += 1 + cappedLen(v);
    return n;
}

// A memory-backed MpByteReader, so the in-memory deserializer is the stream
// deserializer over a buffer -- one implementation, not two.
struct MemReader : MpByteReader {
    const uint8_t* p; const uint8_t* end;
    MemReader(const uint8_t* d, size_t n) : p(d), end(d + n) {}
    size_t read(uint8_t* dst, size_t n) override {
        size_t avail = (size_t)(end - p);
        if (n > avail) n = avail;
        memcpy(dst, p, n); p += n; return n;
    }
};

// Reads from an MpByteReader while folding everything read into a CRC.
struct CrcReader {
    MpByteReader& in;
    uint32_t crc = 0;
    size_t consumed = 0;
    bool ok = true;
    explicit CrcReader(MpByteReader& r) : in(r) {}
    bool raw(void* dst, size_t n) {
        if (!ok) return false;
        if (n == 0) return true;
        size_t got = in.read((uint8_t*)dst, n);
        if (got != n) { ok = false; return false; }
        crc = mp_crc32((const uint8_t*)dst, n, crc);
        consumed += n;
        return true;
    }
    uint8_t  u8()  { uint8_t v = 0; raw(&v, 1); return v; }
    uint16_t u16() { uint8_t b[2] = {0, 0}; raw(b, 2); return (uint16_t)(b[0] | (b[1] << 8)); }
    uint32_t u32() { uint8_t b[4] = {0, 0, 0, 0}; raw(b, 4); return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
    bool str8(String& s) {
        uint8_t n = u8();
        if (!ok) return false;
        char buf[256];
        if (!raw(buf, n)) return false;
        buf[n] = '\0';
        s = String(buf);
        return true;
    }
};

bool validateProgram(const MpProgram& out, const char*& why) {
    const int32_t n = (int32_t)out.code.size();
    const int32_t ne = (int32_t)out.exprs.size();
    const int32_t slots = MP_ENV_SLOT_COUNT + (int32_t)out.varNames.size();
    for (int32_t pc = 0; pc < n; ++pc) {
        const MpInstr& in = out.code[pc];
        switch (in.type) {
            case CMD_REPEAT: case CMD_ENDREPEAT: case CMD_ELSE:
                if (in.x0 < 0 || in.x0 > n) { why = "jump out of range"; return false; }
                break;
            case CMD_IF:
                if (in.x0 < 0 || in.x0 > n) { why = "jump out of range"; return false; }
                if (in.x1 < 0 || in.x2 < 0 || in.x1 + in.x2 > ne) { why = "expr out of range"; return false; }
                break;
            case CMD_VAR: case CMD_LET:
                if (in.x1 < 0 || in.x2 < 0 || in.x1 + in.x2 > ne) { why = "expr out of range"; return false; }
                if (in.x0 < MP_ENV_SLOT_COUNT || in.x0 >= slots) { why = "variable slot out of range"; return false; }
                break;
            case CMD_FILL: case CMD_DRAW:
                if (in.aux != MP_ASSET_SOLID && in.aux != MP_ASSET_UNKNOWN && in.aux >= out.assets.size()) {
                    why = "asset index out of range"; return false;
                }
                break;
            default: break;
        }
        for (int k = 0; k < 4; ++k) {
            const MpOperand& o = in.op[k];
            if (o.kind == MP_OPND_VAR && (o.v < 0 || o.v >= slots)) { why = "operand slot out of range"; return false; }
        }
    }
    for (const MpOperand& o : out.exprs) {
        if (o.kind == MP_OPND_VAR && (o.v < 0 || o.v >= slots)) { why = "expr slot out of range"; return false; }
        if (o.kind == MP_OPND_OP && o.op >= MP_OP_COUNT) { why = "bad operator"; return false; }
    }
    return true;
}

} // namespace

size_t mp_program_serialized_size(const MpProgram& program) {
    return MP_PROGRAM_HEADER_SIZE + bodySizeOf(program);
}

bool mp_program_serialize(const MpProgram& program, std::vector<uint8_t>& out) {
    out.clear();
    if (program.code.size() > 0xFFFFFFu || program.exprs.size() > 0xFFFFFFu ||
        program.assets.size() > 0xFFFF || program.varNames.size() > 0xFFFF) {
        return false;
    }
    const size_t bodySize = bodySizeOf(program);
    out.reserve(MP_PROGRAM_HEADER_SIZE + bodySize);

    Writer w(out);
    w.raw(kMagic, 4);
    w.u16(MP_PROGRAM_FORMAT_VERSION);
    w.u16(0);
    w.u32(program.sourceLength);
    w.u32(program.sourceCrc);
    w.u32((uint32_t)bodySize);
    w.u32(0); // body CRC, patched below

    w.u32((uint32_t)program.code.size());
    if (!program.code.empty()) w.raw(program.code.data(), program.code.size() * sizeof(MpInstr));
    w.u32((uint32_t)program.exprs.size());
    if (!program.exprs.empty()) w.raw(program.exprs.data(), program.exprs.size() * sizeof(MpOperand));
    w.u16(program.maxExprStack);
    w.u16((uint16_t)program.assets.size());
    for (const MicroPatternsAsset& a : program.assets) {
        w.str8(a.name);
        w.str8(a.originalName);
        w.u16((uint16_t)a.width);
        w.u16((uint16_t)a.height);
        const size_t px = (size_t)a.width * (size_t)a.height;
        const size_t bytes = assetPixelBytes(a);
        for (size_t i = 0; i < bytes; ++i) {
            uint8_t b = 0;
            for (int bit = 0; bit < 8; ++bit) {
                size_t idx = i * 8 + (size_t)bit;
                if (idx < px && idx < a.data.size() && a.data[idx]) b |= (uint8_t)(1u << bit);
            }
            w.u8(b);
        }
    }
    w.u16((uint16_t)program.varNames.size());
    for (const String& v : program.varNames) w.str8(v);

    if (out.size() != MP_PROGRAM_HEADER_SIZE + bodySize) { out.clear(); return false; }
    const uint32_t crc = mp_crc32(out.data() + MP_PROGRAM_HEADER_SIZE, bodySize);
    out[20] = (uint8_t)crc; out[21] = (uint8_t)(crc >> 8); out[22] = (uint8_t)(crc >> 16); out[23] = (uint8_t)(crc >> 24);
    return true;
}

bool mp_program_header_matches(const uint8_t* header, size_t len,
                               uint32_t sourceLength, uint32_t sourceCrc) {
    if (!header || len < MP_PROGRAM_HEADER_SIZE) return false;
    if (memcmp(header, kMagic, 4) != 0) return false;
    Reader r(header + 4, len - 4);
    if (r.u16() != MP_PROGRAM_FORMAT_VERSION) return false;
    (void)r.u16(); // flags
    if (r.u32() != sourceLength) return false;
    if (r.u32() != sourceCrc) return false;
    return true;
}

bool mp_program_deserialize_stream(MpByteReader& in, size_t totalLen, MpProgram& out,
                                   String* error, const uint8_t* headerAlreadyRead) {
    out.clear();
    auto fail = [&](const char* why) {
        out.clear();
        if (error) *error = why;
        return false;
    };
    if (totalLen < MP_PROGRAM_HEADER_SIZE) return fail("too short");

    uint8_t header[MP_PROGRAM_HEADER_SIZE];
    if (headerAlreadyRead) {
        memcpy(header, headerAlreadyRead, sizeof(header));
    } else if (in.read(header, sizeof(header)) != sizeof(header)) {
        return fail("truncated header");
    }
    if (memcmp(header, kMagic, 4) != 0) return fail("bad magic");
    Reader h(header + 4, sizeof(header) - 4);
    if (h.u16() != MP_PROGRAM_FORMAT_VERSION) return fail("format version mismatch");
    (void)h.u16();
    const uint32_t sourceLength = h.u32();
    const uint32_t sourceCrc = h.u32();
    const uint32_t bodyLength = h.u32();
    const uint32_t bodyCrc = h.u32();
    if (bodyLength != totalLen - MP_PROGRAM_HEADER_SIZE) return fail("body length mismatch");

    CrcReader r(in);
    const uint32_t instrCount = r.u32();
    if (!r.ok || (size_t)instrCount * sizeof(MpInstr) > bodyLength) return fail("truncated code");
    out.code.resize(instrCount);
    if (instrCount && !r.raw(out.code.data(), (size_t)instrCount * sizeof(MpInstr))) return fail("truncated code");

    const uint32_t exprCount = r.u32();
    if (!r.ok || (size_t)exprCount * sizeof(MpOperand) > bodyLength) return fail("truncated exprs");
    out.exprs.resize(exprCount);
    if (exprCount && !r.raw(out.exprs.data(), (size_t)exprCount * sizeof(MpOperand))) return fail("truncated exprs");

    out.maxExprStack = r.u16();

    const uint16_t assetCount = r.u16();
    if (!r.ok) return fail("truncated body");
    out.assets.resize(assetCount);
    for (uint16_t i = 0; i < assetCount; ++i) {
        MicroPatternsAsset& a = out.assets[i];
        if (!r.str8(a.name) || !r.str8(a.originalName)) return fail("truncated asset");
        a.width = r.u16();
        a.height = r.u16();
        const size_t px = (size_t)a.width * (size_t)a.height;
        const size_t bytes = (px + 7) / 8;
        if (!r.ok || bytes > bodyLength) return fail("truncated asset");
        a.data.assign(px, 0);
        uint8_t packed[64];
        size_t done = 0;
        while (done < bytes) {
            size_t chunk = bytes - done; if (chunk > sizeof(packed)) chunk = sizeof(packed);
            if (!r.raw(packed, chunk)) return fail("truncated asset");
            for (size_t k = 0; k < chunk * 8; ++k) {
                const size_t idx = (done * 8) + k;
                if (idx < px) a.data[idx] = (packed[k >> 3] >> (k & 7)) & 1u;
            }
            done += chunk;
        }
    }

    const uint16_t varCount = r.u16();
    if (!r.ok) return fail("truncated body");
    out.varNames.resize(varCount);
    for (uint16_t i = 0; i < varCount; ++i) if (!r.str8(out.varNames[i])) return fail("truncated body");

    if (r.consumed != bodyLength) return fail("trailing bytes");
    if (r.crc != bodyCrc) return fail("body CRC mismatch");

    const char* why = nullptr;
    if (!validateProgram(out, why)) return fail(why);

    out.sourceLength = sourceLength;
    out.sourceCrc = sourceCrc;
    return true;
}

bool mp_program_deserialize(const uint8_t* data, size_t len, MpProgram& out, String* error) {
    if (!data) { out.clear(); if (error) *error = "too short"; return false; }
    MemReader mem(data, len);
    return mp_program_deserialize_stream(mem, len, out, error);
}
