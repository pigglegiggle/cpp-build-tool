# How to run C++ Build Tool

## Setup and first run

Install the macOS command-line compiler tools if needed, then enter this repository:

```sh
xcode-select --install
make
make demo
```

The first `make` builds this tool using the repository Makefile. `make demo` then runs **our tool** to build the separate example. Those are two different build layers. The example executable prints `Hello from C++ Build Tool`.

```sh
./build/cppbuild build --file examples/hello/Cppbuildfile --jobs 4 --explain
./examples/hello/.cppbuild/bin/hello
```

`--file` locates a manifest; all project paths and compiler working-directory semantics are relative to that manifest's canonical root. Without it, the tool reads `Cppbuildfile` in the current directory. A directory path is not a manifest file.

## CLI reference

```text
cppbuild build [target|all] [--file Cppbuildfile] [--jobs N]
               [--keep-going] [--explain] [--json]
cppbuild graph [target|all] [--file Cppbuildfile] [--json]
cppbuild clean [--file Cppbuildfile] [--all]
```

The default selected target is the first program; when no program exists, the first library is used. `all` selects every target. Target dependencies must name static libraries. `--jobs` accepts 1–32; default is hardware concurrency clamped to that range. `--keep-going` permits independent pending branches to continue after failure. Dependencies of failed actions never run.

`--json` writes the build report to stdout instead of human output; fatal configuration/setup errors go to stderr and may occur before any report is created. Normal build failure still emits its action report. Exit codes are 0 on success, 1 on failure and 130 on signal cancellation. `--explain` shows why each action was reused/restored/rebuilt. Successful compiler warnings and other command output can be inspected under `.cppbuild/logs/`; failed action diagnostics are also in the JSON report and human stderr.

Graph is read-only and prints DOT, or JSON with `--json`. It does not create a generated workspace or execute compilers. It still validates the manifest and declared source paths. DOT can be rendered with Graphviz if installed separately; Graphviz is not required to build or use this tool.

## Manifest reference

```text
compiler "clang++"
archiver "ar"
flags "-std=c++17" "-O2" "-Wall" "-Wextra"
link_flags "-pthread"
includes "include" "third_party/include"

library "math"
sources "src/math.cpp" "src/vector.cpp"

program "app"
sources "src/main.cpp"
depends "math"
```

| Directive | Meaning |
|---|---|
| `compiler` | One executable name/path; default clang++ |
| `archiver` | One POSIX ar-compatible executable; default ar |
| `flags` | Project-wide compile flags, also passed to executable links; default C++17 / O2 |
| `link_flags` | Additional executable-link arguments after objects/libraries |
| `includes` | Existing project-relative include directories; emitted as separate `-I` arguments |
| `program` | Start a named executable target |
| `library` | Start a named static-library target |
| `sources` | One or more existing .cpp/.cc/.cxx files; may appear repeatedly under a target |
| `depends` | Static-library target names; may appear repeatedly under a target |

Project settings must precede the first target and may each appear once. Omitting `flags` uses the default vector; an empty `flags` directive deliberately clears it. Every target needs at least one source. Names begin with a letter and contain letters, digits, `_` or `-`, at most 64 characters. There are at most 256 targets and 4,096 distinct canonical sources.

Arguments use C++ `std::quoted`-style strings, not shell syntax or JSON escape processing. Write UTF-8 text directly. Backslash escapes the next character inside a quoted argument; quote and backslash therefore need escaping. `#` begins a comment only outside quoted strings. Paths with spaces, `#`, `$` and `&` are passed literally. Globs, environment expansion and command substitution are not implemented.

Source/include paths must remain within the project after canonicalization, and cannot reference reserved `.cppbuild` paths. The same source can be shared across targets because flags are project-wide. Generated sources/headers must already exist. Target-specific flags, custom build recipes, modules, shared libraries and package installation are not implemented.

