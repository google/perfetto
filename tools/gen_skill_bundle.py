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
"""Builds the bundle of one or more agent skills for one host (RFC-0052).

A host is defined by its adapter: a markdown file whose frontmatter lists the
actions that the host provides (open tags) and the skill description that it
shows, and whose body says how the host performs those actions:

  ---
  provides: [query, multi_trace, python, record, write_file]
  description: >-
    Analyze or record Perfetto traces ...
  ---
  How to set up trace_processor and perform each action in this host.

The body replaces the {{environment_setup}} line of SKILL-template.md. The
in-tree adapters are the files in environment-references/ with `provides` in
their frontmatter. To add a host, write an adapter, pass it with
--adapter <path> (in-tree or from another location), and select the --layout
that the host reads.

A content file lists in frontmatter the actions it requires and the workflows
to point at:

  ---
  requires: [query, multi_trace, python]
  related:
    - path: heap_dump_diff.md
      when: Two dumps (before/after)
  ---

A host gets a file only if its adapter provides every action that the file
requires, and --drop-report lists the other files. SKILL.md loses its lines to
those files, and the `related` entries that the host gets become a "Related
workflows" section.

From SKILL.md, the generator follows markdown links and `$SKILL_ROOT/<path>`
references. A reference to a missing file, to a dropped file or to an adapter
is an error, and so is a .md file that no reference reaches.

--layout selects the output format, and any host can use either layout:

  tree (default): the skill directory with SKILL.md and the files that the
  host gets, including scripts and data files. The CLI adapter uses it,
  through tools/release/build_ai_agents.py.

  index: only the .md and .sql files that SKILL.md reaches, and an index.json
  with the name, description and files of each skill. The UI adapter uses it,
  through ui/build.mjs.

A skill without adapters, such as an old release tree, is built without one.
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
# The line of SKILL-template.md that the adapter body replaces.
SETUP_SLOT = '{{environment_setup}}'
DEFAULT_SKILL = Path(__file__).resolve().parent.parent / 'ai/skills/perfetto'
ADAPTER_DIR = 'environment-references'
INDEX_FILE = 'index.json'
FOLLOWED_EXTS = ('.md', '.sql')
MAX_FILE_BYTES = 64 * 1024
MAX_DESCRIPTION = 1024
NAME_RE = re.compile(r'^[a-z0-9]+(-[a-z0-9]+)*$')
# The characters the UI accepts in a path segment (PATH_SEGMENT_RE in
# ui/src/core/skill_manager.ts).
PATH_SEGMENT_RE = re.compile(r'^[A-Za-z0-9_.-]+$')

# Source files that are not part of any bundle.
SOURCE_ONLY = (SKILL_TEMPLATE, 'OWNERS', 'TEST.md', 'BUILD')
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


def _parse_list_of_maps(block: List[str]) -> Any:
  """The `- key: value` items under a key, such as `related`.

  Anything else is returned as text, so that the caller rejects it.
  """
  if not block[0].lstrip().startswith('- '):
    return '\n'.join(block)
  items: List[Dict[str, str]] = []
  for line in block:
    text = line.strip()
    if text.startswith('- '):
      items.append({})
      text = text[2:].strip()
    m = _KEY_RE.match(text)
    if m and items:
      items[-1][m.group(1)] = _unquote(m.group(2))
  return items


def parse_frontmatter(fm: str) -> Dict[str, Any]:
  """Reads a frontmatter block.

  Supports what skills use here, without a YAML dependency in the UI build:
  plain, quoted and block scalars ('>', '>-', '|', '|-'), flow lists (kept as
  text, see parse_list), and a list of maps under a key.
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
      values[key] = _parse_list_of_maps(block)
  return values


def frontmatter_entry(fm: str, key: str) -> str:
  """The lines of one key in a frontmatter block, as written."""
  lines = fm.splitlines(keepends=True)
  for i, line in enumerate(lines):
    m = _KEY_RE.match(line)
    if m and m.group(1) == key:
      end = i + 1
      while end < len(lines) and (lines[end].startswith(' ') or
                                  not lines[end].strip()):
        end += 1
      return ''.join(lines[i:end])
  return ''


def parse_list(value: str, where: str) -> List[str]:
  """Reads a flow list such as `[query, python]`."""
  if not (value.startswith('[') and value.endswith(']')):
    raise SkillError(f'{where}: expected a list such as [a, b], got "{value}"')
  return [v.strip() for v in value[1:-1].split(',') if v.strip()]


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


