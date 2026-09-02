#ifndef MP_PROGRAM_H
#define MP_PROGRAM_H

// The flat, compiled form of a MicroPatterns script.
//
// This is what the parser now emits and what the runtime executes. It replaces
// the tree of MicroPatternsCommand nodes -- a std::list of ~256-byte structs
// each owning a String-keyed std::map of parameters and three token vectors of
// String-carrying ParamValues. Measured on the device scripts, that tree was
// 10-50x the size of the display list it produced (~146 KB for Seascape II,
// 387 commands) and was built from thousands of small heap allocations, which
// is what fragmented the Watchy's heap badly enough to reboot it on a button
// press. See docs/explainers/tree-walk-or-bytecode.html.
//
// A program is three contiguous arrays plus the pattern assets:
//
//   code    one 48-byte MpInstr per source command, in program order. REPEAT /
//           IF / ELSE carry jump targets instead of child lists, so the runtime
//           walks the array with a program counter and a small loop stack.
//   exprs   a pool of 8-byte operands. VAR / LET / IF expressions are slices of
//           it, already in POSTFIX (RPN) order with precedence resolved, so the
//           runtime evaluates them with a value stack and no operator passes.
//   assets  DEFINE PATTERN bitmaps, indexed by position. FILL / DRAW refer to
//           them by index, so nothing is looked up by name at runtime.
//
// Variables are interned to slots at parse time: the seven environment values
// take the fixed low slots, user variables follow. Parameters are positional,
// with meaning fixed by the instruction type (see MpInstr).
//
// The program is serializable (mp_program_serialize / deserialize). Both
// firmwares compile a script once -- at sync, after WiFi is torn down -- and
// persist the bytes next to the source, so a render never parses. The stored
// form is a cache: it carries the source length + CRC it was compiled from and
// the format version, and anything that does not match is thrown away and
// recompiled from the source. It never needs to survive; it needs to be
// detected as stale.

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "micropatterns_command.h" // CommandType, MicroPatternsAsset

// Bump whenever MpInstr / MpOperand / the serialized layout changes. A stored
// program with a different version is recompiled from source, never read.
static const uint16_t MP_PROGRAM_FORMAT_VERSION = 1;

// Fixed slots for the environment values. Order is part of the format.
enum MpEnvSlot : uint8_t {
    MP_ENV_WIDTH = 0, MP_ENV_HEIGHT, MP_ENV_HOUR, MP_ENV_MINUTE, MP_ENV_SECOND,
    MP_ENV_COUNTER, MP_ENV_INDEX,
    MP_ENV_SLOT_COUNT
};

enum MpOperandKind : uint8_t {
    MP_OPND_LIT = 0,   // v is the literal value
    MP_OPND_VAR = 1,   // v is a slot index (env or user)
    MP_OPND_OP  = 2,   // op is an MpOperator; v unused
};

enum MpOperator : uint8_t {
    MP_OP_ADD = 0, MP_OP_SUB, MP_OP_MUL, MP_OP_DIV, MP_OP_MOD,
    MP_OP_EQ, MP_OP_NE, MP_OP_LT, MP_OP_LE, MP_OP_GT, MP_OP_GE,
    MP_OP_COUNT
};

struct MpOperand {
    uint8_t  kind = MP_OPND_LIT;
    uint8_t  op   = 0;
    uint16_t pad  = 0;
    int32_t  v    = 0;
};
static_assert(sizeof(MpOperand) == 8, "MpOperand must be 8 bytes (serialized raw)");

// Values of MpInstr::aux for FILL / DRAW.
static const uint8_t MP_ASSET_SOLID   = 0xFF; // FILL NAME=SOLID
static const uint8_t MP_ASSET_UNKNOWN = 0xFE; // name did not resolve: runtime logs and treats as SOLID / skips DRAW

// One instruction. `type` is a CommandType. Field meaning by type:
//
//   VAR, LET          x0 = target slot; x1 = expr begin; x2 = expr length (0: no expression, VAR only)
//   REPEAT            op[0] = count; x0 = pc just past the matching ENDREPEAT
//   ENDREPEAT         x0 = pc of the matching REPEAT
//   IF                x1 = cond begin; x2 = cond length; x0 = pc to jump to when false
//                     (the instruction after ELSE, or after ENDIF); aux = 1 when
//                     the condition is malformed at runtime (no single comparison)
//   ELSE              x0 = pc just past the matching ENDIF (taken when reached from the THEN branch)
//   ENDIF             no operands
//   COLOR             aux = color value (0 white, 15 black)
//   FILL              aux = asset index, MP_ASSET_SOLID or MP_ASSET_UNKNOWN
//   DRAW              aux = asset index or MP_ASSET_UNKNOWN; op[0] = X, op[1] = Y
//   RESET_TRANSFORMS  no operands
//   TRANSLATE         op[0] = DX, op[1] = DY
//   ROTATE            op[0] = DEGREES
//   SCALE             op[0] = FACTOR
//   PIXEL, FILL_PIXEL op[0] = X, op[1] = Y
//   LINE              op[0..3] = X1, Y1, X2, Y2
//   RECT, FILL_RECT   op[0..3] = X, Y, WIDTH, HEIGHT
//   CIRCLE, FILL_CIRCLE op[0..2] = X, Y, RADIUS
//
// A parameter the script omitted is stored as the literal default the old
// runtime substituted (0, or 1 for SCALE FACTOR), so output is unchanged.
struct MpInstr {
    uint8_t   type = CMD_UNKNOWN;
    uint8_t   aux  = 0;
    uint16_t  line = 0;
    int32_t   x0 = 0, x1 = 0, x2 = 0;
    MpOperand op[4];
};
static_assert(sizeof(MpInstr) == 48, "MpInstr must be 48 bytes (serialized raw)");

