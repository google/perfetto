# Copyright (C) 2019 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# A collection of utilities for extracting build rule information from GN
# projects.

from __future__ import print_function
import errno
import filecmp
import json
import os
import re
import shutil
import subprocess
import sys


def _check_command_output(cmd, cwd):
  try:
    output = subprocess.check_output(
        cmd, stderr=subprocess.STDOUT, cwd=cwd)
  except subprocess.CalledProcessError as e:
    print('Command "{}" failed in {}:'.format(' '.join(cmd), cwd),
          file=sys.stderr)
    print(e.output, file=sys.stderr)
    sys.exit(1)
  else:
    return output


def repo_root():
    """Returns an absolute path to the repository root."""
    return os.path.join(
        os.path.realpath(os.path.dirname(__file__)), os.path.pardir)


def _tool_path(name):
    return os.path.join(repo_root(), 'tools', name)


def prepare_out_directory(gn_args, name, root=repo_root()):
    """Creates the JSON build description by running GN.

    Returns (path, desc) where |path| is the location of the output directory
    and |desc| is the JSON build description.
    """
    out = os.path.join(root, 'out', name)
    try:
        os.makedirs(out)
    except OSError as e:
        if e.errno != errno.EEXIST:
            raise
    _check_command_output(
        [_tool_path('gn'), 'gen', out, '--args=%s' % gn_args], cwd=repo_root())
    return out


def load_build_description(out):
    """Creates the JSON build description by running GN."""
    desc = _check_command_output(
        [_tool_path('gn'), 'desc', out, '--format=json',
         '--all-toolchains', '//*'],
        cwd=repo_root())
    return json.loads(desc)


def create_build_description(gn_args, root=repo_root()):
    """Prepares a GN out directory and loads the build description from it.

    The temporary out directory is automatically deleted.
    """
    out = prepare_out_directory(gn_args, 'tmp.gn_utils', root=root)
    try:
        return load_build_description(out)
    finally:
        shutil.rmtree(out)


def build_targets(out, targets, quiet=False):
    """Runs ninja to build a list of GN targets in the given out directory.

    Compiling these targets is required so that we can include any generated
    source files in the amalgamated result.
    """
    targets = [t.replace('//', '') for t in targets]
    with open(os.devnull, 'rw') as devnull:
        stdout = devnull if quiet else None
        subprocess.check_call([_tool_path('ninja')] + targets, cwd=out,
                              stdout=stdout)


def compute_source_dependencies(out):
    """For each source file, computes a set of headers it depends on."""
    ninja_deps = _check_command_output(
        [_tool_path('ninja'), '-t', 'deps'], cwd=out)
    deps = {}
    current_source = None
    for line in ninja_deps.split('\n'):
        filename = os.path.relpath(os.path.join(out, line.strip()))
        if not line or line[0] != ' ':
            current_source = None
            continue
        elif not current_source:
            # We're assuming the source file is always listed before the
            # headers.
            assert os.path.splitext(line)[1] in ['.c', '.cc', '.cpp', '.S']
            current_source = filename
            deps[current_source] = []
        else:
            assert current_source
            deps[current_source].append(filename)
    return deps


def label_to_path(label):
    """Turn a GN output label (e.g., //some_dir/file.cc) into a path."""
    assert label.startswith('//')
    return label[2:]


def label_without_toolchain(label):
    """Strips the toolchain from a GN label.

    Return a GN label (e.g //buildtools:protobuf(//gn/standalone/toolchain:
    gcc_like_host) without the parenthesised toolchain part.
    """
    return label.split('(')[0]


def label_to_target_name_with_path(label):
  """
  Turn a GN label into a target name involving the full path.
  e.g., //src/perfetto:tests -> src_perfetto_tests
  """
  name = re.sub(r'^//:?', '', label)
  name = re.sub(r'[^a-zA-Z0-9_]', '_', name)
  return name

def check_or_commit_generated_files(tmp_files, check):
    """Checks that gen files are unchanged or renames them to the final location

    Takes in input a list of 'xxx.swp' files that have been written.
    If check == False, it renames xxx.swp -> xxx.
    If check == True, it just checks that the contents of 'xxx.swp' == 'xxx'.
    Returns 0 if no diff was detected, 1 otherwise (to be used as exit code).
    """
    res = 0
    for tmp_file in tmp_files:
        assert(tmp_file.endswith('.swp'))
        target_file = os.path.relpath(tmp_file[:-4])
        if check:
            if not filecmp.cmp(tmp_file, target_file):
                sys.stderr.write('%s needs to be regenerated\n' % target_file)
                res = 1
            os.unlink(tmp_file)
        else:
            os.rename(tmp_file, target_file)
    return res