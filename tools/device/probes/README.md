# Hardware probes

Minimal, single-purpose firmwares. Each exists because a bug was being blamed on
the wrong thing, and the only way to settle it was to build something that
contains *nothing else*.

They are kept because the questions recur, and because a probe that reproduced a
platform bug once is the fastest way to check whether a later platform still has
it. Each one records what it proved, including where it proved us wrong.

Run one with `pio run -e <env> -t upload -d <probe-dir> --upload-port ...`
and read it with `../mpcon.py --port ... capture`.

---

## `nvs-probe/` -- the A/B that decided the Watchy's platform

Two envs, identical source: `arduino2` (espressif32@5.1.0, Arduino 2.0.4 /
IDF 4.4) and `arduino3` (pioarduino, Arduino 3.1 / IDF 5.3).

**What it proved.** On this ESP32-PICO-D4, NVS is broken under Arduino 2.0.4:

    nvs_flash_init: ESP_OK
    open: ESP_OK
    set_u8:  ESP_OK
    get BEFORE commit: ESP_ERR_NVS_NOT_FOUND val=0
    commit:  ESP_OK
    get AFTER commit:  ESP_ERR_NVS_NOT_FOUND val=0
    entries used=1 free=629 total=630

`set` returns OK and the very next `get` **on the same open handle** says
NOT_FOUND -- before flash is even involved. The identical source passes on
`arduino3`. That is why provisioned credentials could never be read back, and
why the Watchy moved platform.

**The ten hypotheses this killed.** Before it was built, the missing credentials
were blamed on -- and six of these were real bugs, none of them the cause --
the BLE callback task, a `String`/`std::string` mismatch in `getValue()`, the
Preferences namespace, key length, read-only handles, commit ordering, the
partition table, flash mode, the watchdog, and heap exhaustion. The lesson is
the probe itself: strip everything until only the suspect remains.

---

## `ldf-wifi-probe/` -- why `lib_ldf_mode = deep+` is banned here

Nothing but `#include <WiFi.h>`.

**What it proved.** `deep+` breaks the framework's *own* WiFi library:

    WiFiGeneric.h:44:10: fatal error: Network.h: No such file or directory

`WiFi/library.properties` declares no dependency on `Networking`, and `deep+`
stops PlatformIO wiring the framework libraries' include paths to each other.
Comment out the one line and it builds.

This matters because `deep+` is the obvious fix for the *other* symptom -- the
shared managers arriving via `build_src_filter` cannot find `HTTPClient.h`,
because the dependency finder only scans sources under `src_dir`. Adding it
trades one build failure for a worse one. The actual fix is four `#include`
lines in the Watchy's own `main.cpp`; see the comment in its `platformio.ini`.

Naming the libraries in `lib_deps` does **not** help either: they appear in the
dependency graph and the build fails identically.

---

## `serial-probe/` -- separating "dead board" from "deaf host"

Buzzes the motor *and* prints, so the two channels are independent: a buzz with
no serial means the app runs and UART0 does not reach the host; neither means
the app is not executing.

**What it proved, and the dead ends it closed.** The Watchy booted silently for
a long stretch and this was blamed, in order, on the board being in download
mode (an artifact of running esptool with `--after no_reset`), on reset-line
polarity (twice), and finally on "serial is simply dead on this hardware". All
wrong. The cause was **flash frequency**: at 40MHz serial never arrived; at
80MHz it works.

**Left deliberately broken.** This `platformio.ini` still says
`board_build.flash_mode = qio`, which is the build that proved qio stops this
board executing entirely -- no buzz, no serial, nothing. InkWatchy's sdkconfig
has `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` *and* `CONFIG_ESPTOOLPY_FLASHMODE="dio"`,
because the bootloader header is written DIO and the bootloader upgrades to QIO
at runtime. Writing `qio` into the header instead bricks the boot. Set it to
`dio` to get a probe that actually runs; the `qio` value is kept as the record.
