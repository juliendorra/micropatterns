#include "micropatterns_parser.h"
#include <ctype.h> // For isdigit, isspace, isalnum
#include <limits.h>
#include "esp32-hal-log.h" // For log_w warning
#include "mp_diagnostics.h"

MicroPatternsParser::MicroPatternsParser() {
    reset();
}

void MicroPatternsParser::reset() {
    _program.clear();
    _errors.clear();
    _blocks.clear();
    _pendingAssets.clear();
    _slotByName.clear();
    _declaredByVar.clear();
    _slotByName["WIDTH"]   = MP_ENV_WIDTH;
    _slotByName["HEIGHT"]  = MP_ENV_HEIGHT;
    _slotByName["HOUR"]    = MP_ENV_HOUR;
    _slotByName["MINUTE"]  = MP_ENV_MINUTE;
    _slotByName["SECOND"]  = MP_ENV_SECOND;
    _slotByName["COUNTER"] = MP_ENV_COUNTER;
    _slotByName["INDEX"]   = MP_ENV_INDEX;
    _lineNumber = 0;
}

void MicroPatternsParser::addError(const String& message) {
    _errors.push_back("Line " + String(_lineNumber) + ": " + message);
}

bool MicroPatternsParser::isEnvVar(const String& upperCaseName) const {
    return upperCaseName == "HOUR" || upperCaseName == "MINUTE" || upperCaseName == "SECOND" ||
           upperCaseName == "COUNTER" || upperCaseName == "WIDTH" || upperCaseName == "HEIGHT" ||
           upperCaseName == "INDEX";
}

// A variable must be an env var or already declared by VAR at this point in
// the script. Expects "$MYVAR" (case may vary).
bool MicroPatternsParser::validateVariableUsage(const String& varRefWithDollar) {
    if (!varRefWithDollar.startsWith("$") || varRefWithDollar.length() <= 1) {
        addError("Invalid variable reference format: " + varRefWithDollar);
        return false;
    }
    String bareNameUpper = varRefWithDollar.substring(1);
    bareNameUpper.toUpperCase();
    if (slotForName(bareNameUpper) < 0) {
        addError("Undefined variable used: " + varRefWithDollar);
        return false;
    }
    return true;
}

int MicroPatternsParser::slotForName(const String& upperNoDollar) const {
    auto it = _slotByName.find(upperNoDollar);
    return it == _slotByName.end() ? -1 : it->second;
}

int MicroPatternsParser::internSlot(const String& upperNoDollar) {
    int slot = slotForName(upperNoDollar);
    if (slot >= 0) return slot;
    slot = MP_ENV_SLOT_COUNT + (int)_program.varNames.size();
    _program.varNames.push_back(upperNoDollar);
    _slotByName[upperNoDollar] = slot;
    return slot;
}

int MicroPatternsParser::slotForVariableToken(const ParamValue& token) {
    String upper = token.stringValue.substring(1);
    upper.toUpperCase();
    return internSlot(upper);
}

bool MicroPatternsParser::parse(const String& scriptText) {
    reset();
    mp_diagnostic_source_line(0);

    // Size the output arrays once, from a cheap pre-count, instead of letting
    // std::vector double its way up. Doubling leaves the old buffer alive
    // while the new one is filled -- a 1.5x transient peak in contiguous
    // blocks, on the one device where the largest free block is the number
    // that decides whether a render happens at all.
    {
        size_t lines = 0, ops = 0;
        const char* c = scriptText.c_str();
        if (c) {
            while (*c) {
                // start of a line
                while (*c == ' ' || *c == '\t') ++c;
                if (!*c) break;
                if (*c != '#' && *c != '\n' && *c != '\r') {
                    ++lines;
                    // Only VAR / LET / IF put anything in the expression pool.
                    // Count its tokens: every '$', every operator character,
                    // and every run of digits -- never the digits inside a
                    // quoted DATA="0101..." string.
                    const char k0 = (char)toupper((unsigned char)c[0]);
                    const char k1 = c[0] ? (char)toupper((unsigned char)c[1]) : 0;
                    const char k2 = c[1] ? (char)toupper((unsigned char)c[2]) : 0;
                    const bool exprLine = (k0 == 'V' && k1 == 'A' && k2 == 'R') ||
                                          (k0 == 'L' && k1 == 'E' && k2 == 'T') ||
                                          (k0 == 'I' && k1 == 'F' && (k2 == ' ' || k2 == '\t'));
                    if (exprLine) {
                        bool inDigits = false, inQuote = false;
                        for (const char* q = c; *q && *q != '\n'; ++q) {
                            if (*q == '"') { inQuote = !inQuote; inDigits = false; continue; }
                            if (inQuote) continue;
                            const bool digit = (*q >= '0' && *q <= '9');
                            if (digit) { if (!inDigits) { ++ops; inDigits = true; } continue; }
                            inDigits = false;
                            if (*q == '$' || *q == '+' || *q == '-' || *q == '*' || *q == '/' || *q == '%' ||
                                *q == '<' || *q == '>' || *q == '=' || *q == '!') ++ops;
                        }
                    }
                }
                while (*c && *c != '\n') ++c;
                if (*c == '\n') ++c;
            }
        }
        _program.code.reserve(lines);
        _program.exprs.reserve(ops);
    }

    String currentLine;
    int start = 0;
    int end = scriptText.indexOf('\n');

    while (start < (int)scriptText.length()) {
        _lineNumber++;
        mp_diagnostic_source_line(_lineNumber);
        if (end == -1) {
            currentLine = scriptText.substring(start);
        } else {
            currentLine = scriptText.substring(start, end);
        }
        currentLine.trim();

        if (currentLine.length() > 0 && !currentLine.startsWith("#")) {
            processLine(currentLine); // errors are collected, parsing continues
        }

        if (end == -1) break;
        start = end + 1;
        end = scriptText.indexOf('\n', start);
    }

    if (!_blocks.empty()) {
        const Block& open = _blocks.back();
        String blockType = (open.type == CMD_REPEAT) ? "REPEAT" : "IF";
        addError("Unclosed " + blockType + " block started on line " + String(open.line) + ". Expected END" + blockType + ".");
    }

    resolvePendingAssets();

    // Give back whatever the pre-count over-reserved -- but shrinking is a
    // copy (new exact buffer, then free the old), so only bother when the
    // slack is worth the transient.
    if (_program.code.capacity() > _program.code.size() + _program.code.size() / 4 + 8)
        _program.code.shrink_to_fit();
    if (_program.exprs.capacity() > _program.exprs.size() + _program.exprs.size() / 4 + 16)
        _program.exprs.shrink_to_fit();

    return _errors.empty();
}

