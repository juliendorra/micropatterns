# The Watchy "press it twice" hunt, and the browse timings that came out of it

**Status: RESOLVED.** Root cause in section 5. The wrong turns are kept because
they are the useful part, and because two of them are traps worth never
repeating -- section 7 in particular.

Written at the project owner's request, after a session in which the fix was
four lines and finding it took most of a day, three false root causes and one
proposal that would have damaged the product.

## 1. The symptom

Browsing the Watchy, two complaints that turned out to be different bugs:

- The watch "freezes": the screen goes completely static, buttons do nothing,
  and after a while it "reboots or something, then refreshes several times and
  renders".
- Every press has to be made twice. The first appears to do nothing.

## 2. What was ruled out

| Hypothesis | How it was killed |
|---|---|
| The state machine drives the panel wrongly | Read the whole browse path; the policy was doing what its tests said. |
| A memory leak from abandoned renders | `[heap] before load` reads `free=177456` at 227.05s and `free=177456` at 261.14s, and `largest-block` never moves off 110580. Nothing leaks. |
| The de-ghost budget charged by the indicator | `noteUncharged()` is an empty function. The indicator provably cannot affect it. |
| A crash or watchdog reset | Added `esp_reset_reason()` logging (kept -- commit 96fc460). Every boot in every capture says `power-on`. There is no reset. |
| Light sleep re-running `setup()` | This device uses *light* sleep, which resumes inside `loop()`. A full `setup()` in a capture therefore means a real reset, and there weren't any. |
| The recovery banner meaning a crash | `MPCON\|recovery -- send: run <index>` is printed unconditionally at every boot with a 5s window. It is not a crash state. Mistaking it for one cost a cycle. |
| GPIO wake not firing | The captures say `woke, cause=7` on every single press. The wake was never broken. |
| The browse being refused | `MpBrowsePolicy::browse()` returns `ShowTitle` unconditionally when the list is non-empty. It cannot refuse. |

Four of those were real findings worth keeping. **None of them was the bug.**

## 3. The instrumentation, which is the thing to keep

Nothing here was measurable before this session. Every number in this document
came from temporary `log_i` lines and a serial capture. They are removed from
the firmware, and they are written out here because they will be needed again.

Capture with:

```
~/.platformio/penv/bin/python tools/device/mpcon.py \
    --port /dev/cu.usbserial-110 --no-reset capture --seconds 300 > capture.log
```

`--no-reset` matters: opening the port normally asserts DTR/RTS and reboots the
board, which destroys the state you are trying to observe. Note also that the
port must be free before `pio run -t upload`, or the flash fails -- kill the
capture first (`pkill -f mpcon.py`).

**Panel timing** (in `showScriptName`, `drawCornerIndicator`, `renderScript`):

```c
const unsigned long t0 = millis();
/* ... the frame ... */
log_i("[t] title '%s': %lu ms (full=%d)", name, millis() - t0, (int)fullTitle);
log_i("[t] indicator %s: %lu ms", filled ? "on" : "off", millis() - t0);
```

**Browse phase timing** (in `showScript`, `browseScript`, `serviceBrowse`):

```c
log_i("[t] saveCurrentScriptId: %lu ms", millis() - tIo);
log_i("[t] browseScript total %lu ms; settle starts now", millis() - tEnter);
log_i("[t] settled; starting render");
```

**Sleep decisions** -- this is the pair that found the bug:

```c
if (remaining > 2000) log_i("[s] nap %lums (browsing=%d)", remaining, (int)g_browse.browsing());
Serial.flush();
esp_light_sleep_start();
if (remaining > 2000) log_i("[s] woke, cause=%d", (int)esp_sleep_get_wakeup_cause());
```

The `remaining > 2000` guard keeps short naps from drowning the log. Wake cause
4 is the timer, 7 is GPIO.

## 4. The measurements

All from a Watchy v2 on the bench, panel SSD1681 200x200.

| Frame | Cost |
|---|---|
| Corner indicator, 26x26 | **402 ms** |
| Title, full screen, fast partial | 423 ms |
| Script render, fast partial | 480-930 ms |
| Script render, full de-ghost | **2150-2220 ms** |
| First render after boot (forced full) | 2495 ms |
| `saveCurrentScriptId` (SPIFFS) | 195-265 ms |

