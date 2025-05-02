#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <M5EPD.h>
#include <vector>
#include <map>
#include <set> // Include set for declared variables check
#include "micropatterns_command.h"
#include "micropatterns_drawing.h"

class MicroPatternsRuntime {
public:
    MicroPatternsRuntime(M5EPD_Canvas* canvas, const std::map<String, MicroPatternsAsset>& assets);

    void setCommands(const std::vector<MicroPatternsCommand>* commands);
    // Pass declared variables from parser for initialization and checks
    void setDeclaredVariables(const std::set<String>* declaredVariables);

    // Executes the script commands
    void execute();

    // Update environment variables (e.g., counter, time)
    void setCounter(int counter);
    void setTime(int hour, int minute, int second); // Add method to set time

    // Error reporting
    void runtimeError(const String& message, int lineNumber);

private:
    M5EPD_Canvas* _canvas;
    MicroPatternsDrawing _drawing;
    const std::map<String, MicroPatternsAsset>& _assets; // Reference to assets from parser
    const std::vector<MicroPatternsCommand>* _commands = nullptr; // Pointer to commands from parser
    const std::set<String>* _declaredVariables = nullptr; // Pointer to declared vars from parser

    MicroPatternsState _currentState;
    // Store user variables with '$' prefix as key for easier lookup consistency
    std::map<String, int> _variables; // Stores runtime variable values (Key = $UPPERVAR)
    std::map<String, int> _environment; // Stores env var values ($COUNTER, $WIDTH, $HEIGHT, $HOUR, $MINUTE, $SECOND, $INDEX)

    void resetState();
    // Execute a single command, potentially recursively for blocks
    // loopIndex is passed down for nested execution within REPEAT
    void executeCommand(const MicroPatternsCommand& cmd, int loopIndex = -1);

    // Helper to resolve parameter values (literals or variables)
    int resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber);
    String resolveStringParam(const String& paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber);
    String resolveAssetNameParam(const String& paramName, const std::map<String, ParamValue>& params, int lineNumber);

    // Variable/Expression/Condition evaluation
    // Resolves a single value (int literal or variable reference)
    // Expects val.type == TYPE_INT or TYPE_VARIABLE (stringValue like "$VAR")
    int resolveValue(const ParamValue& val, int lineNumber, int loopIndex);
    // Evaluates a sequence of expression tokens
    int evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex);
    // Evaluates a sequence of condition tokens
    bool evaluateCondition(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex);
};

#endif // MICROPATTERNS_RUNTIME_H