// ---------------------------------------------------------------------------
// One line -> one instruction (or a pattern definition, or a block marker)
// ---------------------------------------------------------------------------
bool MicroPatternsParser::processLine(const String& line) {
    int firstSpace = line.indexOf(' ');
    String commandNameStr;
    String argsString = "";

    if (firstSpace == -1) {
        commandNameStr = line;
    } else {
        commandNameStr = line.substring(0, firstSpace);
        argsString = line.substring(firstSpace + 1);
        argsString.trim();
    }
    commandNameStr.toUpperCase();

    const int32_t pc = (int32_t)_program.code.size();
    MpInstr instr;
    instr.line = (uint16_t)(_lineNumber > 0xFFFF ? 0xFFFF : _lineNumber);

    // --- block ends and ELSE ---------------------------------------------
    if (commandNameStr == "ENDREPEAT") {
        if (_blocks.empty() || _blocks.back().type != CMD_REPEAT) {
            addError("Unexpected ENDREPEAT without matching REPEAT.");
            return false;
        }
        const Block b = _blocks.back();
        _blocks.pop_back();
        instr.type = CMD_ENDREPEAT;
        instr.x0 = b.pc;                 // back-edge
        _program.code.push_back(instr);
        _program.code[b.pc].x0 = pc + 1; // REPEAT skips to here when done
        return true;
    }
    if (commandNameStr == "ENDIF") {
        if (_blocks.empty() || _blocks.back().type != CMD_IF) {
            addError("Unexpected ENDIF without matching IF.");
            return false;
        }
        const Block b = _blocks.back();
        _blocks.pop_back();
        instr.type = CMD_ENDIF;
        _program.code.push_back(instr);
        if (b.elsePc >= 0) {
            _program.code[b.elsePc].x0 = pc + 1; // THEN branch jumps over ELSE part
        } else {
            _program.code[b.pc].x0 = pc + 1;     // false condition jumps past ENDIF
        }
        return true;
    }
    if (commandNameStr == "ELSE") {
        if (_blocks.empty() || _blocks.back().type != CMD_IF) {
            addError("Unexpected ELSE without matching IF.");
            return false;
        }
        Block& b = _blocks.back();
        if (b.elsePc >= 0) {
            addError("Multiple ELSE clauses for the same IF statement (started on line " + String(b.line) + ").");
            return false;
        }
        instr.type = CMD_ELSE;
        _program.code.push_back(instr);
        b.elsePc = pc;
        _program.code[b.pc].x0 = pc + 1;         // false condition lands after ELSE
        return true;
    }

    // --- regular commands and block starts ---------------------------------
    std::map<String, ParamValue> params;
    bool isBlockStart = false;

    if (commandNameStr == "DEFINE") {
        String defineArgs = argsString;
        defineArgs.trim();
        String upperDefineArgs = defineArgs;
        upperDefineArgs.toUpperCase();
        if (!upperDefineArgs.startsWith("PATTERN ")) {
            addError("DEFINE command must be followed by 'PATTERN'.");
            return false;
        }
        String patternArgs = defineArgs.substring(8);
        patternArgs.trim();
        return parseDefinePattern(patternArgs);
    } else if (commandNameStr == "VAR") {
        String varName;
        std::vector<ParamValue> tokens;
        if (!parseVar(argsString, varName, tokens)) return false;
        instr.type = CMD_VAR;
        instr.x0 = internSlot(varName);
        bool malformed = false;
        emitExpression(tokens, false, instr.x1, instr.x2, malformed);
    } else if (commandNameStr == "LET") {
        String targetVar;
        std::vector<ParamValue> tokens;
        if (!parseLet(argsString, targetVar, tokens)) return false;
        instr.type = CMD_LET;
        instr.x0 = internSlot(targetVar);
        bool malformed = false;
        emitExpression(tokens, false, instr.x1, instr.x2, malformed);
    } else if (commandNameStr == "REPEAT") {
        ParamValue countVal;
        if (!parseRepeat(argsString, countVal)) return false;
        instr.type = CMD_REPEAT;
        if (countVal.type == ParamValue::TYPE_INT) {
            instr.op[0].kind = MP_OPND_LIT;
            instr.op[0].v = countVal.intValue;
        } else {
            instr.op[0].kind = MP_OPND_VAR;
            instr.op[0].v = slotForVariableToken(countVal);
        }
        instr.x0 = pc + 1; // patched by ENDREPEAT
        isBlockStart = true;
    } else if (commandNameStr == "IF") {
        std::vector<ParamValue> conditionTokens;
        if (!parseIf(argsString, conditionTokens)) return false;
        instr.type = CMD_IF;
        bool malformed = false;
        emitExpression(conditionTokens, true, instr.x1, instr.x2, malformed);
        instr.aux = malformed ? 1 : 0;
        instr.x0 = pc + 1; // patched by ELSE / ENDIF
        isBlockStart = true;
    } else if (commandNameStr == "COLOR") {
        instr.type = CMD_COLOR;
    } else if (commandNameStr == "FILL") {
        instr.type = CMD_FILL;
    } else if (commandNameStr == "DRAW") {
        instr.type = CMD_DRAW;
    } else if (commandNameStr == "RESET_TRANSFORMS") {
        instr.type = CMD_RESET_TRANSFORMS;
    } else if (commandNameStr == "TRANSLATE") {
        instr.type = CMD_TRANSLATE;
    } else if (commandNameStr == "ROTATE") {
        instr.type = CMD_ROTATE;
    } else if (commandNameStr == "SCALE") {
        instr.type = CMD_SCALE;
    } else if (commandNameStr == "PIXEL") {
        instr.type = CMD_PIXEL;
    } else if (commandNameStr == "FILL_PIXEL") {
        instr.type = CMD_FILL_PIXEL;
    } else if (commandNameStr == "LINE") {
        instr.type = CMD_LINE;
    } else if (commandNameStr == "RECT") {
        instr.type = CMD_RECT;
    } else if (commandNameStr == "FILL_RECT") {
        instr.type = CMD_FILL_RECT;
    } else if (commandNameStr == "CIRCLE") {
        instr.type = CMD_CIRCLE;
    } else if (commandNameStr == "FILL_CIRCLE") {
        instr.type = CMD_FILL_CIRCLE;
    } else {
        addError("Unknown command: " + commandNameStr);
        return false;
    }

    // KEY=VALUE parameters for the commands that take them.
    switch (instr.type) {
        case CMD_COLOR: case CMD_FILL: case CMD_DRAW:
        case CMD_TRANSLATE: case CMD_ROTATE: case CMD_SCALE:
        case CMD_PIXEL: case CMD_FILL_PIXEL: case CMD_LINE:
        case CMD_RECT: case CMD_FILL_RECT: case CMD_CIRCLE: case CMD_FILL_CIRCLE:
            if (!parseParams(argsString, params)) return false;
            break;
        default: break;
    }

    // Positional operands / aux, per type.
    switch (instr.type) {
        case CMD_COLOR: {
            // Missing NAME, or a non-string NAME, meant BLACK in the old runtime
            // (with a logged error for the wrong type). "WHITE" in any case is white.
            instr.aux = 15;
            auto it = params.find("NAME");
            if (it != params.end()) {
                if (it->second.type == ParamValue::TYPE_STRING) {
                    if (it->second.stringValue.equalsIgnoreCase("WHITE")) instr.aux = 0;
                } else {
                    log_w("Line %d: Parameter NAME requires a string/keyword.", _lineNumber);
                }
            }
            break;
        }
        case CMD_FILL: case CMD_DRAW: {
            String upper;
            auto it = params.find("NAME");
            if (it != params.end() && it->second.type == ParamValue::TYPE_STRING) {
                upper = it->second.stringValue;
                upper.toUpperCase();
            } else if (it != params.end()) {
                log_w("Line %d: Parameter NAME requires SOLID or a pattern name string.", _lineNumber);
            }
            instr.aux = MP_ASSET_UNKNOWN;
            _pendingAssets.push_back(PendingAsset{pc, upper, instr.type == CMD_DRAW});
            if (instr.type == CMD_DRAW) {
                instr.op[0] = operandFromParam(params, "X", 0);
                instr.op[1] = operandFromParam(params, "Y", 0);
            }
            break;
        }
        case CMD_TRANSLATE:
            instr.op[0] = operandFromParam(params, "DX", 0);
            instr.op[1] = operandFromParam(params, "DY", 0);
            break;
        case CMD_ROTATE:
            instr.op[0] = operandFromParam(params, "DEGREES", 0);
            break;
        case CMD_SCALE:
            instr.op[0] = operandFromParam(params, "FACTOR", 1);
            break;
        case CMD_PIXEL: case CMD_FILL_PIXEL:
            instr.op[0] = operandFromParam(params, "X", 0);
            instr.op[1] = operandFromParam(params, "Y", 0);
            break;
        case CMD_LINE:
            instr.op[0] = operandFromParam(params, "X1", 0);
            instr.op[1] = operandFromParam(params, "Y1", 0);
            instr.op[2] = operandFromParam(params, "X2", 0);
            instr.op[3] = operandFromParam(params, "Y2", 0);
            break;
        case CMD_RECT: case CMD_FILL_RECT:
            instr.op[0] = operandFromParam(params, "X", 0);
            instr.op[1] = operandFromParam(params, "Y", 0);
            instr.op[2] = operandFromParam(params, "WIDTH", 0);
            instr.op[3] = operandFromParam(params, "HEIGHT", 0);
            break;
        case CMD_CIRCLE: case CMD_FILL_CIRCLE:
            instr.op[0] = operandFromParam(params, "X", 0);
            instr.op[1] = operandFromParam(params, "Y", 0);
            instr.op[2] = operandFromParam(params, "RADIUS", 0);
            break;
        default: break;
    }

    _program.code.push_back(instr);
    if (isBlockStart) {
        _blocks.push_back(Block{instr.type, pc, -1, _lineNumber});
    }
    return true;
}

