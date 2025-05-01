#ifndef MICROPATTERNS_PARSER_H
#define MICROPATTERNS_PARSER_H

#include <Arduino.h>
#include <vector>
#include <map>
#include "micropatterns_command.h"

class MicroPatternsParser {
public:
    MicroPatternsParser();

    // Parses the script and returns a list of commands.
    // Assets are stored internally. Errors are stored internally.
    bool parse(const String& scriptText);

    // Getters
    const std::vector<MicroPatternsCommand>& getCommands() const;
    const std::map<String, MicroPatternsAsset>& getAssets() const;
    const std::vector<String>& getErrors() const;
    const std::vector<String>& getDeclaredVariables() const; // Returns list of declared var names (UPPERCASE, no '$')

private:
    std::vector<MicroPatternsCommand> _commands;
    std::map<String, MicroPatternsAsset> _assets; // Key is UPPERCASE name
    std::vector<String> _errors;
    std::vector<String> _declaredVariables; // Store declared var names (UPPERCASE, no '$')
    int _lineNumber;
    std::vector<MicroPatternsCommand*> _commandStack; // Stack to manage nested blocks (REPEAT, IF)

    void reset();
    void addError(const String& message);
    bool processLine(const String& line);
    bool parseDefinePattern(const String& argsString);
    // Updated signatures to use output parameters
    bool parseVar(const String& argsString, String& outVarName, std::vector<ParamValue>& outTokens);
    bool parseLet(const String& argsString, String& outTargetVarName, std::vector<ParamValue>& outTokens);
    bool parseRepeat(const String& argsString, ParamValue& outCount);
    bool parseParams(const String& argsString, std::map<String, ParamValue>& params);
    ParamValue parseValue(const String& valueString);
    // Parses an expression string into a vector of tokens (numbers, variables, operators)
    bool parseExpression(const String& expressionString, std::vector<ParamValue>& tokens);
};

#endif // MICROPATTERNS_PARSER_H