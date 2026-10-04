# Making a release

fs-9p is distributed as source: a release is a tested commit with a version
number, a changelog entry and a git tag. There are no binary packages;
integrators build with their own SDP.

## Where the version lives

In one place, the `project()` line of `CMakeLists.txt`:

```
project(fs-9p VERSION 0.1.0 LANGUAGES C)
```

Everything else is derived from it:

| Where it shows | How |
|---|---|
| `use fs-9p` (first line) | CMake fills `@PROJECT_VERSION@` in `src/fs-9p.use.in` (`configure_file`), and `usemsg` embeds the result in the binary after linking. |
| The HTML docs (title, `release`) | `docs/conf.py` reads it from `CMakeLists.txt`. |
| `CHANGELOG.md` | Written by hand; the `consistency` test fails if there is no `## [X.Y.Z]` section for the version in `CMakeLists.txt`. |
| The git tag | `vX.Y.Z`, made by hand at the end of this procedure. |

The binary has no `-V` option: the `use` message is the only place it
reports its version, on purpose: one place to keep right.

## Version numbers

Semantic Versioning, as stated at the top of `CHANGELOG.md`: while the
version is 0.x, a change that breaks an existing setup (command line,
documented behaviour) bumps the minor version, as do new options or
behaviour; fixes bump the patch version. From 1.0 on, breaking changes
bump the major version.

## The changelog between releases

Changes that a user would notice get a line in `CHANGELOG.md` as they are
committed, under a section at the top:

```
## [Unreleased]

### Added / Changed / Fixed / Removed
- ...
```

The 0.1.0 section was written before the first release and is named
`## [0.1.0] - unreleased` instead.

## Procedure

Start from a clean, up-to-date working tree on `master`
(`git status` shows nothing).

1. **Set the version.** Edit the `project()` line in `CMakeLists.txt`.
2. **Date the changelog.** Rename the top section to
   `## [X.Y.Z] - YYYY-MM-DD`; check that it describes everything since the
   previous tag (`git log vPREV..HEAD --oneline`).
3. **Check the docs** for anything the release changes: new options in the
   reference page ({doc}`../user/reference`) and the `use` message
   (`src/fs-9p.use.in`), behaviour pages, performance figures if the data
   path changed.
4. **Build and run the unit tests**, plain and with sanitizers:

   ```
   source ~/qnx800/qnxsdp-env.sh
   cmake -S . -B build-host -DFS9P_BUILD_TESTS=ON
   cmake --build build-host && ctest --test-dir build-host --output-on-failure
   cmake -S . -B build-asan -DFS9P_BUILD_TESTS=ON -DFS9P_SANITIZE=ON
   cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
   cmake --build build-qnx
   (cd vm && mkqnximage --build --noprompt)
   ```

   `use build-qnx/fs-9p | head -1` must show the new version.
5. **Run every integration suite** ({doc}`testing`, "Everything in one
   pass"), with `vm/smallfs` mounted so the disk-full suite runs too:

   ```
   tests/integration/run-all.sh
   time sudo -E env "PATH=$PATH" FS9P_SECURITY_MODEL=passthrough tests/integration/write.sh
   sudo chown -R "$USER:" vm
   ```

   `run-all.sh` must end with `ALL SUITES PASSED` and the passthrough run
   with `WRITE: ALL PASS`. Skips must be the expected ones (the host-side
   section with the cache on; in the root run, the checks where the host
   refuses QEMU, and `qcc` if the root shell lacks the SDP).
6. **Update the timings** the docs quote, from this run. Round them
   ("about 50 s", "about 1.5 min") and change a figure only when it moved
   noticeably:

   | What | Read it from | Write it in |
   |---|---|---|
   | Each suite's time | `run-all.sh`'s summary (seconds per run; includes `diskfull` when `vm/smallfs` was mounted) | `docs/dev/testing.md`, the Time column of the suites table |
   | The passthrough run | the `time` output of step 5 | `docs/dev/testing.md`, the sentence after the passthrough command |
   | The whole pass | the sum of `run-all.sh`'s runs | `docs/dev/testing.md` ("About N minutes without `diskfull`") and the header comment of `tests/integration/run-all.sh` |
   | Guest boot time | `grep -h 'booted in' vm/test-logs/*.log` | `docs/dev/test-environment.md` (section 5) and `docs/user/quickstart.md` (step 3) |

   Benchmark figures ({doc}`../user/performance`) are not timings of the
   tests: rerun `bench.sh` and update that page only when the data path
   changed (step 3).
7. **Build the docs** ({doc}`docs`): `cmake --build build-host --target
   docs`; no warnings. Open `build-host/docs/html/index.html` and check the
   version in the title.
8. **Install from a clean checkout**, to catch files that exist only in
   your working tree:

   ```
   rm -rf /tmp/fs9p-rel && git clone -q . /tmp/fs9p-rel && cd /tmp/fs9p-rel
   python3 -m venv .venv-docs && .venv-docs/bin/pip install -q -r docs/requirements.txt
   cmake -S . -B build-qnx -DCMAKE_TOOLCHAIN_FILE=cmake/qnx8-aarch64le.cmake
   cmake --build build-qnx && cmake --build build-qnx --target docs
   DESTDIR=/tmp/fs9p-rel/stage cmake --install build-qnx --prefix /opt/fs-9p
   find stage -type f | sort
   ```

   Expected: `opt/fs-9p/sbin/fs-9p`, `opt/fs-9p/share/fs-9p/fs-9p.build`,
   and under `opt/fs-9p/share/doc/fs-9p/`: `README.md`, `CHANGELOG.md`,
   `LICENSE`, the Markdown sources and `html/index.html` with the rest of
   the HTML. (`git clone .` clones the committed state, so commit first or
   clone the release commit.)
9. **Commit and tag:**

   ```
   git commit -am "Release X.Y.Z"
   git tag -a vX.Y.Z -m "fs-9p X.Y.Z"
   ```

10. **Open the next cycle:** add an empty `## [Unreleased]` section at the
   top of `CHANGELOG.md` and commit.

## Distributing

Nothing is published automatically. To hand a release to someone:

```
git archive --prefix=fs-9p-X.Y.Z/ -o fs-9p-X.Y.Z.tar.gz vX.Y.Z
```

The archive contains the sources and the Markdown docs; the recipient
builds the HTML with the `docs` target, or you send
`build-qnx/docs/html` as well. Publishing the repository or the HTML (e.g.
on GitHub) is a separate decision; record it in {doc}`architecture`
("Decisions and why") when it's made. Before publishing anything, check
the QNX license terms for material that could be seen as derived from the
SDK: the repository includes SDK headers by name and never contains
copies or sample code.
