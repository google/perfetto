#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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
"""
Enforce import rules for https://ui.perfetto.dev.
"""

import sys
import os
import re
import collections
import argparse
import fnmatch

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI_SRC_DIR = os.path.join(ROOT_DIR, 'ui', 'src')

NODE_MODULES = '%node_modules%'  # placeholder to depend on any node module.

# The format of this array is: (src) -> (dst).
# If src or dst are arrays, the semantic is the cartesian product, e.g.:
# [a,b] -> [c,d] is equivalent to allowing a>c, a>d, b>c, b>d.
DEPS_ALLOWLIST = [
    # Everything can depend on base/, protos and NPM packages.
    ('*', ['/base/*', '/protos/index', '/gen/perfetto_version', NODE_MODULES]),

    # Integration tests can depend on everything.
    ('/test/*', '*'),

    # Dependencies allowed for internal UI code.
    (
        [
            '/frontend/*',
            '/core/*',
            '/common/*',
        ],
        [
            '/frontend/*',
            '/core/*',
            '/common/*',
            '/public/*',
            '/trace_processor/*',
            '/widgets/*',
            '/protos/*',
            '/gen/perfetto_version',
        ],
    ),

    # /public (interfaces + lib) can depend only on a restricted surface.
    ('/public/*', ['/base/*', '/trace_processor/*']),

    # /public/lib can also depend on the plublic interface and widgets.
    ('/public/lib/*', ['/public/*', '/frontend/widgets/*', '/widgets/*']),

    # /plugins (and core_plugins) can depend only on a restricted surface.
    (
        '/*plugins/*',
        [
            '/base/*',
            '/public/*',
            '/trace_processor/*',
            '/widgets/*',
            '/frontend/widgets/*',
        ],
    ),

    # Extra dependencies allowed for core_plugins only.
    # TODO(priniano): remove this entry to figure out what it takes to move the
    # remaining /core_plugins to /plugins and get rid of core_plugins.
    (
        ['/core_plugins/*'],
        ['/core/*', '/frontend/*', '/common/actions'],
    ),

    # Miscl legitimate deps.
    ('/frontend/index', ['/gen/*']),
    ('/traceconv/index', '/gen/traceconv'),
    ('/engine/wasm_bridge', '/gen/trace_processor'),
    ('/trace_processor/sql_utils/*', '/trace_processor/*'),
    ('/protos/index', '/gen/protos'),

    # ------ Technical debt that needs cleaning up below this point ------

    # TODO(primiano): this dependency for BaseSliceTrack & co needs to be moved
    # to /public/lib or something similar.
    ('/*plugins/*', '/frontend/*track'),

    # TODO(primiano): clean up generic_slice_details_tab.
    ('/*plugins/*', '/frontend/generic_slice_details_tab'),

    # TODO(primiano): these dependencies require a discussion with stevegolton@.
    # unclear if they should be moved to public/lib/* or be part of the
    # {Base/Named/Slice}Track overhaul.
    ('/*plugins/*', [
        '/frontend/slice_layout',
        '/frontend/slice_args',
        '/frontend/checkerboard',
        '/common/track_helper',
        '/common/track_data',
    ]),

    # TODO(primiano): clean up dependencies on feature flags.
    (['/public/lib/colorizer'], '/core/feature_flags'),

    # TODO(primiano): Record page-related technical debt.
    ('/frontend/record*', '/controller/*'),
    ('/frontend/permalink', '/controller/*'),
    ('/common/*', '/controller/record_config_types'),
    ('/controller/index', '/common/recordingV2/target_factories/index'),
    ('/common/recordingV2/*', '/controller/*'),
    ('/controller/record_controller*', '*'),
    ('/controller/adb_*', '*'),
    ('/chrome_extension/chrome_tracing_controller', '/controller/*'),
    ('/chrome_extension/chrome_tracing_controller', '/core/trace_config_utils'),

    # TODO(primiano): query-table tech debt.
    (
        '/public/lib/query_table/query_table',
        ['/frontend/*', '/core/app_impl', '/core/router'],
    ),

    # TODO(primiano): tracks tech debt.
    ('/public/lib/tracks/*', [
        '/frontend/base_counter_track',
        '/frontend/slice_args',
        '/frontend/tracks/custom_sql_table_slice_track',
        '/frontend/tracks/generic_slice_details_tab',
    ]),

    # TODO(primiano): controller-related tech debt.
    ('/frontend/index', '/controller/*'),
    ('/controller/*', ['/base/*', '/core/*', '/common/*']),

    # TODO(primiano): check this with stevegolton@. Unclear if widgets should
    # be allowed to depend on trace_processor.
    ('/widgets/vega_view', '/trace_processor/*'),

    # Bigtrace deps.
    ('/bigtrace/*', ['/base/*', '/widgets/*', '/trace_processor/*']),

    # TODO(primiano): rationalize recordingv2. RecordingV2 is a mess of subdirs.
    ('/common/recordingV2/*', '/common/recordingV2/*'),

    # TODO(primiano): misc tech debt.
    ('/public/lib/extensions', '/frontend/*'),
    ('/bigtrace/index', ['/core/live_reload', '/core/raf_scheduler']),
    ('/plugins/dev.perfetto.HeapProfile/*', '/frontend/trace_converter'),
]


