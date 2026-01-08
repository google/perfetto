#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# disibuted under the License is disibuted on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import html
import sys
import json
from typing import Any, List, Dict, Set
from collections import defaultdict

INTRODUCTION = '''
# PerfettoSQL standard library
*This page documents the PerfettoSQL standard library.*

## Introduction
The PerfettoSQL standard library is a repository of tables, views, functions
and macros, contributed by domain experts, which make querying traces easier.
Its design is heavily inspired by standard libraries in languages like Python,
C++ and Java.

Some of the purposes of the standard library include:
1) Acting as a way of sharing and commonly written queries without needing
to copy/paste large amounts of SQL.
2) Raising the abstraction level when exposing data in the trace. Many
modules in the standard library convert low-level trace concepts
e.g. slices, tracks and into concepts developers may be more familar with
e.g. for Android developers: app startups, binder transactions etc.

Standard library modules can be included as follows:
```
-- Include all tables/views/functions from the android.startup.startups
-- module in the standard library.
INCLUDE PERFETTO MODULE android.startup.startups;

-- Use the android_startups table defined in the android.startup.startups
-- module.
SELECT *
FROM android_startups;
```

Prelude is a special module is automatically included. It contains key helper
tables, views and functions which are universally useful.

More information on importing modules is available in the
[syntax documentation](/docs/analysis/perfetto-sql-syntax#including-perfettosql-modules)
for the `INCLUDE PERFETTO MODULE` statement.

<!-- TODO(b/290185551): talk about experimental module and contributions. -->

<style>
/* Make module names bold only when expanded */
details[open] > summary h3 {
  font-weight: bold;
}
details:not([open]) > summary h3 {
  font-weight: normal;
}

/* Add spacing and visual separation between modules */
details {
  margin-bottom: 1em;
  padding: 0.5em;
  border: 1px solid #e0e0e0;
  border-radius: 4px;
  background-color: #fafafa;
}

/* Expanded module gets different styling */
details[open] {
  background-color: #ffffff;
  padding-bottom: 2em;
}

/* Module summary cursor */
details > summary {
  cursor: pointer;
  padding: 0.5em;
}

/* Indent all content inside an open module */
details[open] > *:not(summary) {
  margin-left: 2em;
  margin-right: 1em;
}

/* Add spacing between artifact sections within a module */
details h4 {
  margin-top: 2em;
  margin-bottom: 1em;
  padding-bottom: 0.5em;
  border-bottom: 1px solid #e8e8e8;
  color: #333;
}

/* First h4 in a module shouldn't have as much top margin */
details > h4:first-of-type {
  margin-top: 1em;
}

/* Add spacing between individual artifacts */
details > details {
  margin-bottom: 1.5em;
  background-color: #f9f9f9;
  padding: 0.5em;
  border-left: 3px solid #d0d0d0;
}

/* Tag filter buttons */
.tag-filter {
  display: inline-block;
  padding: 0.3em 0.8em;
  margin: 0.2em;
  border: 1px solid #ccc;
  border-radius: 16px;
  background-color: #f5f5f5;
  cursor: pointer;
  font-size: 0.9em;
  transition: all 0.2s ease;
}

.tag-filter:hover {
  background-color: #e0e0e0;
  border-color: #999;
}

.tag-filter.active {
  background-color: #1a73e8;
  color: white;
  border-color: #1a73e8;
}

.tag-filter.active:hover {
  background-color: #1557b0;
  border-color: #1557b0;
}

#clear-filters {
  display: none;
  margin-left: 1em;
  padding: 0.3em 0.8em;
  border: 1px solid #d93025;
  border-radius: 16px;
  background-color: #fff;
  color: #d93025;
  cursor: pointer;
  font-size: 0.9em;
}

#clear-filters:hover {
  background-color: #fce8e6;
}

/* Hidden modules when filtered */
.module-details.hidden-by-filter {
  display: none;
}

/* Package sections that have no visible modules */
.package-section.hidden-by-filter {
  display: none;
}
</style>

<script>
// Auto-expand details when navigating to an anchor
function openDetailsOnHash() {
  const hash = window.location.hash;
  if (hash) {
    const element = document.querySelector(hash);
    if (element && element.tagName === 'DETAILS') {
      element.open = true;
    }
  }
}

// Tag filtering functionality
let activeTags = new Set();

function filterByTags() {
  const modules = document.querySelectorAll('.module-details');
  const clearBtn = document.getElementById('clear-filters');

  // Show/hide clear button
  if (clearBtn) {
    clearBtn.style.display = activeTags.size > 0 ? 'inline-block' : 'none';
  }

  modules.forEach(module => {
    if (activeTags.size === 0) {
      // No filter active - show all
      module.classList.remove('hidden-by-filter');
    } else {
      const moduleTags = (module.dataset.tags || '').split(',').filter(t => t);
      // Modules with no tags are always visible
      const hasNoTags = moduleTags.length === 0;
      const hasMatchingTag = moduleTags.some(tag => activeTags.has(tag));
      if (hasNoTags || hasMatchingTag) {
        module.classList.remove('hidden-by-filter');
      } else {
        module.classList.add('hidden-by-filter');
      }
    }
  });

  // Hide package sections with no visible modules
  document.querySelectorAll('h2').forEach(h2 => {
    if (!h2.textContent.startsWith('Package:')) return;
    let sibling = h2.nextElementSibling;
    let hasVisibleModule = false;
    while (sibling && sibling.tagName !== 'H2') {
      if (sibling.classList.contains('module-details') &&
          !sibling.classList.contains('hidden-by-filter')) {
        hasVisibleModule = true;
        break;
      }
      sibling = sibling.nextElementSibling;
    }
    h2.style.display = (activeTags.size === 0 || hasVisibleModule) ? '' : 'none';
  });
}

function toggleTag(tag, button) {
  if (activeTags.has(tag)) {
    activeTags.delete(tag);
    button.classList.remove('active');
  } else {
    activeTags.add(tag);
    button.classList.add('active');
  }
  filterByTags();
}

function clearAllFilters() {
  activeTags.clear();
  document.querySelectorAll('.tag-filter').forEach(btn => {
    btn.classList.remove('active');
  });
  filterByTags();
}

// Run on page load and hash change
window.addEventListener('DOMContentLoaded', openDetailsOnHash);
window.addEventListener('hashchange', openDetailsOnHash);
</script>
'''


