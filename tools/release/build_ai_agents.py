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
"""Assembles the `ai-agents` release branch tree from main.

Reads the source-of-truth manifests under ai/extensions/ and the skills
under ai/skills/, and writes the namespace-distinct branch layout each
supported agent's loader expects. See ai/extensions/README.md and
RFC-0026 for the design.

The branch layout produced:

    .claude-plugin/marketplace.json     ← Claude marketplace
    .agents/plugins/marketplace.json    ← Codex marketplace
    bin/trace_processor                 ← bundled Python wrapper
    plugins/perfetto/
        .claude-plugin/plugin.json      ← Claude plugin manifest
        .codex-plugin/plugin.json       ← Codex plugin manifest
        skills/<dashed-slug>/SKILL.md   ← plugin-target skill set
    skills/
        index.json                      ← OpenCode discovery
        <dashed-slug>/SKILL.md          ← fallback-target skill set
    BRANCH_METADATA.json                ← main_sha, tag, built_at

Two skill sets are produced because plugin-style agents (Claude, Codex)
consume the in-tree subdirectory while fallback-style agents (Pi,
OpenCode, Antigravity, and generic fallback installs) consume the root
`skills/`. Antigravity is intentionally treated as a fallback consumer,
not a plugin consumer, because the release branch cannot currently pin
`agy plugin install` to a non-default git ref. The split is driven by the
`targets:` field in each SKILL.md frontmatter.

Local usage:
    tools/release/build_ai_agents.py --output /tmp/ai-agents-tree
        [--version v0.0.0-prototype] [--commit-and-git-init]

To push a locally-built tree to a remote ai-agents branch:
    git -C <output> push <remote> --force HEAD:refs/heads/ai-agents
"""

import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[2]
SKILLS_SRC = REPO_ROOT / 'ai' / 'skills'
EXTENSIONS_SRC = REPO_ROOT / 'ai' / 'extensions'
TRACE_PROCESSOR_SRC = REPO_ROOT / 'tools' / 'trace_processor'
VERSION_SENTINEL = '0.0.0-dev'

# Which targets each consumer's skill set is built against.
PLUGIN_TARGETS = ('claude-code', 'codex')
FALLBACK_TARGETS = ('fallback',)


def _parse_targets(skill_md: Path) -> Optional[List[str]]:
  text = skill_md.read_text()
  m = re.search(r'^---\n(.*?)\n---', text, flags=re.S)
  if not m:
    return None
  tm = re.search(r'^targets:\s*\[(.*?)\]', m.group(1), flags=re.M)
  if not tm:
    return None
  return [t.strip() for t in tm.group(1).split(',') if t.strip()]


def _emit_skills(targets: Sequence[str], dest_dir: Path) -> List[str]:
  """Copy every skill whose `targets:` overlaps with `targets` into dest_dir.

  Skills with no `targets:` field are included for every target.
  Returns the list of dashed slug names emitted.
  """
  emitted = []
  for slug in sorted(os.listdir(SKILLS_SRC)):
    skill_md = SKILLS_SRC / slug / 'SKILL.md'
    if not skill_md.is_file():
      continue
    skill_targets = _parse_targets(skill_md)
    if skill_targets is not None and not any(t in targets for t in skill_targets):
      continue
    dashed = slug.replace('_', '-')
    out_dir = dest_dir / dashed
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(skill_md, out_dir / 'SKILL.md')
    # Copy any non-SKILL.md files that ship with the skill (references/,
    # scripts/, assets/). Skip BUILD / OWNERS / EVAL.txtpb — those are
    # build-system / eval metadata, not part of the shipped skill.
    for extra in ('references', 'assets', 'scripts'):
      src_extra = SKILLS_SRC / slug / extra
      if src_extra.is_dir():
        shutil.copytree(src_extra, out_dir / extra)
    emitted.append(dashed)
  return emitted


def _write_index(skills_dir: Path) -> None:
  """Write skills/index.json for OpenCode skills.urls discovery."""
  skills = []
  for d in sorted(os.listdir(skills_dir)):
    sub = skills_dir / d
    if not sub.is_dir():
      continue
    files = []
    for root, _, fnames in os.walk(sub):
      for f in fnames:
        rel = Path(root, f).relative_to(sub)
        files.append(str(rel))
    skills.append({'name': d, 'files': sorted(files)})
  (skills_dir / 'index.json').write_text(
      json.dumps({'skills': skills}, indent=2) + '\n')


