#!/usr/bin/env python3
"""Measure real builds, incremental invalidation and verified cache restoration."""
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/cppbuild').resolve()


def call(root, *arguments):
    started = time.perf_counter()
    completed = subprocess.run([str(BINARY), *arguments], cwd=root, text=True,
                               capture_output=True, timeout=60, check=True)
    return completed.stdout, (time.perf_counter() - started) * 1000


def generate(root):
    (root / 'include').mkdir()
    (root / 'src').mkdir()
    (root / 'include/shared.hpp').write_text('#pragma once\n#define FACTOR 2\n')
    (root / 'include/leaf.hpp').write_text('#pragma once\n#define LEAF 3\n')
    sources = []
    for index in range(12):
        path = f'src/part{index}.cpp'
        sources.append(path)
        includes = '#include "shared.hpp"\n' + ('#include "leaf.hpp"\n' if index == 0 else '')
        expression = 'FACTOR + LEAF' if index == 0 else f'FACTOR + {index}'
        (root / path).write_text(includes + f'int part{index}() {{ return {expression}; }}\n')
    sources.append('src/main.cpp')
    declarations = ''.join(f'int part{i}();\n' for i in range(12))
    total = '+'.join(f'part{i}()' for i in range(12))
    (root / 'src/main.cpp').write_text('#include <cstdio>\n' + declarations + f'int main(){{std::printf("%d\\n",{total});}}\n')
    (root / 'Cppbuildfile').write_text('flags "-std=c++17" "-O2"\nincludes "include"\nprogram "fixture"\nsources '
                                     + ' '.join(json.dumps(source) for source in sources) + '\n')


def main():
    observations = []
    with tempfile.TemporaryDirectory(prefix='cppbuild-benchmark-') as temporary:
        root = Path(temporary).resolve()
        generate(root)
        for batch in range(3):
            if (root / '.cppbuild').exists():
                call(root, 'clean', '--all')
            (root / 'include/shared.hpp').write_text('#pragma once\n#define FACTOR 2\n')
            (root / 'include/leaf.hpp').write_text('#pragma once\n#define LEAF 3\n')
            cases = [('full', None), ('unchanged', None),
                     ('leaf_header', ('include/leaf.hpp', '#pragma once\n#define LEAF 7\n')),
                     ('shared_header', ('include/shared.hpp', '#pragma once\n#define FACTOR 5\n')),
                     ('cache_restore', None)]
            for name, change in cases:
                if change:
                    (root / change[0]).write_text(change[1])
                if name == 'cache_restore':
                    call(root, 'clean')
                raw, wall = call(root, 'build', '--jobs', '4', '--json')
                result = json.loads(raw)
                value = subprocess.check_output([str(root / '.cppbuild/bin/fixture')], text=True).strip()
                expected = 12 * (5 if name in ('shared_header', 'cache_restore') else 2) + sum(range(12)) + (3 if name in ('full', 'unchanged') else 7)
                assert result['success'] and value == str(expected), (name, result, value)
                compilations = sum(a['status'] == 'built' and a['id'].startswith('compile:') for a in result['actions'])
                assert compilations == {'full': 13, 'unchanged': 0, 'leaf_header': 1, 'shared_header': 12, 'cache_restore': 0}[name]
                if name == 'cache_restore':
                    assert result['restored'] == 14 and result['build_commands'] == 0
                observations.append({'batch': batch + 1, 'case': name, 'wall_ms': wall, 'program_output': value,
                                     'compiled_sources': compilations, 'report': result})
    source_files = [*ROOT.glob('src/*.cpp'), *ROOT.glob('include/cppbuild/*.hpp'), Path(__file__).resolve()]
    report = {'recorded_at': datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=7))).isoformat(),
              'platform': platform.platform(), 'compiler': subprocess.check_output(['clang++', '--version'], text=True).splitlines()[0],
              'binary_sha256': hashlib.sha256(BINARY.read_bytes()).hexdigest(),
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_files},
              'method': 'Three batches, each with a fresh full build followed by unchanged, leaf-header, shared-header and output-clean/cache-restore cases. 13 real translation units, 4 workers. Wall time includes CLI setup, tool hashing, dependency scanning, build actions and report persistence. Every executable result is checked. No make/Ninja comparison, cold filesystem cache control, CPU affinity or statistical confidence claim.',
              'observations': observations}
    output = ROOT / 'docs/benchmarks/latest.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    import statistics
    lines = ['# Recorded build observations', '', f"Recorded: {report['recorded_at']}", '',
             f"Platform: {report['platform']}. Compiler: {report['compiler']}.", '',
             '13 translation units, four workers, three measured batches. Each build produced and ran a checked executable.', '',
             '| Case | Source compilations | Artifact build commands | Cache restores | Median end-to-end ms |',
             '|---|---:|---:|---:|---:|---:|']
    for name in ('full', 'unchanged', 'leaf_header', 'shared_header', 'cache_restore'):
        rows = [r for r in observations if r['case'] == name]
        representative = rows[0]
        lines.append(f"| {name} | {representative['compiled_sources']} | {representative['report']['build_commands']} | {representative['report']['restored']} | {statistics.median(r['wall_ms'] for r in rows):.2f} |")
    lines += ['', 'All cases still run compiler-driven dependency scans for each selected translation unit, including no-op and cache-restore builds. The main source does not include the shared header, so that edit recompiles 12 sources and relinks; the leaf header affects only one source.', '',
              'These numbers are observations on this fixture, not proof that this tool beats make or Ninja. There is no competing-tool benchmark, filesystem cold-cache control, CPU affinity, thermal control or large-project scalability test. CLI/tool fingerprint overhead can dominate small builds. Parallel workers generate real compiler work, not simulated task durations.', '',
              'Wall time includes process setup, manifest handling, tool hashes, dependency scanning, build actions and report persistence. The JSON report’s milliseconds field starts inside the build engine before tool fingerprinting and stops before final report persistence; individual action times exclude that shared setup.', '',
              'Reproduce with `make benchmark`. [Raw batches, configurations and source/binary hashes](benchmarks/latest.json).']
    (ROOT / 'docs/PERFORMANCE_REPORT.md').write_text('\n'.join(lines) + '\n')
    print(output)
    print(ROOT / 'docs/PERFORMANCE_REPORT.md')


if __name__ == '__main__':
    main()
