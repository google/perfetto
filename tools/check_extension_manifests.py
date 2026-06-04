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
"""Validates the per-agent extension manifests under ai/extensions/ and
the skill-to-target mapping at ai/skills/targets.json.

These files are the source of truth for the `ai-agents` release branch
(see ai/extensions/README.md and RFC-0026). This lint catches malformed
JSON, missing required fields, and drift between `ai/skills/` and
`targets.json` before the release pipeline builds the branch.
"""

import json
import os
import re
import sys
from typing import Any, Dict, List

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTENSIONS_DIR = os.path.join(REPO_ROOT, 'ai', 'extensions')
SKILLS_DIR = os.path.join(REPO_ROOT, 'ai', 'skills')
TARGETS_JSON = os.path.join(SKILLS_DIR, 'targets.json')
# Before the first prebuilt roll the manifests carry the dev sentinel;
# roll-prebuilts stamps the release version (vX.Y) in place. Accept either.
VERSION_SENTINEL = '0.0.0-dev'
VERSION_RE = re.compile(r'^(0\.0\.0-dev|v[0-9]+\.[0-9]+)$')
VALID_TARGETS = ('claude-code', 'codex', 'fallback')

# Per-agent required fields. Each entry maps a manifest filename to the
# list of top-level keys that must be present and non-empty. Sub-field
# shape is checked further in _check_specific().
REQUIRED = {
    'claude-code/plugin.json': ['name', 'version', 'description'],
    'claude-code/marketplace.json': [
        'name', 'version', 'description', 'owner', 'plugins'
    ],
    'codex/plugin.json': ['name', 'version', 'description'],
    'codex/marketplace.json': ['name', 'plugins'],
}


def _check_specific(rel_path: str, data: Dict[str, Any]) -> List[str]:
  """Schema checks that don't reduce to a required-key list."""
  errors = []
  fname = os.path.basename(rel_path)

  if 'version' in data and not VERSION_RE.match(str(data['version'])):
    errors.append(
        f'{rel_path}: version must be the {VERSION_SENTINEL!r} sentinel or a '
        f'release version like "v54.0" (roll-prebuilts stamps it); '
        f'got {data["version"]!r}')

  if fname == 'marketplace.json':
    plugins = data.get('plugins', [])
    if not isinstance(plugins, list) or not plugins:
      errors.append(f'{rel_path}: "plugins" must be a non-empty array')
    for i, p in enumerate(plugins):
      if not isinstance(p, dict):
        errors.append(f'{rel_path}: plugins[{i}] must be an object')
        continue
      if 'name' not in p or not p['name']:
        errors.append(f'{rel_path}: plugins[{i}] missing "name"')
      if 'source' not in p:
        errors.append(f'{rel_path}: plugins[{i}] missing "source"')

  if rel_path == 'claude-code/marketplace.json':
    owner = data.get('owner')
    if not isinstance(owner, dict) or not owner.get('name'):
      errors.append(f'{rel_path}: "owner" must be an object with a "name"')

  return errors


def _check_file(rel_path: str) -> List[str]:
  abs_path = os.path.join(EXTENSIONS_DIR, rel_path)
  if not os.path.isfile(abs_path):
    return [f'{rel_path}: required manifest is missing']
  try:
    with open(abs_path, 'r') as f:
      data = json.load(f)
  except json.JSONDecodeError as e:
    return [f'{rel_path}: invalid JSON: {e}']
  except OSError as e:
    return [f'{rel_path}: cannot read: {e}']

  errors = []
  for key in REQUIRED[rel_path]:
    if key not in data:
      errors.append(f'{rel_path}: missing required field "{key}"')
    elif data[key] in (None, '', [], {}):
      errors.append(f'{rel_path}: field "{key}" must not be empty')
  errors.extend(_check_specific(rel_path, data))
  return errors


def _available_skill_slugs() -> List[str]:
  out = []
  if not os.path.isdir(SKILLS_DIR):
    return out
  for d in sorted(os.listdir(SKILLS_DIR)):
    if os.path.isfile(os.path.join(SKILLS_DIR, d, 'SKILL.md')):
      out.append(d.replace('_', '-'))
  return out


def _check_targets_json() -> List[str]:
  if not os.path.isfile(TARGETS_JSON):
    return ['ai/skills/targets.json: required file is missing']
  try:
    with open(TARGETS_JSON, 'r') as f:
      data = json.load(f)
  except json.JSONDecodeError as e:
    return [f'ai/skills/targets.json: invalid JSON: {e}']

  errors = []
  entries = data.get('skills') if isinstance(data, dict) else None
  if not isinstance(entries, list):
    return ['ai/skills/targets.json: top-level "skills" must be an array']

  declared: List[str] = []
  for i, entry in enumerate(entries):
    prefix = f'ai/skills/targets.json: skills[{i}]'
    if not isinstance(entry, dict):
      errors.append(f'{prefix}: must be an object')
      continue
    name = entry.get('name')
    targets = entry.get('targets')
    if not isinstance(name, str) or not name:
      errors.append(f'{prefix}: missing "name"')
    else:
      if name in declared:
        errors.append(f'{prefix}: duplicate entry for {name!r}')
      declared.append(name)
    if not isinstance(targets, list) or not targets:
      errors.append(f'{prefix} ({name}): "targets" must be a non-empty array')
    else:
      bad = [t for t in targets if t not in VALID_TARGETS]
      if bad:
        errors.append(f'{prefix} ({name}): unknown targets {bad}; '
                      f'valid: {list(VALID_TARGETS)}')

  available = set(_available_skill_slugs())
  missing = sorted(available - set(declared))
  extra = sorted(set(declared) - available)
  if missing:
    errors.append(
        f'ai/skills/targets.json: missing entries for {missing}; every skill '
        f'under ai/skills/ must be listed explicitly')
  if extra:
    errors.append(
        f'ai/skills/targets.json: unknown entries {extra}; no matching '
        f'directory under ai/skills/')
  return errors


def main() -> int:
  if not os.path.isdir(EXTENSIONS_DIR):
    print(f'ai/extensions/ not found at {EXTENSIONS_DIR}', file=sys.stderr)
    return 1

  errors = []
  for rel_path in sorted(REQUIRED):
    errors.extend(_check_file(rel_path))
  errors.extend(_check_targets_json())

  if errors:
    print('Extension manifest errors:', file=sys.stderr)
    for e in errors:
      print(f'  {e}', file=sys.stderr)
    return 1
  return 0


if __name__ == '__main__':
  sys.exit(main())