class Adapter:
  """How one host performs the actions of a skill."""

  def __init__(self, path: Path):
    where = str(path)
    if not path.is_file():
      raise SkillError(f'{where}: no such adapter file')
    fm, body = split_frontmatter(path.read_text(encoding='utf-8'), where)
    meta = parse_frontmatter(fm)
    self.name = path.name
    self.provides = parse_list(meta.get('provides', ''), f'{where}: provides')
    self.description = meta.get('description', '')
    if not self.description:
      raise SkillError(f'{where}: description is missing')
    if len(self.description) > MAX_DESCRIPTION:
      raise SkillError(f'{where}: description is longer than '
                       f'{MAX_DESCRIPTION} characters')
    self.description_entry = frontmatter_entry(fm, 'description')
    self.body = body.strip('\n') + '\n'

  @classmethod
  def find_in(cls, src: Path) -> Dict[str, 'Adapter']:
    """The adapters in a skill's environment-references/, by relative path.

    An adapter is a file there whose frontmatter has `provides`.
    """
    adapters: Dict[str, 'Adapter'] = {}
    for path in sorted((src / ADAPTER_DIR).glob('*.md')):
      text = path.read_text(encoding='utf-8')
      if (text.startswith('---\n') and 'provides' in parse_frontmatter(
          split_frontmatter(text, str(path))[0])):
        adapters[path.relative_to(src).as_posix()] = cls(path)
    return adapters


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
    # None when the file does not declare what it needs.
    self.requires = (
        parse_list(meta['requires'], f'{where}: requires')
        if 'requires' in meta else None)
    related = meta.get('related', [])
    if not isinstance(related, list) or not all(
        isinstance(r, dict) and r.get('path') and r.get('when')
        for r in related):
      raise SkillError(f'{where}: related needs a list of path and when')
    self.related = [(_resolve(r['path'], posixpath.dirname(rel),
                              False), r['when']) for r in related]