MpOperand MicroPatternsParser::operandFromParam(const std::map<String, ParamValue>& params,
                                                const char* key, int defaultValue) {
    MpOperand o;
    o.kind = MP_OPND_LIT;
    o.v = defaultValue;
    auto it = params.find(String(key));
    if (it == params.end()) return o;
    const ParamValue& val = it->second;
    if (val.type == ParamValue::TYPE_INT) {
        o.v = val.intValue;
    } else if (val.type == ParamValue::TYPE_VARIABLE) {
        o.kind = MP_OPND_VAR;
        o.v = slotForVariableToken(val);
    } else {
        log_w("Line %d: Parameter %s requires an integer or variable.", _lineNumber, key);
    }
    return o;
}

void MicroPatternsParser::resolvePendingAssets() {
    for (const PendingAsset& p : _pendingAssets) {
        if (p.pc < 0 || p.pc >= (int32_t)_program.code.size()) continue;
        MpInstr& instr = _program.code[p.pc];
        instr.aux = MP_ASSET_UNKNOWN;
        if (p.upperName == "SOLID") {
            // FILL NAME=SOLID is the solid fill. DRAW NAME=SOLID was always an
            // error ("Invalid asset name") and drew nothing; UNKNOWN keeps that.
            if (!p.isDraw) instr.aux = MP_ASSET_SOLID;
            continue;
        }
        for (size_t i = 0; i < _program.assets.size(); ++i) {
            if (_program.assets[i].name == p.upperName) { instr.aux = (uint8_t)i; break; }
        }
    }
    _pendingAssets.clear();
}

