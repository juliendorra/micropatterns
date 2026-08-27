#ifndef MICROPATTERNS_COMMAND_H
#define MICROPATTERNS_COMMAND_H

#include <Arduino.h>
#include <vector>
#include <list> // Added for std::list
#include <map> // Use map for parameters
#include "matrix_utils.h" // For matrix_identity

// Enum for command types
enum CommandType {
    CMD_UNKNOWN,
    CMD_DEFINE_PATTERN, // Handled at parse time -> NOOP
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

// Structure for parameter values (can be int, string, variable ref, or operator)
struct ParamValue {
    enum ValueType { TYPE_INT, TYPE_STRING, TYPE_VARIABLE, TYPE_OPERATOR } type;
    int intValue;
    String stringValue; // Also used for variable names ("$COUNTER") and operators ("+")

    // --- Runtime memo (not parse data) -------------------------------------
    // For TYPE_VARIABLE tokens the runtime resolves the (uppercased) name to a
    // small integer slot exactly once and remembers it here, so the second and
    // every subsequent evaluation of the same token -- i.e. every iteration of
    // the REPEAT loop it sits in -- costs an array index instead of a String
    // copy + toUpperCase() + two red-black-tree lookups.
    // `slotEpoch` tags which runtime instance filled the memo in; a fresh
    // MicroPatternsRuntime takes a new epoch, so a stale memo can never be
    // read. Epoch 0 is never issued, which makes the default value invalid.
    mutable int32_t slotCache = -1;
    mutable uint32_t slotEpoch = 0;

    ParamValue() : type(TYPE_INT), intValue(0) {}
    ParamValue(int v) : type(TYPE_INT), intValue(v) {}
    // Constructor for strings, variables, operators
    ParamValue(String s, ValueType t = TYPE_STRING) : type(t), stringValue(s) {}
};

struct MicroPatternsAsset; // defined below

// Structure for a parsed command
struct MicroPatternsCommand {
    CommandType type = CMD_UNKNOWN;
    int lineNumber = 0;
    std::map<String, ParamValue> params; // Use map for named parameters (Key = UPPERCASE NAME)

    // --- Fields for specific commands ---

    // For VAR command
    String varName; // UPPERCASE, no '$'
    std::vector<ParamValue> initialExpressionTokens; // Stores tokenized expression (numbers, $VARS, operators)

    // For LET command
    String letTargetVar; // UPPERCASE, no '$'
    std::vector<ParamValue> letExpressionTokens; // Stores tokenized expression

    // For REPEAT command
    ParamValue count; // Stores the parsed COUNT value (int or variable)
    std::list<MicroPatternsCommand> nestedCommands; // Stores commands inside the REPEAT block

    // For IF command
    std::vector<ParamValue> conditionTokens; // Stores tokenized condition expression
    std::list<MicroPatternsCommand> thenCommands;
    std::list<MicroPatternsCommand> elseCommands; // Populated only if ELSE is present

    // Runtime memo for the VAR/LET assignment target, same scheme as
    // ParamValue::slotCache above (avoids rebuilding "$" + varName, which is a
    // heap allocation on Arduino String, on every execution).
    mutable int32_t targetSlotCache = -1;
    mutable uint32_t targetSlotEpoch = 0;

    // Runtime memo for the NAME parameter of FILL / DRAW. The name is always a
    // literal (never a variable), so the asset it resolves to is fixed for the
    // life of the parse -- but uppercasing it and looking it up in the
    // String-keyed asset map costs two heap allocations per execution on
    // Arduino String, which inside a REPEAT body means per drawn item.
    // assetKind: 0 = not resolved yet, 1 = SOLID, 2 = asset (assetCache), 3 = unknown name.
    mutable const MicroPatternsAsset* assetCache = nullptr;
    mutable uint32_t assetEpoch = 0;
    mutable uint8_t assetKind = 0;

    MicroPatternsCommand(CommandType t = CMD_UNKNOWN, int line = 0) : type(t), lineNumber(line) {}
};

// Structure for defined patterns/assets
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

    // Default constructor initializes state
    MicroPatternsState() : color(15), fillAsset(nullptr), scale(1.0f) {
        // Initialize matrix and inverseMatrix to identity
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

// Structure for an item in the display list.
//
// This used to be `std::map<String,int> intParams` + `std::map<String,String>
// stringParams` + an inline copy of the transform state: ~128 bytes of struct
// plus 2-5 heap-allocated tree nodes (each with an Arduino String key) PER
// ITEM. A 20k-item script therefore paid ~60k allocations during generation and
// a String-keyed map lookup per parameter per item again during rasterization's
// per-item cull loop.
//
// It is now a trivially-copyable POD: parameters live in a fixed 4-slot array
// whose meaning is fixed by `type` (see the accessors), the DRAW asset is
// resolved to a pointer at generation time rather than carried as a name
// String, and the transform is a pointer into the runtime's snapshot pool.
// 40 bytes on a 32-bit target, zero heap allocations, and growing the
// std::vector is now a memcpy instead of thousands of map deep-copies.
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
