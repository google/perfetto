#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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
"""Builds the bundle of one or more agent skills for one host.

A skill is written once and read by several hosts (RFC-0052), and each host
performs the skill's actions in its own way.

What differs between hosts is in one adapter per host: a markdown file in
environment-references/ whose frontmatter names the host and the actions it
provides (query, multi_trace, python, record, write_file; query is required):

  ---
  host: cli
  provides: [query, multi_trace, python, record, write_file]
  ---

The body of the adapter for --host (or of the file given with --adapter)
replaces the `{{environment_setup}}` line of SKILL-template.md, so the agent
reads it with the router. Adapter files are not copied, and a reference to one
is an error.

A content file lists in frontmatter the actions it needs beyond query and the
workflows to point at:

  ---
  requires: [multi_trace, python]
  related:
    - path: heap_dump_diff.md
      when: Two dumps (before/after)
  ---

A host gets a file only if its adapter provides every action the file
requires; --drop-report lists the files it does not get. In SKILL.md, a line
that links to a file the host does not get is removed, and so is a numbered
item that has no such line left. In other files, the `related` entries the host
gets become a "Related workflows" section. Frontmatter is not copied.

Starting from SKILL.md, the script follows the references (markdown links and
`$SKILL_ROOT/<path>` text). A reference to a missing .md or .sql file, or to a
file the host does not get, is an error. References to directories and other
files (such as Python scripts) are not followed.

  --host cli (see tools/release/build_ai_agents.py):
    <out>/<skill>/...    the skill tree, with SKILL.md

  --host ui: the Perfetto UI agent (dev.perfetto.Intelletto) has no shell and
  no file system, and reads a skill through tools that fetch one file at a
  time. It gets only the .md and .sql files reached from SKILL.md, and an index
  (one entry per skill: name, description, files).
    For the UI build (see ui/build.mjs):
      <out>/index.json
      <out>/<skill>/<path>
    With --module <id>, for an extension server (see
    docs/visualization/extension-server-protocol.md):
      <out>/modules/<id>/skills
      <out>/modules/<id>/skill_files/<skill>/<path>
"""

import argparse
import json
import posixpath
import re
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

SKILL_FILE = 'SKILL.md'
SKILL_TEMPLATE = 'SKILL-template.md'
ADAPTER_DIR = 'environment-references'
ADAPTER_SLOT = '{{environment_setup}}'
INDEX_FILE = 'index.json'
FOLLOWED_EXTS = ('.md', '.sql')
MAX_FILE_BYTES = 64 * 1024
MAX_DESCRIPTION = 1024
NAME_RE = re.compile(r'^[a-z0-9]+(-[a-z0-9]+)*$')
# The characters the UI accepts in a path segment (PATH_SEGMENT_RE in
# ui/src/core/skill_manager.ts).
PATH_SEGMENT_RE = re.compile(r'^[A-Za-z0-9_.-]+$')

# The hosts that an adapter can name.
HOSTS = ('cli', 'ui')
ACTIONS = ('query', 'multi_trace', 'python', 'record', 'write_file')
BASELINE_ACTION = 'query'
# Source-only files that the tree layout does not copy.
TREE_IGNORE = (SKILL_TEMPLATE, 'OWNERS', 'TEST.md', 'BUILD')
# The frontmatter keys of a content file.
DOC_KEYS = ('requires', 'related')
RELATED_HEADING = '## Related workflows'

_KEY_RE = re.compile(r'^([A-Za-z0-9_-]+):\s*(.*)$')
_ITEM_RE = re.compile(r'^(\d+)\. ')

_LINK_RE = re.compile(r'\]\(([^)\s]+)\)')
_ROOT_REF_RE = re.compile(r'\$SKILL_ROOT/([A-Za-z0-9_./-]+)')


class SkillError(Exception):
  pass


def split_frontmatter(text: str, where: str) -> Tuple[str, str]:
  if not text.startswith('---\n'):
    raise SkillError(f'{where}: missing YAML frontmatter')
  end = text.find('\n---\n', 4)
  if end < 0:
    raise SkillError(f'{where}: unterminated YAML frontmatter')
  return text[4:end + 1], text[end + 5:]


def _unquote(value: str) -> str:
  value = value.strip()
  if len(value) >= 2 and value[0] == value[-1] and value[0] in '"\'':
    return value[1:-1]
  return value


