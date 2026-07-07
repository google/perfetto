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

Reads the source-of-truth manifests under ai/extensions/ and the single
consolidated `perfetto` skill under ai/skills/, and writes the
namespace-distinct branch layout each supported agent's loader expects.
See ai/extensions/README.md and RFC-0026 for the design.

The branch layout produced:

    .claude-plugin/marketplace.json     ← Claude marketplace
    .agents/plugins/marketplace.json    ← Codex marketplace
    plugins/perfetto/
        .claude-plugin/plugin.json      ← Claude plugin manifest
        .codex-plugin/plugin.json       ← Codex plugin manifest
        skills/
            index.json                  ← OpenCode discovery
            perfetto/SKILL.md           ← the skill
            perfetto/bin/trace_processor  ← bundled wrapper
    BRANCH_METADATA.json                ← main_sha, tag, built_at

The fallback installer is not bundled here: get.perfetto.dev/agents-install
serves it straight from main's `tools/agents-install`.

There is one skill, `ai/skills/perfetto/`, whose entry point is
`SKILL-template.md` (not a loadable `SKILL.md`, so the source tree is a
build input, never a drop-in). The bundler renames it to `SKILL.md` and
copies the `tools/trace_processor` wrapper into the skill's `bin/`.

The skill is emitted exactly once, into `plugins/perfetto/skills/`.
Plugin-style agents (Claude, Codex) install the `plugins/perfetto/`
subdir; every other consumer (Pi, OpenCode, Antigravity, generic
fallback installs via tools/agents-install) reads the same tree at its
full `plugins/perfetto/skills/` path. The wrapper at
`<skill root>/bin/trace_processor` is what the skill's
`environment-references/setup.md` points `$SKILL_ROOT`-based invocations
at. Antigravity is intentionally treated as a fallback consumer, not a
plugin consumer, because the release branch cannot currently pin
`agy plugin install` to a non-default git ref.

This script does no stamping: the release version is written into the
source manifests and `tools/agents-install` by tools/release/
roll-prebuilts (alongside the prebuilt binary roll), so this just copies
already-versioned files. At release time the finalize-release GitHub
Action rolls the prebuilts, checks the release tag out into a separate
worktree, runs this with `--skills-src <worktree>/ai/skills` (so the
bundle ships the tag's skills, not main's), then opens a PR (base:
ai-agents) for a maintainer to review and merge — the bundle is never
pushed to ai-agents directly.

Local usage (builds the tree only, skills from this checkout):
    tools/release/build_ai_agents.py --output /tmp/ai-agents-tree