def all_source_files():
  for root, dirs, files in os.walk(UI_SRC_DIR, followlinks=False):
    for name in files:
      if name.endswith('.ts') and not name.endswith('.d.ts'):
        yield os.path.join(root, name)


def is_dir(path, cache={}):
  try:
    return cache[path]
  except KeyError:
    result = cache[path] = os.path.isdir(path)
    return result


def remove_prefix(s, prefix):
  return s[len(prefix):] if s.startswith(prefix) else s


def remove_suffix(s, suffix):
  return s[:-len(suffix)] if s.endswith(suffix) else s


def normalize_path(path):
  return remove_suffix(remove_prefix(path, UI_SRC_DIR), '.ts')


def find_plugin_declared_deps(path):
  """Returns the set of deps declared by the plugin (if any)

  It scans the plugin/index.ts file, and resolves the declared dependencies,
  working out the path of the plugin we depend on (by looking at the imports).
  Returns a tuple of the form (src_plugin_path, set{dst_plugin_path})
  Where:
    src_plugin_path: is the normalized path of the input (e.g. /plugins/foo)
    dst_path: is the normalized path of the declared dependency.
  """
  src = normalize_path(path)
  src_plugin = get_plugin_path(src)
  if src_plugin is None or src != src_plugin + '/index':
    # If the file is not a plugin, or is not the plugin index.ts, bail out.
    return
  # First extract all the default-imports in the file. Usually there is one for
  # each imported plugin, of the form:
  # import ThreadPlugin from '../plugins/dev.perfetto.Thread'
  import_map = {}  # 'ThreadPlugin' -> '/plugins/dev.perfetto.Thread'
  for (src, target, default_import) in find_imports(path):
    target_plugin = get_plugin_path(target)
    if default_import is not None or target_plugin is not None:
      import_map[default_import] = target_plugin

  # Now extract the declared dependencies for the plugin. This looks for the
  # statement 'static readonly dependencies = [ThreadPlugin]'. It can be broken
  # down over multiple lines, so we approach this in two steps. First we find
  # everything within the square brackets; then we remove spaces and \n and
  # tokenize on commas
  with open(path) as f:
    s = f.read()
  DEP_REGEX = r'^\s*static readonly dependencies\s*=\s*\[([^\]]*)\]'
  all_deps = re.findall(DEP_REGEX, s, flags=re.MULTILINE)
  if len(all_deps) == 0:
    return
  if len(all_deps) > 1:
    raise Exception('Ambiguous plugin deps in %s: %s' % (path, all_deps))
  declared_deps = re.sub('\s*', '', all_deps[0]).split(',')
  for imported_as in declared_deps:
    resolved_dep = import_map.get(imported_as)
    if resolved_dep is None:
      raise Exception('Could not resolve import %s in %s' % (imported_as, src))
    yield (src_plugin, resolved_dep)


def find_imports(path):
  src = normalize_path(path)
  directory, _ = os.path.split(src)
  with open(path) as f:
    s = f.read()
  for m in re.finditer(
      "^import\s+([^;]+)\s+from\s+'([^']+)';$", s, flags=re.MULTILINE):
    # Flatten multi-line imports into one line, removing spaces. The resulting
    # import line can look like:
    # '{foo,bar,baz}' in most cases
    # 'DefaultImportName' when doing import DefaultImportName from '...'
    # 'DefaultImportName,{foo,bar,bar}' when doing a mixture of the above.
    imports = re.sub('\s', '', m[1])
    default_import = (re.findall('^\w+', imports) + [None])[0]

    # Normalize the imported file
    target = m[2]
    if target.startswith('.'):
      target = os.path.normpath(os.path.join(directory, target))
      if is_dir(UI_SRC_DIR + target):
        target = os.path.join(target, 'index')

    yield (src, target, default_import)


def path_to_id(path):
  path = path.replace('/', '_')
  path = path.replace('-', '_')
  path = path.replace('@', '_at_')
  path = path.replace('.', '_')
  return path


def is_external_dep(path):
  return not path.startswith('/')


def write_dot(graph, f):
  print('digraph g {', file=f)
  for node, edges in graph.items():
    node_id = path_to_id(node)
    shape = 'rectangle' if is_external_dep(node) else 'ellipse'
    print(f'{node_id} [shape={shape}, label="{node}"];', file=f)

    for edge in edges:
      edge_id = path_to_id(edge)
      print(f'{node_id} -> {edge_id};', file=f)
  print('}', file=f)


def get_plugin_path(path):
  m = re.match('^(/(?:core_)?plugins/([^/]+))/.*', path)
  return m.group(1) if m is not None else None