def _parse_nested(block: List[str]) -> Any:
  """A map (`key: value` lines) or a list (`- ...` items) under a key."""
  if not block[0].lstrip().startswith('- '):
    return {
        m.group(1): _unquote(m.group(2)) for m in (_KEY_RE.match(l.strip())
                                                  for l in block) if m
    }
  items: List[Any] = []
  for line in block:
    text = line.strip()
    new_item = text.startswith('- ')
    if new_item:
      text = text[2:].strip()
    m = _KEY_RE.match(text)
    if new_item:
      items.append({m.group(1): _unquote(m.group(2))} if m else _unquote(text))
    elif m and items and isinstance(items[-1], dict):
      items[-1][m.group(1)] = _unquote(m.group(2))
  return items


def parse_frontmatter(fm: str) -> Dict[str, Any]:
  """Reads a frontmatter block.

  Supports what skills use here, without a YAML dependency in the UI build:
  plain, quoted and block scalars ('>', '>-', '|', '|-'), flow lists (kept as
  text, see parse_list), and one level of nesting: a map or a list of maps.
  """
  values: Dict[str, Any] = {}
  lines = fm.splitlines()
  i = 0
  while i < len(lines):
    m = _KEY_RE.match(lines[i])
    i += 1
    if not m:
      continue
    key, rest = m.group(1), m.group(2).strip()
    block = []
    while i < len(lines) and (lines[i].startswith(' ') or not lines[i]):
      if lines[i].strip():
        block.append(lines[i])
      i += 1
    if rest in ('>', '>-', '|', '|-'):
      sep = ' ' if rest.startswith('>') else '\n'
      values[key] = sep.join(l.strip() for l in block).strip()
    elif rest:
      values[key] = _unquote(rest)
    elif block:
      values[key] = _parse_nested(block)
  return values


def parse_list(value: str, where: str) -> List[str]:
  """Reads a flow list such as `[query, python]`."""
  if not (value.startswith('[') and value.endswith(']')):
    raise SkillError(f'{where}: expected a list such as [a, b], got "{value}"')
  return [v.strip() for v in value[1:-1].split(',') if v.strip()]


class Adapter:
  """How one host performs the actions of a skill."""

  def __init__(self, path: Path):
    where = str(path)
    fm, body = split_frontmatter(path.read_text(encoding='utf-8'), where)
    meta = parse_frontmatter(fm)
    self.host = meta.get('host', '')
    if self.host not in HOSTS:
      raise SkillError(f'{where}: unknown host "{self.host}", expected one of '
                       f'{", ".join(HOSTS)}')
    self.provides = parse_list(meta.get('provides', ''), f'{where}: provides')
    unknown = sorted(set(self.provides) - set(ACTIONS))
    if unknown:
      raise SkillError(f'{where}: unknown actions {unknown}, expected some of '
                       f'{", ".join(ACTIONS)}')
    if BASELINE_ACTION not in self.provides:
      raise SkillError(f'{where}: every host provides "{BASELINE_ACTION}"')
    self.body = body.strip('\n') + '\n'


def find_adapters(src: Path) -> Dict[str, Adapter]:
  """The adapters in a skill's environment-references/, by relative path."""
  adapters: Dict[str, Adapter] = {}
  hosts: Dict[str, str] = {}
  for path in sorted((src / ADAPTER_DIR).glob('*.md')):
    text = path.read_text(encoding='utf-8')
    if not text.startswith('---\n'):
      continue
    if 'host' not in parse_frontmatter(split_frontmatter(text, str(path))[0]):
      continue
    rel = path.relative_to(src).as_posix()
    adapter = Adapter(path)
    if adapter.host in hosts:
      raise SkillError(f'{src}: {hosts[adapter.host]} and {rel} are both '
                       f'adapters for host "{adapter.host}"')
    hosts[adapter.host] = rel
    adapters[rel] = adapter
  return adapters


def insert_adapter(body: str, adapter: Optional[Adapter], where: str) -> str:
  """Replaces the {{environment_setup}} line of the router with the adapter."""
  lines = body.splitlines(keepends=True)
  slots = [i for i, l in enumerate(lines) if l.strip() == ADAPTER_SLOT]
  if adapter is None:
    if slots:
      raise SkillError(f'{where}: {ADAPTER_SLOT} but no adapter')
    return body
  if len(slots) != 1:
    raise SkillError(f'{where}: an adapter needs one {ADAPTER_SLOT} line')
  i = slots[0]
  return ''.join(lines[:i] + [adapter.body] + lines[i + 1:])