def _escape(desc: str) -> str:
  """Escapes special characters in a markdown table."""
  return desc.replace('|', '\\|')


def _md_table_header(cols: List[str]) -> str:
  col_str = ' | '.join(cols) + '\n'
  lines = ['-' * len(col) for col in cols]
  underlines = ' | '.join(lines)
  return col_str + underlines


def _md_rolldown(summary: str, content: str) -> str:
  return f"""<details>
  <summary style="cursor: pointer;">{summary}</summary>

  {content}

  </details>
  """


def _bold(s: str) -> str:
  return f"<strong>{s}</strong>"


def _build_dependency_maps(
    stdlib_json: List[Dict]) -> tuple[Dict[str, Set[str]], Dict[str, Set[str]]]:
  """Build maps of module dependencies.

  Returns:
    (dependencies, dependents) where:
    - dependencies[module] = set of modules that 'module' includes
    - dependents[module] = set of modules that include 'module'
  """
  dependencies = defaultdict(set)
  dependents = defaultdict(set)

  for package in stdlib_json:
    for module_dict in package['modules']:
      module_name = module_dict['module_name']
      includes = module_dict.get('includes', [])

      for included in includes:
        dependencies[module_name].add(included)
        dependents[included].add(module_name)

  return dict(dependencies), dict(dependents)


def _generate_dependency_graph(module_name: str, dependencies: Dict[str,
                                                                    Set[str]],
                               dependents: Dict[str, Set[str]]) -> str:
  """Generate Mermaid dependency graph for a module.

  Args:
    module_name: The module to generate graph for
    dependencies: Map of module -> modules it includes
    dependents: Map of module -> modules that include it

  Returns:
    Mermaid graph markdown or empty string if no dependencies
  """
  module_deps = dependencies.get(module_name, set())
  module_dependents = dependents.get(module_name, set())

  # Only show public dependents (filter out internal modules)
  public_dependents = {d for d in module_dependents if not d.startswith('_')}

  # Only generate graph if there are dependencies or dependents
  if not module_deps and not public_dependents:
    return ''

  lines = ['```mermaid', 'graph TD']

  # Sanitize node names for Mermaid (replace dots with underscores)
  def sanitize(name: str) -> str:
    return name.replace('.', '_').replace('-', '_')

  # Generate anchor link for a module
  # The markdown renderer converts dots to hyphens but keeps underscores
  def get_anchor(name: str) -> str:
    # Replace dots with hyphens, keep underscores as-is
    return '#' + name.replace('.', '-')

  current = sanitize(module_name)

  # Add the current module as a styled node
  lines.append(f'  {current}["{module_name}"]')
  lines.append(f'  class {current} currentModule')

  # Add modules this module includes with click events
  # Arrow points FROM dependency TO current (dependency provides to current)
  for dep in sorted(module_deps):
    dep_sanitized = sanitize(dep)
    lines.append(f'  {dep_sanitized}["{dep}"]')
    lines.append(f'  click {dep_sanitized} "{get_anchor(dep)}" _self')
    lines.append(f'  {dep_sanitized} --> {current}')

  # Add modules that include this module (public only) with click events
  # Arrow points FROM current TO dependent (current provides to dependent)
  for dependent in sorted(public_dependents):
    dependent_sanitized = sanitize(dependent)
    lines.append(f'  {dependent_sanitized}["{dependent}"]')
    lines.append(
        f'  click {dependent_sanitized} "{get_anchor(dependent)}" _self')
    lines.append(f'  {current} --> {dependent_sanitized}')

  # Add styling
  lines.append(
      '  classDef currentModule fill:#e1f5ff,stroke:#01579b,stroke-width:2px')

  lines.append('```')
  return '\n'.join(lines)