class Skill:
  """One skill for one host: its router and the files it reaches."""

  def __init__(self, src: Path, adapter_path: Optional[Path]):
    self.src = src
    self.files: Dict[str, str] = {}
    self.adapters = Adapter.find_in(src)
    self.adapter = Adapter(adapter_path) if adapter_path is not None else None
    if self.adapters and self.adapter is None:
      raise SkillError(f'{src}: the skill has adapters, so --adapter is needed')
    fm, body = self._read_router()
    self.docs = self._read_docs()
    # The files the host does not get, and the actions it lacks for each.
    self.dropped = self._find_dropped()
    body = self._insert_adapter(body)
    body = self._drop_router_links(body)
    self._collect(SKILL_FILE, f'---\n{fm}---\n{body}')
    self._check_reached()

  def _read_router(self) -> Tuple[str, str]:
    """The frontmatter and body of SKILL.md, with the host's description."""
    template = self.src / SKILL_TEMPLATE
    path = template if template.exists() else self.src / SKILL_FILE
    if not path.exists():
      raise SkillError(f'{self.src}: no {SKILL_FILE} or {SKILL_TEMPLATE}')
    self.router_path = path
    fm, body = split_frontmatter(path.read_text(encoding='utf-8'), str(path))
    meta = parse_frontmatter(fm)
    self.name = meta.get('name', '')
    if not NAME_RE.match(self.name) or len(self.name) > 64:
      raise SkillError(f'{path}: invalid skill name "{self.name}"')
    if self.adapter is None:
      self.description = meta.get('description', '')
      if not self.description or len(self.description) > MAX_DESCRIPTION:
        raise SkillError(f'{path}: description must be 1-1024 characters')
    elif 'description' in meta:
      raise SkillError(f'{path}: the description goes in each adapter')
    else:
      self.description = self.adapter.description
      fm += self.adapter.description_entry
    return fm, body

  def _read_docs(self) -> Dict[str, Doc]:
    """The content files of the skill, by relative path."""
    docs: Dict[str, Doc] = {}
    for path in sorted(self.src.rglob('*.md')):
      rel = path.relative_to(self.src).as_posix()
      if (rel in (SKILL_TEMPLATE, SKILL_FILE) or rel in self.adapters or
          path.name in SOURCE_ONLY):
        continue
      docs[rel] = Doc(self.src, rel)
    for rel, doc in docs.items():
      for path, _ in doc.related:
        if path not in docs:
          raise SkillError(f'{self.src / rel}: related {path} is not a content '
                           'file of the skill')
    return docs

  def _find_dropped(self) -> Dict[str, List[str]]:
    dropped: Dict[str, List[str]] = {}
    for rel, doc in self.docs.items():
      if self.adapter is None:
        if doc.requires is not None:
          raise SkillError(
              f'{self.src / rel}: requires needs an adapter per host')
        continue
      if doc.requires is None:
        raise SkillError(f'{self.src / rel}: requires is missing; list every '
                         'action the file needs, such as [query]')
      missing = sorted(set(doc.requires) - set(self.adapter.provides))
      if missing:
        dropped[rel] = missing
    return dropped

  def _insert_adapter(self, body: str) -> str:
    """Replaces the {{environment_setup}} line with the adapter body."""
    where = self.router_path
    lines = body.splitlines(keepends=True)
    slots = [i for i, line in enumerate(lines) if line.strip() == SETUP_SLOT]
    if len(slots) > 1:
      raise SkillError(f'{where}: two {SETUP_SLOT} lines')
    if self.adapter is None:
      if slots:
        raise SkillError(f'{where}: {SETUP_SLOT} but no adapter')
      return body
    if not slots:
      raise SkillError(f'{where}: no {SETUP_SLOT} line for the adapter')
    lines[slots[0]] = self.adapter.body
    return ''.join(lines)

  def _drop_router_links(self, body: str) -> str:
    """Removes the router lines that link to a file the host does not get.

    A numbered item that loses all its lines this way is removed too, and the
    items are numbered again.
    """
    lines = body.splitlines(keepends=True)
    gone = [
        any(
            _resolve(m.group(1), '', False) in self.dropped
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
      return f'adapter, which is read with {SKILL_FILE}', ''
    if path in self.dropped:
      return (f'requires {", ".join(self.dropped[path])}, which '
              f'{self.adapter.name} does not provide'), ''
    if path in self.docs:
      return None, self.render(path)
    full = self.src / path
    if not full.is_file():
      return ('missing' if not full.exists() else 'not a file'), ''
    return None, full.read_text(encoding='utf-8')

  def _check_reached(self) -> None:
    unreached = sorted(set(self.docs) - set(self.dropped) - set(self.files))
    if unreached:
      raise SkillError(f'{self.src}: no link from {SKILL_FILE} reaches '
                       f'{", ".join(unreached)}')

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
        f'# Files of the {self.name} skill that the host of '
        f'{self.adapter.name if self.adapter else "no adapter"} does not\n',
        '# get, and the actions it lacks for each. Written by\n',
        '# tools/gen_skill_bundle.py --drop-report.\n',
    ]
    lines += [
        f'{rel}: {", ".join(missing)}\n'
        for rel, missing in sorted(self.dropped.items())
    ]
    return ''.join(lines)

  def index_entry(self) -> dict:
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
    return {
        'name': self.name,
        'description': self.description,
        'files': files,
    }

  def write_tree(self, out: Path) -> None:
    """Writes the host's skill tree with SKILL.md and rendered content files."""
    # SKILL.md and the content files are written from their rendered text.
    skipped = {SKILL_FILE, *self.adapters, *self.dropped, *self.docs}
    dst = out / self.name
    if dst.exists():
      shutil.rmtree(dst)
    for path in sorted(self.src.rglob('*')):
      rel = path.relative_to(self.src)
      if (not path.is_file() or rel.as_posix() in skipped or
          any(part in SOURCE_ONLY for part in rel.parts)):
        continue
      target = dst / rel
      target.parent.mkdir(parents=True, exist_ok=True)
      # copyfile does not copy the mode, so the output stays writable when the
      # source is read-only (such as in a build sandbox). Only +x is kept.
      shutil.copyfile(path, target)
      target.chmod(target.stat().st_mode | (path.stat().st_mode & 0o111))
    rendered = {
        rel: self.render(rel) for rel in self.docs if rel not in self.dropped
    }
    rendered[SKILL_FILE] = self.files[SKILL_FILE]
    for rel, text in rendered.items():
      target = dst / rel
      target.parent.mkdir(parents=True, exist_ok=True)
      target.write_bytes(text.encode('utf-8'))

  def write_index(self, out: Path) -> None:
    """Writes SKILL.md and the files it reaches under <out>/<name>/."""
    dst_root = out / self.name
    if dst_root.exists():
      shutil.rmtree(dst_root)
    for rel, text in self.files.items():
      dst = dst_root / rel
      dst.parent.mkdir(parents=True, exist_ok=True)
      dst.write_bytes(text.encode('utf-8'))


def main(argv: Optional[List[str]] = None) -> int:
  parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
  parser.add_argument(
      '--skill',
      action='append',
      help='skill directory (repeatable; default: the in-tree perfetto '
      'skill)')
  parser.add_argument(
      '--adapter',
      help='the adapter file of the host to build for; needed when the skill '
      'has adapters')
  parser.add_argument(
      '--drop-report',
      help='write the list of files the host does not get here (one --skill '
      'only)')
  parser.add_argument(
      '--layout',
      choices=('tree', 'index'),
      default='tree',
      help='the form of the output (default: tree)')
  parser.add_argument('--out', required=True, help='output directory')
  args = parser.parse_args(argv)
  args.skill = args.skill or [str(DEFAULT_SKILL)]

  try:
    if args.drop_report and len(args.skill) != 1:
      raise SkillError('--drop-report needs exactly one --skill')
    adapter = Path(args.adapter) if args.adapter else None
    skills = [Skill(Path(s), adapter) for s in args.skill]
    if args.drop_report:
      report = Path(args.drop_report)
      report.parent.mkdir(parents=True, exist_ok=True)
      report.write_text(skills[0].drop_report())
    out = Path(args.out)
    if args.layout == 'index':
      names = [s.name for s in skills]
      if len(set(names)) != len(names):
        raise SkillError(f'duplicate skill names: {names}')
      # The entries check every file before anything is written.
      entries = [s.index_entry() for s in skills]
      for skill in skills:
        skill.write_index(out)
      out.mkdir(parents=True, exist_ok=True)
      (out / INDEX_FILE).write_bytes(
          (json.dumps({'skills': entries}, indent=2) + '\n').encode('utf-8'))
    else:
      for skill in skills:
        skill.write_tree(out)
  except SkillError as e:
    print(f'gen_skill_bundle: error: {e}', file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
