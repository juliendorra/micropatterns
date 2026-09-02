#ifndef MICROPATTERNS_PARSER_H
#define MICROPATTERNS_PARSER_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <set>
#include "micropatterns_command.h"
#include "mp_program.h"

// Parses MicroPatterns source and COMPILES it, in one pass, to an MpProgram.
//
// There is no intermediate command tree any more: each source line is
// tokenized (into short-lived ParamValues), validated, and immediately emitted
// as one MpInstr with interned variable slots, positional operands, postfix
// expressions and resolved asset indices. The parser's own bookkeeping --
// the variable slot table, the open-block stack, per-line parameter maps --
// still uses String-keyed containers, because it runs once per script and is
// thrown away; only the OUTPUT had to stop being made of Strings.
//
// Peak memory during a parse is therefore the source text plus the program
// plus one line's worth of scratch, instead of the source plus a tree that
// measured 10-50x the display list on the real scripts.
class MicroPatternsParser {
public:
    MicroPatternsParser();

    // Parses and compiles. Returns true if there were no errors; the program is
    // complete only in that case. The source fingerprint (length + CRC) is NOT
    // computed here -- storage code calls mp_program_fingerprint() when it is
    // about to persist the program, so a parse that is only going to render
    // does not pay for a CRC of the whole source.
    bool parse(const String& scriptText);

    const MpProgram& getProgram() const { return _program; }
    MpProgram& program() { return _program; }
    // Convenience for callers that only want the patterns (the editor).
    const std::vector<MicroPatternsAsset>& getAssets() const { return _program.assets; }
    const std::vector<String>& getErrors() const { return _errors; }
    void reset();

private:
    MpProgram _program;
    std::vector<String> _errors;
    int _lineNumber = 0;

    // Variable name (UPPERCASE, no '$') -> slot. Env names are seeded so one
    // table answers every lookup. A user variable is entered by VAR, or -- as
    // the old runtime did -- on first sight in a parameter, so a name that is
    // used but never declared still gets a slot and fails at runtime with the
    // same "Undefined variable" the old code produced, rather than a parse
    // error the old code never raised.
    std::map<String, int> _slotByName;
    // Names declared by VAR so far (UPPERCASE). Distinct from having a slot.
    std::set<String> _declaredByVar;

    // Open REPEAT / IF blocks, innermost last.
    struct Block {
        uint8_t type;      // CMD_REPEAT or CMD_IF
        int32_t pc;        // index of the REPEAT / IF instruction
        int32_t elsePc;    // index of the ELSE instruction, or -1
        int line;
    };
    std::vector<Block> _blocks;

    // FILL / DRAW asset names, resolved after the whole script is read because
    // DEFINE PATTERN may follow its first use (the old runtime resolved lazily
    // against the complete asset map, so that always worked).
    struct PendingAsset { int32_t pc; String upperName; bool isDraw; };
    std::vector<PendingAsset> _pendingAssets;

    void addError(const String& message);
    bool processLine(const String& line);
    bool parseDefinePattern(const String& argsString);
    bool parseVar(const String& argsString, String& outVarName, std::vector<ParamValue>& outTokens);
    bool parseLet(const String& argsString, String& outTargetVarName, std::vector<ParamValue>& outTokens);
    bool parseRepeat(const String& argsString, ParamValue& outCount);
    bool parseIf(const String& argsString, std::vector<ParamValue>& outConditionTokens);
    bool parseParams(const String& argsString, std::map<String, ParamValue>& params);
    ParamValue parseValue(const String& valueString);
    bool parseExpression(const String& expressionString, std::vector<ParamValue>& tokens);
    bool parseCondition(const String& conditionString, std::vector<ParamValue>& tokens);
    bool isEnvVar(const String& upperCaseName) const;
    bool validateVariableUsage(const String& varRefWithDollar);

    // --- compilation -------------------------------------------------------
    int  slotForName(const String& upperNoDollar) const;       // -1 if unknown
    int  internSlot(const String& upperNoDollar);              // creates if needed
    int  slotForVariableToken(const ParamValue& token);        // "$name" any case
    // Converts one parameter into a positional operand. A missing parameter
    // becomes the literal `defaultValue`; a parameter of the wrong type logs a
    // warning and also becomes the default -- exactly what the old runtime
    // substituted, so output is unchanged.
    MpOperand operandFromParam(const std::map<String, ParamValue>& params,
                               const char* key, int defaultValue);
    // Appends the tokens to the expression pool in postfix order. Returns the
    // slice; for conditions, sets `malformed` when there is not exactly one
    // comparison operator with a non-empty operand on each side.
    void emitExpression(const std::vector<ParamValue>& tokens, bool isCondition,
                        int32_t& outBegin, int32_t& outLen, bool& malformed);
    void resolvePendingAssets();
};

#endif // MICROPATTERNS_PARSER_H
