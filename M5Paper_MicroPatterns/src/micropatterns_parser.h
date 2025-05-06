#ifndef MICROPATTERNS_PARSER_H
#define MICROPATTERNS_PARSER_H

#include <Arduino.h>
#include <vector>
#include <list> // Added for std::list
#include <map>
#include <set> // For declared variables
#include "micropatterns_command.h"

class MicroPatternsParser {
public:
    MicroPatternsParser();

    // Parses the script and returns true if successful (no errors).
    // Commands, assets, errors, and declared variables are stored internally.
    bool parse(const String& scriptText);

    // Getters
    const std::list<MicroPatternsCommand>& getCommands() const; // Changed to std::list
    const std::map<String, MicroPatternsAsset>& getAssets() const;
    const std::vector<String>& getErrors() const;
    const std::set<String>& getDeclaredVariables() const; // Returns set of declared var names (UPPERCASE, no '$')
    void reset(); // Moved to public

private:
    std::list<MicroPatternsCommand> _commands; // Changed to std::list
    std::map<String, MicroPatternsAsset> _assets; // Key is UPPERCASE name
    std::vector<String> _errors;
    std::set<String> _declaredVariables; // Store declared var names (UPPERCASE, no '$')
    int _lineNumber;
    std::vector<MicroPatternsCommand*> _commandStack; // Stack to manage nested blocks (REPEAT, IF)

    // void reset(); // Moved to public
    void addError(const String& message);
    bool processLine(const String& line);
    bool parseDefinePattern(const String& argsString);
    bool parseVar(const String& argsString, String& outVarName, std::vector<ParamValue>& outTokens);
    bool parseLet(const String& argsString, String& outTargetVarName, std::vector<ParamValue>& outTokens);
    bool parseRepeat(const String& argsString, ParamValue& outCount);
    bool parseIf(const String& argsString, std::vector<ParamValue>& outConditionTokens);
    bool parseParams(const String& argsString, std::map<String, ParamValue>& params);
    ParamValue parseValue(const String& valueString);
    // Parses an expression string into a vector of tokens (numbers, variables, operators)
    bool parseExpression(const String& expressionString, std::vector<ParamValue>& tokens);
    // Parses a condition string into a vector of tokens (similar to expression, but used by IF)
    bool parseCondition(const String& conditionString, std::vector<ParamValue>& tokens);
    // Helper to check if a variable name (UPPERCASE, no '$') is an environment variable
    bool isEnvVar(const String& upperCaseName) const;
    // Helper to validate variable usage in expressions/conditions
    bool validateVariableUsage(const String& varRefWithDollar);
};

#endif // MICROPATTERNS_PARSER_H