The tool owns `-c`, `-o*`, `-M*` handling and rejects manifest flags overriding these or using response-file `@` arguments. Other flags are passed to trusted tools; this is not a sandbox. Compiler/linker options may have side effects, so review manifests before running them.

## What gets built

```text
.cppbuild/
  owner                 Canonical project owner marker
  lock                  Single-writer flock inode
  objects/              One object path per selected source
  lib/                  lib<target>.a
  bin/                  <program>
  state/                Last successful action keys/digests
  cache/<key>/          Verified artifact + cache state
  work/                 Per-invocation temporary files
  logs/                 Per-action bounded command output
  last-build.json       Latest action report
```

The cache stores complete artifacts; it is local to a project root and has no eviction daemon. A moved/copied generated workspace is rejected when its owner path no longer matches. Prefer a clean checkout after moving, or explicitly remove only its generated `.cppbuild` directory after confirming it contains no original files.

## Incremental and cache demonstration

1. Build `examples/hello` and run its executable.
2. Repeat with `--explain`: valid outputs become `up_to_date`, with zero build commands. Dependency scans still run.
3. Edit `src/greeting.cpp`: that object is reconsidered; changed artifacts invalidate the archive and link by content.
4. Edit a used header: all consuming source actions are reconsidered, including transitive includes.
5. Run clean without `--all`, then build: artifacts can be restored from cache.
6. Change source/flags back to an earlier version: that version can also hit the local action cache.

An input edit can produce identical object bytes; downstream archive/link actions may remain up to date when their actual input content and command did not change. The tool follows bytes, not a blanket “every ancestor must always rebuild” rule.

```sh
./build/cppbuild clean --file examples/hello/Cppbuildfile
./build/cppbuild build --file examples/hello/Cppbuildfile --json
./build/cppbuild clean --file examples/hello/Cppbuildfile --all
```

Clean uses the manifest file location without parsing its contents or requiring source files to exist. It removes generated outputs/state/work/logs. `--all` additionally clears the cache. Both retain the lock inode and owner marker. They reject unrecognized workspaces and managed-directory symlinks. Keep original data outside `.cppbuild`.

## Linux and Windows

Ubuntu prerequisites:

```sh
sudo apt update
sudo apt install build-essential clang python3
make
make test
```

Windows uses WSL2/Ubuntu, not native MSVC. Install WSL from administrator PowerShell:

```powershell
wsl --install -d Ubuntu
```

Finish setup and use the Linux commands inside Ubuntu. Keep builds in the Linux filesystem rather than a Windows-mounted directory when measuring performance. These paths were not executed on this Mac. POSIX `posix_spawn`, process groups, `flock` and directory syncing are part of this implementation.

## Troubleshooting

| Symptom | Check |
|---|---|
| `Nothing to be done` | The repository's Makefile is up to date; run `make demo` or build/cppbuild. |
| Manifest not found | Run from its directory or supply `--file` with the actual file path. |
| Unknown target/dependency | Check names; dependency targets must be libraries. |
| Dependency cycle | Break the target cycle; unresolved descendants may also appear in the message. |
| Recompiles after environment change | Keys hash most environment entries conservatively; use consistent launch environments. |
| No-op still takes time | Compiler scans and content/tool hashing are deliberate work, not a cached mtime-only shortcut. |
| Link error after a passing compile | Inspect the link action diagnostics and library declarations/order. |
| Unrecognized workspace | Do not adopt arbitrary .cppbuild contents; use a known generated workspace. |
| Workspace belongs elsewhere | A canonical root changed after moving; recreate generated state deliberately. |
| Another build owns workspace | Wait for it to finish; builds and cleans share one writer lock. |
| Prior binary still exists after failure | Publication is per successful action; the last linked binary is deliberately preserved. |
| Missing or corrupt cache artifact | The tool recompiles rather than trusting it; inspect disk/permissions. |
| No space left on device | Reclaim storage, then retry; cache growth is not automatically evicted. |
