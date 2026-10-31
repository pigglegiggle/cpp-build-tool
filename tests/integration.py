#!/usr/bin/env python3
"""Black-box checks using actual compiler/archive/linker subprocesses."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BINARY = Path(sys.argv[1]).resolve()
CHECKS = 0


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        raise AssertionError(message)


def run(root, *arguments, success=True):
    result = subprocess.run([str(BINARY), *arguments], cwd=root, text=True,
                            capture_output=True, timeout=25)
    check((result.returncode == 0) == success, f'{arguments}: {result.returncode}\n{result.stdout}\n{result.stderr}')
    return result


def build(root, *arguments, success=True):
    result = run(root, 'build', *arguments, '--jobs', '3', '--json', success=success)
    report = json.loads(result.stdout)
    check(report['success'] == success, 'report agrees with exit status')
    check(report['peak_workers'] <= 3, 'configured worker bound')
    return report


def write(root, path, content):
    file = root / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(content)


def fixture(root):
    root.mkdir()
    write(root, 'include/detail/config.hpp', '#pragma once\n#define FACTOR 2\n')
    write(root, 'include/math.hpp', '#pragma once\n#include "detail/config.hpp"\nint calculate(int);\nint extra();\n')
    write(root, 'src/math.cpp', '#include "math.hpp"\nint calculate(int value) { return value * FACTOR; }\n')
    write(root, 'src/extra.cpp', 'int extra() { return 3; }\n')
    write(root, 'src/main.cpp', '#include "math.hpp"\n#include <cstdio>\nint main() { std::printf("%d\\n", calculate(4) + extra()); }\n')
    write(root, 'Cppbuildfile', 'flags "-std=c++17" "-O0"\nincludes "include"\nlibrary "math"\nsources "src/math.cpp" "src/extra.cpp"\nprogram "hello"\nsources "src/main.cpp"\ndepends "math"\n')


def program(root):
    return subprocess.check_output([str(root / '.cppbuild/bin/hello')], text=True).strip()


def compiled(report):
    return {a['id'] for a in report['actions'] if a['id'].startswith('compile:') and a['status'] == 'built'}


def incremental(root):
    first = build(root)
    check(first['built'] == 5 and first['build_commands'] == 5 and first['dependency_scans'] == 3, 'initial compile/archive/link')
    check(program(root) == '11', 'first linked executable runs')
    unchanged = build(root)
    check(unchanged['up_to_date'] == 5 and unchanged['build_commands'] == 0 and unchanged['dependency_scans'] == 3, 'no-op still scans actual dependencies')
    source = root / 'src/extra.cpp'
    before = source.stat()
    source.write_text(source.read_text().replace('return 3', 'return 5'))
    os.utime(source, ns=(before.st_atime_ns, before.st_mtime_ns))
    changed = build(root)
    check(compiled(changed) == {'compile:src/extra.cpp'}, 'same-size same-mtime content edit detected')
    check(program(root) == '13', 'changed source reaches executable')
    write(root, 'include/detail/config.hpp', '#pragma once\n#define FACTOR 3\n')
    header = build(root)
    check(compiled(header) == {'compile:src/math.cpp', 'compile:src/main.cpp'}, 'transitive header consumers rebuilt, unrelated source reused')
    check(program(root) == '17', 'transitive header change reaches binary')
    source.touch()
    check(build(root)['build_commands'] == 0, 'mtime-only edit does not recompile')
    run(root, 'clean')
    restored = build(root)
    check(restored['restored'] == 5 and restored['build_commands'] == 0, 'clean outputs restore from verified cache')
    check(program(root) == '17', 'restored executable keeps executable permissions')
    (root / '.cppbuild/bin/hello').write_bytes(b'broken output')
    check(build(root)['restored'] == 1, 'modified final output repaired from cache')
    check(program(root) == '17', 'repaired executable runs')
    node_id = 'compile:src/extra.cpp'
    state = root / '.cppbuild/state' / (hashlib.sha256(node_id.encode()).hexdigest() + '.state')
    key = state.read_text().splitlines()[1]
    (root / '.cppbuild/cache' / key / 'artifact').write_bytes(b'corrupt artifact')
    object_file = root / '.cppbuild/objects' / (hashlib.sha256(str(source).encode()).hexdigest() + '.o')
    object_file.unlink()
    repaired = build(root)
    check(compiled(repaired) == {node_id}, 'corrupt cache entry triggers real compilation')
    check(program(root) == '17', 'cache corruption cannot silently poison executable')
    manifest = root / 'Cppbuildfile'
    manifest.write_text(manifest.read_text().replace('"-O0"', '"-O0" "-DUNUSED_SETTING=1"'))
    check(len(compiled(build(root))) == 3, 'compiler flag change invalidates all object keys')
    graph = json.loads(run(root, 'graph', '--json').stdout)
    check(len(graph['nodes']) == 5, 'graph includes compile/archive/link nodes')
    check('digraph build' in run(root, 'graph').stdout, 'DOT graph export')
    with (root / '.cppbuild/lock').open('r+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        check('another build' in run(root, 'build', success=False).stderr, 'single-writer workspace lock')


def failure_preservation(root):
    build(root)
    executable = root / '.cppbuild/bin/hello'
    old = executable.read_bytes()
    source = root / 'src/math.cpp'
    original = source.read_text()
    source.write_text('this is not valid C++\n')
    failed = build(root, success=False)
    check(failed['failed'] >= 1 and failed['blocked'] >= 1, 'compiler failure blocks dependent outputs')
    check(executable.read_bytes() == old and program(root) == '11', 'failed build preserves last executable')
    check(any('error:' in a['error'] for a in failed['actions']), 'compiler diagnostics retained')
    source.write_text(original)
    check(build(root)['build_commands'] == 0, 'original source reuses intact prior state')
    source.unlink()
    run(root, 'clean')
    check(not executable.exists(), 'clean works after a declared source is removed')
    source.write_text(original)


def shadowed_headers(root):
    root.mkdir()
    (root / 'first').mkdir()
    write(root, 'second/choice.hpp', '#define CHOICE 1\n')
    write(root, 'main.cpp', '#include <choice.hpp>\n#include <cstdio>\nint main(){std::printf("%d\\n",CHOICE);}\n')
    write(root, 'Cppbuildfile', 'includes "first" "second"\nprogram "hello"\nsources "main.cpp"\n')
    build(root)
    check(program(root) == '1', 'initial include search binding')
    write(root, 'first/choice.hpp', '#define CHOICE 2\n')
    check(len(compiled(build(root))) == 1, 'new earlier include path is detected without source edits')
    check(program(root) == '2', 'shadowing header changes actual program')


def escaped_paths(root):
    root.mkdir()
    write(root, 'include/space # $ &.hpp', '#define VALUE 7\n')
    write(root, 'main file.cpp', '#include "space # $ &.hpp"\n#include <cstdio>\nint main(){std::printf("%d\\n",VALUE);}\n')
    write(root, 'Cppbuildfile', 'includes "include"\nprogram "hello"\nsources "main file.cpp"\n')
    build(root)
    check(program(root) == '7', 'shell metacharacters and dependency escapes preserved')
    check(build(root)['build_commands'] == 0, 'escaped paths remain incremental')


def transitive_libraries(root):
    root.mkdir()
    write(root, 'b.cpp', 'int b(){return 2;}\n')
    write(root, 'a.cpp', 'int b();int a(){return b()+1;}\n')
    write(root, 'c.cpp', 'int b();int c(){return b()+2;}\n')
    write(root, 'main.cpp', '#include <cstdio>\nint a();int c();int main(){std::printf("%d\\n",a()+c());}\n')
    write(root, 'Cppbuildfile', 'library "b"\nsources "b.cpp"\nlibrary "a"\nsources "a.cpp"\ndepends "b"\nlibrary "c"\nsources "c.cpp"\ndepends "b"\nprogram "hello"\nsources "main.cpp"\ndepends "a" "c"\n')
    build(root)
    check(program(root) == '7', 'diamond static library dependencies link in correct order')
    graph = json.loads(run(root, 'graph', '--json').stdout)
    link = next(n for n in graph['nodes'] if n['id'] == 'link:hello')
    check(link['dependencies'][-1] == 'archive:b', 'shared dependency follows both dependents')


def malformed_and_clean(root):
    root.mkdir()
    write(root, 'a.cpp', 'int value;\n')
    write(root, 'Cppbuildfile', 'library "a"\nsources "a.cpp"\ndepends "b"\nlibrary "b"\nsources "a.cpp"\ndepends "a"\n')
    check('cycle' in run(root, 'build', success=False).stderr, 'target cycles rejected before execution')
    check(not (root / '.cppbuild').exists(), 'cycle rejection has no build-side effects')
    write(root, 'Cppbuildfile', 'program "hello"\nsources "../outside.cpp"\n')
    check('escapes project' in run(root, 'build', success=False).stderr, 'source traversal rejected')
    write(root, 'Cppbuildfile', 'flags "-oowned"\nprogram "hello"\nsources "a.cpp"\n')
    check('managed by cppbuild' in run(root, 'build', success=False).stderr, 'managed output options cannot be overridden')
    write(root, 'Cppbuildfile', 'library "safe"\nsources "a.cpp"\n')
    write(root, '.cppbuild/precious.txt', 'keep this user file')
    run(root, 'clean', '--all', success=False)
    check((root / '.cppbuild/precious.txt').read_text() == 'keep this user file', 'clean refuses unowned workspace')
    shutil.rmtree(root / '.cppbuild')
    build(root)
    outside = root.parent / 'outside-dir'
    outside.mkdir()
    (outside / 'keep').write_text('untouched')
    shutil.rmtree(root / '.cppbuild/lib')
    (root / '.cppbuild/lib').symlink_to(outside, target_is_directory=True)
    run(root, 'clean', '--all', success=False)
    check((outside / 'keep').read_text() == 'untouched', 'managed-directory symlink cannot redirect clean')


def keep_going(root):
    root.mkdir()
    write(root, 'bad.cpp', 'bad C++\n')
    write(root, 'good.cpp', 'int main(){return 0;}\n')
    write(root, 'Cppbuildfile', 'program "bad"\nsources "bad.cpp"\nprogram "good"\nsources "good.cpp"\n')
    result = build(root, 'all', '--keep-going', success=False)
    check(next(a for a in result['actions'] if a['id'] == 'link:good')['status'] == 'built', 'independent program completes with keep-going')


def cancellation_and_logs(root):
    fixture(root)
    compiler = shutil.which('clang++')
    check(compiler is not None, 'test compiler found')
    wrapper = root / 'compiler wrapper'
    wrapper.write_text('#!/usr/bin/env python3\nimport os,sys,time\nfrom pathlib import Path\n'
                       'if "-c" in sys.argv:\n'
                       ' Path(".cppbuild/started").write_text("yes")\n'
                       ' with open(".cppbuild/output-paths","a") as log: log.write(sys.argv[sys.argv.index("-o")+1]+"\\n")\n'
                       ' if Path(".cppbuild/noise").exists(): os.write(1,b"x"*2000000)\n'
                       ' while Path(".cppbuild/hold").exists(): time.sleep(.02)\n'
                       f'os.execv({compiler!r}, [{compiler!r}]+sys.argv[1:])\n')
    wrapper.chmod(0o755)
    manifest = root / 'Cppbuildfile'
    manifest.write_text('compiler "compiler wrapper"\n' + manifest.read_text())
    # A bare executable name is resolved through PATH; use an explicit relative path here.
    manifest.write_text(manifest.read_text().replace('"compiler wrapper"', '"./compiler wrapper"'))
    build(root)
    old = (root / '.cppbuild/bin/hello').read_bytes()
    write(root, 'src/extra.cpp', 'int extra(){return 9;}\n')
    write(root, '.cppbuild/hold', 'hold')
    (root / '.cppbuild/started').unlink()
    child = subprocess.Popen([str(BINARY), 'build', '--jobs', '2', '--json'], cwd=root,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 10
        while not (root / '.cppbuild/started').exists() and time.monotonic() < deadline:
            if child.poll() is not None:
                raise AssertionError(child.communicate())
            time.sleep(.02)
        check((root / '.cppbuild/started').exists(), 'compiler phase reached before cancellation')
        child.send_signal(signal.SIGINT)
        stdout, stderr = child.communicate(timeout=5)
        check(child.returncode == 130, f'Ctrl+C returns 130: {stderr}')
        check(not json.loads(stdout)['success'], 'cancelled report is unsuccessful')
        check((root / '.cppbuild/bin/hello').read_bytes() == old, 'cancellation preserves prior executable')
    finally:
        if child.poll() is None:
            child.kill()
            child.wait(timeout=5)
    (root / '.cppbuild/hold').unlink()
    write(root, '.cppbuild/noise', 'noise')
    build(root)
    logs = list((root / '.cppbuild/logs').glob('*.log'))
    check(max(p.stat().st_size for p in logs) <= 1_048_610, 'compiler diagnostics are bounded and drained')
    check(program(root) == '17', 'build recovers after cancellation and verbose output')
    outputs = (root / '.cppbuild/output-paths').read_text().splitlines()
    check(len(outputs) == len(set(outputs)), 'each compiler invocation has an isolated staging path')


with tempfile.TemporaryDirectory(prefix='cppbuild-tests-') as temporary:
    base = Path(temporary).resolve()
    for name, test in [('incremental', incremental), ('failure', failure_preservation),
                       ('shadow', shadowed_headers), ('space & $ # ไทย', escaped_paths),
                       ('transitive', transitive_libraries), ('malformed', malformed_and_clean),
                       ('keep-going', keep_going), ('cancellation', cancellation_and_logs)]:
        root = base / name
        if name in ('incremental', 'failure'):
            fixture(root)
        started = time.monotonic()
        test(root)
        print(f'PASS {name} ({time.monotonic()-started:.2f}s)', flush=True)
print(f'{CHECKS} integration checks passed')