`--commit-and-git-init` additionally inits a throwaway orphan repo in the
output dir for ad-hoc testing against a personal fork. It is not how the
release branch is published; the Action opens a reviewable PR.
"""

import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SKILLS_SRC = REPO_ROOT / 'ai' / 'skills'
SKILL_NAME = 'perfetto'
EXTENSIONS_SRC = REPO_ROOT / 'ai' / 'extensions'
TRACE_PROCESSOR_SRC = REPO_ROOT / 'tools' / 'trace_processor'
# The manifest whose `version` field we treat as the bundle's version (all
# manifests carry the same value, stamped by roll-prebuilts).
VERSION_MANIFEST = EXTENSIONS_SRC / 'claude-code' / 'marketplace.json'

# The router entry point is a template in source control so the source tree is
# never mistaken for a loadable skill; the bundler renames it to SKILL.md.
SKILL_TEMPLATE = 'SKILL-template.md'
# Source-only files that must never ship to the release branch: the template
# (re-emitted as SKILL.md) and dev/test metadata.
_EMIT_IGNORE = shutil.ignore_patterns(SKILL_TEMPLATE, 'OWNERS', 'TEST.md',
                                      'BUILD')


def _emit_skill(skill_src: Path, dest_dir: Path) -> str:
  """Emit the single `perfetto` skill into dest_dir.

  Copies the skill tree verbatim except for the source-only transform
  SKILL-template.md -> SKILL.md (rename), and adds the trace_processor
  wrapper at bin/trace_processor so `$SKILL_ROOT/bin/trace_processor`
  resolves in every install. Returns the emitted skill name.
  """
  out_dir = dest_dir / SKILL_NAME
  shutil.copytree(skill_src, out_dir, ignore=_EMIT_IGNORE)
  # Router: SKILL-template.md -> SKILL.md (verbatim, no content rewrite).
  shutil.copy(skill_src / SKILL_TEMPLATE, out_dir / 'SKILL.md')
  # The bundled wrapper, inside the skill so it survives every install
  # method (plugin subdir, agents-install copytree, index.json fetch).
  (out_dir / 'bin').mkdir()
  shutil.copy(TRACE_PROCESSOR_SRC, out_dir / 'bin' / 'trace_processor')
  (out_dir / 'bin' / 'trace_processor').chmod(0o755)
  return SKILL_NAME


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
  (skills_dir /
   'index.json').write_text(json.dumps({'skills': skills}, indent=2) + '\n')


def _bundle_version() -> str:
  return json.loads(VERSION_MANIFEST.read_text()).get('version', '')


def _main_sha() -> str:
  return subprocess.check_output(
      ['git', '-C', str(REPO_ROOT), 'rev-parse', 'HEAD']).decode().strip()


def build(output: Path, skills_src: Path) -> None:
  skill_src = skills_src / SKILL_NAME
  if not (skill_src / SKILL_TEMPLATE).is_file():
    sys.exit(f'error: {skill_src / SKILL_TEMPLATE} not found. The skills '
             'source must use the single-skill layout (#6156); release tags '
             'from before that migration cannot be bundled.')
  if output.exists():
    shutil.rmtree(output)
  output.mkdir(parents=True)

  # Manifests → their namespace-distinct destinations.
  plugin_dir = output / 'plugins' / 'perfetto'
  copies = [
      (EXTENSIONS_SRC / 'claude-code' / 'marketplace.json',
       output / '.claude-plugin' / 'marketplace.json'),
      (EXTENSIONS_SRC / 'claude-code' / 'plugin.json',
       plugin_dir / '.claude-plugin' / 'plugin.json'),
      (EXTENSIONS_SRC / 'codex' / 'marketplace.json',
       output / '.agents' / 'plugins' / 'marketplace.json'),
      (EXTENSIONS_SRC / 'codex' / 'plugin.json',
       plugin_dir / '.codex-plugin' / 'plugin.json'),
  ]
  for src, dst in copies:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, dst)

  _emit_skill(skill_src, plugin_dir / 'skills')
  _write_index(plugin_dir / 'skills')

  # Branch metadata. The version is read from the source manifests, which
  # roll-prebuilts already stamped — this script does not rewrite it.
  version = _bundle_version()
  meta = {
      'main_sha':
          _main_sha(),
      'tag':
          version,
      'built_at':
          datetime.datetime.now(datetime.timezone.utc
                               ).isoformat().replace('+00:00', 'Z'),
  }
  (output /
   'BRANCH_METADATA.json').write_text(json.dumps(meta, indent=2) + '\n')

  print(f'Built ai-agents tree at {output}')
  print(f'  skills src:     {skills_src}')
  print(f'  skill:     {SKILL_NAME} (emitted to skills/, wrapper at '
        f'bin/trace_processor)')
  print(f'  main_sha:  {meta["main_sha"]}')
  print(f'  version:   {version}')


def commit(output: Path, message: str) -> None:
  subprocess.check_call(['git', 'init', '-q', '-b', 'ai-agents'], cwd=output)
  subprocess.check_call(['git', 'add', '.'], cwd=output)
  user_name = subprocess.check_output(['git', 'config', 'user.name'
                                      ]).decode().strip() or 'ai-agents-builder'
  user_email = subprocess.check_output([
      'git', 'config', 'user.email'
  ]).decode().strip() or 'noreply@perfetto.dev'
  subprocess.check_call([
      'git', '-c', f'user.email={user_email}', '-c', f'user.name={user_name}',
      'commit', '-q', '-m', message
  ],
                        cwd=output)


def main() -> int:
  ap = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument(
      '--output',
      type=Path,
      default=Path('/tmp/ai-agents-tree'),
      help='Directory to write the assembled tree into '
      '(will be removed if it exists).')
  ap.add_argument(
      '--skills-src',
      type=Path,
      default=DEFAULT_SKILLS_SRC,
      help='ai/skills tree to bundle, e.g. from a worktree of the release '
      'tag (default: this checkout\'s ai/skills). Everything else (manifests, '
      'trace_processor wrapper, installer) always comes from this checkout.')
  ap.add_argument(
      '--commit-and-git-init',
      action='store_true',
      help='After assembling, initialize the output as a fresh git repo '
      'with one commit on an `ai-agents` branch (for ad-hoc local testing '
      'against a personal fork).')
  args = ap.parse_args()

  build(args.output, args.skills_src.resolve())
  if args.commit_and_git_init:
    commit(args.output, f'RFC-0026 ai-agents branch (built from {_main_sha()})')
    print(f'  committed to {args.output}/.git (branch: ai-agents)')
  return 0


if __name__ == '__main__':
  sys.exit(main())
