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

    ParamValue() : type(TYPE_INT), intValue(0) {}
    ParamValue(int v) : type(TYPE_INT), intValue(v) {}
    // Constructor for strings, variables, operators
    ParamValue(String s, ValueType t = TYPE_STRING) : type(t), stringValue(s) {}
};

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

// Structure for an item in the display list
struct DisplayListItem {
    CommandType type = CMD_UNKNOWN;
    int sourceLine = 0;

    // Resolved parameters
    std::map<String, int> intParams;
    std::map<String, String> stringParams; // For asset names, color keywords etc.

    // Snapshotted rendering state
    float matrix[6];
    float inverseMatrix[6];
    float scaleFactor = 1.0f;
    uint8_t color = 15; // Resolved color (0=white, 15=black)
    const MicroPatternsAsset* fillAsset = nullptr; // Pointer to asset, or nullptr for SOLID

    bool isOpaque = false; // Hint for occlusion culling

    DisplayListItem() {
        matrix_identity(matrix);
        matrix_identity(inverseMatrix);
    }
};

#endif // MICROPATTERNS_COMMAND_H