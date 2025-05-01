#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <M5EPD.h>
#include <vector>
#include <map>
#include "micropatterns_command.h"
#include "micropatterns_drawing.h"

class MicroPatternsRuntime {
public:
    MicroPatternsRuntime(M5EPD_Canvas* canvas, const std::map<String, MicroPatternsAsset>& assets);

    void setCommands(const std::vector<MicroPatternsCommand>* commands);
    void setDeclaredVariables(const std::vector<String>* declaredVariables);

    // Executes the script commands
    void execute();

    // Update environment variables (e.g., counter)
    void setCounter(int counter);

    // Error reporting
    void runtimeError(const String& message, int lineNumber);

private:
    M5EPD_Canvas* _canvas;
    MicroPatternsDrawing _drawing;
    const std::map<String, MicroPatternsAsset>& _assets; // Reference to assets from parser
    const std::vector<MicroPatternsCommand>* _commands = nullptr; // Pointer to commands from parser
    const std::vector<String>* _declaredVariables = nullptr; // Pointer to declared vars from parser

    MicroPatternsState _currentState;
    std::map<String, int> _variables; // Stores runtime variable values (Key = UPPERCASE name, no '$')
    std::map<String, int> _environment; // Stores env var values ($COUNTER, $WIDTH, $HEIGHT)

    void resetState();
    void executeCommand(const MicroPatternsCommand& cmd);

    // Helper to resolve parameter values (literals or variables)
    // Returns defaultValue if parameter not found or logs error if invalid type.
    // Requires command line number for error reporting.
    int resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber);
    // Returns defaultValue if parameter not found or logs error if invalid type
    String resolveStringParam(const String& paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber);
    // Handles SOLID or pattern name, returns "SOLID" if parameter not found
    String resolveAssetNameParam(const String& paramName, const std::map<String, ParamValue>& params, int lineNumber);

    // Variable/Expression evaluation
    // Resolves a single value (int literal or variable reference)
    int resolveValue(const ParamValue& val, int lineNumber);
    // Evaluates a sequence of tokens (numbers, variables, operators)
    int evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber);
};

#endif // MICROPATTERNS_RUNTIME_H