class Doc:
  """A content file: its body and what its frontmatter declares."""

  def __init__(self, src: Path, rel: str):
    where = str(src / rel)
    text = (src / rel).read_text(encoding='utf-8')
    meta: Dict[str, Any] = {}
    if text.startswith('---\n'):
      fm, text = split_frontmatter(text, where)
      meta = parse_frontmatter(fm)
    unknown = sorted(set(meta) - set(DOC_KEYS))
    if unknown:
      raise SkillError(f'{where}: unknown frontmatter keys {unknown}, expected '
                       f'some of {", ".join(DOC_KEYS)}')
    self.rel = rel
    self.body = text
    self.requires = parse_list(meta.get('requires', '[]'), f'{where}: requires')
    unknown = sorted(set(self.requires) - set(ACTIONS))
    if unknown:
      raise SkillError(f'{where}: unknown actions {unknown}')
    related = meta.get('related', [])
    if not isinstance(related, list) or not all(
        isinstance(r, dict) and r.get('path') and r.get('when')
        for r in related):
      raise SkillError(f'{where}: related needs a list of path and when')
    self.related = [(_resolve(r['path'], posixpath.dirname(rel),
                              False), r['when']) for r in related]


def drop_router_links(body: str, dropped: Set[str]) -> str:
  """Removes the router lines that link to a file the host does not get.

  A numbered item that loses all its lines this way is removed too, and the
  items are numbered again.
  """
  lines = body.splitlines(keepends=True)
  gone = [
      any(
          _resolve(m.group(1), '', False) in dropped
          for m in _LINK_RE.finditer(line))
      for line in lines
  ]
  # A numbered item runs to the next item or heading.
  starts = [
      i for i, l in enumerate(lines) if _ITEM_RE.match(l) or l.startswith('#')
  ]
  for start, end in zip(starts, starts[1:] + [len(lines)]):
    if not _ITEM_RE.match(lines[start]):
      continue
    item = range(start + 1, end)
    if any(gone[i] for i in item) and not any(lines[i].strip() and not gone[i]
                                              for i in item):
      for i in range(start, end):
        gone[i] = True
  out = [l for l, g in zip(lines, gone) if not g]
  n = 0
  for i, line in enumerate(out):
    if line.startswith('#'):
      n = 0
    elif _ITEM_RE.match(line):
      n += 1
      out[i] = _ITEM_RE.sub(f'{n}. ', line, count=1)
  return ''.join(out)


def _resolve(ref: str, from_dir: str, root_relative: bool) -> Optional[str]:
  """Returns the skill-relative path for a reference, or None if external."""
  # URLs, anchors and site-absolute paths are not files of the skill.
  if (re.match(r'^[a-z][a-z0-9+.-]*:', ref) or ref.startswith('#') or
      ref.startswith('/')):
    return None
  ref = ref.split('#', 1)[0]
  if not ref:
    return None
  if ref.startswith('$SKILL_ROOT/'):
    ref, root_relative = ref[len('$SKILL_ROOT/'):], True
  path = posixpath.normpath(
      ref if root_relative else posixpath.join(from_dir, ref))
  if path.startswith('../') or path == '..':
    raise SkillError(f'reference leaves the skill: {ref}')
  return path


def find_references(text: str, rel_path: str) -> List[str]:
  """Returns the skill-relative path of each link and $SKILL_ROOT reference."""
  from_dir = posixpath.dirname(rel_path)
  refs: List[str] = []
  for m in _LINK_RE.finditer(text):
    path = _resolve(m.group(1), from_dir, root_relative=False)
    if path is not None:
      refs.append(path)
  without_links = _LINK_RE.sub('](...)', text)
  for m in _ROOT_REF_RE.finditer(without_links):
    path = _resolve(m.group(1).rstrip('.'), '', root_relative=True)
    if path is not None:
      refs.append(path)
  return refs