// ---------------------------------------------------------------------------
// Expressions -> postfix
// ---------------------------------------------------------------------------
namespace {

bool operatorFromString(const String& s, uint8_t& op, int& precedence) {
    const char* c = s.c_str();
    if (!c) return false;
    if (c[1] == '\0') {
        switch (c[0]) {
            case '*': op = MP_OP_MUL; precedence = 3; return true;
            case '/': op = MP_OP_DIV; precedence = 3; return true;
            case '%': op = MP_OP_MOD; precedence = 3; return true;
            case '+': op = MP_OP_ADD; precedence = 2; return true;
            case '-': op = MP_OP_SUB; precedence = 2; return true;
            case '<': op = MP_OP_LT;  precedence = 1; return true;
            case '>': op = MP_OP_GT;  precedence = 1; return true;
            default: return false;
        }
    }
    if (c[2] == '\0' && c[1] == '=') {
        switch (c[0]) {
            case '=': op = MP_OP_EQ; precedence = 1; return true;
            case '!': op = MP_OP_NE; precedence = 1; return true;
            case '<': op = MP_OP_LE; precedence = 1; return true;
            case '>': op = MP_OP_GE; precedence = 1; return true;
            default: return false;
        }
    }
    return false;
}

bool isComparison(uint8_t op) { return op >= MP_OP_EQ && op <= MP_OP_GE; }

} // namespace

// Shunting-yard. All operators are binary and left-associative, with the same
// precedence the old two-pass evaluator implemented (* / % before + -, and a
// comparison last), so the postfix form computes exactly the same values in
// exactly the same order.
//
// A condition must contain exactly one comparison with something on both
// sides. The old parser only checked "at least three tokens" and left the rest
// to the runtime, which logged an error and took the condition as false; the
// same input is compiled here to an empty slice with `malformed` set, and the
// runtime reproduces that behaviour.
void MicroPatternsParser::emitExpression(const std::vector<ParamValue>& tokens, bool isCondition,
                                         int32_t& outBegin, int32_t& outLen, bool& malformed) {
    malformed = false;
    outBegin = (int32_t)_program.exprs.size();
    outLen = 0;
    if (tokens.empty()) return;

    if (isCondition) {
        int comparisons = 0;
        size_t cmpIndex = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i].type != ParamValue::TYPE_OPERATOR) continue;
            uint8_t op; int prec;
            if (operatorFromString(tokens[i].stringValue, op, prec) && isComparison(op)) {
                comparisons++;
                cmpIndex = i;
            }
        }
        if (comparisons != 1 || cmpIndex == 0 || cmpIndex + 1 >= tokens.size()) {
            malformed = true;
            return;
        }
        // Each side must be a well-formed infix expression (value, op, value,
        // ...). The old evaluator returned 0 for a side that was not -- e.g.
        // the "- 5" that "$x < -5" tokenizes to -- so such a side is compiled
        // to the literal 0.
        auto sideOk = [&](size_t from, size_t to) {
            bool expectValue = true;
            for (size_t i = from; i < to; ++i) {
                const bool isOp = tokens[i].type == ParamValue::TYPE_OPERATOR;
                if (isOp == expectValue) return false;
                expectValue = !expectValue;
            }
            return !expectValue; // must end on a value
        };
        const bool leftOk = sideOk(0, cmpIndex);
        const bool rightOk = sideOk(cmpIndex + 1, tokens.size());
        if (!leftOk || !rightOk) {
            std::vector<ParamValue> fixed;
            if (leftOk) fixed.insert(fixed.end(), tokens.begin(), tokens.begin() + cmpIndex);
            else fixed.push_back(ParamValue(0));
            fixed.push_back(tokens[cmpIndex]);
            if (rightOk) fixed.insert(fixed.end(), tokens.begin() + cmpIndex + 1, tokens.end());
            else fixed.push_back(ParamValue(0));
            log_w("Line %d: malformed operand in condition; that side evaluates to 0.", _lineNumber);
            bool m2 = false;
            emitExpression(fixed, false, outBegin, outLen, m2);
            return;
        }
    }

    struct OpEntry { uint8_t op; int prec; };
    std::vector<OpEntry> ops;
    int depth = 0, maxDepth = 0;

    auto emitOp = [&](const OpEntry& e) {
        MpOperand o;
        o.kind = MP_OPND_OP;
        o.op = e.op;
        _program.exprs.push_back(o);
        depth--; // two operands in, one result out
    };

    for (const ParamValue& t : tokens) {
        if (t.type == ParamValue::TYPE_INT) {
            MpOperand o; o.kind = MP_OPND_LIT; o.v = t.intValue;
            _program.exprs.push_back(o);
            if (++depth > maxDepth) maxDepth = depth;
        } else if (t.type == ParamValue::TYPE_VARIABLE) {
            MpOperand o; o.kind = MP_OPND_VAR; o.v = slotForVariableToken(t);
            _program.exprs.push_back(o);
            if (++depth > maxDepth) maxDepth = depth;
        } else if (t.type == ParamValue::TYPE_OPERATOR) {
            OpEntry e;
            if (!operatorFromString(t.stringValue, e.op, e.prec)) {
                // Cannot happen: the tokenizers only produce the operators above.
                addError("Internal parser error: unknown operator '" + t.stringValue + "'.");
                return;
            }
            while (!ops.empty() && ops.back().prec >= e.prec) {
                emitOp(ops.back());
                ops.pop_back();
            }
            ops.push_back(e);
        } else {
            addError("Internal parser error: unexpected token in expression.");
            return;
        }
    }
    while (!ops.empty()) { emitOp(ops.back()); ops.pop_back(); }

    outLen = (int32_t)_program.exprs.size() - outBegin;
    if (maxDepth > (int)_program.maxExprStack) _program.maxExprStack = (uint16_t)maxDepth;
}

