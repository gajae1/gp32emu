import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

source = Path('/tmp/gp32-round4')
build = Path('/tmp/gp32-round4-build-ubsan-final')
expected_head = 'eebb337b7133fa19b330fe82fd9f002fa215b8b4'
expected_tree = 'dd3b0d94445c3053d95faba2d59fd71fd7d9df65'
cmake = '/tmp/gp32-round4-tools/cmake/data/bin/cmake'
ctest = '/tmp/gp32-round4-tools/cmake/data/bin/ctest'
compiler = '/usr/bin/gcc'
sanitizer = '-fsanitize=undefined -fno-sanitize-recover=undefined'
ubsan_options = 'halt_on_error=1:print_stacktrace=1'

def output(args):
    return subprocess.check_output(args, cwd=source, text=True).strip()

def stamp():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

head = output(['git', 'rev-parse', 'HEAD'])
tree = output(['git', 'rev-parse', 'HEAD^{tree}'])
if (head, tree) != (expected_head, expected_tree):
    raise SystemExit(f'Source identity changed: {head} / {tree}')
subprocess.run(['git', 'diff', '--exit-code', expected_head, '--',
                '.', ':(exclude)docs/**'], cwd=source, check=True)
build.mkdir()
evidence = build / 'verification'
evidence.mkdir()
shutil.copyfile(__file__, evidence / 'runner.py')

configure = [cmake, '-S', str(source), '-B', str(build), '-G', 'Unix Makefiles',
    '-DCMAKE_C_COMPILER=' + compiler,
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DGP32EMU_REQUIRE_C23=ON',
    '-DGP32EMU_BUILD_TESTS=ON',
    '-DGP32EMU_BUILD_LIBRETRO=ON',
    '-DGP32EMU_CPU_PROFILE=ON',
    '-DGP32EMU_ENABLE_IPO=OFF',
    '-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
    '-DCMAKE_C_FLAGS=' + sanitizer,
    '-DCMAKE_EXE_LINKER_FLAGS=' + sanitizer,
    '-DCMAKE_SHARED_LINKER_FLAGS=' + sanitizer]
compile_command = [cmake, '--build', str(build), '-j2']
test_command = [ctest, '--test-dir', str(build), '--output-on-failure', '-j2']
environment = dict(os.environ, UBSAN_OPTIONS=ubsan_options)
metadata = {
    'started_utc': stamp(), 'source_directory': str(source),
    'source_head': head, 'source_tree': tree,
    'source_status_at_start': output(['git', 'status', '--short']),
    'build_directory': str(build), 'build_directory_created_fresh': True,
    'compiler_path': compiler, 'compiler_resolved': str(Path(compiler).resolve()),
    'compiler_version': output([compiler, '--version']),
    'compiler_target': output([compiler, '-dumpmachine']),
    'libubsan': output([compiler, '-print-file-name=libubsan.so']),
    'cmake_version': output([cmake, '--version']),
    'ctest_version': output([ctest, '--version']),
    'host': dict(platform.uname()._asdict()),
    'UBSAN_OPTIONS': ubsan_options,
    'expected_tests': 54,
    'configure_command': configure,
    'build_command': compile_command,
    'ctest_command': test_command,
    'stages': []
}

def save_metadata():
    (evidence / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')

def run_logged(name, args):
    entry = {'name': name, 'command': args, 'started_utc': stamp()}
    metadata['stages'].append(entry)
    save_metadata()
    print(f'{name}: started {entry["started_utc"]}', flush=True)
    start = time.monotonic()
    with (evidence / f'{name}.log').open('wb') as log:
        result = subprocess.run(args, cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT)
    entry.update(returncode=result.returncode, finished_utc=stamp(),
                 elapsed_seconds=round(time.monotonic() - start, 3))
    save_metadata()
    print(f'{name}: exit={result.returncode} elapsed={entry["elapsed_seconds"]}s', flush=True)
    return result.returncode

save_metadata()
result = run_logged('configure', configure)
if result == 0:
    shutil.copyfile(build / 'CMakeCache.txt', evidence / 'CMakeCache.txt')
    result = run_logged('test-inventory', [ctest, '--test-dir', str(build), '--show-only=json-v1'])
    if result == 0:
        inventory = json.loads((evidence / 'test-inventory.log').read_text())
        metadata['observed_tests'] = len(inventory['tests'])
        metadata['test_names'] = [test['name'] for test in inventory['tests']]
        save_metadata()
        if metadata['observed_tests'] != metadata['expected_tests']:
            print(f'Unexpected test count: {metadata["observed_tests"]}', flush=True)
            result = 3
if result == 0:
    result = run_logged('build', compile_command)
if result == 0:
    result = run_logged('ctest', test_command)

for name in ['LastTest.log', 'LastTestsFailed.log', 'CTestCostData.txt']:
    log = build / 'Testing' / 'Temporary' / name
    if log.exists():
        shutil.copyfile(log, evidence / name)
commands_file = build / 'compile_commands.json'
if commands_file.exists():
    shutil.copyfile(commands_file, evidence / 'compile_commands.json')
    commands = json.loads(commands_file.read_text())
    metadata['compile_command_count'] = len(commands)
    metadata['all_compile_commands_c23'] = all('-std=c2x' in item['command'] or '-std=c23' in item['command'] for item in commands)
    metadata['all_compile_commands_ubsan'] = all('-fsanitize=undefined' in item['command'] and
        '-fno-sanitize-recover=undefined' in item['command'] for item in commands)
metadata['source_head_at_end'] = output(['git', 'rev-parse', 'HEAD'])
metadata['source_tree_at_end'] = output(['git', 'rev-parse', 'HEAD^{tree}'])
metadata['source_diff_from_frozen'] = output(['git', 'diff', '--name-only', expected_head, '--',
    '.', ':(exclude)docs/**'])
metadata['source_status_at_end'] = output(['git', 'status', '--short'])
metadata['finished_utc'] = stamp()
if metadata['source_diff_from_frozen']:
    print('Compiled source changed during verification', flush=True)
    result = 4
metadata['returncode'] = result
metadata['log_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(evidence.glob('*.log'))}
save_metadata()
print(f'Matrix finished: exit={result}; evidence={evidence}', flush=True)
sys.exit(result)