The single most important number is the first one. **This panel's partial
refresh runs a fixed-length waveform, so a 26x26 box costs the same as the
whole 200x200 screen.** Every intuition that treats a small update as a cheap
one is wrong on this hardware, and two separate bugs in this document come from
that one false intuition.

Second most important: the de-ghost budget was being charged **two units per
press**, one for the title and one for the render, so a 24-update interval
produced a 2.2s flash roughly every twelfth press.

## 5. Root cause of "press it twice"

A press is two edges and only the release does anything -- deliberate, so a
long press does not also fire the short action. The firmware must stay awake
between them.

The indicator draw takes 402ms, which is longer than a human holds a button. So
the loop pass that saw the press edge spent its time drawing that frame, and by
the time it reached the sleep check every pin was low again. The check asked
the pins, the pins said nothing was held, and the watch slept with the release
unconsumed -- until the 83s timer. The next press was spent re-arming that
state; only the one after it acted.

The evidence, unambiguous once the sleep instrumentation was in:

```
97.59s  [s] nap 82762ms (browsing=0)
98.01s  [s] woke, cause=7          <- the press DID wake it
98.42s  [s] nap 81940ms (browsing=0)   <- straight back to sleep, no Button event
104.03s [s] woke, cause=7          <- second press
104.81s Button: bottom-right       <- only now does anything happen
```

The fix (commit 38c8627) is to ask a different question in the sleep guard.
"Is a pin high right now" is about hardware state; what matters is whether the
firmware owes the user an answer to an edge it has already seen.

This regressed in `a51e4cf`, which rewrote the guard from `g_pendingScript >= 0`
("a render is due", true until a render completed) to `g_browse.browsing()`
("a title is settling", false as soon as the policy commits). Those differ
exactly in the window this bug lives in. The M5Paper was unaffected because its
acknowledgement *is* the title -- no 402ms indicator between the edges -- and
its sleep path is separate code that commit never touched.

## 6. The other two fixes that came out of the same measurements

Commit `e628507`:

- **The indicator was drawn twice per press**, once to show it and once to
  clear it: 804ms, not 402. The clear is waste, because every button except
  top-left is followed within a few hundred ms by a full-screen frame that
  erases the corner for free. Removing it puts the title up 830ms after release
  instead of 1230ms. Top-left keeps its clear -- it opens the BLE window and
  paints nothing afterwards, so its box would sit there forever.
- **The de-ghost flash landed mid-browse.** A browse render now never spends
  it; the 83s idle re-render always may. The debt is deferred, never forgiven.

## 7. The traps

**Do not weaken the sleep.** Partway through this session, on the false
conclusion that GPIO wake was broken, the sleep was capped so the watch woke
every 120ms to poll the buttons. That is polling a watch eight times a second
to paper over a four-line bug, and the owner rejected it immediately and
correctly: the sleep is the entire point of an ESP32 device. It was reverted
within minutes and no version of it was ever committed. **If a future
investigation finds itself proposing to shorten, cap, or remove the light
sleep, that is a signal the root cause has not been found yet.**

**Do not conclude from a gap in the log that the device is broken.** An
82-second silence was read as proof that button wake had failed. It was a
normal idle sleep -- during a capture in which the tester had been explicitly
asked *not* to press anything. The instruction created the evidence. Check what
the human was told to do before interpreting their device's silence.

**Do not read the boot recovery banner as a crash.** It prints every boot.

**Do not assume a small panel update is a cheap one.** See section 4.

**"It worked a few commits ago" is worth more than any amount of reading.** Two
sentences from the owner -- that it began when the state machine took over, and
that the M5Paper is fine -- narrowed a day of speculation to a single function,
because that function is Watchy-only code that `a51e4cf` rewrote. Ask for that
information early.

## 8. What is still open

`Render did not complete` leaves no record anywhere that a render is still
owed: `poll()` has already cleared `_pending`, so `browsing()` is false and
`renderFinished(completed=false)` deliberately does not move `_lastRenderAt`.
In every capture the next press scheduled a new render, so it always recovered,
and it is **not** what the "freeze" reports were about. It is left untouched on
purpose, and noted here so it is not rediscovered from scratch.

The de-ghost deferral in `e628507` is not ported to the M5Paper. It needs a
periodic render to pay the deferred debt, and that device has one -- the 77s
light-sleep wake, not the policy's `autoRerunMs`, which is 0 there precisely
because the sleep cycle owns it.
