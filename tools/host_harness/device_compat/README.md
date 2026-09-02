# Vendored ESP32 compatibility sources

These files reproduce allocator behavior in the constrained WebAssembly device
renderer. They are upstream sources, not MicroPatterns implementations.

| Directory | Device build | Upstream source |
|---|---|---|
| `arduino-esp32-2.0.4` | M5Paper | PlatformIO `framework-arduinoespressif32@3.20004.0` |
| `arduino-esp32-3.1.3` | Watchy | Installed pioarduino 53.03.13 source package |
| `esp-idf-4.4.1` | M5Paper, Arduino ESP32 2.0.4 | Espressif ESP-IDF tag `v4.4.1`, commit `1329b19fe494500aeb79d19b27cfd99b40c37aec` |
| `esp-idf-5.3.2` | Watchy, Arduino ESP32 3.1.3 | PlatformIO `framework-espidf` package `3.50302` (ESP-IDF 5.3.2) |

The M5Paper files were copied from `components/heap` in an official sparse
checkout of `https://github.com/espressif/esp-idf.git`. The Watchy files were
copied from the PlatformIO ESP-IDF source package used by its installed build
toolchain. Upstream SPDX and license text remain in every source file.

`multi_heap*` is Apache-2.0 licensed by Espressif. TLSF is BSD-3-Clause licensed
by Matthew Conte. The TLSF files contain the complete BSD notice; the multi-heap
files contain the Apache-2.0 SPDX identifier or full notice.

Both firmware sdkconfigs enable `CONFIG_HEAP_POISONING_LIGHT`. Compatibility
builds therefore compile `multi_heap_poisoning.c`, not only the underlying TLSF
source. ESP-IDF 4.4.1 names its allocator files `heap_tlsf.*`; 5.3.2 places them
under `tlsf/` and splits common/block helpers into headers.

The Arduino directories contain the LGPL-2.1-or-later WString and
`stdlib_noniso` sources from the exact installed framework packages. Their
SHA-256 checksums at vendoring time are:

```text
Arduino 3.1.3
WString.h       9e035bac489372d802c1215556ab6b04b10295582a71111eea7c4ce6c6e71719
WString.cpp     811b07d63fa5d57bfb7fa18f60a4e755b2ce7658afed867de5d199bd1cc7b158
stdlib_noniso.h b85abe4c290993129db999eb947816eb7a433e873d4b196bae1ddfa75eca3c37
stdlib_noniso.c c67d7a521b4730a4177c704a8c1173495cadde2dbd4a221dce406d751044404f

Arduino 2.0.4
WString.h       af9bc0cefafaebfd5eb77ff57a3cc4ffff855925417a5d90415f104264ff41d3
WString.cpp     10704fe417a70e513d0c27bf1d3f26ea256a4a7d5409983a279b364186d2ef00
stdlib_noniso.h 5e4c062377c976af8bfee56953b34fcc7e7e18918be190a48d8093e8371cbd4b
stdlib_noniso.c 81914bd4e3f99b3f06942eefcc234f216662d7a13ad6ed0b7992a2f5d7f1470c
```

`arduino_host/` is MicroPatterns glue, not upstream code. It supplies only
the ESP32 headers/functions absent from Emscripten and maps flash-string reads
to ordinary wasm32 memory. `device_string_alloc_redirect.h` redirects the
upstream WString allocation calls without editing the vendored files.

Do not silently update these sources. An allocator update is a device-toolchain
change and must be made together with the corresponding PlatformIO pin, source
checksums, allocation smoke tests, and hardware calibration captures.
