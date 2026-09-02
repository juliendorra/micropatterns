#ifndef MICROPATTERNS_COMMAND_H
#define MICROPATTERNS_COMMAND_H

#include <Arduino.h>
#include <vector>
#include "matrix_utils.h" // For matrix_identity

// Command types. The values double as MpInstr::type in the compiled program
// (mp_program.h), so this enum is part of the stored-program format: append,
// never renumber.
enum CommandType : uint8_t {
    CMD_UNKNOWN = 0,
    CMD_DEFINE_PATTERN, // Handled at parse time -> no instruction
    CMD_VAR,
    CMD_LET,
    CMD_COLOR,
    CMD_FILL,
    CMD_DRAW,
    CMD_RESET_TRANSFORMS,
    CMD_TRANSLATE,
    CMD_ROTATE,
    CMD_SCALE,
    CMD_PIXEL,
    CMD_FILL_PIXEL,
    CMD_LINE,
    CMD_RECT,
    CMD_FILL_RECT,
    CMD_CIRCLE,
    CMD_FILL_CIRCLE,
    CMD_REPEAT,     // Block start
    CMD_ENDREPEAT,  // Block end
    CMD_IF,         // Block start
    CMD_ELSE,       // Mid-block marker
    CMD_ENDIF,      // Block end
    CMD_NOOP        // For commands handled entirely at parse time
};

// A parsed parameter or expression token. PARSER-INTERNAL: it lives only for
// the duration of one source line, while the parser turns KEY=VALUE pairs and
// expression text into MpOperands. It no longer appears in anything the
// runtime touches -- that was the whole problem (see mp_program.h).
struct ParamValue {
    enum ValueType { TYPE_INT, TYPE_STRING, TYPE_VARIABLE, TYPE_OPERATOR } type;
    int intValue;
    String stringValue; // Also used for variable names ("$COUNTER") and operators ("+")

    ParamValue() : type(TYPE_INT), intValue(0) {}
    ParamValue(int v) : type(TYPE_INT), intValue(v) {}
    ParamValue(String s, ValueType t = TYPE_STRING) : type(t), intValue(0), stringValue(s) {}
};

// A DEFINE PATTERN bitmap. Owned by MpProgram::assets; DisplayListItems point
// at these, so a display list must not outlive the program it was built from.
struct MicroPatternsAsset {
    String name; // Uppercase name (used as key)
    String originalName; // Original case name for display/errors
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data; // 0 or 1
};

// Structure for drawing state
struct MicroPatternsState {
    uint8_t color = 15; // 0=white, 15=black (M5EPD uses 4bpp)
    const MicroPatternsAsset* fillAsset = nullptr; // Pointer to current fill pattern, null for SOLID

    // Absolute scale factor, applied BEFORE the matrix transformation.
    float scale = 1.0f;

    // Affine transformation matrix representing cumulative TRANSLATE and ROTATE operations.
    // Applied AFTER 'scale'.
    // Format: [m0, m1, m2, m3, m4, m5] => | m0 m2 m4 |
    //                                     | m1 m3 m5 |
    //                                     |  0  0  1 |
    // (x', y') = (m0*x + m2*y + m4, m1*x + m3*y + m5)
    float matrix[6];

    // Inverse of 'matrix'. Used for transforming screen coordinates back.
    float inverseMatrix[6];

    MicroPatternsState() : color(15), fillAsset(nullptr), scale(1.0f) {
        matrix_identity(matrix);
        matrix_identity(inverseMatrix);
    }
};

// Snapshot of the transform state shared by a run of display-list items.
//
// Transform state changes only on TRANSLATE / ROTATE / SCALE / RESET_TRANSFORMS,
// which are far rarer than the primitives between them, so items point at a
// pooled snapshot instead of each carrying their own 52-byte copy. The pool is
// owned by MicroPatternsRuntime and lives as long as the display list does.
struct TransformSnapshot {
    float matrix[6];
    float inverseMatrix[6];
    float scale = 1.0f;

    TransformSnapshot() {
        matrix_identity(matrix);
        matrix_identity(inverseMatrix);
    }
};

// Identity snapshot used when an item somehow has no pooled transform, so that
// consumers never have to null-check `xf`.
extern const TransformSnapshot kIdentityTransform;

// One item in the display list: a trivially-copyable POD. Parameters live in a
// fixed 4-slot array whose meaning is fixed by `type` (see the accessors), the
// DRAW asset is a resolved pointer, and the transform is a pointer into the
// runtime's snapshot pool. 40 bytes on a 32-bit target, zero heap allocations.
struct DisplayListItem {
    CommandType type = CMD_UNKNOWN;
    int32_t sourceLine = 0;

    // Resolved integer parameters. Slot meaning by command type:
    //   PIXEL / FILL_PIXEL : [0]=X  [1]=Y
    //   LINE               : [0]=X1 [1]=Y1 [2]=X2 [3]=Y2
    //   RECT / FILL_RECT   : [0]=X  [1]=Y  [2]=WIDTH [3]=HEIGHT
    //   CIRCLE/FILL_CIRCLE : [0]=X  [1]=Y  [2]=RADIUS
    //   DRAW               : [0]=X  [1]=Y  (asset in `asset`)
    int32_t p[4] = {0, 0, 0, 0};

    // Asset drawn by a DRAW item, resolved at generation time. nullptr otherwise.
    const MicroPatternsAsset* asset = nullptr;

    // Snapshotted rendering state
    const TransformSnapshot* xf = &kIdentityTransform;
    const MicroPatternsAsset* fillAsset = nullptr; // Current FILL pattern, or nullptr for SOLID
    uint8_t color = 15;                            // Resolved color (0=white, 15=black)
    bool isOpaque = false;                         // Hint for occlusion culling

    // Named accessors -- the readable spelling of the slots above.
    int x()  const { return p[0]; }
    int y()  const { return p[1]; }
    int w()  const { return p[2]; }
    int h()  const { return p[3]; }
    int x1() const { return p[0]; }
    int y1() const { return p[1]; }
    int x2() const { return p[2]; }
    int y2() const { return p[3]; }
    int radius() const { return p[2]; }
};

#endif // MICROPATTERNS_COMMAND_H
