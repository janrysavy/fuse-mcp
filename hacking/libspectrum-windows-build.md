# libspectrum for the current Windows build

The MIC tape recording tests write PZX, so a library that can only read PZX is insufficient. This integration was qualified with libspectrum commit `fc76526231e1aa03b5fe014391fbf50be5dbfb98` from [the upstream repository](https://sourceforge.net/p/fuse-emulator/libspectrum/ci/fc76526231e1aa03b5fe014391fbf50be5dbfb98/tree/).

On MinGW, `tap_pzx.c` declares a private function named `near`. Windows headers define a macro with that name, producing a syntax error at the function declaration. Apply `hacking/libspectrum-mingw-pulse-match.patch` in a clean libspectrum checkout. It renames the private helper and six call sites to `pulse_matches`, without changing the duration tolerance or algorithm.

Build into a fresh prefix, run `make check`, and configure Fuse with that prefix's `lib/pkgconfig` in `PKG_CONFIG_PATH`. Bundle its new `libspectrum-21.dll` beside Fuse; linking a new library while retaining an old runtime DLL does not upgrade the runtime. The reviewed library test run passed 450 tests with two skips. Keep older dependency/runtime bundles intact for comparisons.

Native Windows unit tests also need existing `TMP`/`TEMP` directories and the test ROMs beside the executable. Five optional tests require external Multiface One, uSource, Beta 128, Didaktik and Opus ROMs; absence of these does not constitute a passing complete peripheral suite.
