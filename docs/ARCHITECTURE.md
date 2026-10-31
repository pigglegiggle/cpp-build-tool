# Architecture and invariants

## Project and action graphs

`read_project` parses a bounded manifest with quoted arguments. Paths are canonicalized under the manifest root, and source/include paths cannot use the managed `.cppbuild` tree. Target names are unique; dependencies name existing static libraries. Project-wide flags make one object action per canonical source sufficient even when several targets share that source.

`graph` first builds the target adjacency list and validates it with Kahn's algorithm. A min-priority queue supplies deterministic index tie-breaking. If some nodes remain unresolved, the manifest contains a cycle; the error names unresolved targets, which may include descendants of the cycle. This is cycle detection, not a minimal-cycle finder.

After selecting a target closure, compile actions are shared by source. Archive actions consume their own objects. Executable link actions consume own objects and all transitive library artifacts, ordered in reverse target topological order so dependent libraries precede their dependencies for one-pass linkers. A library archive does not pack its dependent libraries into itself. Independent selected libraries still build as separate actions.

The exported graph contains runnable compile/archive/link actions only. Header dependencies are discovered inputs of compile actions, not separate runnable nodes. The explanatory README drawing includes those file-level edges.

## Compiler dependency discovery

Every selected translation unit runs the configured driver with `-M -MT cppbuild -MF <scratch>` and the same project flags/include directories. The depfile parser supports escaped spaces, escaped punctuation, continued lines and doubled dollar signs; paths are canonicalized, sorted and deduplicated. `-M` includes system headers. The fresh scan detects new files earlier in include search paths, a case that a stale depfile plus content hashes alone would miss.

A real compile uses `-c -MD -MT cppbuild -MF <scratch> -o <scratch>`. The compile-produced dependency set must equal the scan's set before publication. Source/header content is fingerprinted again after compilation. Changed input content or dependency sets reject that action's publication. There is no generated-header scheduling or coordinated filesystem snapshot, so input trees should remain stable throughout a build.

Scans also happen on unchanged/cache-hit builds. A no-op report can therefore show zero build commands and several dependency scans. This trades startup work for direct compiler-driven dependency correctness.

## Action keys and state

SHA-256 fields use length delimiters so concatenated values are unambiguous. A key contains an action format version, logical ID, tool fingerprint, environment digest, normalized argument vector and sorted input paths/content hashes. Logical output/depfile placeholders avoid invalidating keys merely because scratch filenames differ.

Compile keys fingerprint source and compiler-reported headers. Archive/link keys fingerprint prerequisite artifacts. Tool identity includes executable path, file content and version-command output. Driver symlink names are preserved when executing because `clang++` can behave differently from `clang`; file hashing still follows the selected executable file.

Environment entries are sorted and hashed, except PWD, OLDPWD, SHLVL, `_` and TERM_SESSION_ID. This is conservative: unrelated environment changes can invalidate cache keys. Environment values themselves are not saved in state files. This is not a hermetic toolchain: shared tool libraries, wrapper internals, plugins, system linker inputs and time-dependent macros are not comprehensively tracked.

Each action's state records its last key and output digest in a small versioned file. A local action-cache directory stores the artifact and matching state under its key. State files are bounded and validated. Existing outputs and cache artifacts are hashed before trust; timestamps are not used as content identity. Missing/corrupt state or artifacts trigger restoration/rebuild rather than silent acceptance.

## Publication and locking

`Workspace` checks the generated-directory marker and canonical owner root, rejects managed directory symlinks and acquires a nonblocking exclusive `flock`. The lock fd is close-on-exec and remains on the same inode during clean. A second build/clean fails instead of modifying the same workspace concurrently.

Scratch names are unique per invocation and process, keeping leftover compiler output from a killed process isolated from subsequent builds. On normal exit, scratch depfiles and staged output are removed. Unexpected parent termination may leave scratch files or compiler descendants; process supervision after SIGKILL is not guaranteed. `clean` can reclaim generated scratch files.

Commands write staged output, never the last committed artifact directly. After a successful exit and input checks, output is copied to a same-directory temporary file, synced and atomically renamed. Artifact/cache/state writes use individual replacements and parent-directory syncing. There is no multi-file or whole-build transaction: independent actions already committed are not rolled back if another action fails, or if a later state/report persistence step fails. A directory-sync error after rename can mean the replacement has already happened.

Cache contents can be sensitive and are not authenticated against another actor who can alter the entire workspace. SHA-256 corruption checks are not a remote-cache trust system. Filesystem/hardware power-loss guarantees were not physically tested.

## Scheduler ownership

One mutex protects action states, pending prerequisite counts, reverse edges, the ready heap, remaining-action count and active-worker gauge. Workers remove a ready action while holding that mutex, perform hashing/scanning/subprocess work outside it, then publish their result under the mutex. Successful prerequisites decrement pending counts and release newly ready users.

States are pending, running, built, restored, up_to_date, failed or blocked. A failure blocks pending descendants. Without `--keep-going`, every pending action is blocked; already running independent work can finish. With keep-going, unaffected branches remain eligible. Worker count is 1–32, capped by the selected action count. Thread creation failures request cancellation and join already-created workers before propagating the error.

## Process lifecycle and cancellation

Compiler commands are argument vectors passed to `posix_spawn`, not shell text. Subprocess stdin is `/dev/null`; stdout/stderr share a captured pipe. Launch setup is serialized so pipe close-on-exec flags are set before another thread spawns. A child starts a new process group.

Output is drained with nonblocking reads and short polls. At most 1 MiB is retained per subprocess; excess bytes are still read and discarded. Read work per iteration is bounded so cancellation remains observable during verbose output. A pipe that reaches EOF is no longer polled for readiness while its child is being reaped.

Signal handlers set a lock-free atomic cancellation flag. The scheduler blocks pending work; active process calls signal child groups with SIGTERM and escalate to SIGKILL after one second. Worker threads are joined before the CLI exits with 130. A trusted custom compiler that leaves descendants holding output fds can delay normal completion until cancellation; arbitrary process wrappers are not sandboxed.

## Reporting

`.cppbuild/last-build.json` and `--json` expose the same bounded action report. Action durations include that action's scanning/hashing/cache/command work. The run milliseconds field includes tool fingerprint setup and scheduled actions, ending before report persistence. External benchmark wall time also includes CLI parsing/startup and report persistence. `peak_workers` is active action workers, including dependency scans and cache verification, not a CPU-utilization metric or peak simultaneous compiler count.
