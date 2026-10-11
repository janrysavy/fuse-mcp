# Building Fuse for Emscripten

Fuse can be built as a single-threaded WebAssembly application using the SDL 2
Widget UI. This target deliberately provides the existing Fuse interface in a
browser canvas; it does not provide browser file pickers, persistent storage,
networking, or a JavaScript control API.

## C11 atomics and threads

Fuse's SDL sound FIFO uses lock-free C11 atomic unsigned integer indices with
acquire/release ordering. Current Emscripten supports these operations without
`-pthread` in a single-threaded build; they do not require shared WebAssembly
memory or a pthread runtime. The FIFO unit tests pass in this configuration
(the pthread stress test is skipped). Fuse's optional network thread is disabled
for this target.

When configured with `--with-fake-glib`, libspectrum uses three `atomic_flag`
objects: one each for the global free lists in `myglib/ghash.c` and
`myglib/gslist.c`, operated on by `atomic_lock()` and `atomic_unlock()` in
`myglib/glock.c`. The only operations are
`atomic_flag_test_and_set_explicit(..., memory_order_acquire)` and
`atomic_flag_clear_explicit(..., memory_order_release)`. The locks make those
global allocators safe if libspectrum is called from multiple native threads;
they do not imply that Fuse needs threads at runtime.

Current Emscripten supports those operations in both configurations. In a
normal single-threaded build Clang lowers them without requiring shared
WebAssembly memory or a pthread runtime. With `-pthread`, it emits operations
for shared memory and adds the worker/pthread runtime. The libspectrum
configure link test checks the exact operations above and succeeds without
`-pthread`. A clean libspectrum and Fuse build also compiles and links without
it. Consequently this target does not use `-pthread`; enabling it would add a
runtime requirement for `SharedArrayBuffer` and cross-origin isolation without
providing a Fuse feature that needs a worker thread.

Do not disable `stdatomic.h` for Emscripten. The historical
`HAVE_STDATOMIC_H && !defined(__EMSCRIPTEN__)` workaround is obsolete.

## Tested toolchain

The procedure below was tested with Emscripten 6.0.9-git. Install and activate
that version with emsdk (replace the directory with your preferred location):

```sh
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 6.0.9
./emsdk activate 6.0.9
. ./emsdk_env.sh
```

The commands assume clean, adjacent `libspectrum` and `fuse` source trees.
They use separate build and installation directories so native libraries from
the host cannot satisfy target checks. The explicit
`--host=wasm32-unknown-emscripten` is required so libtool does not configure
itself for the native host. `emconfigure` may nevertheless report
`cross_compiling=no` because configure test programs can execute through Node.

## Build libspectrum

```sh
mkdir -p build/wasm/libspectrum build/wasm/prefix
cd build/wasm/libspectrum

emconfigure ../../../libspectrum/configure \
  --host=wasm32-unknown-emscripten \
  --prefix="$(cd ../prefix && pwd)" \
  --disable-shared --enable-static \
  --with-zlib --without-bzip2 --without-libgcrypt \
  --without-libaudiofile --with-wav-backend=none \
  --with-fake-glib \
  CFLAGS='-O2' LDFLAGS='-sUSE_ZLIB'
emmake make -j4
emmake make install
```

## Build Fuse

From the directory containing the two source trees:

```sh
mkdir -p build/wasm/fuse
cd build/wasm/fuse
PREFIX="$(cd ../prefix && pwd)"

EM_PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
LIBSPECTRUM_CFLAGS="-I$PREFIX/include" \
LIBSPECTRUM_LIBS="$PREFIX/lib/libspectrum.a" \
SDL2_CFLAGS='-sUSE_SDL=2' \
SDL2_LIBS='-sUSE_SDL=2' \
emconfigure ../../../fuse/configure \
  --host=wasm32-unknown-emscripten \
  --with-sdl --without-x \
  --without-pthread --disable-sockets --without-joystick \
  --with-zlib --without-png --without-libxml2 \
  --disable-desktop-integration \
  CFLAGS='-O2' LDFLAGS='-sUSE_ZLIB'

EM_PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" \
emmake make -j4
```

