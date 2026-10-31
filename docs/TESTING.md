# Testing and measurement scope

## Executed on this development Mac

- `make test`: **8,154 unit checks** and **119 integration checks** passed.
- `make sanitize`: the same unit and integration suites passed AddressSanitizer and UndefinedBehaviorSanitizer.
- `make demo`: built and ran the included executable against a real static library.
- `make benchmark`: three real-build measurement batches, with every resulting executable checked. See PERFORMANCE_REPORT.md for observations and exact boundaries.

## Unit proof

SHA-256 tests use standard empty/abc/million-a vectors and chunked updates. Eighty seeded random DAGs check topological edges and worker execution prerequisites. A synchronization barrier proves four worker actions can overlap; independent tests check worker limits, fail-fast behavior, keep-going branches, cancellation and cycle/invalid-edge rejection. Dependency parser tests cover escaped spaces/punctuation, dollar signs, continuation lines and duplicate input paths.

These are assertions over actual state and independently checkable invariants, not generated test-count filler. Random graph seed: 20261101.

## Black-box compiler proof

The Python harness uses temporary projects and real clang++/ar/linker subprocesses. It verifies:

- Full build, repeated no-op, source edit with unchanged size/mtime, mtime-only touch and changed flags.
- Transitive header invalidation and newly introduced earlier include-search matches.
- Output-clean/cache restoration, executable permissions, tampered outputs and corrupt cache fallback.
- Static-library dependency diamonds and successful executable output.
- Spaces and shell metacharacters in paths without invoking a shell.
- Manifest cycles/path traversal/managed-flag rejection before build work.
- Safe cleaning after source removal and of owned data, rejection of unowned/redirected workspaces and concurrent-writer locking.
- Failed compilation with retained diagnostics and preservation of the old executable.
- Keep-going completion of an independent executable after another target fails.
- Ctrl+C while a controlled compiler wrapper is pending, exit code 130, preserved old executable and subsequent recovery.
- More than 1 MiB of compiler output being bounded in storage while still drained, plus isolated staging names per compiler invocation.

The compiler wrapper exists only in temporary tests to control cancellation/output timing; production actions invoke the configured real tools. Benchmark durations are real build measurements, not wrapper-generated sleep times.

## Reproduce

```sh
make test
make sanitize
make demo
make benchmark
```

These commands do not need third-party Python packages. Integration fixtures and benchmark source trees are temporary and cleaned up; test binaries, example outputs and caches are ignored. Public raw observations under docs/benchmarks are retained intentionally.

## Limits

Linux/WSL, GCC-specific driver behavior, native Windows, remote filesystems, physical power cuts, adversarial concurrent workspace modification, exhaustive filesystem mutation races, system-library updates under an unchanged toolchain and large production dependency graphs were not validated here. No comparative make/Ninja performance claim is made. Threads and compiler children can consume more memory than the scheduler's own structures; no total-RSS bound is claimed.