class Skill:
  """One skill for one host: its router and the files it reaches."""

  def __init__(self, src: Path, host: str, adapter_path: Optional[Path] = None):
    self.src = src
    self.files: Dict[str, str] = {}
    template = src / SKILL_TEMPLATE
    skill_md = template if template.exists() else src / SKILL_FILE
    if not skill_md.exists():
      raise SkillError(f'{src}: no {SKILL_FILE} or {SKILL_TEMPLATE}')
    fm, body = split_frontmatter(
        skill_md.read_text(encoding='utf-8'), str(skill_md))
    meta = parse_frontmatter(fm)
    self.name = meta.get('name', '')
    self.description = meta.get('description', '')
    if not NAME_RE.match(self.name) or len(self.name) > 64:
      raise SkillError(f'{skill_md}: invalid skill name "{self.name}"')
    if not self.description or len(self.description) > MAX_DESCRIPTION:
      raise SkillError(f'{skill_md}: description must be 1-1024 characters')

    self.adapters = find_adapters(src)
    if adapter_path is not None:
      adapter = Adapter(adapter_path)
      if adapter.host != host:
        raise SkillError(f'{adapter_path}: adapter for host "{adapter.host}", '
                         f'not "{host}"')
    else:
      adapter = next((a for a in self.adapters.values() if a.host == host),
                     None)
      if self.adapters and adapter is None:
        raise SkillError(f'{src}: no adapter for host "{host}"')
    self.host = host
    self.docs: Dict[str, Doc] = {}
    for path in sorted(src.rglob('*.md')):
      rel = path.relative_to(src).as_posix()
      if (rel in (SKILL_TEMPLATE, SKILL_FILE) or rel in self.adapters or
          path.name in TREE_IGNORE):
        continue
      self.docs[rel] = Doc(src, rel)

    # The files the host does not get, and the actions it lacks for each.
    self.dropped: Dict[str, List[str]] = {}
    for rel, doc in self.docs.items():
      if doc.requires and adapter is None:
        raise SkillError(f'{src / rel}: requires needs an adapter per host')
      missing = sorted(set(doc.requires) -
                       set(adapter.provides)) if adapter else []
      if missing:
        self.dropped[rel] = missing
    for rel, doc in self.docs.items():
      for path, _ in doc.related:
        if path not in self.docs:
          raise SkillError(f'{src / rel}: related {path} is not a content '
                           'file of the skill')

    body = insert_adapter(body, adapter, str(skill_md))
    body = drop_router_links(body, set(self.dropped))
    self._collect(SKILL_FILE, f'---\n{fm}---\n{body}')

  def render(self, rel: str) -> str:
    """A content file without frontmatter, with its related workflows."""
    doc = self.docs[rel]
    related = [(p, w) for p, w in doc.related if p not in self.dropped]
    if not related:
      return doc.body
    lines = [
        f'- {w}: [{posixpath.basename(p)}]($SKILL_ROOT/{p})\n'
        for p, w in related
    ]
    return (doc.body.rstrip('\n') + f'\n\n{RELATED_HEADING}\n\n' +
            ''.join(lines))

  def drop_report(self) -> str:
    lines = [
        f'# Files of the {self.name} skill that host "{self.host}" does not\n',
        '# get, and the actions it lacks for each. Written by\n',
        '# tools/gen_skill_bundle.py --drop-report.\n',
    ]
    lines += [
        f'{rel}: {", ".join(missing)}\n'
        for rel, missing in sorted(self.dropped.items())
    ]
    return ''.join(lines)

  def _collect(self, root_rel: str, root_text: str) -> None:
    queue: List[Tuple[str, str]] = [(root_rel, root_text)]
    seen: Set[str] = {root_rel}
    while queue:
      rel, text = queue.pop(0)
      self.files[rel] = text
      if not rel.endswith('.md'):
        continue
      for path in find_references(text, rel):
        if path in seen or not path.endswith(FOLLOWED_EXTS):
          continue
        problem, src_text = self._load(path)
        if problem is not None:
          raise SkillError(f'{self.name}: {rel} references {path}: {problem}')
        seen.add(path)
        queue.append((path, src_text))

  def _load(self, path: str) -> Tuple[Optional[str], str]:
    """Returns (None, the text) or (why the file cannot be used, '')."""
    if path in self.adapters:
      return (f'adapter for host "{self.adapters[path].host}", which is read '
              f'with {SKILL_FILE}'), ''
    if path in self.dropped:
      return (f'requires {", ".join(self.dropped[path])}, which host '
              f'"{self.host}" does not provide'), ''
    if path in self.docs:
      return None, self.render(path)
    full = self.src / path
    if not full.is_file():
      return ('missing' if not full.exists() else 'not a file'), ''
    return None, full.read_text(encoding='utf-8')

  def index_entry(self, replaces: Optional[str] = None) -> dict:
    paths = sorted(self.files, key=lambda p: (p != SKILL_FILE, p))
    files = []
    for p in paths:
      if not all(PATH_SEGMENT_RE.match(s) for s in p.split('/')):
        raise SkillError(f'{self.name}: {p}: the UI accepts only letters, '
                         'digits, "_", "." and "-" in a path')
      size = len(self.files[p].encode('utf-8'))
      if size > MAX_FILE_BYTES:
        raise SkillError(f'{self.name}: {p} is {size} bytes, over the limit')
      files.append({'path': p, 'bytes': size})
    entry = {
        'name': self.name,
        'description': self.description,
        'files': files,
    }
    if replaces is not None:
      entry['replaces'] = replaces
    return entry


