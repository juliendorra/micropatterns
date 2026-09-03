#ifndef MP_REFRESH_BUDGET_H
#define MP_REFRESH_BUDGET_H

#include <stdint.h>

// When to spend a full, flashing de-ghost refresh.
//
// E-paper fast updates leave residue, so every so often the panel has to be
// driven black and white to clear it. That refresh takes seconds, and WHERE it
// lands decides how the device feels: on a frame the user is already waiting
// for it is invisible, and on a transient one it reads as a fault. On the
// Watchy it could land on a browse title, so roughly one press in six answered
// with a long flash instead of a script name -- the whole "unpredictable" feel
// of paging, and not something any amount of looking at the panel would name.
//
// Kept apart from the panel drivers, which share no types, so both firmwares
// can run the same accounting and the harness can test it.
class MpRefreshBudget {
public:
    explicit MpRefreshBudget(int interval) : _interval(interval < 1 ? 1 : interval),
                                             _since(interval < 1 ? 1 : interval) {}

    // Starts at the interval so the first update of a session is a full one:
    // partial updates onto a panel of unknown contents leave it grey.
    void setInterval(int n) { _interval = n < 1 ? 1 : n; }
    int  interval() const { return _interval; }
    int  since() const { return _since; }

    // Call once per update that actually drives the panel. Returns true if this
    // one should be the full refresh.
    //
    // allowDeghost == false means "not on this frame". The debt is deferred,
    // never forgiven: the counter keeps climbing and the next frame that allows
    // it pays. Pass false for anything transient -- titles, banners, notices.
    bool beginUpdate(bool forceFull, bool allowDeghost)
    {
        const bool full = forceFull || (allowDeghost && _since >= _interval);
        if (full) _since = 0; else _since++;
        return full;
    }

    // An update too small to be worth charging for, such as a corner press
    // indicator. Its residue is local and cannot outlive the next full-screen
    // frame; charging it two units a press for 1.7% of the panel pulled the
    // flash in to every fifth or sixth press on the Watchy.
    void noteUncharged() {}

    // A frame that beginUpdate() was called for but which was never pushed --
    // the Watchy abandons a render mid-page loop when a button is pressed. It
    // did not ghost anything, so it must not be charged.
    void undoUncommitted() { if (_since > 0) _since--; }

    // The panel was cleared by other means (an explicit sweep, a fresh boot).
    void reset() { _since = 0; }
    // Make the next allowed update a full one.
    void forceFullNext() { _since = _interval; }

private:
    int _interval;
    int _since;
};

#endif // MP_REFRESH_BUDGET_H