def _rewrite_version(manifest_path: Path, new_version: str) -> None:
  data = json.loads(manifest_path.read_text())
  if 'version' in data:
    data['version'] = new_version
  manifest_path.write_text(json.dumps(data, indent=2) + '\n')


def _main_sha() -> str:
  return subprocess.check_output(['git', '-C', str(REPO_ROOT), 'rev-parse',
                                  'HEAD']).decode().strip()


def build(output: Path, version: str) -> None:
  if output.exists():
    shutil.rmtree(output)
  output.mkdir(parents=True)

  # Manifests → their namespace-distinct destinations.
  copies = [
      (EXTENSIONS_SRC / 'claude-code' / 'marketplace.json',
       output / '.claude-plugin' / 'marketplace.json'),
      (EXTENSIONS_SRC / 'claude-code' / 'plugin.json',
       output / 'plugins' / 'perfetto' / '.claude-plugin' / 'plugin.json'),
      (EXTENSIONS_SRC / 'codex' / 'marketplace.json',
       output / '.agents' / 'plugins' / 'marketplace.json'),
      (EXTENSIONS_SRC / 'codex' / 'plugin.json',
       output / 'plugins' / 'perfetto' / '.codex-plugin' / 'plugin.json'),
      (TRACE_PROCESSOR_SRC, output / 'bin' / 'trace_processor'),
  ]
  for src, dst in copies:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, dst)
  (output / 'bin' / 'trace_processor').chmod(0o755)

  # Two skill sets per the per-consumer split.
  plugin_skills = _emit_skills(PLUGIN_TARGETS,
                               output / 'plugins' / 'perfetto' / 'skills')
  fallback_skills = _emit_skills(FALLBACK_TARGETS, output / 'skills')
  _write_index(output / 'skills')

  # Branch metadata.
  meta = {
      'main_sha': _main_sha(),
      'tag': version,
      'built_at':
          datetime.datetime.now(
              datetime.UTC).isoformat().replace('+00:00', 'Z'),
  }
  (output / 'BRANCH_METADATA.json').write_text(json.dumps(meta, indent=2) + '\n')

  # Rewrite the version sentinel in every manifest that carries one.
  manifest_paths = [
      output / '.claude-plugin' / 'marketplace.json',
      output / 'plugins' / 'perfetto' / '.claude-plugin' / 'plugin.json',
      output / 'plugins' / 'perfetto' / '.codex-plugin' / 'plugin.json',
      output / '.agents' / 'plugins' / 'marketplace.json',
  ]
  for p in manifest_paths:
    _rewrite_version(p, version)

  print(f'Built ai-agents tree at {output}')
  print(f'  plugin skills ({len(plugin_skills)}):    {", ".join(plugin_skills)}')
  print(f'  fallback skills ({len(fallback_skills)}): '
        f'{", ".join(fallback_skills)}')
  print(f'  main_sha:  {meta["main_sha"]}')
  print(f'  version:   {version}')


def commit(output: Path, message: str) -> None:
  subprocess.check_call(['git', 'init', '-q', '-b', 'ai-agents'], cwd=output)
  subprocess.check_call(['git', 'add', '.'], cwd=output)
  user_name = subprocess.check_output(
      ['git', 'config', 'user.name']).decode().strip() or 'ai-agents-builder'
  user_email = subprocess.check_output(
      ['git', 'config', 'user.email']).decode().strip() or 'noreply@perfetto.dev'
  subprocess.check_call([
      'git', '-c', f'user.email={user_email}', '-c', f'user.name={user_name}',
      'commit', '-q', '-m', message
  ],
                        cwd=output)


def main() -> int:
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('--output',
                  type=Path,
                  default=Path('/tmp/ai-agents-tree'),
                  help='Directory to write the assembled tree into '
                  '(will be removed if it exists).')
  ap.add_argument('--version',
                  default=VERSION_SENTINEL,
                  help='Value to write into every manifest version field. '
                  'Use the release tag (e.g. v54.0) at release time.')
  ap.add_argument(
      '--commit-and-git-init',
      action='store_true',
      help='After assembling, initialize the output as a fresh git repo '
      'with one commit on an `ai-agents` branch (so it can be pushed '
      'directly via `git push <remote> --force HEAD:refs/heads/ai-agents`).')
  args = ap.parse_args()

  build(args.output, args.version)
  if args.commit_and_git_init:
    commit(args.output, f'RFC-0026 ai-agents branch (built from {_main_sha()})')
    print(f'  committed to {args.output}/.git (branch: ai-agents)')
  return 0


if __name__ == '__main__':
  sys.exit(main())