def write_tree(skill: Skill, out: Path) -> None:
  """Copies the skill tree, without the adapters, and writes SKILL.md."""
  # SKILL.md is written from the rendered router.
  skipped = {
      (skill.src / rel).resolve()
      for rel in list(skill.adapters) + list(skill.dropped) + [SKILL_FILE]
  }

  def ignore(directory: str, names: List[str]) -> Set[str]:
    return {
        n for n in names
        if n in TREE_IGNORE or (Path(directory) / n).resolve() in skipped
    }

  dst = out / skill.name
  if dst.exists():
    shutil.rmtree(dst)
  shutil.copytree(skill.src, dst, ignore=ignore)
  adapter_dir = dst / ADAPTER_DIR
  if adapter_dir.is_dir() and not any(adapter_dir.iterdir()):
    adapter_dir.rmdir()
  (dst / SKILL_FILE).write_bytes(skill.files[SKILL_FILE].encode('utf-8'))
  for rel in skill.docs:
    if rel not in skill.dropped:
      (dst / rel).write_bytes(skill.render(rel).encode('utf-8'))


def write_skills(skills: List[Skill],
                 index_path: Path,
                 files_dir: Path,
                 replaces: Optional[Set[str]] = None):
  names = [s.name for s in skills]
  if len(set(names)) != len(names):
    raise SkillError(f'duplicate skill names: {names}')
  replaces = replaces or set()
  unknown = replaces - set(names)
  if unknown:
    raise SkillError(f'--replaces names no input skill: {sorted(unknown)}')
  entries = [
      s.index_entry(s.name if s.name in replaces else None) for s in skills
  ]
  for skill in skills:
    dst_root = files_dir / skill.name
    if dst_root.exists():
      shutil.rmtree(dst_root)
    for rel, text in skill.files.items():
      dst = dst_root / rel
      dst.parent.mkdir(parents=True, exist_ok=True)
      dst.write_bytes(text.encode('utf-8'))
  index_path.parent.mkdir(parents=True, exist_ok=True)
  index_path.write_bytes(
      (json.dumps({'skills': entries}, indent=2) + '\n').encode('utf-8'))


def main(argv: Optional[List[str]] = None) -> int:
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument(
      '--host',
      required=True,
      choices=list(HOSTS),
      help='the host to build for')
  parser.add_argument(
      '--skill',
      action='append',
      required=True,
      help='skill directory (repeatable)')
  parser.add_argument(
      '--adapter',
      help='adapter file to use instead of the skill\'s own adapter for '
      '--host (one --skill only)')
  parser.add_argument(
      '--module',
      help='with --host ui: write the files of this module of an extension '
      'server, under modules/<id>/, instead of the files for the UI build')
  parser.add_argument(
      '--replaces',
      action='append',
      default=[],
      help='with --module: mark the skill of this name as the '
      'replacement of the builtin skill of the same name (repeatable). The UI '
      'honours it only from its embedder-managed server.')
  parser.add_argument(
      '--drop-report',
      help='write the list of files the host does not get here (one --skill '
      'only)')
  parser.add_argument('--out', required=True, help='output directory')
  args = parser.parse_args(argv)

  try:
    if (args.adapter or args.drop_report) and len(args.skill) != 1:
      raise SkillError('--adapter and --drop-report need exactly one --skill')
    if args.host != 'ui' and (args.module or args.replaces):
      raise SkillError('--module and --replaces need --host ui')
    if args.replaces and not args.module:
      raise SkillError('--replaces needs --module')
    if args.module is not None and (not PATH_SEGMENT_RE.match(args.module) or
                                    args.module in ('.', '..')):
      raise SkillError(f'invalid --module "{args.module}"')
    adapter = Path(args.adapter) if args.adapter else None
    skills = [Skill(Path(s), args.host, adapter) for s in args.skill]
    if args.drop_report:
      report = Path(args.drop_report)
      report.parent.mkdir(parents=True, exist_ok=True)
      report.write_text(skills[0].drop_report())
    out = Path(args.out)
    if args.host != 'ui':
      for skill in skills:
        write_tree(skill, out)
    elif args.module is None:
      write_skills(skills, out / INDEX_FILE, out)
    else:
      mod_dir = out / 'modules' / args.module
      write_skills(skills, mod_dir / 'skills', mod_dir / 'skill_files',
                   set(args.replaces))
  except SkillError as e:
    print(f'gen_skill_bundle: error: {e}', file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
