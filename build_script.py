#!/usr/bin/env python3

import argparse
import os
import re
import shutil
import subprocess
import sys


def job_count() -> int:
    try:
        match = re.search(r'(?m)^Cpus_allowed:\s*(.*)$',
                          open('/proc/self/status').read())
        if match:
            count = bin(int(match.group(1).replace(',', ''), 16)).count('1')
            if count > 0:
                return count + 1
    except IOError:
        pass

    try:
        import multiprocessing
        return multiprocessing.cpu_count() + 1
    except (ImportError, NotImplementedError):
        pass

    return 3


def cmake(source_dir: str, install_dir: str, cwd: str) -> bool:
    cmake_process = subprocess.Popen(['cmake', source_dir, '-G', 'Unix Makefiles',
                                      '-D', 'CMAKE_INSTALL_PREFIX=' + install_dir,
                                      '-D', 'CMAKE_BUILD_TYPE=' + args.build_type],
                                     stdout=subprocess.PIPE, cwd=cwd,
                                     universal_newlines=True)

    while True:
        output: str = cmake_process.stdout.readline().strip()
        if output is not None:
            print(output)
        returncode = cmake_process.poll()

        if returncode is not None:
            return returncode == 0


def make_and_install(cwd: str) -> bool:
    make_process = subprocess.Popen(['make', '-j', str(job_count()), 'install'],
                                    stdout=subprocess.PIPE, cwd=cwd,
                                    universal_newlines=True)

    while True:
        output = make_process.stdout.readline().strip()
        match = re.match(r'^\[\s*(\d+)%]', output)

        if match is not None:
            progress = match.group(1)
            print('\rProgress: ' + progress, end='%')
        elif output is not None:
            print(output)

        returncode = make_process.poll()

        if returncode is not None:
            print('')
            print('Done!\n')
            return returncode == 0


parser = argparse.ArgumentParser(prog='BrainFuck build script',
                                 description='Build the BrainFuck compiler',
                                 epilog='Defaults to building in release mode.')

build_type_group = parser.add_mutually_exclusive_group()
build_type_group.add_argument('-d',
                              '--debug',
                              action='store_const',
                              const='Debug',
                              dest='build_type',
                              help='build in debug mode')
build_type_group.add_argument('-r',
                              '--release',
                              action='store_const',
                              const='Release',
                              dest='build_type',
                              help='build in release mode')

parser.add_argument('--build-llvm',
                    action='store_true',
                    dest='should_build_llvm',
                    help='build the llvm libraries')

parser.set_defaults(build_type='Release')

args = parser.parse_args()


repo_dir = os.path.dirname(os.path.realpath(__file__))

build_dir = os.path.join(repo_dir, 'build')
cmake_dir = os.path.join(build_dir, 'cmake')
bin_dir = os.path.join(build_dir, 'bin')

llvm_repo_dir = os.path.join(repo_dir, 'vendor/llvm')
llvm_llvm_dir = os.path.join(llvm_repo_dir, 'llvm')
llvm_build_dir = os.path.join(llvm_llvm_dir, 'build')
llvm_install_dir = os.path.join(llvm_build_dir, 'install')

if args.should_build_llvm or not os.path.exists(llvm_build_dir):
    os.makedirs(llvm_install_dir, exist_ok=True)

    if not cmake(llvm_llvm_dir, llvm_install_dir, llvm_build_dir):
        print('Something went wrong while generating the llvm Makefiles.', file=sys.stderr)
        exit(1)

    print('Building the llvm library.')

    make_and_install(llvm_build_dir)

if os.path.exists(build_dir):
    shutil.rmtree(build_dir)

os.makedirs(cmake_dir)
os.makedirs(bin_dir)

if not cmake(repo_dir, build_dir, cmake_dir):
    print('Something went wrong while generating the llvm Makefiles.', file=sys.stderr)
    exit(1)

print('Building the BrainFuck compiler.')

make_and_install(cmake_dir)

