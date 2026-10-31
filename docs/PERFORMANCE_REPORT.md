# Recorded build observations

Recorded: 2026-11-01T04:39:06.628005+07:00

Platform: macOS-26.5.2-arm64-arm-64bit-Mach-O. Compiler: Apple clang version 21.0.0 (clang-2100.1.1.101).

13 translation units, four workers, three measured batches. Each build produced and ran a checked executable.

| Case | Source compilations | Artifact build commands | Cache restores | Median end-to-end ms |
|---|---:|---:|---:|---:|---:|
| full | 13 | 14 | 0 | 404.02 |
| unchanged | 0 | 0 | 0 | 182.11 |
| leaf_header | 1 | 2 | 0 | 254.96 |
| shared_header | 12 | 13 | 0 | 399.97 |
| cache_restore | 0 | 0 | 14 | 181.66 |

All cases still run compiler-driven dependency scans for each selected translation unit, including no-op and cache-restore builds. The main source does not include the shared header, so that edit recompiles 12 sources and relinks; the leaf header affects only one source.

These numbers are observations on this fixture, not proof that this tool beats make or Ninja. There is no competing-tool benchmark, filesystem cold-cache control, CPU affinity, thermal control or large-project scalability test. CLI/tool fingerprint overhead can dominate small builds. Parallel workers generate real compiler work, not simulated task durations.

Wall time includes process setup, manifest handling, tool hashes, dependency scanning, build actions and report persistence. The JSON report’s milliseconds field starts inside the build engine before tool fingerprinting and stops before final report persistence; individual action times exclude that shared setup.

Reproduce with `make benchmark`. [Raw batches, configurations and source/binary hashes](benchmarks/latest.json).
