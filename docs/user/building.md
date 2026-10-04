# Building and installing

fs-9p is distributed as source. You build it with your own QNX SDP.

## Requirements

- A Linux host (tested on Ubuntu) with the QNX SDP 8.0 installed
  (`~/qnx800` in the examples).
- CMake 3.20 or later.
- Optional, for the HTML documentation: Python 3 with `venv` (see
  {ref}`build-docs`).

QEMU is not needed to build, only to run.

## Build

```
source ~/qnx800/qnxsdp-env.sh
cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake
cmake --build build-qnx
```

`qnxsdp-env.sh` sets `QNX_HOST` and `QNX_TARGET`, which the toolchain file
`cmake/qnx8-aarch64le.cmake` uses to find `qcc` (`-Vgcc_ntoaarch64le`). The
result is `build-qnx/fs-9p`, an aarch64le executable with its usage message
embedded (`usemsg`). This build compiles only what is shipped; no test
programs.

The toolchain file is what makes this the QNX build. Without it, `cmake`
configures the host build that developers use for the unit tests: it
builds only the protocol library and its tests, installs nothing, and
says so when it configures.

Check it:

```
use build-qnx/fs-9p
```

The first line shows the version.

## Install

```
cmake --install build-qnx --prefix /opt/fs-9p
```

`DESTDIR` is honoured for staging (`DESTDIR=/tmp/stage cmake --install
build-qnx --prefix /usr`). What gets installed:

| Path under the prefix | Contents |
|---|---|
| `sbin/fs-9p` | The program, the only binary. |
| `share/doc/fs-9p/` | README, CHANGELOG, LICENSE, and the documentation sources. |
| `share/doc/fs-9p/html/` | This documentation as HTML, if it was built first ({ref}`build-docs`). |
| `share/fs-9p/fs-9p.build` | Lines to add fs-9p to a QNX image ({doc}`integrating`). |

fs-9p links `lib9p` (its 9P protocol library) statically and needs nothing
else on the target besides `libc.so.6` and `libgcc_s.so.1`.

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_TOOLCHAIN_FILE` | none | `cmake/qnx8-aarch64le.cmake` for the QNX build. Without it, only the host-side library and its tests build, and nothing is installed. |
| `FS9P_BUILD_TESTS` | `ON` on the host, `OFF` for QNX | Build the `lib9p` unit tests (host only). |
| `FS9P_SANITIZE` | `OFF` | Host builds: compile the tests with AddressSanitizer and UndefinedBehaviorSanitizer. |
| `FS9P_INSTALL_LIB` | `OFF` | Also install `lib9p` and its headers. |

(build-docs)=
## Building this documentation

The HTML documentation is built with Sphinx, which only the documentation
needs. Once, in the source directory:

```
python3 -m venv .venv-docs
.venv-docs/bin/pip install -r docs/requirements.txt
```

A venv only works where it was created: in a copy of the source tree,
delete `.venv-docs` and create it again.

Then, in any build directory configured after that:

```
cmake --build build-qnx --target docs
cmake --install build-qnx --prefix /opt/fs-9p
```

The HTML goes to `build-qnx/docs/html/` and is installed to
`share/doc/fs-9p/html/`; open `index.html`.
