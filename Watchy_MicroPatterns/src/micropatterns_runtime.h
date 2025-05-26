#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <vector>
#include <list> 
#include <map>
#include <set> 
#include <functional> 
#include <esp_task_wdt.h> 
#include "micropatterns_command.h" 
#include "IDrawingHAL.h" // Added IDrawingHAL include

class MicroPatternsRuntime {
public:
    // Constructor now takes IDrawingHAL to get screen dimensions
    MicroPatternsRuntime(IDrawingHAL* hal, const std::map<String, MicroPatternsAsset>& assets);

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
    
    IDrawingHAL* _hal; // Store pointer to the HAL
    int _canvasWidth;  // Still useful for $WIDTH, $HEIGHT internal script vars
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