// ---------------------------------------------------------------------------
// Line-level parsers (unchanged in behaviour from the tree-building parser)
// ---------------------------------------------------------------------------

// Parses REPEAT COUNT=value
bool MicroPatternsParser::parseRepeat(const String& argsString, ParamValue& outCount) {
    String trimmedArgs = argsString;
    trimmedArgs.trim();
    String upperArgs = trimmedArgs;
    upperArgs.toUpperCase();

    if (!upperArgs.startsWith("COUNT=")) {
        addError("REPEAT requires COUNT= parameter.");
        return false;
    }
    int equalsPos = trimmedArgs.indexOf('=');
    if (equalsPos == -1) {
        addError("Invalid format for REPEAT COUNT parameter. Missing '='.");
        return false;
    }
    String countValueStr = trimmedArgs.substring(equalsPos + 1);
    countValueStr.trim();
    if (countValueStr.length() == 0) {
        addError("Missing value for REPEAT COUNT.");
        return false;
    }
    outCount = parseValue(countValueStr);
    if (outCount.type != ParamValue::TYPE_INT && outCount.type != ParamValue::TYPE_VARIABLE) {
        addError("REPEAT COUNT value must be an integer or a variable ($var). Got: '" + countValueStr + "' which parsed as type " + String(outCount.type));
        return false;
    }
    if (outCount.type == ParamValue::TYPE_VARIABLE) {
        if (!validateVariableUsage(outCount.stringValue)) return false;
    }
    return true;
}

// Parses IF condition THEN
bool MicroPatternsParser::parseIf(const String& argsString, std::vector<ParamValue>& outConditionTokens) {
    String trimmedArgs = argsString;
    trimmedArgs.trim();
    String upperArgs = trimmedArgs;
    upperArgs.toUpperCase();

    int thenPos = upperArgs.lastIndexOf(" THEN");
    if (thenPos == -1 || thenPos != (int)upperArgs.length() - 5) {
        addError("IF requires ' THEN' at the end of the condition.");
        return false;
    }
    String conditionStr = trimmedArgs.substring(0, thenPos);
    conditionStr.trim();
    if (conditionStr.length() == 0) {
        addError("Missing condition for IF statement.");
        return false;
    }
    return parseCondition(conditionStr, outConditionTokens);
}

// Parses the arguments for DEFINE PATTERN NAME=... WIDTH=... HEIGHT=... DATA=...
bool MicroPatternsParser::parseDefinePattern(const String& argsString) {
    std::map<String, ParamValue> patternParams;
    if (!parseParams(argsString, patternParams)) return false;

    if (patternParams.find("NAME") == patternParams.end() || patternParams["NAME"].type != ParamValue::TYPE_STRING) {
        addError("DEFINE PATTERN requires NAME=\"...\" parameter.");
        return false;
    }
    if (patternParams.find("WIDTH") == patternParams.end() || patternParams["WIDTH"].type != ParamValue::TYPE_INT) {
        addError("DEFINE PATTERN requires WIDTH=... parameter.");
        return false;
    }
    if (patternParams.find("HEIGHT") == patternParams.end() || patternParams["HEIGHT"].type != ParamValue::TYPE_INT) {
        addError("DEFINE PATTERN requires HEIGHT=... parameter.");
        return false;
    }
    if (patternParams.find("DATA") == patternParams.end() || patternParams["DATA"].type != ParamValue::TYPE_STRING) {
        addError("DEFINE PATTERN requires DATA=\"...\" parameter.");
        return false;
    }

    MicroPatternsAsset asset;
    asset.originalName = patternParams["NAME"].stringValue;
    asset.name = asset.originalName;
    asset.name.toUpperCase();
    asset.width = patternParams["WIDTH"].intValue;
    asset.height = patternParams["HEIGHT"].intValue;
    String dataStr = patternParams["DATA"].stringValue;

    if (asset.width <= 0 || asset.height <= 0) {
        addError("Pattern WIDTH and HEIGHT must be positive.");
        return false;
    }
    if (asset.width > 20 || asset.height > 20) {
        log_w("Line %d: Pattern '%s' dimensions (%dx%d) exceed recommended maximum (20x20).", _lineNumber, asset.originalName.c_str(), asset.width, asset.height);
    }
    // The stored form keeps width/height in 16 bits; nothing sane is larger.
    if (asset.width > 0xFFFF || asset.height > 0xFFFF || (long)asset.width * asset.height > 65535L) {
        addError("Pattern '" + asset.originalName + "' is too large.");
        return false;
    }

    int expectedLen = asset.width * asset.height;
    if ((int)dataStr.length() != expectedLen) {
        String action = ((int)dataStr.length() < expectedLen) ? "padded with '0'" : "truncated";
        log_w("Line %d: DATA length (%d) for pattern '%s' does not match WIDTH*HEIGHT (%d). Data will be %s.",
              _lineNumber, (int)dataStr.length(), asset.originalName.c_str(), expectedLen, action.c_str());
        if ((int)dataStr.length() < expectedLen) {
            while ((int)dataStr.length() < expectedLen) dataStr += '0';
        } else {
            dataStr = dataStr.substring(0, expectedLen);
        }
    }

    asset.data.reserve(expectedLen);
    for (int i = 0; i < (int)dataStr.length(); ++i) {
        if (dataStr[i] == '0') {
            asset.data.push_back(0);
        } else if (dataStr[i] == '1') {
            asset.data.push_back(1);
        } else {
            addError("DATA string must contain only '0' or '1'. Found '" + String(dataStr[i]) + "' in pattern '" + asset.originalName + "'.");
            return false;
        }
    }

    for (const MicroPatternsAsset& existing : _program.assets) {
        if (existing.name == asset.name) {
            addError("Pattern '" + asset.originalName + "' (or equivalent case) already defined.");
            return false;
        }
    }
    if (_program.assets.size() >= 16) {
        addError("Maximum number of defined patterns (16) reached.");
        return false;
    }

    _program.assets.push_back(asset);
    return true;
}

