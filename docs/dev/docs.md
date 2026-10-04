# Updating the documentation

## What lives where

| Reader | Where | Source |
|---|---|---|
| Someone on the target | `use fs-9p` | `src/fs-9p.use.in` |
| A visitor to the repository | `README.md` | `README.md` |
| System integrators | User Guide (HTML) | `docs/user/*.md` |
| The maintainer | Developer Guide (HTML) | `docs/dev/*.md` |
| Everyone | Changelog (HTML and file) | `CHANGELOG.md`, included by `docs/changelog.md` |
| Image builders | `share/fs-9p/fs-9p.build` | `packaging/fs-9p.build` |
| Comments for the next developer | the code | `src/`, `lib9p/` |

The Developer Guide is also the project's record: decisions and their
reasons in {doc}`architecture`, every QNX API the transport uses and every
surprise in {doc}`virtio-notes`, the message mapping in {doc}`op-mapping`.
Keep them current with the code ({doc}`workflow`).

## The toolchain

Sphinx with the MyST parser (Markdown instead of reStructuredText) and the
furo theme, pinned in `docs/requirements.txt`, producing HTML only (no
PDF). A docs-only dependency: fs-9p's build never needs it. Set it up once, in the source directory:

```
python3 -m venv .venv-docs
.venv-docs/bin/pip install -r docs/requirements.txt
```

`.venv-docs/` is ignored by git, and can't be moved or copied (it refers
to its own absolute path): create it again in another tree. CMake looks for `sphinx-build` there (and
on `PATH`) when a build directory is configured; configure again
(`cmake -S . -B build-host`) if the venv was made afterwards.

## Building and previewing

```
cmake --build build-host --target docs
xdg-open build-host/docs/html/index.html
```

The `docs` target is never part of the default build. It runs
`sphinx-build -W --keep-going`: any warning (a broken `{doc}` or `{ref}`
link, a page missing from every toctree, a heading-level jump) fails the
build and is listed. The same target exists in `build-qnx`;
`cmake --install` copies the HTML to `share/doc/fs-9p/html/` when it was
built, and skips it otherwise.

Without CMake:

```
.venv-docs/bin/sphinx-build -W -b html docs /tmp/fs9p-html
```

## Writing

- One Markdown file per page; the first `#` heading is the page title.
  A new page must be added to the `toctree` in `docs/user/index.md` or
  `docs/dev/index.md`.
- Links between pages: `` {doc}`behaviour` `` (same directory),
  `` {doc}`../user/reference` `` (the other guide). Links to a section:
  put a label above it, `(my-label)=`, and link with `` {ref}`my-label` ``.
  The build checks both kinds.
- Definition lists (`term` on one line, `: definition` on the next) are
  enabled and used by the reference page; so are `:::` fences.
- The User Guide follows the style of the QNX documentation: tasks first,
  short sentences, commands to copy, and the reference page in the layout
  of the QNX Utilities Reference (Syntax, Runs on, Options, Description,
  Examples, Exit status, Errors, Caveats, See also).
- State what was verified and how. Claims about QEMU or QNX behaviour
  should come from a test or an experiment; say so when one doesn't.

## Keeping things in step

Some facts are written in more than one place, on purpose: the `use`
message must stand alone on the target.

| Fact | Places | Checked by |
|---|---|---|
| The mount options | `main.c`, `src/fs-9p.use.in`, `docs/user/reference.md` | The `consistency` ctest: every option `main.c` accepts must appear in the other two. Descriptions and defaults are checked by eye. |
| The version | `CMakeLists.txt` only; `CHANGELOG.md` must have its section | The `consistency` ctest. |
| Error messages at startup | `main.c`, `transport_virtio.c`, `docs/user/reference.md` ("Errors"), `docs/user/troubleshooting.md` | By eye. |
| Abilities needed without root | `src/fs-9p.use.in`, `docs/user/integrating.md`, `docs/user/reference.md` | By eye. |
| Performance figures | `docs/user/performance.md` | Rerun `bench.sh` when the data path changes. |
| Message mapping | `docs/dev/op-mapping.md` | By eye, with each new 9P message ({doc}`workflow`). |

The `consistency` check is part of the host build's ctest, so a missing
option or changelog section fails `ctest` before anything is committed.