def flatten_rules(rules):
  flat_deps = []
  for rule_src, rule_dst in rules:
    src_list = rule_src if isinstance(rule_src, list) else [rule_src]
    dst_list = rule_dst if isinstance(rule_dst, list) else [rule_dst]
    for src in src_list:
      for dst in dst_list:
        flat_deps.append((src, dst))
  return flat_deps


def get_node_modules(graph):
  """Infers the dependencies onto NPM packages (node_modules)

  An import is guessed to be a node module if doesn't contain any . or .. in the
  path, and optionally starts with @.
  """
  node_modules = set()
  for _, imports in graph.items():
    for dst in imports:
      if re.match(r'^[@a-z][a-z0-9-_/]+$', dst):
        node_modules.add(dst)
  return node_modules


def check_one_import(src, dst, allowlist, plugin_declared_deps, node_modules):
  # Translate node_module deps into the wildcard '%node_modules%' so it can be
  # treated as a single entity.
  if dst in node_modules:
    dst = NODE_MODULES

  # Always allow imports from the same directory or its own subdirectories.
  src_dir = '/'.join(src.split('/')[:-1])
  dst_dir = '/'.join(dst.split('/')[:-1])
  if dst_dir.startswith(src_dir):
    return True

  # Match against the (flattened) allowlist.
  for rule_src, rule_dst in allowlist:
    if fnmatch.fnmatch(src, rule_src) and fnmatch.fnmatch(dst, rule_dst):
      return True

  # Check inter-plugin deps.
  src_plugin = get_plugin_path(src)
  dst_plugin = get_plugin_path(dst)
  extra_err = ''
  if src_plugin is not None and dst_plugin is not None:
    if src_plugin == dst_plugin:
      # Allow a plugin to depends on arbitrary subdirectories of itself.
      return True
    # Check if there is a dependency declared by plugins, via
    # static readonly dependencies = [DstPlugin]
    declared_deps = plugin_declared_deps.get(src_plugin, set())
    extra_err = '(plugin deps: %s)' % ','.join(declared_deps)
    if dst_plugin in declared_deps:
      return True
  print('Import not allowed %s -> %s %s' % (src, dst, extra_err))
  return False


def do_check(_options, graph):
  result = 0
  rules = flatten_rules(DEPS_ALLOWLIST)
  node_modules = get_node_modules(graph)

  # Build a map of depencies declared between plugin. The maps looks like:
  # 'Foo' -> {'Bar', 'Baz'}  # Foo declares a dependency on Bar and Baz
  plugin_declared_deps = collections.defaultdict(set)
  for path in all_source_files():
    for src_plugin, dst_plugin in find_plugin_declared_deps(path):
      plugin_declared_deps[src_plugin].add(dst_plugin)

  for src, imports in graph.items():
    for dst in imports:
      if not check_one_import(src, dst, rules, plugin_declared_deps,
                              node_modules):
        result = 1
  return result


def do_desc(options, graph):
  print('Rules:')
  for rule in flatten_rules(DEPS_ALLOWLIST):
    print(' - %s' % rule)


def do_print(options, graph):
  for node, edges in graph.items():
    for edge in edges:
      print("{}\t{}".format(node, edge))


def do_dot(options, graph):

  def simplify(path):
    if is_external_dep(path):
      return path
    return os.path.dirname(path)

  new_graph = collections.defaultdict(set)
  for node, edges in graph.items():
    for edge in edges:
      new_graph[simplify(edge)]
      new_graph[simplify(node)].add(simplify(edge))
  graph = new_graph

  if options.ignore_external:
    new_graph = collections.defaultdict(set)
    for node, edges in graph.items():
      if is_external_dep(node):
        continue
      for edge in edges:
        if is_external_dep(edge):
          continue
        new_graph[edge]
        new_graph[node].add(edge)
    graph = new_graph

  write_dot(graph, sys.stdout)
  return 0


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.set_defaults(func=do_check)
  subparsers = parser.add_subparsers()

  check_command = subparsers.add_parser(
      'check', help='Check the rules (default)')
  check_command.set_defaults(func=do_check)

  desc_command = subparsers.add_parser('desc', help='Print the rules')
  desc_command.set_defaults(func=do_desc)

  print_command = subparsers.add_parser('print', help='Print all imports')
  print_command.set_defaults(func=do_print)

  dot_command = subparsers.add_parser(
      'dot',
      help='Output dependency graph in dot format suitble for use in ' +
      'graphviz (e.g. ./tools/check_imports dot | dot -Tpng -ograph.png)')
  dot_command.set_defaults(func=do_dot)
  dot_command.add_argument(
      '--ignore-external',
      action='store_true',
      help='Don\'t show external dependencies',
  )

  # This is a general import graph of the form /plugins/foo/index -> /base/hash
  graph = collections.defaultdict(set)

  # Build the dep graph
  for path in all_source_files():
    for src, target, _ in find_imports(path):
      graph[src].add(target)
      graph[target]

  options = parser.parse_args()
  return options.func(options, graph)


if __name__ == '__main__':
  sys.exit(main())
