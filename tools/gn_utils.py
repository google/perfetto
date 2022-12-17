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
import collections
from compat import iteritems
import errno
import filecmp
import json
import os
import re
import shutil
import subprocess
import sys
from typing import Dict
from typing import Optional
from typing import Set
from typing import Tuple

BUILDFLAGS_TARGET = '//gn:gen_buildflags'
GEN_VERSION_TARGET = '//src/base:version_gen_h'
TARGET_TOOLCHAIN = '//gn/standalone/toolchain:gcc_like_host'
HOST_TOOLCHAIN = '//gn/standalone/toolchain:gcc_like_host'
LINKER_UNIT_TYPES = ('executable', 'shared_library', 'static_library')

# TODO(primiano): investigate these, they require further componentization.
ODR_VIOLATION_IGNORE_TARGETS = {
    '//test/cts:perfetto_cts_deps',
    '//:perfetto_integrationtests',
}


def _check_command_output(cmd, cwd):
  try:
    output = subprocess.check_output(cmd, stderr=subprocess.STDOUT, cwd=cwd)
  except subprocess.CalledProcessError as e:
    print(
        'Command "{}" failed in {}:'.format(' '.join(cmd), cwd),
        file=sys.stderr)
    print(e.output.decode(), file=sys.stderr)
    sys.exit(1)
  else:
    return output.decode()


def repo_root():
  """Returns an absolute path to the repository root."""
  return os.path.join(
      os.path.realpath(os.path.dirname(__file__)), os.path.pardir)


def _tool_path(name, system_buildtools=False):
  # Pass-through to use name if the caller requests to use the system
  # toolchain.
  if system_buildtools:
    return [name]
  wrapper = os.path.abspath(
      os.path.join(repo_root(), 'tools', 'run_buildtools_binary.py'))
  return ['python3', wrapper, name]


def prepare_out_directory(gn_args,
                          name,
                          root=repo_root(),
                          system_buildtools=False):
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
      _tool_path('gn', system_buildtools) +
      ['gen', out, '--args=%s' % gn_args],
      cwd=repo_root())
  return out


