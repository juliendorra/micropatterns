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

    void reset();
    void addError(const String& message);
    bool processLine(const String& line);
    bool parseDefinePattern(const String& argsString);
    bool parseVar(const String& argsString);
    bool parseLet(const String& argsString);
    bool parseParams(const String& argsString, std::map<String, ParamValue>& params);
    ParamValue parseValue(const String& valueString);
};

#endif // MICROPATTERNS_PARSER_H