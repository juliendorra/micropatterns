#ifndef MICROPATTERNS_RUNTIME_H
#define MICROPATTERNS_RUNTIME_H

#include <vector>
#include <deque> // Transform snapshot pool (stable addresses across growth)
#include <functional> // For std::function
#include <esp_task_wdt.h> // For watchdog resets
#include "micropatterns_command.h"
#include "mp_program.h"

// Executes a compiled MpProgram and produces the display list.
//
// This is a small virtual machine: a program counter over MpProgram::code, a
// loop stack for REPEAT, a value stack for postfix expressions, and a flat
// array of variable values indexed by slot. Nothing is looked up by name and
// nothing is allocated per instruction; the only allocations are the display
// list vector and the transform-snapshot pool, both of which grow only when
// the picture does. See mp_program.h for what replaced the tree walk.
class MicroPatternsRuntime {
public:
    // The program must outlive the runtime AND the display list it produces:
    // DRAW / FILL items point at assets owned by the program.
    MicroPatternsRuntime(int canvasWidth, int canvasHeight, const MpProgram& program);

    // Runs the program from the top and fills the display list.
    void generateDisplayList();
    const std::vector<DisplayListItem>& getDisplayList() const { return _displayList; }
    const MpProgram& program() const { return _program; }

    void setCounter(int counter);
    void setTime(int hour, int minute, int second);
    int getCounter() const;
    void getTime(int& hour, int& minute, int& second) const;

    void runtimeError(const String& message, int lineNumber);

    void requestInterrupt() { _interrupt_requested = true; }
    bool isInterrupted() const { return _interrupt_requested; }
    void clearInterrupt() { _interrupt_requested = false; }
    void setInterruptCheckCallback(std::function<bool()> cb) { _interrupt_check_cb = cb; }

    // Instructions executed by the last generateDisplayList(), for logs.
    uint32_t instructionsExecuted() const { return _executed; }

private:
    const MpProgram& _program;
    volatile bool _interrupt_requested;
    std::function<bool()> _interrupt_check_cb;

    std::vector<DisplayListItem> _displayList;

    // Pool of transform snapshots referenced by _displayList items. A deque,
    // not a vector, because items hold raw pointers into it and deque never
    // moves existing elements when it grows.
    std::deque<TransformSnapshot> _xfPool;
    bool _xfDirty = true;

    MicroPatternsState _currentState;

    // Variable values by slot: the MP_ENV_SLOT_COUNT environment values first,
    // then one per user variable. `_defined` shadows the user part: a VAR that
    // has not executed yet reads as an "Undefined variable" error and 0, as it
    // always did.
    std::vector<int32_t> _vals;
    std::vector<uint8_t> _defined;

    struct Loop { int32_t bodyPc; int32_t count; int32_t i; };
    std::vector<Loop> _loops;
    std::vector<int32_t> _stack; // expression value stack, sized from the program

    int _canvasWidth;
    int _canvasHeight;
    uint32_t _executed = 0;

    void resetStateAndList();
    const TransformSnapshot* currentTransform();

    int32_t resolve(const MpOperand& o, int line);
    // Evaluates exprs[begin, begin+len) in postfix. An empty slice is 0.
    int32_t eval(int32_t begin, int32_t len, int line);
    bool evalCondition(const MpInstr& in);

    bool isAssetDataFullyOpaque(const MicroPatternsAsset* asset) const;
};

#endif // MICROPATTERNS_RUNTIME_H