// Parses VAR $name [= expression]
bool MicroPatternsParser::parseVar(const String& argsString, String& outVarName, std::vector<ParamValue>& outTokens) {
    outTokens.clear();
    String trimmedArgs = argsString;
    trimmedArgs.trim();

    if (!trimmedArgs.startsWith("$")) {
        addError("VAR requires a variable name starting with '$'.");
        return false;
    }

    int equalsPos = trimmedArgs.indexOf('=');
    int nameEndPos = trimmedArgs.length();
    String expressionPart = "";

    if (equalsPos != -1) {
        nameEndPos = equalsPos;
        expressionPart = trimmedArgs.substring(equalsPos + 1);
        expressionPart.trim();
        if (expressionPart.length() == 0) {
            addError("Missing expression after '=' in VAR declaration.");
            return false;
        }
    } else {
        int spacePos = trimmedArgs.indexOf(' ');
        if (spacePos != -1) {
            String extraContent = trimmedArgs.substring(spacePos);
            extraContent.trim();
            addError("Invalid VAR syntax. Use 'VAR $name' or 'VAR $name = expression'. Found extra content: '" + extraContent + "'");
            return false;
        }
    }

    String varRef = trimmedArgs.substring(0, nameEndPos);
    varRef.trim();
    if (varRef.length() <= 1) {
        addError("Invalid variable name '$' in VAR declaration.");
        return false;
    }
    outVarName = varRef.substring(1);
    outVarName.toUpperCase();

    if (outVarName.length() == 0) {
        addError("Invalid variable name in VAR declaration.");
        return false;
    }
    for (char c : outVarName) {
        if (!isalnum(c) && c != '_') {
            addError("Invalid character '" + String(c) + "' in variable name: " + varRef);
            return false;
        }
    }

    // "Already declared" is about VAR, not about a slot existing: a name first
    // seen in a parameter has a slot but no declaration yet.
    if (isEnvVar(outVarName)) {
        addError("Cannot declare variable with the same name as an environment variable: " + varRef);
        return false;
    }
    if (_declaredByVar.count(outVarName)) {
        addError("Variable '" + varRef + "' (or equivalent case) already declared.");
        return false;
    }
    _declaredByVar.insert(outVarName);
    internSlot(outVarName); // declared before the expression is parsed, as before

    if (expressionPart.length() > 0) {
        if (!parseExpression(expressionPart, outTokens)) return false;
    }
    return true;
}

// Parses LET $name = expression
bool MicroPatternsParser::parseLet(const String& argsString, String& outTargetVarName, std::vector<ParamValue>& outTokens) {
    outTokens.clear();
    String trimmedArgs = argsString;
    trimmedArgs.trim();

    int equalsPos = trimmedArgs.indexOf('=');
    if (equalsPos == -1) {
        addError("LET statement requires '=' for assignment.");
        return false;
    }
    String targetVarStr = trimmedArgs.substring(0, equalsPos);
    targetVarStr.trim();
    String expressionStr = trimmedArgs.substring(equalsPos + 1);
    expressionStr.trim();

    if (!targetVarStr.startsWith("$") || targetVarStr.length() <= 1) {
        addError("LET target variable must start with '$' followed by a name.");
        return false;
    }
    if (expressionStr.length() == 0) {
        addError("LET statement requires an expression after '='.");
        return false;
    }

    outTargetVarName = targetVarStr.substring(1);
    outTargetVarName.toUpperCase();

    if (!_declaredByVar.count(outTargetVarName)) {
        if (isEnvVar(outTargetVarName)) {
            addError("Cannot assign to environment variable: " + targetVarStr);
        } else {
            addError("Cannot assign to undeclared variable: " + targetVarStr);
        }
        return false;
    }

    if (!parseExpression(expressionStr, outTokens)) return false;
    if (outTokens.empty()) {
        addError("Internal parser error: LET expression parsed to empty token list.");
        return false;
    }
    return true;
}