def load_build_description(out, system_buildtools=False):
  """Creates the JSON build description by running GN."""
  desc = _check_command_output(
      _tool_path('gn', system_buildtools) +
      ['desc', out, '--format=json', '--all-toolchains', '//*'],
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


def build_targets(out, targets, quiet=False, system_buildtools=False):
  """Runs ninja to build a list of GN targets in the given out directory.

    Compiling these targets is required so that we can include any generated
    source files in the amalgamated result.
    """
  targets = [t.replace('//', '') for t in targets]
  with open(os.devnull, 'w') as devnull:
    stdout = devnull if quiet else None
    cmd = _tool_path('ninja', system_buildtools) + targets
    subprocess.check_call(cmd, cwd=os.path.abspath(out), stdout=stdout)


def compute_source_dependencies(out, system_buildtools=False):
  """For each source file, computes a set of headers it depends on."""
  ninja_deps = _check_command_output(
      _tool_path('ninja', system_buildtools) + ['-t', 'deps'], cwd=out)
  deps = {}
  current_source = None
  for line in ninja_deps.split('\n'):
    filename = os.path.relpath(os.path.join(out, line.strip()), repo_root())
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


def gen_buildflags(gn_args, target_file):
  """Generates the perfetto_build_flags.h for the given config.

    target_file: the path, relative to the repo root, where the generated
        buildflag header will be copied into.
    """
  tmp_out = prepare_out_directory(gn_args, 'tmp.gen_buildflags')
  build_targets(tmp_out, [BUILDFLAGS_TARGET], quiet=True)
  src = os.path.join(tmp_out, 'gen', 'build_config', 'perfetto_build_flags.h')
  shutil.copy(src, os.path.join(repo_root(), target_file))
  shutil.rmtree(tmp_out)


def check_or_commit_generated_files(tmp_files, check):
  """Checks that gen files are unchanged or renames them to the final location

    Takes in input a list of 'xxx.swp' files that have been written.
    If check == False, it renames xxx.swp -> xxx.
    If check == True, it just checks that the contents of 'xxx.swp' == 'xxx'.
    Returns 0 if no diff was detected, 1 otherwise (to be used as exit code).
    """
  res = 0
  for tmp_file in tmp_files:
    assert (tmp_file.endswith('.swp'))
    target_file = os.path.relpath(tmp_file[:-4])
    if check:
      if not filecmp.cmp(tmp_file, target_file):
        sys.stderr.write('%s needs to be regenerated\n' % target_file)
        res = 1
      os.unlink(tmp_file)
    else:
      os.rename(tmp_file, target_file)
  return res


class ODRChecker(object):
  """Detects ODR violations in linker units

  When we turn GN source sets into Soong & Bazel file groups, there is the risk
  to create ODR violations by including the same file group into different
  linker unit (this is because other build systems don't have a concept
  equivalent to GN's source_set). This class navigates the transitive
  dependencies (mostly static libraries) of a target and detects if multiple
  paths end up including the same file group. This is to avoid situations like:

  traced.exe -> base(file group)
  traced.exe -> libperfetto(static lib) -> base(file group)
  """

  def __init__(self, gn: 'GnParser', target_name: str):
    self.gn = gn
    self.root = gn.get_target(target_name)
    self.source_sets: Dict[str, Set[str]] = collections.defaultdict(set)
    self.deps_visited = set()
    self.source_set_hdr_only = {}

    self._visit(target_name)
    num_violations = 0
    if target_name in ODR_VIOLATION_IGNORE_TARGETS:
      return
    for sset, paths in self.source_sets.items():
      if self.is_header_only(sset):
        continue
      if len(paths) != 1:
        num_violations += 1
        print(
            'ODR violation in target %s, multiple paths include %s:\n  %s' %
            (target_name, sset, '\n  '.join(paths)),
            file=sys.stderr)
    if num_violations > 0:
      raise Exception('%d ODR violations detected. Build generation aborted' %
                      num_violations)

  def _visit(self, target_name: str, parent_path=''):
    target = self.gn.get_target(target_name)
    path = ((parent_path + ' > ') if parent_path else '') + target_name
    if not target:
      raise Exception('Cannot find target %s' % target_name)
    for ssdep in target.transitive_source_set_deps():
      name_and_path = '%s (via %s)' % (target_name, path)
      self.source_sets[ssdep.name].add(name_and_path)
    deps = set(target.non_proto_or_source_set_deps()).union(
        target.transitive_proto_deps()) - self.deps_visited
    for dep in deps:
      if dep.type == 'executable':
        continue  # Execs are strong boundaries and don't cause ODR violations.
      # static_library dependencies should reset the path. It doesn't matter if
      # we get to a source file via:
      # source_set1 > static_lib > source.cc OR
      # source_set1 > source_set2 > static_lib > source.cc
      # This is NOT an ODR violation because source.cc is linked from the same
      # static library
      next_parent_path = path if dep.type != 'static_library' else ''
      self.deps_visited.add(dep.name)
      self._visit(dep.name, next_parent_path)

  def is_header_only(self, source_set_name: str):
    cached = self.source_set_hdr_only.get(source_set_name)
    if cached is not None:
      return cached
    target = self.gn.get_target(source_set_name)
    if target.type != 'source_set':
      raise TypeError('%s is not a source_set' % source_set_name)
    res = all(src.endswith('.h') for src in target.sources)
    self.source_set_hdr_only[source_set_name] = res
    return res


class GnParser(object):
  """A parser with some cleverness for GN json desc files

    The main goals of this parser are:
    1) Deal with the fact that other build systems don't have an equivalent
       notion to GN's source_set. Conversely to Bazel's and Soong's filegroups,
       GN source_sets expect that dependencies, cflags and other source_set
       properties propagate up to the linker unit (static_library, executable or
       shared_library). This parser simulates the same behavior: when a
       source_set is encountered, some of its variables (cflags and such) are
       copied up to the dependent targets. This is to allow gen_xxx to create
       one filegroup for each source_set and then squash all the other flags
       onto the linker unit.
    2) Detect and special-case protobuf targets, figuring out the protoc-plugin
       being used.
    """

  class Target(object):
    """Reperesents A GN target.

        Maked properties are propagated up the dependency chain when a
        source_set dependency is encountered.
        """

    def __init__(self, name, type):
      self.name = name  # e.g. //src/ipc:ipc

      VALID_TYPES = ('static_library', 'shared_library', 'executable', 'group',
                     'action', 'source_set', 'proto_library', 'generated_file')
      assert (type in VALID_TYPES)
      self.type = type
      self.testonly = False
      self.toolchain = None

      # These are valid only for type == proto_library.
      # This is typically: 'proto', 'protozero', 'ipc'.
      self.proto_plugin: Optional[str] = None
      self.proto_paths = set()
      self.proto_exports = set()

      self.sources = set()
      # TODO(primiano): consider whether the public section should be part of
      # bubbled-up sources.
      self.public_headers = set()  # 'public'

      # These are valid only for type == 'action'
      self.data = set()
      self.inputs = set()
      self.outputs = set()
      self.script = None
      self.args = []
      self.custom_action_type = None
      self.python_main = None

      # These variables are propagated up when encountering a dependency
      # on a source_set target.
      self.cflags = set()
      self.defines = set()
      self.deps: Set[GnParser.Target] = set()
      self.transitive_deps: Set[GnParser.Target] = set()
      self.libs = set()
      self.include_dirs = set()
      self.ldflags = set()

      # Deps on //gn:xxx have this flag set to True. These dependencies
      # are special because they pull third_party code from buildtools/.
      # We don't want to keep recursing into //buildtools in generators,
      # this flag is used to stop the recursion and create an empty
      # placeholder target once we hit //gn:protoc or similar.
      self.is_third_party_dep_ = False

    def non_proto_or_source_set_deps(self):
      return set(d for d in self.deps
                 if d.type != 'proto_library' and d.type != 'source_set')

    def proto_deps(self):
      return set(d for d in self.deps if d.type == 'proto_library')

    def transitive_proto_deps(self):
      return set(d for d in self.transitive_deps if d.type == 'proto_library')

    def transitive_cpp_proto_deps(self):
      return set(
          d for d in self.transitive_deps if d.type == 'proto_library' and
          d.proto_plugin != 'descriptor' and d.proto_plugin != 'source_set')

    def transitive_source_set_deps(self):
      return set(d for d in self.transitive_deps if d.type == 'source_set')

    def __lt__(self, other):
      if isinstance(other, self.__class__):
        return self.name < other.name
      raise TypeError(
          '\'<\' not supported between instances of \'%s\' and \'%s\'' %
          (type(self).__name__, type(other).__name__))

    def __repr__(self):
      return json.dumps({
          k: (list(sorted(v)) if isinstance(v, set) else v)
          for (k, v) in iteritems(self.__dict__)
      },
                        indent=4,
                        sort_keys=True)

    def update(self, other):
      for key in ('cflags', 'data', 'defines', 'deps', 'include_dirs',
                  'ldflags', 'transitive_deps', 'libs', 'proto_paths'):
        self.__dict__[key].update(other.__dict__.get(key, []))

  def __init__(self, gn_desc):
    self.gn_desc_ = gn_desc
    self.all_targets = {}
    self.linker_units = {}  # Executables, shared or static libraries.
    self.source_sets = {}
    self.actions = {}
    self.proto_libs = {}

  def get_target(self, gn_target_name: str) -> Target:
    """Returns a Target object from the fully qualified GN target name.

        It bubbles up variables from source_set dependencies as described in the
        class-level comments.
        """
    target = self.all_targets.get(gn_target_name)
    if target is not None:
      return target  # Target already processed.

    desc = self.gn_desc_[gn_target_name]
    target = GnParser.Target(gn_target_name, desc['type'])
    target.testonly = desc.get('testonly', False)
    target.toolchain = desc.get('toolchain', None)
    self.all_targets[gn_target_name] = target

    # We should never have GN targets directly depend on buidtools. They
    # should hop via //gn:xxx, so we can give generators an opportunity to
    # override them.
    assert (not gn_target_name.startswith('//buildtools'))

    # Don't descend further into third_party targets. Genrators are supposed
    # to either ignore them or route to other externally-provided targets.
    if gn_target_name.startswith('//gn'):
      target.is_third_party_dep_ = True
      return target

    proto_target_type, proto_desc = self.get_proto_target_type(target)
    if proto_target_type:
      assert proto_desc
      self.proto_libs[target.name] = target
      target.type = 'proto_library'
      target.proto_plugin = proto_target_type
      target.proto_paths.update(self.get_proto_paths(proto_desc))
      target.proto_exports.update(self.get_proto_exports(proto_desc))
      target.sources.update(proto_desc.get('sources', []))
      assert (all(x.endswith('.proto') for x in target.sources))
    elif target.type == 'source_set':
      self.source_sets[gn_target_name] = target
      target.sources.update(desc.get('sources', []))
      target.inputs.update(desc.get('inputs', []))
    elif target.type in LINKER_UNIT_TYPES:
      self.linker_units[gn_target_name] = target
      target.sources.update(desc.get('sources', []))
    elif target.type == 'action':
      self.actions[gn_target_name] = target
      target.data.update(desc.get('metadata', {}).get('perfetto_data', []))
      target.inputs.update(desc.get('inputs', []))
      target.sources.update(desc.get('sources', []))
      outs = [re.sub('^//out/.+?/gen/', '', x) for x in desc['outputs']]
      target.outputs.update(outs)
      target.script = desc['script']
      # Args are typically relative to the root build dir (../../xxx)
      # because root build dir is typically out/xxx/).
      target.args = [re.sub('^../../', '//', x) for x in desc['args']]
      action_types = desc.get('metadata',
                              {}).get('perfetto_action_type_for_generator', [])
      target.custom_action_type = action_types[0] if len(
          action_types) > 0 else None
      python_main = desc.get('metadata', {}).get('perfetto_python_main', [])
      target.python_main = python_main[0] if python_main else None

    # Default for 'public' is //* - all headers in 'sources' are public.
    # TODO(primiano): if a 'public' section is specified (even if empty), then
    # the rest of 'sources' is considered inaccessible by gn. Consider
    # emulating that, so that generated build files don't end up with overly
    # accessible headers.
    public_headers = [x for x in desc.get('public', []) if x != '*']
    target.public_headers.update(public_headers)

    target.cflags.update(desc.get('cflags', []) + desc.get('cflags_cc', []))
    target.libs.update(desc.get('libs', []))
    target.ldflags.update(desc.get('ldflags', []))
    target.defines.update(desc.get('defines', []))
    target.include_dirs.update(desc.get('include_dirs', []))

    # Recurse in dependencies.
    for dep_name in desc.get('deps', []):
      dep = self.get_target(dep_name)

      # generated_file targets only exist for GN builds: we can safely ignore
      # them.
      if dep.type == 'generated_file':
        continue

      # When a proto_library depends on an action, that is always the "_gen"
      # rule of the action which is "private" to the proto_library rule.
      # therefore, just ignore it for dep tracking purposes.
      if dep.type == 'action' and proto_target_type is not None:
        target_no_toolchain = label_without_toolchain(target.name)
        dep_no_toolchain = label_without_toolchain(dep.name)
        assert (dep_no_toolchain == f'{target_no_toolchain}_gen')
        continue

      # Non-third party groups are only used for bubbling cflags etc so don't
      # add a dep.
      if dep.type == 'group' and not dep.is_third_party_dep_:
        target.update(dep)  # Bubble up groups's cflags/ldflags etc.
        continue

      # Linker units act as a hard boundary making all their internal deps
      # opaque to the outside world. For this reason, do not propogate deps
      # transitively across them.
      if dep.type in LINKER_UNIT_TYPES:
        target.deps.add(dep)
        continue

      if dep.type == 'source_set':
        target.update(dep)  # Bubble up source set's cflags/ldflags etc.
      elif dep.type == 'proto_library':
        target.proto_paths.update(dep.proto_paths)

      target.deps.add(dep)
      target.transitive_deps.add(dep)
      target.transitive_deps.update(dep.transitive_deps)

    return target

  def get_proto_exports(self, proto_desc):
    # exports in metadata will be available for source_set targets.
    metadata = proto_desc.get('metadata', {})
    return metadata.get('exports', [])

  def get_proto_paths(self, proto_desc):
    # import_dirs in metadata will be available for source_set targets.
    metadata = proto_desc.get('metadata', {})
    return metadata.get('import_dirs', [])

  def get_proto_target_type(self, target: Target
                           ) -> Tuple[Optional[str], Optional[Dict]]:
    """ Checks if the target is a proto library and return the plugin.

        Returns:
            (None, None): if the target is not a proto library.
            (plugin, proto_desc) where |plugin| is 'proto' in the default (lite)
            case or 'protozero' or 'ipc' or 'descriptor'; |proto_desc| is the GN
            json desc of the target with the .proto sources (_gen target for
            non-descriptor types or the target itself for descriptor type).
        """
    parts = target.name.split('(', 1)
    name = parts[0]
    toolchain = '(' + parts[1] if len(parts) > 1 else ''

    # Descriptor targets don't have a _gen target; instead we look for the
    # characteristic flag in the args of the target itself.
    desc = self.gn_desc_.get(target.name)
    if '--descriptor_set_out' in desc.get('args', []):
      return 'descriptor', desc

    # Source set proto targets have a non-empty proto_library_sources in the
    # metadata of the description.
    metadata = desc.get('metadata', {})
    if 'proto_library_sources' in metadata:
      return 'source_set', desc

    # In all other cases, we want to look at the _gen target as that has the
    # important information.
    gen_desc = self.gn_desc_.get('%s_gen%s' % (name, toolchain))
    if gen_desc is None or gen_desc['type'] != 'action':
      return None, None
    args = gen_desc.get('args', [])
    if '/protoc' not in args[0]:
      return None, None
    plugin = 'proto'
    for arg in (arg for arg in args if arg.startswith('--plugin=')):
      # |arg| at this point looks like:
      #  --plugin=protoc-gen-plugin=gcc_like_host/protozero_plugin
      # or
      #  --plugin=protoc-gen-plugin=protozero_plugin
      plugin = arg.split('=')[-1].split('/')[-1].replace('_plugin', '')
    return plugin, gen_desc
