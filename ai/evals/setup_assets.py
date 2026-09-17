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
"""Assembles the assets directory that conditions.json refers to as {assets}.

The things under test are deliberately not in the source tree in loadable
form (the skill needs bundling, the binary needs building), so this script
gathers them once into a directory outside the checkout:

    <assets>/skill-published/plugins/perfetto   the skill from the ai-agents
                                                branch (what users install)
    <assets>/skill-local/plugins/perfetto       this checkout's ai/skills,
                                                bundled by build_ai_agents.py
    <assets>/bin/trace_processor                a trace_processor_shell binary,
                                                exposed under both names

Usage:
    ai/evals/setup_assets.py --out /path/to/assets \\
        --tp-binary out/mac_release/trace_processor_shell
    export EVAL_ASSETS=/path/to/assets

Pass --published-ref to bundle a different published ref (default:
origin/ai-agents; fetch it first).
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent


def _run(cmd, **kw):
  print('+', ' '.join(str(c) for c in cmd), flush=True)
  subprocess.run(cmd, check=True, **kw)


def main() -> int:
  ap = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('--out', required=True, help='assets directory to create')
  ap.add_argument(
      '--tp-binary',
      help='trace_processor_shell to expose at <assets>/bin (skip to omit)')
  ap.add_argument('--published-ref', default='origin/ai-agents')
  args = ap.parse_args()

  out = Path(args.out).resolve()
  if str(out).startswith(str(REPO) + os.sep):
    sys.exit('assets must live outside the checkout: agents are sandboxed '
             'away from the repo and would not be able to read them')
  out.mkdir(parents=True, exist_ok=True)

  # Published skill: the tree users get from the marketplace.
  published = out / 'skill-published'
  shutil.rmtree(published, ignore_errors=True)
  published.mkdir()
  archive = subprocess.run([
      'git', '-C',
      str(REPO), 'archive', args.published_ref, 'plugins/perfetto'
  ],
                           check=True,
                           capture_output=True).stdout
  subprocess.run(['tar', '-x', '-C', str(published)], input=archive, check=True)

  # Local skill: this checkout's ai/skills, assembled the way the release
  # pipeline does it.
  local = out / 'skill-local'
  _run([
      sys.executable,
      str(REPO / 'tools' / 'release' / 'build_ai_agents.py'), '--output',
      str(local)
  ])

  if args.tp_binary:
    bin_dir = out / 'bin'
    bin_dir.mkdir(exist_ok=True)
    dst = bin_dir / 'trace_processor'
    shutil.copy2(args.tp_binary, dst)
    dst.chmod(0o755)
    alias = bin_dir / 'trace_processor_shell'
    if alias.exists() or alias.is_symlink():
      alias.unlink()
    alias.symlink_to('trace_processor')
    _run([str(dst), '--version'])

  print(f'\nassets ready in {out}\nexport EVAL_ASSETS={out}')
  return 0


if __name__ == '__main__':
  sys.exit(main())