// Parses "KEY=VALUE KEY2="VALUE 2" KEY3=$VAR" into the params map
bool MicroPatternsParser::parseParams(const String& argsString, std::map<String, ParamValue>& params) {
    String remainingArgs = argsString;
    remainingArgs.trim();
    const char* ptr = remainingArgs.c_str();
    if (!ptr) return true; // empty String: nothing to parse

    while (*ptr != '\0') {
        while (*ptr != '\0' && isspace(*ptr)) ptr++;
        if (*ptr == '\0') break;

        const char* keyStart = ptr;
        while (*ptr != '\0' && *ptr != '=' && !isspace(*ptr)) ptr++;
        String key(keyStart, ptr - keyStart);
        key.toUpperCase();

        if (key.length() == 0) {
            addError("Empty parameter name found near '" + String(keyStart) + "'.");
            return false;
        }

        while (*ptr != '\0' && isspace(*ptr)) ptr++;
        if (*ptr != '=') {
            addError("Missing '=' after parameter name '" + key + "'.");
            return false;
        }
        ptr++;
        while (*ptr != '\0' && isspace(*ptr)) ptr++;
        if (*ptr == '\0') {
            addError("Missing value for parameter '" + key + "'.");
            return false;
        }

        String valueString;
        ParamValue::ValueType valueType = ParamValue::TYPE_STRING;

        if (*ptr == '"') {
            ptr++;
            String tempVal = "";
            while (*ptr != '\0') {
                if (*ptr == '"') break;
                if (*ptr == '\\' && *(ptr + 1) != '\0') {
                    ptr++;
                    if (*ptr == '"' || *ptr == '\\') {
                        tempVal += *ptr;
                    } else {
                        tempVal += '\\';
                        tempVal += *ptr;
                    }
                } else {
                    tempVal += *ptr;
                }
                ptr++;
            }
            if (*ptr != '"') {
                addError("Unterminated string literal for parameter '" + key + "'.");
                return false;
            }
            valueString = tempVal;
            valueType = ParamValue::TYPE_STRING;
            ptr++;
        } else {
            const char* valueStart = ptr;
            while (*ptr != '\0' && !isspace(*ptr)) ptr++;
            valueString = String(valueStart, ptr - valueStart);
            if (valueString.length() == 0) {
                addError("Missing value for parameter '" + key + "'.");
                return false;
            }
            ParamValue parsedVal = parseValue(valueString);
            valueString = parsedVal.stringValue;
            valueType = parsedVal.type;
            if (valueType == ParamValue::TYPE_INT) {
                if (params.count(key)) {
                    addError("Duplicate parameter: " + key);
                    return false;
                }
                params[key] = ParamValue(parsedVal.intValue);
                continue;
            }
        }

        if (params.count(key)) {
            addError("Duplicate parameter: " + key);
            return false;
        }
        params[key] = ParamValue(valueString, valueType);

        while (*ptr != '\0' && isspace(*ptr)) ptr++;
    }
    return true;
}

// Parses a single unquoted value string: integer, variable, or keyword string.
ParamValue MicroPatternsParser::parseValue(const String& valueString) {
    if (valueString.startsWith("$")) {
        if (valueString.length() <= 1 || !isalpha(valueString[1])) {
            return ParamValue(valueString, ParamValue::TYPE_STRING);
        }
        return ParamValue(valueString, ParamValue::TYPE_VARIABLE);
    }

    int startIndex = 0;
    if (valueString.startsWith("-")) startIndex = 1;
    bool allDigits = true;
    if ((int)valueString.length() == startIndex) allDigits = false;
    for (int i = startIndex; i < (int)valueString.length(); ++i) {
        if (!isdigit(valueString[i])) { allDigits = false; break; }
    }

    if (allDigits) {
        char* endptr;
        long val = strtol(valueString.c_str(), &endptr, 10);
        if (*endptr == '\0') {
            if (val >= INT_MIN && val <= INT_MAX) {
                return ParamValue((int)val);
            } else {
                addError("Integer value out of range: " + valueString);
                return ParamValue(0);
            }
        }
    }
    return ParamValue(valueString, ParamValue::TYPE_STRING);
}

// Parses an expression string like "10 + $VAR * 2" into tokens.
bool MicroPatternsParser::parseExpression(const String& expressionString, std::vector<ParamValue>& tokens) {
    tokens.clear();
    String currentToken;
    enum Expectation { EXPECT_VALUE, EXPECT_OPERATOR } expected = EXPECT_VALUE;
    bool unaryMinusPossible = true;

    for (int i = 0; i < (int)expressionString.length(); ++i) {
        char c = expressionString[i];
        if (isspace(c)) continue;

        if ((c == '$') || isdigit(c) || (c == '-' && unaryMinusPossible)) {
            if (expected != EXPECT_VALUE) {
                addError("Syntax error in expression: Unexpected value '" + String(c) + "...'. Expected operator.");
                return false;
            }
            currentToken = "";
            if (c == '-') {
                currentToken += c;
                i++;
                if (i >= (int)expressionString.length()) {
                    addError("Syntax error in expression: Incomplete expression after unary '-'.");
                    return false;
                }
                if (!isdigit(expressionString[i]) && expressionString[i] != '$') {
                    addError("Syntax error in expression: Invalid character after unary '-'. Expected digit or '$'.");
                    return false;
                }
                c = expressionString[i];
            }

            if (c == '$') {
                currentToken += c;
                i++;
                if (i >= (int)expressionString.length() || !isalpha(expressionString[i])) {
                    addError("Syntax error in expression: Expected letter after '$'.");
                    return false;
                }
                currentToken += expressionString[i];
                i++;
                while (i < (int)expressionString.length() && (isalnum(expressionString[i]) || expressionString[i] == '_')) {
                    currentToken += expressionString[i];
                    i++;
                }
                i--;
                ParamValue val = parseValue(currentToken);
                if (val.type != ParamValue::TYPE_VARIABLE) {
                    addError("Internal parser error: Expected variable token for '" + currentToken + "'.");
                    return false;
                }
                if (!validateVariableUsage(val.stringValue)) return false;
                tokens.push_back(val);
            } else if (isdigit(c)) {
                currentToken += c;
                i++;
                while (i < (int)expressionString.length() && isdigit(expressionString[i])) {
                    currentToken += expressionString[i];
                    i++;
                }
                i--;
                ParamValue val = parseValue(currentToken);
                if (val.type != ParamValue::TYPE_INT) {
                    addError("Internal parser error: Expected integer token for '" + currentToken + "'.");
                    return false;
                }
                tokens.push_back(val);
            } else {
                addError("Internal parser error: Unexpected state parsing value.");
                return false;
            }
            expected = EXPECT_OPERATOR;
            unaryMinusPossible = false;
        } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            if (expected != EXPECT_OPERATOR) {
                addError("Syntax error in expression: Unexpected operator '" + String(c) + "'. Expected value.");
                return false;
            }
            tokens.push_back(ParamValue(String(c), ParamValue::TYPE_OPERATOR));
            expected = EXPECT_VALUE;
            unaryMinusPossible = true;
        } else {
            addError("Invalid character '" + String(c) + "' in expression.");
            return false;
        }
    }

    if (tokens.empty()) {
        String tempExpression = expressionString;
        tempExpression.trim();
        if (tempExpression.length() > 0) {
            addError("Empty expression parsed from non-empty input.");
            return false;
        }
        return true;
    }
    if (expected == EXPECT_VALUE) {
        addError("Syntax error: Expression cannot end with an operator.");
        return false;
    }
    return true;
}