struct MpProgram {
    std::vector<MpInstr>            code;
    std::vector<MpOperand>          exprs;
    std::vector<MicroPatternsAsset> assets;
    // Names of the user variables, uppercase without '$'. Slot = MP_ENV_SLOT_COUNT + index.
    // Kept for runtime error messages; not needed to execute.
    std::vector<String>             varNames;
    // Deepest value stack any expression needs. Computed by the compiler so the
    // runtime can size its scratch once, before the hot loop.
    uint16_t maxExprStack = 0;
    // Fingerprint of the source text this program was compiled from.
    uint32_t sourceLength = 0;
    uint32_t sourceCrc = 0;

    void clear();
    // Bytes the program occupies in RAM (vectors' payloads, not their headers).
    size_t byteSize() const;
    int userVariableCount() const { return (int)varNames.size(); }
};

// CRC-32 (IEEE, reflected), incremental: pass the previous result as `seed`
// to continue over the next chunk; start from 0.
uint32_t mp_crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

// Records which source text `program` was compiled from, so a stored copy can
// later be matched against the source file without re-parsing it.
void mp_program_fingerprint(MpProgram& program, const uint8_t* source, size_t len);

// --- serialized form ---------------------------------------------------------
//
// Little-endian throughout (both ESP32 targets, the host harness and WASM are
// little-endian; the writer/reader use explicit byte order regardless).
//
//   magic        "MPC1"                         4
//   version      u16 = MP_PROGRAM_FORMAT_VERSION 2
//   flags        u16 (reserved, 0)               2
//   sourceLength u32                             4
//   sourceCrc    u32                             4
//   bodyLength   u32                             4   bytes after this header
//   bodyCrc      u32                             4   CRC-32 of the body
//   -- body --
//   instrCount   u32, then instrCount x 48 raw MpInstr
//   exprCount    u32, then exprCount  x 8  raw MpOperand
//   maxExprStack u16
//   assetCount   u16, then per asset: u8 nameLen, name, u8 origLen, origName,
//                u16 width, u16 height, ceil(w*h/8) bytes of pixels (bit 0 of
//                byte 0 = first pixel)
//   varCount     u16, then per variable: u8 len, name
static const size_t MP_PROGRAM_HEADER_SIZE = 24;

// Exact number of bytes mp_program_serialize() will produce.
size_t mp_program_serialized_size(const MpProgram& program);

// Appends nothing on failure; `out` is cleared first. `out` is reserved to the
// exact size, so this allocates once.
bool mp_program_serialize(const MpProgram& program, std::vector<uint8_t>& out);

// Validates magic, version, body length and both CRCs, then rebuilds the
// program. On failure `out` is cleared and `error` (if given) says why.
bool mp_program_deserialize(const uint8_t* data, size_t len, MpProgram& out, String* error = nullptr);

// Streaming variant: reads the program straight from a source (a file on the
// device) into its final containers, checking the body CRC as it goes, so the
// serialized bytes never exist in RAM as a whole. `totalLen` is the size of
// the whole stored program (header included). If the caller has already read
// and checked the 24-byte header (ScriptManager does, to decide whether the
// stored program matches the source), pass it in `headerAlreadyRead`.
struct MpByteReader {
    virtual ~MpByteReader() {}
    // Returns the number of bytes actually read; anything short of `n` fails the load.
    virtual size_t read(uint8_t* dst, size_t n) = 0;
};
bool mp_program_deserialize_stream(MpByteReader& in, size_t totalLen, MpProgram& out,
                                   String* error = nullptr,
                                   const uint8_t* headerAlreadyRead = nullptr);

// Cheap check of the header only: is this a current-format program compiled
// from a source of exactly this length and CRC? Does not touch the body, so a
// caller can decide whether to read the rest of a stored file at all.
bool mp_program_header_matches(const uint8_t* header, size_t len,
                               uint32_t sourceLength, uint32_t sourceCrc);

#endif // MP_PROGRAM_H
