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
    bin/trace_processor                 ← bundled Python wrapper
    plugins/perfetto/
        .claude-plugin/plugin.json      ← Claude plugin manifest
        .codex-plugin/plugin.json       ← Codex plugin manifest
        skills/perfetto/SKILL.md        ← plugin variant of the skill
    skills/
        index.json                      ← OpenCode discovery
        perfetto/SKILL.md               ← fallback variant of the skill
    agents-install                      ← bundled fallback installer
    BRANCH_METADATA.json                ← main_sha, tag, built_at

There is one skill, `ai/skills/perfetto/`, whose entry point is
`SKILL-template.md` (not a loadable `SKILL.md`, so the source tree is a
build input, never a drop-in). The bundler renames it to `SKILL.md` and
resolves the one piece of per-environment variance: which
`environment-references/setup-*.md` variant becomes `setup.md`.

It is emitted twice. Plugin-style agents (Claude, Codex) consume the
in-tree `plugins/perfetto/skills/` copy and get the `setup-bundled.md`
variant (the plugin ships `bin/trace_processor`). Fallback-style agents
(Pi, OpenCode, Antigravity, and generic fallback installs) consume the
root `skills/` copy and get the `setup-standalone.md` variant (they fetch
the binary themselves). Antigravity is intentionally treated as a
fallback consumer, not a plugin consumer, because the release branch
cannot currently pin `agy plugin install` to a non-default git ref.

This script does no stamping: the release version is written into the
source manifests and the bundled `agents-install` by tools/release/
roll-prebuilts (alongside the prebuilt binary roll), so this just copies
already-versioned files. At release time the finalize-release GitHub
Action rolls the prebuilts, overlays this tag's ai/skills, runs this, then
opens a PR (base: ai-agents) for a maintainer to review and merge — the
bundle is never pushed to ai-agents directly.

Local usage (builds the tree only):
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
SKILLS_SRC = REPO_ROOT / 'ai' / 'skills'
SKILL_NAME = 'perfetto'
SKILL_SRC = SKILLS_SRC / SKILL_NAME
EXTENSIONS_SRC = REPO_ROOT / 'ai' / 'extensions'
TRACE_PROCESSOR_SRC = REPO_ROOT / 'tools' / 'trace_processor'
AGENTS_INSTALL_SRC = REPO_ROOT / 'tools' / 'agents-install'
# The manifest whose `version` field we treat as the bundle's version (all
# manifests carry the same value, stamped by roll-prebuilts).
VERSION_MANIFEST = EXTENSIONS_SRC / 'claude-code' / 'marketplace.json'

# The router entry point is a template in source control so the source tree is
# never mistaken for a loadable skill; the bundler renames it to SKILL.md.
SKILL_TEMPLATE = 'SKILL-template.md'
ENV_REF_DIR = 'environment-references'
# Which environment-references/setup-*.md variant becomes setup.md, keyed by
# target class. Plugin installs ship bin/trace_processor; fallback installs
# fetch it themselves.
SETUP_VARIANT = {
    'plugin': 'setup-bundled.md',
    'fallback': 'setup-standalone.md',
}
# Source-only files that must never ship to the release branch: the template
# (re-emitted as SKILL.md), the unselected setup variants (re-emitted as
# setup.md), and dev/test metadata.
_EMIT_IGNORE = shutil.ignore_patterns(SKILL_TEMPLATE, 'setup-bundled.md',
                                      'setup-standalone.md', 'OWNERS',
                                      'TEST.md', 'BUILD')


def _emit_skill(variant: str, dest_dir: Path) -> str:
  """Emit the single `perfetto` skill into dest_dir, resolved for `variant`.

  Copies the skill tree verbatim except for the two source-only transforms:
  SKILL-template.md -> SKILL.md (rename) and the chosen setup-*.md -> setup.md.
  Returns the emitted skill name.
  """
  out_dir = dest_dir / SKILL_NAME
  shutil.copytree(SKILL_SRC, out_dir, ignore=_EMIT_IGNORE)
  # Router: SKILL-template.md -> SKILL.md (verbatim, no content rewrite).
  shutil.copy(SKILL_SRC / SKILL_TEMPLATE, out_dir / 'SKILL.md')
  # Environment setup: select the variant for this target class.
  shutil.copy(SKILL_SRC / ENV_REF_DIR / SETUP_VARIANT[variant],
              out_dir / ENV_REF_DIR / 'setup.md')
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


def build(output: Path) -> None:
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
      (AGENTS_INSTALL_SRC, output / 'agents-install'),
  ]
  for src, dst in copies:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(src, dst)
  (output / 'bin' / 'trace_processor').chmod(0o755)
  (output / 'agents-install').chmod(0o755)

  _emit_skill('plugin', output / 'plugins' / 'perfetto' / 'skills')
  _emit_skill('fallback', output / 'skills')
  _write_index(output / 'skills')

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
  print(f'  plugin skill:   {SKILL_NAME} (setup: {SETUP_VARIANT["plugin"]})')
  print(f'  fallback skill: {SKILL_NAME} (setup: {SETUP_VARIANT["fallback"]})')
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
      '--commit-and-git-init',
      action='store_true',
      help='After assembling, initialize the output as a fresh git repo '
      'with one commit on an `ai-agents` branch (for ad-hoc local testing '
      'against a personal fork).')
  args = ap.parse_args()

  build(args.output)
  if args.commit_and_git_init:
    commit(args.output, f'RFC-0026 ai-agents branch (built from {_main_sha()})')
    print(f'  committed to {args.output}/.git (branch: ai-agents)')
  return 0


if __name__ == '__main__':
  sys.exit(main())