// Parses a condition string (e.g., "$COUNTER > 10", "$X % 2 == 0") into tokens.
bool MicroPatternsParser::parseCondition(const String& conditionString, std::vector<ParamValue>& tokens) {
    tokens.clear();
    String currentToken;
    enum State { NONE, NUMBER, VARIABLE } state = NONE;

    for (int i = 0; i < (int)conditionString.length(); ++i) {
        char c = conditionString[i];
        char next_c = (i + 1 < (int)conditionString.length()) ? conditionString[i + 1] : '\0';

        if (isspace(c)) {
            if (currentToken.length() > 0) {
                ParamValue val = parseValue(currentToken);
                if (val.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(val.stringValue)) return false;
                tokens.push_back(val);
                currentToken = "";
                state = NONE;
            }
            continue;
        }

        String op_found = "";
        if (next_c != '\0') {
            String two_char_op = String(c) + String(next_c);
            if (two_char_op == "==" || two_char_op == "!=" || two_char_op == "<=" || two_char_op == ">=") {
                op_found = two_char_op;
            }
        }
        if (op_found.length() == 0) {
            if (c == '<' || c == '>') {
                op_found = String(c);
            } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
                // NOTE: '-' is always an operator here, never a sign. That is
                // what the original tokenizer did (its unary-minus branch below
                // was unreachable), and the runtime behaviour it produced --
                // "$x < -5" compares against 0 -- is preserved by emitExpression.
                op_found = String(c);
            } else if (c == '=') {
                addError("Invalid operator '=' in condition. Use '==' for comparison.");
                return false;
            } else if (c == '!') {
                addError("Invalid operator '!' in condition. Use '!=' for not equals.");
                return false;
            }
        }

        if (op_found.length() > 0) {
            if (currentToken.length() > 0) {
                ParamValue val = parseValue(currentToken);
                if (val.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(val.stringValue)) return false;
                tokens.push_back(val);
                currentToken = "";
            }
            tokens.push_back(ParamValue(op_found, ParamValue::TYPE_OPERATOR));
            i += (op_found.length() - 1);
            state = NONE;
        } else {
            if (c == '$') {
                if (currentToken.length() > 0 && state != NONE) {
                    addError("Syntax error: Unexpected '$' in token '" + currentToken + "'.");
                    return false;
                }
                state = VARIABLE;
                currentToken += c;
            } else if (isdigit(c)) {
                if (state == NONE || state == NUMBER) {
                    state = NUMBER;
                    currentToken += c;
                } else if (state == VARIABLE) {
                    currentToken += c;
                } else {
                    addError("Syntax error: Unexpected digit '" + String(c) + "' in token '" + currentToken + "'.");
                    return false;
                }
            } else if (c == '-') {
                if (state == NONE && (tokens.empty() || tokens.back().type == ParamValue::TYPE_OPERATOR)) {
                    state = NUMBER;
                    currentToken += c;
                } else {
                    addError("Syntax error: Unexpected '-' in token '" + currentToken + "'. Use as unary operator or for subtraction.");
                    return false;
                }
            } else if (isalpha(c) || c == '_') {
                if (state == NONE && currentToken.length() == 0) {
                    addError("Invalid character '" + String(c) + "' in condition. Values must be numbers or variables ($var).");
                    return false;
                } else if (state == VARIABLE) {
                    currentToken += c;
                } else {
                    addError("Invalid character '" + String(c) + "' in condition token '" + currentToken + "'.");
                    return false;
                }
            } else {
                addError("Invalid character '" + String(c) + "' in condition.");
                return false;
            }
        }
    }

    if (currentToken.length() > 0) {
        ParamValue val = parseValue(currentToken);
        if (val.type == ParamValue::TYPE_VARIABLE && !validateVariableUsage(val.stringValue)) return false;
        tokens.push_back(val);
    }

    if (tokens.empty()) {
        addError("Empty condition.");
        return false;
    }
    if (tokens.size() < 3) {
        addError("Invalid condition structure. Expected 'value operator value' or '$var % literal op value'.");
        return false;
    }
    return true;
}
