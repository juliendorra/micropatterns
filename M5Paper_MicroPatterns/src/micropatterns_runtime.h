#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <M5EPD.h>
#include <vector>
#include <list> // Added for std::list
#include <map>
#include <set> // Include set for declared variables check
#include <functional> // For std::function
#include <esp_task_wdt.h> // For watchdog resets
#include "micropatterns_command.h" // For MicroPatternsCommand, DisplayListItem, MicroPatternsAsset, MicroPatternsState
// MicroPatternsDrawing is no longer directly used by runtime

class MicroPatternsRuntime {
public:
    MicroPatternsRuntime(int canvasWidth, int canvasHeight, const std::map<String, MicroPatternsAsset>& assets);

    void setCommands(const std::list<MicroPatternsCommand>* commands);
    void setDeclaredVariables(const std::set<String>* declaredVariables);

    // Generates the display list from the script commands
    void generateDisplayList();
    const std::vector<DisplayListItem>& getDisplayList() const;

    void setCounter(int counter);
    void setTime(int hour, int minute, int second);

    int getCounter() const;
    void getTime(int& hour, int& minute, int& second) const;

    void runtimeError(const String& message, int lineNumber);

    void requestInterrupt() { _interrupt_requested = true; }
    bool isInterrupted() const { return _interrupt_requested; }
    void clearInterrupt() { _interrupt_requested = false; }
    
    void setInterruptCheckCallback(std::function<bool()> cb) { _interrupt_check_cb = cb; }

private:
    volatile bool _interrupt_requested;
    std::function<bool()> _interrupt_check_cb;

    const std::map<String, MicroPatternsAsset>& _assets;
    const std::list<MicroPatternsCommand>* _commands = nullptr;
    const std::set<String>* _declaredVariables = nullptr;

    std::vector<DisplayListItem> _displayList;
    MicroPatternsState _currentState; // Used to track state during display list generation
    std::map<String, int> _variables;
    std::map<String, int> _environment;
    
    int _canvasWidth;
    int _canvasHeight;

    void resetStateAndList();
    // Processes a command and adds to _displayList, potentially recursively for blocks
    void processCommandForDisplayList(const MicroPatternsCommand& cmd, int loopIndex = -1);

    int resolveIntParam(const String& paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber, int loopIndex);
    String resolveStringParam(const String& paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber);
    String resolveAssetNameParam(const String& paramName, const std::map<String, ParamValue>& params, int lineNumber);

    int resolveValue(const ParamValue& val, int lineNumber, int loopIndex);
    int evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex);
    bool evaluateCondition(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex);

    bool isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const;
    bool determineItemOpacity(CommandType type, const std::map<String, ParamValue>& params, const String& assetNameParamValue) const;
};

#endif // MICROPATTERNS_RUNTIME_H