#include "mp_browse_policy.h"

int MpBrowsePolicy::wrap(int i) const
{
    if (_count <= 0) return 0;
    return ((i % _count) + _count) % _count;
}

MpBrowseAction MpBrowsePolicy::browse(int delta, uint32_t now)
{
    (void)now;
    if (_count <= 0) return MpBrowseAction::Nothing;

    // Step from the pending selection when there is one, so a burst of presses
    // walks the list rather than oscillating around the committed script.
    const int from = browsing() ? _pending : _current;
    _pending = wrap(from + delta);

    // The window is NOT armed here. It cannot be: drawing the title is the
    // caller's job and takes real time on an e-paper panel, so arming now would
    // spend the whole window driving the frame it was meant to leave up.
    _titleUp = false;
    _settleDeadline = 0;
    _holdBlockedSince = 0;
    return MpBrowseAction::ShowTitle;
}

void MpBrowsePolicy::titleDrawn(uint32_t now)
{
    if (!browsing()) return;
    _titleUp = true;
    _settleDeadline = now + _cfg.titleSettleMs;
}

MpBrowseAction MpBrowsePolicy::reRun(uint32_t now)
{
    (void)now;
    if (_count <= 0) return MpBrowseAction::Nothing;
    _pending = -1;
    _titleUp = false;
    _holdBlockedSince = 0;
    return MpBrowseAction::Render;
}

MpBrowseAction MpBrowsePolicy::poll(uint32_t now, bool anyButtonDown, uint32_t pressCount)
{
    (void)pressCount;
    if (_count <= 0 || _renderInFlight) return MpBrowseAction::Nothing;

    if (browsing()) {
        // A title that has been asked for but not yet reported as drawn keeps
        // the render waiting indefinitely. That is deliberate: the caller is
        // mid-update, and there is no deadline that could help.
        if (!_titleUp) return MpBrowseAction::Nothing;
        if ((int32_t)(now - _settleDeadline) < 0) return MpBrowseAction::Nothing;

        if (anyButtonDown) {
            // Further presses should extend the window, which is what makes
            // paging feel like paging. But only for so long: a contact that
            // never reads low would otherwise hold the device on a title
            // forever, indistinguishable from a crash, and that is exactly what
            // it looked like on the bench.
            if (_holdBlockedSince == 0) _holdBlockedSince = now;
            if ((int32_t)(now - _holdBlockedSince) < (int32_t)_cfg.buttonHoldCapMs) {
                _settleDeadline = now + _cfg.titleSettleMs;
                return MpBrowseAction::Nothing;
            }
        }

        _current = _pending;
        _pending = -1;
        _titleUp = false;
        _holdBlockedSince = 0;
        return MpBrowseAction::Render;
    }

    // Periodic re-render, never while a selection is waiting: it would repaint
    // the outgoing script over the title the user is reading, and then render
    // the chosen one straight after.
    //
    // Nor while a button is DOWN. A press has two edges and a browse is only
    // reported on the second one, so between them there is a window in which
    // the user has plainly started something and browsing() is still false.
    // The periodic re-render used to slip into exactly that window: press next
    // on the last script, watch the OUTGOING one repaint, and only then get the
    // title and the render you asked for. It is the same fault as the line
    // above and it needed its own guard, because the first thing the caller can
    // tell us about a press is not the press -- it is the release.
    //
    // A contact stuck down therefore holds the periodic re-render off for as
    // long as it is stuck. That is deliberate and harmless: nothing is waiting
    // on it, unlike the settled render that rule 3 has to cap.
    if (anyButtonDown) return MpBrowseAction::Nothing;

    if (!_autoRerunSuspended && _cfg.autoRerunMs > 0 &&
        (int32_t)(now - _lastRenderAt) >= (int32_t)_cfg.autoRerunMs) {
        return MpBrowseAction::Render;
    }
    return MpBrowseAction::Nothing;
}

uint32_t MpBrowsePolicy::msUntilAutoRerun(uint32_t now) const
{
    if (_autoRerunSuspended || _cfg.autoRerunMs == 0) return _cfg.autoRerunMs;
    const int32_t elapsed = (int32_t)(now - _lastRenderAt);
    if (elapsed >= (int32_t)_cfg.autoRerunMs) return 0;
    return _cfg.autoRerunMs - (uint32_t)elapsed;
}

void MpBrowsePolicy::renderStarted(uint32_t now, uint32_t pressCount)
{
    (void)now;
    _renderInFlight = true;
    _pressAtRenderStart = pressCount;
}

bool MpBrowsePolicy::abortRequested(uint32_t pressCount) const
{
    return _renderInFlight && pressCount != _pressAtRenderStart;
}

void MpBrowsePolicy::renderFinished(uint32_t now, bool completed)
{
    _renderInFlight = false;
    // An abandoned render is not a frame the user saw, so it must not push the
    // periodic deadline out; otherwise a run of interrupted renders would stop
    // time-dependent scripts from ever advancing.
    if (completed) _lastRenderAt = now;
}