class ModuleMd:
  """Responsible for module level markdown generation."""

  def __init__(self,
               package_name: str,
               module_dict: Dict,
               dependencies: Dict[str, Set[str]] = None,
               dependents: Dict[str, Set[str]] = None):
    self.module_name = module_dict['module_name']
    self.include_str = self.module_name if package_name != 'prelude' else 'N/A'
    self.objs, self.funs, self.view_funs, self.macros = [], [], [], []
    self.dependencies = dependencies or {}
    self.dependents = dependents or {}
    self.dependency_graph = ''
    self.tags = module_dict.get('tags', [])

    # Views/tables (only public)
    for data in module_dict['data_objects']:
      if not data['cols'] or data.get('visibility') != 'public':
        continue

      obj_summary = (f'''{_bold(data['name'])}. {data['summary_desc']}\n''')
      content = [f"{data['type']}"]
      if (data['summary_desc'] != data['desc']):
        content.append(data['desc'])

      table = [_md_table_header(['Column', 'Type', 'Description'])]
      for info in data['cols']:
        name = info["name"]
        table.append(f'{name} | {info["type"]} | {_escape(info["desc"])}')
      content.append('\n\n')
      content.append('\n'.join(table))

      self.objs.append(_md_rolldown(obj_summary, '\n'.join(content)))

      self.objs.append('\n\n')

    # Functions
    for d in module_dict['functions']:
      summary = f'''{_bold(d['name'])} -> {d['return_type']}. {d['summary_desc']}\n\n'''
      content = []
      if (d['summary_desc'] != d['desc']):
        content.append(d['desc'])

      content.append(f"Returns {d['return_type']}: {d['return_desc']}\n\n")
      if d['args']:
        content.append(_md_table_header(['Argument', 'Type', 'Description']))
        for arg_dict in d['args']:
          content.append(
              f'''{arg_dict['name']} | {arg_dict['type']} | {_escape(arg_dict['desc'])}'''
          )

      self.funs.append(_md_rolldown(summary, '\n'.join(content)))
      self.funs.append('\n\n')

    # Table functions
    for data in module_dict['table_functions']:
      obj_summary = f'''{_bold(data['name'])}. {data['summary_desc']}\n\n'''
      content = []
      if (data['summary_desc'] != data['desc']):
        content.append(data['desc'])

      if data['args']:
        args_table = [_md_table_header(['Argument', 'Type', 'Description'])]
        for arg_dict in data['args']:
          args_table.append(
              f'''{arg_dict['name']} | {arg_dict['type']} | {_escape(arg_dict['desc'])}'''
          )
        content.append('\n'.join(args_table))
        content.append('\n\n')

      content.append(_md_table_header(['Column', 'Type', 'Description']))
      for column in data['cols']:
        content.append(
            f'{column["name"]} | {column["type"]} | {column["desc"]}')

      self.view_funs.append(_md_rolldown(obj_summary, '\n'.join(content)))
      self.view_funs.append('\n\n')

    # Macros
    for data in module_dict['macros']:
      obj_summary = f'''{_bold(data['name'])}. {data['summary_desc']}\n\n'''
      content = []
      if (data['summary_desc'] != data['desc']):
        content.append(data['desc'])

      content.append(
          f'''Returns: {data['return_type']}, {data['return_desc']}\n\n''')
      if data['args']:
        table = [_md_table_header(['Argument', 'Type', 'Description'])]
        for arg_dict in data['args']:
          table.append(
              f'''{arg_dict['name']} | {arg_dict['type']} | {_escape(arg_dict['desc'])}'''
          )
        content.append('\n'.join(table))

      self.macros.append(_md_rolldown(obj_summary, '\n'.join(content)))
      self.macros.append('\n\n')

    # Generate dependency graph if module has any public content
    if any((self.objs, self.funs, self.view_funs, self.macros)):
      self.dependency_graph = _generate_dependency_graph(
          self.module_name, self.dependencies, self.dependents)


