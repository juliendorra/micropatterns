#ifndef MICROPATTERNS_COMMAND_H
#define MICROPATTERNS_COMMAND_H

#include <Arduino.h>
#include <vector>
#include <map> // Use map for parameters

// Enum for command types
enum CommandType {
    CMD_UNKNOWN,
    CMD_DEFINE_PATTERN,
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
    CMD_REPEAT, // Not implemented in this basic version
    CMD_IF,     // Not implemented in this basic version
    CMD_NOOP    // For commands handled entirely at parse time (like DEFINE)
};

// Structure for parameter values (can be int or string)
struct ParamValue {
    enum ValueType { TYPE_INT, TYPE_STRING, TYPE_VARIABLE } type;
    int intValue;
    String stringValue; // Also used for variable names (e.g., "$COUNTER")

    ParamValue() : type(TYPE_INT), intValue(0) {}
    ParamValue(int v) : type(TYPE_INT), intValue(v) {}
    ParamValue(String s, bool isVariable = false) : type(isVariable ? TYPE_VARIABLE : TYPE_STRING), stringValue(s) {}
};

// Structure for a parsed command
struct MicroPatternsCommand {
    CommandType type = CMD_UNKNOWN;
    int lineNumber = 0;
    std::map<String, ParamValue> params; // Use map for named parameters (Key = UPPERCASE NAME)

    // Specific fields for commands not using generic params map
    String definePatternName;
    int definePatternWidth = 0;
    int definePatternHeight = 0;
    String definePatternData;

    String varName; // For VAR command (UPPERCASE, no '$')
    // std::vector<ExpressionToken> initialExpression; // For VAR init (complex, skip for now)

    String letTargetVar; // For LET command (UPPERCASE, no '$')
    // std::vector<ExpressionToken> letExpression; // For LET assignment (complex, skip for now)
    // Removed: int letValue = 0; // Use params["VALUE"] instead

    MicroPatternsCommand(CommandType t = CMD_UNKNOWN, int line = 0) : type(t), lineNumber(line) {}
};

// Structure for defined patterns/assets
struct MicroPatternsAsset {
    String name; // Uppercase name
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data; // 0 or 1
};

// Structure for drawing state
struct MicroPatternsState {
    uint8_t color = 15; // 0=white, 15=black (M5EPD uses 4bpp)
    const MicroPatternsAsset* fillAsset = nullptr; // Pointer to current fill pattern, null for SOLID
    float scale = 1.0;
    int rotationDegrees = 0;
    float translateX = 0;
    float translateY = 0;
    // Note: For simplicity, this state uses absolute values set by the *last*
    // relevant command, not a cumulative transformation stack like the JS version.
};

#endif // MICROPATTERNS_COMMAND_H