Serve the build directory over HTTP rather than opening the HTML file directly:

```sh
python3 -m http.server 8000
```

Then open `http://localhost:8000/fuse.html`.

The build produces these publication-ready files:

- `fuse.html`
- `fuse.js`
- `fuse.wasm`
- `fuse.data`

`fuse.data` contains the ROMs and UI resources installed by the normal Fuse
build. It contains no demonstration tape and no special configuration file, so
Fuse starts with its ordinary defaults.

## Manually publishing to GitHub Pages

`.github/workflows/build_emscripten_pages.yml` builds and publishes this target
only when manually dispatched. There are no push, tag, or scheduled triggers.

Before the first run:

1. In the GitHub repository's **Settings → Pages**, select **GitHub Actions** as
   the build/deployment source.
2. Ensure repository rules allow the workflow's `GITHUB_TOKEN` to push to
   `gh-pages`. The workflow requests `contents: write` for its build job.
3. Ensure the `github-pages` environment permits deployments from the branch
   on which you run the workflow. Run the workflow from the default branch
   containing these scripts; the source selections are separate inputs.

In **Actions → Build and publish Fuse for the web → Run workflow**, select:

- `fuse_ref=master` and `libspectrum_ref=master` to replace the current
  development build at `master/`.
- Two explicit tags to add a release at
  `releases/<fuse-tag>/<libspectrum-tag>/`. These are tags, not arbitrary branch
  names or commit IDs. Mixing `master` with a tag is rejected. Tag names are
  encoded as individual directory components if necessary.

Fuse is checked out from the repository running the workflow; libspectrum is
checked out from `fuse-emulator/libspectrum`. Both selected sources must already
support the documented Emscripten build. This does not backfill older releases
or apply compatibility patches. The SDK is pinned to Emscripten 6.0.9.

The workflow maintains the entire published tree on `gh-pages`, then uploads
that tree as a Pages artifact and deploys it. This branch is storage, not the
Pages build source: keep Pages configured for GitHub Actions. Existing releases
are preserved; publishing an already present tag pair fails rather than
replacing it. Do not use this branch for unrelated site content. Workflows are
serialized using the `emscripten-pages` concurrency group; any other publishers
must use the same group. GitHub concurrency keeps at most one pending run, so
queue additional manual requests only after earlier runs have started.

The site's root index lists builds. Each build includes the four application
files (with `fuse.html` copied to `index.html`), source commit SHAs, SDK version,
build timestamp, and licence/ROM notices. Assets remain alongside their HTML
file and use relative URLs, including under a repository Pages subpath.
There are no browser file pickers or persistent settings added by publication.

A successful history push followed by a failed Pages deployment can leave the
stored tree newer than the live site. Re-run the failed deployment job, not the
whole immutable release build. Pages storage and bandwidth limits still apply
as the release archive grows. Review bundled ROM redistribution notices before
making the site public.

The shared build script can also build clean adjacent checkouts locally:

```sh
# With emsdk_env.sh already sourced, and SOURCE_ROOT containing fuse/ and libspectrum/:
bash fuse/.github/scripts/build_emscripten.sh /absolute/path/to/SOURCE_ROOT
```

Offline publication tests (no SDK required):

```sh
python3 .github/scripts/test_publish_emscripten.py
```

## Deliberately disabled facilities

- POSIX threads and sockets: browser networking is outside this milestone, and
  Fuse has no remaining web-target runtime need for a thread.
- Hardware joystick access: browser gamepad integration is not included yet;
  keyboard input remains available.
- libxml2 settings, bzip2 compression, libgcrypt, libaudiofile, libpng, and
  desktop integration: omitted to keep the initial target self-contained and
  free of accidental native dependencies. Zlib remains enabled because it is
  required for compressed snapshot data. Fuse uses its built-in/default
  settings and SDL 2 audio and display paths.
- Internal unit tests: these are a non-interactive developer facility built
  only with Fuse's null UI. `make check` on an interactive-UI build reports
  that a null-UI build is required.

The Emscripten target uses Asyncify because Fuse and the Widget UI retain their
blocking main loop and delay calls. Replacing that loop with a browser-specific
UI architecture is intentionally deferred.