class PackageMd:
  """Responsible for package level markdown generation."""

  def __init__(self,
               package_name: str,
               module_files: List[Dict[str, Any]],
               dependencies: Dict[str, Set[str]] = None,
               dependents: Dict[str, Set[str]] = None) -> None:
    self.package_name = package_name
    self.modules_md = sorted([
        ModuleMd(package_name, file_dict, dependencies, dependents)
        for file_dict in module_files
    ],
                             key=lambda x: x.module_name)

  def get_md(self) -> str:
    if not self.modules_md:
      return ''

    lines = []
    lines.append(f'## Package: {self.package_name}')

    for file in self.modules_md:
      # Skip modules with no public artifacts (objs, funs, view_funs, macros)
      # The dependency graph alone is not enough - we only show modules with actual public content
      if not any((file.objs, file.funs, file.view_funs, file.macros)):
        continue

      # Wrap each module in a collapsible details section
      # Add id to the details element for anchor links to work
      module_anchor = file.module_name.replace('.', '-')
      # Prelude is always open by default
      open_attr = ' open' if self.package_name == 'prelude' else ''
      # Add data-tags attribute for filtering (escape for HTML attribute)
      tags_attr = f' data-tags="{html.escape(",".join(file.tags))}"' if file.tags else ''
      lines.append(
          f'<details id="{module_anchor}"{open_attr}{tags_attr} class="module-details">'
      )
      lines.append(
          f'<summary style="cursor: pointer;"><h3 style="display: inline;">{file.module_name}</h3></summary>'
      )
      lines.append('')

      # Add dependency graph if available
      if file.dependency_graph:
        lines.append('#### Module Dependencies')
        lines.append(file.dependency_graph)
        lines.append('')

      if file.objs:
        lines.append('#### Views/Tables')
        lines.append('\n'.join(file.objs))
      if file.funs:
        lines.append('#### Functions')
        lines.append('\n'.join(file.funs))
      if file.view_funs:
        lines.append('#### Table Functions')
        lines.append('\n'.join(file.view_funs))
      if file.macros:
        lines.append('#### Macros')
        lines.append('\n'.join(file.macros))

      lines.append('</details>')
      lines.append('')

    return '\n'.join(lines)

  def is_empty(self) -> bool:
    for file in self.modules_md:
      if any((file.objs, file.funs, file.view_funs, file.macros)):
        return False
    return True


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--input', required=True)
  parser.add_argument('--output', required=True)
  args = parser.parse_args()

  with open(args.input) as f:
    stdlib_json = json.load(f)

  # Build dependency maps for all modules
  dependencies, dependents = _build_dependency_maps(stdlib_json)

  # Merge prelude modules into one synthetic module
  for package in stdlib_json:
    if package["name"] == "prelude":
      # Collect all artifacts from all prelude modules
      merged_module = {
          'module_name': 'prelude',
          'module_doc': None,
          'tags': [],
          'includes': [],
          'data_objects': [],
          'functions': [],
          'table_functions': [],
          'macros': []
      }

      for module in package["modules"]:
        merged_module['data_objects'].extend(module.get('data_objects', []))
        merged_module['functions'].extend(module.get('functions', []))
        merged_module['table_functions'].extend(
            module.get('table_functions', []))
        merged_module['macros'].extend(module.get('macros', []))

      # Replace all prelude modules with one synthetic module
      package["modules"] = [merged_module]
      break

  # Collect all unique tags from the stdlib
  all_tags: Set[str] = set()
  for package in stdlib_json:
    for module in package["modules"]:
      all_tags.update(module.get('tags', []))

  # Fetch the modules from json documentation.
  packages: Dict[str, PackageMd] = {}
  for package in stdlib_json:
    package_name = package["name"]
    modules = package["modules"]
    # Remove 'common' when it has been removed from the code.
    if package_name not in ['deprecated', 'common']:
      package = PackageMd(package_name, modules, dependencies, dependents)
      if (not package.is_empty()):
        packages[package_name] = package

  # Get prelude first, then all other packages
  prelude = packages.pop('prelude')

  with open(args.output, 'w') as f:
    f.write(INTRODUCTION)

    # Write tags list with interactive filter buttons
    if all_tags:
      f.write('\n## Tags\n')
      f.write('Click on tags to filter modules by category:\n\n')
      f.write('<div id="tag-filters">\n')
      for tag in sorted(all_tags):
        # Escape tag for both JS string (single quotes) and HTML content
        escaped_tag = html.escape(tag).replace("'", "\\'")
        f.write(
            f'<span class="tag-filter" onclick="toggleTag(\'{escaped_tag}\', this)">{html.escape(tag)}</span>\n'
        )
      f.write(
          '<button id="clear-filters" onclick="clearAllFilters()">Clear filters</button>\n'
      )
      f.write('</div>\n\n')

    f.write(prelude.get_md())
    f.write('\n')
    f.write('\n'.join(module.get_md() for module in packages.values()))

  return 0


if __name__ == '__main__':
  sys.exit(main())
