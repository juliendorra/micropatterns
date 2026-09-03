#ifndef MP_BROWSE_POLICY_H
#define MP_BROWSE_POLICY_H

#include <stdint.h>

// The rules for "which script is on screen, and when does it get rendered",
// shared by both firmwares and testable off-device.
//
// This exists because every bug in that area so far has been a timing bug found
// by squinting at a panel, one device at a time, and fixed in one firmware
// while the other kept its own version of the mistake. None of them needed
// hardware to reproduce; they needed a clock you can move by hand. The five
// that shipped, each of which is now a test in the host harness:
//
//   1. The settle window was armed BEFORE the title was drawn. Drawing it costs
//      about as long as the window, so the title was overwritten the moment it
//      appeared. Here the window cannot be armed except by titleDrawn().
//   2. The periodic re-render was not gated on a pending selection, so it
//      repainted the OUTGOING script over a fresh title.
//   3. A held button postponed the render for as long as it was held, so a
//      sticky contact froze the UI on a title with the press indicator lit.
//   4. A render aborted on any button being DOWN rather than on a new press, so
//      a held button also aborted every render it triggered.
//   5. (mp_refresh_budget.h) The de-ghost flash could land on a title frame.
//
// Deliberately free of Arduino, FreeRTOS and any display type: it takes a
// millisecond clock and returns what to do. The caller owns the panel, the
// buttons and the script list.
enum class MpBrowseAction : uint8_t {
    Nothing = 0,
    ShowTitle,  // draw the name of pending(); then call titleDrawn()
    Render,     // render current(), already committed
};

class MpBrowsePolicy {
public:
    struct Config {
        // How long a title stays readable before its render starts. Measured
        // from when the title is ON the panel, not from when we asked for it.
        uint32_t titleSettleMs = 450;
        // Periodic re-render, so time- and counter-dependent scripts advance.
        uint32_t autoRerunMs = 83000;
        // The longest a held button may postpone a settled render. Without a
        // cap, a stuck contact is indistinguishable from a frozen device.
        uint32_t buttonHoldCapMs = 1500;
    };

    void configure(const Config& c) { _cfg = c; }
    const Config& config() const { return _cfg; }

    void setScriptCount(int n) { _count = n < 0 ? 0 : n; }
    int  scriptCount() const { return _count; }

    // The committed selection: what is on the panel, or about to be.
    int  current() const { return _current; }
    void setCurrent(int i) { _current = wrap(i); }

    // The browsed-to selection, -1 when not browsing.
    int  pending() const { return _pending; }
    bool browsing() const { return _pending >= 0; }

    // Steps the selection without committing it. Always asks for a title;
    // the settle window starts only when titleDrawn() says it is up.
    MpBrowseAction browse(int delta, uint32_t now);

    // The title for pending() is now on the panel.
    void titleDrawn(uint32_t now);

    // Re-render what is already selected, with no title (the confirm button and
    // the boot path). Commits immediately.
    MpBrowseAction reRun(uint32_t now);

    // Call every pass. `anyButtonDown` is the live level; `pressCount` counts
    // press EDGES seen so far and only ever increases.
    MpBrowseAction poll(uint32_t now, bool anyButtonDown, uint32_t pressCount);

    // A render for current() has begun. Records the press count so that a
    // button still held from before cannot be mistaken for an abort request.
    void renderStarted(uint32_t now, uint32_t pressCount);

    // True only if a NEW press arrived since renderStarted(). A button that was
    // already down when the render began is not a request to abandon it.
    bool abortRequested(uint32_t pressCount) const;

    // A render finished, aborted or not. Only a completed one resets the
    // periodic re-render deadline: an abandoned frame is not a frame shown.
    void renderFinished(uint32_t now, bool completed);

    // For a firmware that suppresses the periodic re-render itself.
    void suspendAutoRerun(bool on) { _autoRerunSuspended = on; }

    uint32_t lastRenderAt() const { return _lastRenderAt; }
    // Milliseconds until the periodic re-render falls due, 0 if it is due now.
    // A firmware that sleeps between passes needs this to size the sleep, and
    // getting it from here keeps the deadline in one place rather than having
    // the sleep code keep its own copy of the same arithmetic.
    uint32_t msUntilAutoRerun(uint32_t now) const;

private:
    int wrap(int i) const;

    Config   _cfg;
    int      _count = 0;
    int      _current = 0;
    int      _pending = -1;
    bool     _titleUp = false;          // pending's title is on the panel
    uint32_t _settleDeadline = 0;
    uint32_t _holdBlockedSince = 0;     // 0: not currently blocked by a held button
    uint32_t _lastRenderAt = 0;
    uint32_t _pressAtRenderStart = 0;
    bool     _renderInFlight = false;
    bool     _autoRerunSuspended = false;
};

#endif // MP_BROWSE_POLICY_H
