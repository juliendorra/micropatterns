#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <M5EPD.h>
#include <vector>
#include <list> // Added for std::list
#include <map>
#include <set> // Include set for declared variables check
#include <functional> // For std::function
#include <esp_task_wdt.h> // For watchdog resets
#include "micropatterns_command.h"
#include "micropatterns_drawing.h"

class MicroPatternsRuntime {
public:
    MicroPatternsRuntime(M5EPD_Canvas* canvas, const std::map<String, MicroPatternsAsset>& assets);

    void setCommands(const std::list<MicroPatternsCommand>* commands); // Changed to std::list
    // Pass declared variables from parser for initialization and checks
    void setDeclaredVariables(const std::set<String>* declaredVariables);

    // Executes the script commands
    void execute();

    // Update environment variables (e.g., counter, time)
    void setCounter(int counter);
    void setTime(int hour, int minute, int second); // Add method to set time

    // Getters for state (used by RenderController)
    int getCounter() const;
    void getTime(int& hour, int& minute, int& second) const;

    // Error reporting
    void runtimeError(const String& message, int lineNumber);

    // Interrupt handling
    void requestInterrupt() { _interrupt_requested = true; }
    bool isInterrupted() const { return _interrupt_requested; }
    void clearInterrupt() { _interrupt_requested = false; } // Clear before new execution
    
    // Set callback for interrupt checking (passed to MicroPatternsDrawing)
    void setInterruptCheckCallback(std::function<bool()> cb) { _interrupt_check_cb = cb; }

private:
    volatile bool _interrupt_requested; // Flag to signal interruption
    std::function<bool()> _interrupt_check_cb; // Callback for drawing to check interrupt

    M5EPD_Canvas* _canvas;
    MicroPatternsDrawing _drawing;
    const std::map<String, MicroPatternsAsset>& _assets; // Reference to assets from parser
    const std::list<MicroPatternsCommand>* _commands = nullptr; // Changed to std::list
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
    int resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber, int loopIndex);
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