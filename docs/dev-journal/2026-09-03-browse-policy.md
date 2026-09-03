# 2026-09-03 — Three fixes on a panel, then one tested policy

## How this started

After scripts stopped being parsed at render time (see the compiled-programs
entry from the same day), browsing the Watchy broke: press next, the panel sits
on the outgoing script, then the new one appears with no title in between.

I fixed it three times against hardware and told the user each time it was
fixed. Each fix was real. None was sufficient.

1. **The settle window was armed before the title was drawn.** `showScriptName`
   is a full-screen partial update costing about as much as a render's own panel
   push, so the whole 450 ms window was spent driving the frame it was meant to
   leave up. Parse time used to sit between the title and the render and hid
   this by accident; loading a compiled program takes 38 ms for City, so the
   accident stopped happening.
2. **The periodic re-render was not gated on a pending selection.** Browsing
   does not touch the last-render timestamp, so a press landing more than 83 s
   after the last render repainted the *outgoing* script over the fresh title,
   then rendered the chosen one. It fires on exactly the presses most likely to
   happen: the ones after the watch has been idle.
3. **The de-ghost flash could land on a title frame.** Measured over a real
   browsing session, the budget was spent at about four units per press against
   an interval of 24 — progression `1, 6, 10, 17, 21, 5, 8, 16, 19, 23, 1` —
   so a 2.6 s black-and-white refresh arrived every fifth or sixth press. Half
   of that was the corner press indicator, charging two units for 1.7% of the
   panel. This is why the fault looked intermittent rather than broken.

Then the user reported the device frozen on a title with the press indicator
lit, recovering "after a very long time". That is a fourth bug, and the point at
which they said: stop debugging timing on the device.

They were right. Every one of these needed a clock you can move by hand, not a
panel to stare at.

## What replaced it

`mp_browse_policy.{h,cpp}` and `mp_refresh_budget.h`: no Arduino, no FreeRTOS,
no display type. They take a millisecond clock and answer what to do.
`tools/host_harness/src/policy_test.cpp` drives them; `make verify` runs it, 58
checks alongside the 18 golden renders.

Six rules, each one a bug that shipped:

1. The settle window cannot be armed except by `titleDrawn()`.
2. The periodic re-render yields to a pending selection.
3. A held button postpones a settled render, but only up to a cap.
4. A render is abandoned by a **new** press, not by a button merely down.
5. An abandoned render does not reset the periodic deadline.
6. The de-ghost flash never lands on a transient frame; the debt is deferred.

Rule 4 was found by writing the test for rule 3, not on hardware. Without it,
the button that survives the hold cap goes on to abort every render it triggers,
so the device escapes one freeze straight into another. That is the bug this
whole exercise was for: it does not exist until rule 3 does, and no amount of
looking at a panel would have named it.

## What stayed per-device

Only the rules are shared. The de-ghost interval is 24 on the Watchy and 8 on
the M5Paper because the panels and waveforms genuinely differ. The M5Paper sets
`autoRerunMs = 0` and relies on its 77 s light-sleep wake, because a second
deadline running beside the sleep timer could only ever disagree with it — the
exact class of bug being cured.

## The other half

Surveying the two firmwares side by side to write the policy also turned up
three M5Paper bugs the Watchy did not have: an unbounded retry loop on a
permanently failing script, title frames escaping the de-ghost budget, and
`triggerScriptRender` ignoring its own script-id argument. Those are fixed in
`ab7ad93`, along with replacing `why.startsWith("Parse")` — which both firmwares
had grown independently — with a typed `ScriptManager::LoadReason`. Two
firmwares agreeing by spelling is how this pair keeps drifting apart.

## Still open

- The Watchy clears its press indicator with a separate ~400 ms panel update
  that the title frame then overwrites anyway. Dropping it would make paging
  noticeably snappier.
- A `FetchTask` task-watchdog abort was seen once on the M5Paper during button
  testing, unrelated to these changes: each task calls `esp_task_wdt_init()`
  with its own timeout on a shared timer.
- GPIO 39 on the M5Paper was seen firing continuously and being rejected as
  noise, which looks like a hardware issue on that unit.
