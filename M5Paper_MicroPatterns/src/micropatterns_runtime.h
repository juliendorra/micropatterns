#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <vector>
#include <list> // Added for std::list
#include <deque> // Transform snapshot pool (stable addresses across growth)
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

    // Pool of transform snapshots referenced by _displayList items. A deque,
    // not a vector, because items hold raw pointers into it and deque never
    // moves existing elements when it grows. Lives as long as the runtime, so
    // the display list must not outlive its runtime (it never does: every
    // caller renders it before the runtime goes out of scope).
    std::deque<TransformSnapshot> _xfPool;
    bool _xfDirty = true; // current state not yet represented in the pool

    MicroPatternsState _currentState; // Used to track state during display list generation

    // --- Variable storage: integer slots, not String-keyed maps -------------
    // Env vars occupy the fixed low slots; user variables are appended. The
    // parser forbids declaring a variable whose name shadows an env var
    // (micropatterns_parser.cpp isEnvVar / parseVarCommand), so one flat slot
    // space cannot collide and no priority rule is needed at lookup time.
    enum EnvSlot {
        ENV_WIDTH = 0, ENV_HEIGHT, ENV_HOUR, ENV_MINUTE, ENV_SECOND, ENV_COUNTER, ENV_INDEX,
        ENV_SLOT_COUNT
    };
    int _envValues[ENV_SLOT_COUNT];
    std::map<String, int> _slotByName; // name (with '$') -> slot; built once per script, cold path
    std::vector<int> _varValues;       // user-variable values, index = slot - ENV_SLOT_COUNT
    std::vector<uint8_t> _varDefined;  // has the VAR command for this slot executed yet?
    uint32_t _epoch;                   // tags slot memos in ParamValue/MicroPatternsCommand

    int _canvasWidth;
    int _canvasHeight;

    void resetStateAndList();
    // Processes a command and adds to _displayList, potentially recursively for blocks
    void processCommandForDisplayList(const MicroPatternsCommand& cmd, int loopIndex = -1);

    // Interns `nameWithDollar` (already uppercase) to a slot index, creating a
    // user slot if needed. Cold path -- callers memoise the result.
    int internSlot(const String& nameWithDollar);
    int internSlotForToken(const ParamValue& val); // cold path of slotForVariableToken
    // Hot path: one comparison and one load once the memo is warm. Inline
    // because it is called for every variable token of every expression of
    // every loop iteration.
    int slotForVariableToken(const ParamValue& val) {
        if (val.slotEpoch == _epoch) return val.slotCache;
        return internSlotForToken(val);
    }
    int slotForCommandTarget(const MicroPatternsCommand& cmd, const String& bareUpperName);

    // Parameter lookup by literal name. Takes const char* and compares against
    // the map keys directly: params maps hold at most a handful of entries, so
    // a linear strcmp scan beats a tree walk and, unlike the old code, does not
    // heap-allocate an Arduino String per parameter per command execution.
    // Resolves (and memoises) the NAME parameter of a FILL / DRAW command.
    // Returns the assetKind codes documented on MicroPatternsCommand.
    uint8_t resolveAssetForCommand(const MicroPatternsCommand& cmd, const MicroPatternsAsset*& outAsset);

    static const ParamValue* findParam(const std::map<String, ParamValue>& params, const char* name);
    int resolveIntParam(const char* paramName, const std::map<String, ParamValue>& params, int defaultValue, int lineNumber, int loopIndex);
    String resolveStringParam(const char* paramName, const std::map<String, ParamValue>& params, const String& defaultValue, int lineNumber);
    String resolveAssetNameParam(const char* paramName, const std::map<String, ParamValue>& params, int lineNumber);

    // Appends the current transform state to the pool if it changed since the
    // last snapshot, and returns the snapshot every item emitted now shares.
    const TransformSnapshot* currentTransform();

    int resolveValue(const ParamValue& val, int lineNumber, int loopIndex);
    int evaluateExpression(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex);
    // Evaluates tokens[begin, end). evaluateCondition uses it to avoid copying
    // the two operand sub-expressions into fresh vectors on every comparison.
    int evaluateExpressionRange(const std::vector<ParamValue>& tokens, size_t begin, size_t end,
                                int lineNumber, int loopIndex);
    bool evaluateCondition(const std::vector<ParamValue>& tokens, int lineNumber, int loopIndex);

    // Scratch buffers for expression evaluation, reused across calls so a
    // REPEAT body does not allocate two vectors per expression per iteration.
    // Expression evaluation never nests (no recursion, and evaluateCondition
    // evaluates its two operands one after the other), so a single pair is safe.
    struct EvalTok {
        int value;              // valid when op == nullptr
        const ParamValue* op;   // non-null => operator token
    };
    std::vector<EvalTok> _evalScratchA;
    std::vector<EvalTok> _evalScratchB;

    bool isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const;
};

#endif // MICROPATTERNS_RUNTIME_H