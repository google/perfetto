#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyyaml>=6"]
# ///
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
"""Eval runner for the Perfetto agent skill and agent-facing tooling.

Runs each case under `cases/` against every requested *condition* (which
skill is installed, what is on PATH, extra env), N times each, in a
throwaway sandboxed workspace, driving a coding-agent harness in
non-interactive mode. The transcript is normalised to a harness-neutral
shape, graded with deterministic graders (regex on the final answer,
tool-call assertions) and optional LLM-judge graders, and a set of process
indicators is computed for every run (did it use the skill, did it use a
warm session, how many trace_processor calls, SQL errors, cost, ...).

Harnesses: `claude` (Claude Code, `claude -p`), `codex` (OpenAI Codex,
`codex exec --json`), `gemini` (Gemini CLI, `gemini -p`; untested).

Case layout:

    cases/<case>/prompt.md          frontmatter + prompt body
    cases/<case>/graders/*.md       frontmatter (type: regex|bash|tool_used|
                                    file_exists|llm) + description body
    conditions.json                 named conditions

Usage:
    run_evals.py run  --conditions baseline-tp,skill-local [--agent codex]
                      [--cases 'cpu-*'] [--tag hard] [--runs 3] [--jobs 3]
                      [--model opus] [--out DIR]
    run_evals.py grade   RESULTS_DIR [--skip-llm]   # re-grade transcripts
    run_evals.py report  RESULTS_DIR                # per-dir markdown report
    run_evals.py compare RESULTS_DIR...             # one table across dirs
"""

import argparse
import concurrent.futures
import datetime
import fnmatch
import glob
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

import yaml

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
CASES_DIR = ROOT / 'cases'

# ----------------------------------------------------------------------------
# Loading cases / conditions
# ----------------------------------------------------------------------------


def parse_frontmatter(text: str):
  """Splits a markdown file into (frontmatter dict, body)."""
  if not text.startswith('---'):
    return {}, text
  end = text.find('\n---', 3)
  if end < 0:
    return {}, text
  return yaml.safe_load(text[3:end]) or {}, text[end + 4:].lstrip('\n')


def expand(s: str, vars_: Dict[str, str]) -> str:
  s = os.path.expandvars(s)
  for k, v in vars_.items():
    s = s.replace('{' + k + '}', v)
  return s


def load_cases(pattern: str) -> List[Dict[str, Any]]:
  cases = []
  for d in sorted(CASES_DIR.iterdir()):
    if not d.is_dir() or not (d / 'prompt.md').exists():
      continue
    if not fnmatch.fnmatch(d.name, pattern):
      continue
    fm, body = parse_frontmatter((d / 'prompt.md').read_text())
    graders = []
    gdir = d / 'graders'
    for g in sorted(gdir.glob('*.md')) if gdir.is_dir() else []:
      gfm, gbody = parse_frontmatter(g.read_text())
      gfm['name'] = g.stem
      gfm['description'] = gbody.strip()
      graders.append(gfm)
    cases.append({
        'id': d.name,
        'dir': str(d),
        'name': fm.get('name', d.name),
        'tags': fm.get('tags', []),
        'runs': fm.get('runs'),
        'files': fm.get('files', []),
        'timeout_seconds': fm.get('timeout_seconds'),
        'prompt': body.strip(),
        'graders': graders,
    })
  return cases


def load_conditions(path: Path) -> Dict[str, Any]:
  return json.loads(path.read_text())


# ----------------------------------------------------------------------------
# Harnesses. Each one knows how to launch its agent non-interactively, how to
# make a skill available to it, and how to turn its transcript into the
# neutral shape the graders consume:
#
#   {'tool_uses':    [{'id', 'name', 'input'}]   name is Bash|Read|Skill|...
#    'tool_results': [{'id', 'text'}]
#    'texts':        [assistant text blocks in order]
#    'final':        the last substantial answer
#    'usage':        {'cost_usd', 'duration_ms', 'num_turns',
#                     'input_tokens', 'output_tokens', 'is_error'}}
# ----------------------------------------------------------------------------


def _skill_dirs(cond: Dict[str, Any], vars_: Dict[str, str]) -> List[Path]:
  """The skill folders (each holding a SKILL.md) a condition provides.
  Conditions name plugin directories (`<plugin>/skills/<name>/SKILL.md`);
  harnesses without a plugin concept get the skill folders copied into
  their project-level discovery path instead."""
  out = []
  for pd in cond.get('plugin_dirs', []):
    for skill_md in sorted(Path(expand(pd, vars_)).glob('skills/*/SKILL.md')):
      out.append(skill_md.parent)
  return out


def _pick_final(texts: List[str], last: str) -> str:
  # Agents often end with a one-liner ("Session cleaned up.") after the
  # real answer: prefer the last substantial text block.
  if len(last.strip()) >= 200:
    return last
  for t in reversed(texts):
    if len(t.strip()) >= 200:
      return t
  return last or (texts[-1] if texts else '')


def _read_jsonl(path: Path) -> List[Dict[str, Any]]:
  events = []
  with open(path) as f:
    for line in f:
      line = line.strip()
      if line:
        try:
          events.append(json.loads(line))
        except json.JSONDecodeError:
          pass
  return events


class ClaudeHarness:
  name = 'claude'
  default_model = 'opus'

  def prepare(self, ws: Path, cond: Dict[str, Any], vars_: Dict[str, str]):
    pass  # skills are passed as --plugin-dir; nothing to copy.

  def build_cmd(self, prompt: str, cond: Dict[str, Any], model: Optional[str],
                vars_: Dict[str, str], budget: Optional[float]) -> List[str]:
    cmd = [
        'claude', '-p', prompt, '--output-format', 'stream-json', '--verbose',
        '--setting-sources', '', '--dangerously-skip-permissions', '--model',
        model or self.default_model
    ]
    for pd in cond.get('plugin_dirs', []):
      cmd += ['--plugin-dir', expand(pd, vars_)]
    if cond.get('append_system_prompt'):
      cmd += [
          '--append-system-prompt',
          expand(cond['append_system_prompt'], vars_)
      ]
    if cond.get('effort'):
      cmd += ['--effort', cond['effort']]
    if budget:
      cmd += ['--max-budget-usd', str(budget)]
    return cmd

  def parse(self, transcript: Path) -> Dict[str, Any]:
    tool_uses, tool_results, texts = [], [], []
    result = None
    for e in _read_jsonl(transcript):
      t = e.get('type')
      if t == 'assistant':
        for c in e.get('message', {}).get('content', []):
          if c.get('type') == 'tool_use':
            tool_uses.append({
                'id': c.get('id'),
                'name': c.get('name'),
                'input': c.get('input', {})
            })
          elif c.get('type') == 'text' and c.get('text', '').strip():
            texts.append(c['text'])
      elif t == 'user':
        for c in e.get('message', {}).get('content', []) or []:
          if isinstance(c, dict) and c.get('type') == 'tool_result':
            content = c.get('content')
            if isinstance(content, list):
              content = '\n'.join(
                  x.get('text', '') for x in content if isinstance(x, dict))
            tool_results.append({
                'id': c.get('tool_use_id'),
                'text': content or ''
            })
      elif t == 'result':
        result = e
    res = result or {}
    usage = res.get('usage', {})
    return {
        'tool_uses': tool_uses,
        'tool_results': tool_results,
        'texts': texts,
        'final': _pick_final(texts,
                             res.get('result') or ''),
        'usage': {
            'cost_usd': res.get('total_cost_usd'),
            'duration_ms': res.get('duration_ms'),
            'num_turns': res.get('num_turns'),
            'input_tokens': (usage.get('input_tokens', 0) +
                             usage.get('cache_creation_input_tokens', 0) +
                             usage.get('cache_read_input_tokens', 0)),
            'output_tokens': usage.get('output_tokens'),
            'is_error': bool(res.get('is_error')) if result else True,
        },
    }


class CodexHarness:
  """OpenAI Codex: `codex exec --json`. Skills are discovered from the
  workspace's `.agents/skills/`, so the condition's skills are copied
  there. Approvals and Codex's own sandbox are bypassed because the trial
  is already sandboxed by this runner (Codex documents that flag for
  exactly this situation)."""
  name = 'codex'
  default_model = None  # Codex's configured default.

  def prepare(self, ws: Path, cond: Dict[str, Any], vars_: Dict[str, str]):
    for skill in _skill_dirs(cond, vars_):
      shutil.copytree(skill, ws / '.agents' / 'skills' / skill.name)

  def build_cmd(self, prompt, cond, model, vars_, budget):
    cmd = [
        'codex', 'exec', '--json', '--ephemeral',
        '--dangerously-bypass-approvals-and-sandbox'
    ]
    if model or self.default_model:
      cmd += ['-m', model or self.default_model]
    if cond.get('append_system_prompt'):
      prompt = expand(cond['append_system_prompt'], vars_) + '\n\n' + prompt
    return cmd + [prompt]

  def parse(self, transcript):
    tool_uses, tool_results, texts = [], [], []
    usage: Dict[str, Any] = {}
    n = 0
    for e in _read_jsonl(transcript):
      item = e.get('item') or {}
      if e.get('type') == 'item.completed':
        n += 1
        if item.get('type') == 'command_execution':
          tid = item.get('id') or f'cmd-{n}'
          # Codex wraps commands as `/bin/zsh -lc '<cmd>'`; strip the shell.
          cmd = item.get('command', '')
          m = re.match(r"^\S*(?:ba|z)?sh -lc '(.*)'$", cmd, re.S)
          if m:
            cmd = m.group(1).replace("'\\''", "'")
          tool_uses.append({
              'id': tid,
              'name': 'Bash',
              'input': {
                  'command': cmd
              }
          })
          tool_results.append({
              'id': tid,
              'text': item.get('aggregated_output') or ''
          })
        elif item.get('type') == 'agent_message' and item.get('text'):
          texts.append(item['text'])
      elif e.get('type') == 'turn.completed':
        usage = e.get('usage', {})
      elif e.get('type') == 'error':
        texts.append('[error] ' + str(e.get('message', '')))
    return {
        'tool_uses': tool_uses,
        'tool_results': tool_results,
        'texts': texts,
        'final': _pick_final(texts, texts[-1] if texts else ''),
        'usage': {
            'cost_usd': None,  # Codex does not report cost.
            'duration_ms': None,  # Filled from wall time by the runner.
            'num_turns': n,
            'input_tokens': usage.get('input_tokens'),
            'output_tokens': usage.get('output_tokens'),
            'is_error': not texts,
        },
    }


class GeminiHarness:
  """Gemini CLI: `gemini -p --output-format stream-json --yolo`. Skills are
  copied to the workspace's `.gemini/skills/`. UNTESTED: written against
  the documented interface only; verify the event names below against a
  real transcript before trusting its numbers."""
  name = 'gemini'
  default_model = None

  def prepare(self, ws, cond, vars_):
    for skill in _skill_dirs(cond, vars_):
      shutil.copytree(skill, ws / '.gemini' / 'skills' / skill.name)

  def build_cmd(self, prompt, cond, model, vars_, budget):
    cmd = ['gemini', '-p', prompt, '--output-format', 'stream-json', '--yolo']
    if model or self.default_model:
      cmd += ['-m', model or self.default_model]
    return cmd

  def parse(self, transcript):
    tool_uses, tool_results, texts = [], [], []
    n = 0
    for e in _read_jsonl(transcript):
      t = e.get('type')
      if t in ('tool_call', 'tool_use'):
        n += 1
        name = e.get('name') or e.get('tool_name') or ''
        args = e.get('args') or e.get('input') or {}
        mapped = 'Bash' if 'shell' in name.lower() else name
        if mapped == 'Bash' and 'command' not in args:
          args = dict(args, command=args.get('cmd', ''))
        tool_uses.append({
            'id': e.get('id') or f'call-{n}',
            'name': mapped,
            'input': args
        })
      elif t in ('tool_result', 'tool_response'):
        tool_results.append({
            'id': e.get('id') or e.get('call_id'),
            'text': str(e.get('output') or e.get('result') or '')
        })
      elif t in ('message', 'assistant', 'content') and e.get(
          'role', 'assistant') == 'assistant':
        text = e.get('text') or e.get('content') or ''
        if isinstance(text, str) and text.strip():
          texts.append(text)
    return {
        'tool_uses': tool_uses,
        'tool_results': tool_results,
        'texts': texts,
        'final': _pick_final(texts, texts[-1] if texts else ''),
        'usage': {
            'cost_usd': None,
            'duration_ms': None,
            'num_turns': n,
            'input_tokens': None,
            'output_tokens': None,
            'is_error': not texts,
        },
    }


HARNESSES = {
    h.name: h for h in (ClaudeHarness(), CodexHarness(), GeminiHarness())
}

# ----------------------------------------------------------------------------
# Isolation. The no-skill baseline only means something if the agent cannot
# find help lying around: it must not see the developer's checkout (agents
# walk up from cwd and use whatever build they find) nor the wrapper's
# prebuilt cache (some models know its path and look there).
# ----------------------------------------------------------------------------


def workspace_root(out_dir: Path) -> Path:
  root = os.environ.get('EVAL_WORKSPACE_ROOT') or os.path.join(
      tempfile.gettempdir(), 'agent-evals')
  return Path(root) / out_dir.name


def sandbox_wrap(cmd: List[str], hide_subtrees: List[str],
                 hide_listing: List[str]) -> List[str]:
  """Wraps |cmd| so |hide_subtrees| are unreadable and |hide_listing| cannot
  be listed or found. On macOS this uses sandbox-exec; the listing-only
  denial keeps files below reachable by exact path, so a wrapper that knows
  its cache path keeps working. On Linux it uses bubblewrap, mounting an
  empty tmpfs over every hidden path (a wrapper then re-downloads into the
  tmpfs, which is discarded with the sandbox). Elsewhere, or without
  bwrap, the command runs unsandboxed and a warning is printed; rely on
  the `contaminated` indicator."""
  if not (hide_subtrees or hide_listing):
    return cmd
  if sys.platform == 'darwin':
    rules = ''.join(f'(deny file-read* (subpath "{p}"))' for p in hide_subtrees)
    rules += ''.join(
        f'(deny file-read-data (literal "{p}"))' for p in hide_listing)
    return ['sandbox-exec', '-p', f'(version 1)(allow default){rules}'] + cmd
  if sys.platform.startswith('linux') and shutil.which('bwrap'):
    wrap = ['bwrap', '--dev-bind', '/', '/', '--die-with-parent']
    for p in hide_subtrees + hide_listing:
      if os.path.isdir(p):
        wrap += ['--tmpfs', p]
    return wrap + ['--'] + cmd
  print(
      'WARNING: no sandbox available on this platform (install bubblewrap '
      'on Linux); hidden paths are NOT hidden, check the contaminated column',
      file=sys.stderr)
  return cmd


def default_hidden_paths():
  subtrees = [str(REPO)]
  common = subprocess.run(['git', 'rev-parse', '--git-common-dir'],
                          cwd=REPO,
                          capture_output=True,
                          text=True).stdout.strip()
  if common:
    subtrees.append(str(Path(common).resolve().parent))
  cache = Path(os.path.expanduser('~/.local/share/perfetto'))
  listing = [str(cache), str(cache / 'prebuilts')] if cache.exists() else []
  return sorted(set(subtrees)), listing


# ----------------------------------------------------------------------------
# Running one trial
# ----------------------------------------------------------------------------


def _copy_file(src: Path, dst: Path) -> None:
  dst.parent.mkdir(parents=True, exist_ok=True)
  if sys.platform == 'darwin':
    # APFS clone: instant and free for multi-MB traces.
    if subprocess.run(['cp', '-c', str(src), str(dst)],
                      capture_output=True).returncode == 0:
      return
  shutil.copy2(src, dst)


def _stage_workspace(case: Dict[str, Any], ws: Path,
                     vars_: Dict[str, str]) -> List[str]:
  ws.mkdir(parents=True, exist_ok=True)
  staged = []
  for f in case['files']:
    if isinstance(f, str):
      src, dst = f, os.path.basename(f)
    else:
      src, dst = f['src'], f.get('dst', os.path.basename(f['src']))
    src = Path(expand(src, vars_))
    if not src.is_absolute():
      src = Path(case['dir']) / src
    if not src.exists():
      raise FileNotFoundError(f'{case["id"]}: input file missing: {src}')
    _copy_file(src, ws / dst)
    staged.append(dst)
  return staged


def _kill_workspace_processes(ws: Path) -> None:
  # A trace_processor daemon the agent left behind has the workspace path
  # (trace file or socket dir) on its command line.
  subprocess.run(['pkill', '-f', str(ws)], capture_output=True)


def run_trial(harness, case: Dict[str, Any], cond_name: str, cond: Dict[str,
                                                                        Any],
              run_idx: int, out_dir: Path, model: Optional[str],
              vars_: Dict[str, str], timeout: int, budget: Optional[float],
              keep_inputs: bool, hide_subtrees: List[str],
              hide_listing: List[str]) -> Dict[str, Any]:
  run_dir = out_dir / cond_name / case['id'] / f'run-{run_idx}'
  ws = workspace_root(out_dir) / cond_name / case['id'] / f'run-{run_idx}'
  for d in (run_dir, ws):
    if d.exists():
      shutil.rmtree(d)
  run_dir.mkdir(parents=True)
  (run_dir / 'workspace_path.txt').write_text(str(ws))
  staged = _stage_workspace(case, ws, vars_)
  harness.prepare(ws, cond, vars_)
  # Short path on purpose: trace_processor puts warm-session sockets under
  # $TMPDIR/perfetto and AF_UNIX paths are limited to ~104 bytes on macOS.
  tmp = Path(tempfile.mkdtemp(prefix='ae-'))

  env = dict(os.environ)
  path_prefix = [expand(p, vars_) for p in cond.get('path', [])]
  if path_prefix:
    env['PATH'] = os.pathsep.join(path_prefix + [env.get('PATH', '')])
  env['TMPDIR'] = str(tmp)
  # Keep pip installs out of the user's Python: refuse system installs (the
  # agent can still create a venv inside the workspace).
  env['PIP_REQUIRE_VIRTUALENV'] = '1'
  env['PYTHONUSERBASE'] = str(tmp / 'pyuser')
  for k, v in cond.get('env', {}).items():
    env[k] = expand(v, vars_)
  # Don't let a nested agent think it is inside this session.
  for k in list(env):
    if k.startswith('CLAUDE_CODE_') and k != 'CLAUDE_CODE_OAUTH_TOKEN':
      env.pop(k)
  env.pop('CLAUDECODE', None)

  model = cond.get('model', model)
  cmd = sandbox_wrap(
      harness.build_cmd(case['prompt'], cond, model, vars_, budget),
      hide_subtrees, hide_listing)
  (run_dir / 'command.json').write_text(
      json.dumps(
          {
              'cmd': cmd,
              'cwd': str(ws),
              'path': env['PATH'],
              'condition': cond_name,
              'tmpdir': str(tmp),
              'agent': harness.name,
          },
          indent=1))

  t0 = time.time()
  timed_out = False
  # Stream output into the private tmp dir, not into run_dir: run_dir sits
  # inside the (hidden) checkout and Node-based harnesses abort at startup
  # when they cannot stat their own stdout. Moved into place afterwards.
  with open(tmp / 'transcript.jsonl', 'wb') as out, \
       open(tmp / 'stderr.txt', 'wb') as err:
    proc = subprocess.Popen(
        cmd,
        cwd=ws,
        env=env,
        stdout=out,
        stderr=err,
        stdin=subprocess.DEVNULL,
        start_new_session=True)
    try:
      proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
      timed_out = True
      os.killpg(proc.pid, signal.SIGTERM)
      try:
        proc.wait(timeout=15)
      except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
  wall = time.time() - t0
  _kill_workspace_processes(ws)
  for name in ('transcript.jsonl', 'stderr.txt'):
    shutil.move(str(tmp / name), str(run_dir / name))

  if not keep_inputs:
    for s in staged:
      try:
        (ws / s).unlink()
      except FileNotFoundError:
        pass
  shutil.rmtree(tmp, ignore_errors=True)

  meta = {
      'case': case['id'],
      'condition': cond_name,
      'run': run_idx,
      'agent': harness.name,
      'exit_code': proc.returncode,
      'timed_out': timed_out,
      'wall_seconds': round(wall, 1),
      'model': model or harness.default_model,
  }
  (run_dir / 'meta.json').write_text(json.dumps(meta, indent=1))
  return meta


# ----------------------------------------------------------------------------
# Transcript analysis (harness-neutral)
# ----------------------------------------------------------------------------

TP_RE = re.compile(r'trace_processor(_shell)?\b')
SQL_ERR_RE = re.compile(
    r'no such (table|column|function)|syntax error|Traceback|ERROR|error:',
    re.I)
MISSING_RE = re.compile(r'no such (table|column|function)')
# Probing (`ls ~/.local/share/perfetto`) is fine and returns nothing under
# the sandbox; using a binary found there or in a checkout is not.
CONTAMINATION_RE = os.environ.get(
    'EVAL_CONTAMINATION_RE',
    r'/Users/[^ ]*/perfetto/|/home/[^ ]*/perfetto/|worktrees/|/out/(mac|linux)_|'
    r'\.local/share/perfetto/prebuilts/trace_processor')


def compute_indicators(tr: Dict[str, Any],
                       wall_seconds: float) -> Dict[str, Any]:
  bash = [
      tu['input'].get('command', '')
      for tu in tr['tool_uses']
      if tu['name'] == 'Bash'
  ]
  reads = [
      str(tu['input'].get('file_path', ''))
      for tu in tr['tool_uses']
      if tu['name'] == 'Read'
  ]
  skills = [
      tu['input'].get('skill', '')
      for tu in tr['tool_uses']
      if tu['name'] == 'Skill'
  ]
  fetches = [
      tu['input'].get('url', '')
      for tu in tr['tool_uses']
      if tu['name'] in ('WebFetch', 'WebSearch')
  ]
  tp_cmds = [c for c in bash if TP_RE.search(c)]
  results_by_id = {r['id']: r for r in tr['tool_results']}
  # Harnesses without a Skill tool show skill use as reads of its files.
  skill_file_refs = (
      len([r for r in reads if '/skills/' in r or 'SKILL.md' in r]) +
      len([c for c in bash if re.search(r'/skills/[^ /]+/|SKILL\.md', c)]))
  sql_errors = 0
  tp_seq = []  # per trace_processor call: did it fail with a missing name?
  for tu in tr['tool_uses']:
    if tu['name'] != 'Bash' or not TP_RE.search(tu['input'].get('command', '')):
      continue
    r = results_by_id.get(tu['id'])
    text = r['text'] if r else ''
    if SQL_ERR_RE.search(text):
      sql_errors += 1
    tp_seq.append(bool(MISSING_RE.search(text)))
  missing = sum(tp_seq)
  recovered = sum(
      1 for i, bad in enumerate(tp_seq[:-1]) if bad and not tp_seq[i + 1])
  u = tr['usage']
  return {
      'turns':
          u['num_turns'],
      'cost_usd':
          u['cost_usd'],
      'duration_s':
          round((u['duration_ms'] or wall_seconds * 1000) / 1000, 1),
      'input_tokens':
          u['input_tokens'],
      'output_tokens':
          u['output_tokens'],
      'bash_calls':
          len(bash),
      'tp_invocations':
          len(tp_cmds),
      # Robust to agents that download the wrapper under another name
      # (./tp, /tmp/tp): count shell calls that contain SQL.
      'sql_bash_calls':
          len([
              c for c in bash
              if re.search(r'\b(SELECT|INCLUDE PERFETTO)\b', c, re.I)
          ]),
      'used_warm_session':
          any('--remote' in c or 'server unix' in c or '--httpd' in c or
              'server http' in c for c in tp_cmds),
      'used_python_api':
          any(
              re.search(r'from perfetto|import perfetto|TraceProcessor\(', c)
              for c in bash),
      'downloaded_tp':
          any(
              re.search(
                  r'get\.perfetto\.dev|pip3? install .*perfetto|'
                  r'perfetto-luci-artifacts|releases/download', c)
              for c in bash),
      'skill_invoked':
          bool(skills) or skill_file_refs > 0,
      'skills':
          skills,
      'read_skill_files':
          skill_file_refs,
      'read_setup_md':
          any(r.endswith('setup.md') for r in reads)
          or any('setup.md' in c for c in bash),
      'read_querying_md':
          any(r.endswith('querying.md') for r in reads)
          or any('querying.md' in c for c in bash),
      'ran_help':
          any(
              re.search(r'trace_processor\S*\s+(-h|--help|help)\b', c)
              for c in bash),
      'ran_help_agent':
          any(re.search(r'trace_processor\S*\s+help\s+agent', c) for c in bash),
      'web_fetches':
          len(fetches),
      'fetched_perfetto_dev':
          any('perfetto.dev' in u or 'github.com/google/perfetto' in u
              for u in fetches),
      'sql_errors':
          sql_errors,
      'missing_name_errors':
          missing,
      'missing_name_recovered_next':
          recovered,
      'wrote_report_file':
          any(tu['name'] == 'Write' and
              'report' in str(tu['input'].get('file_path', '')).lower()
              for tu in tr['tool_uses'])
          or any(re.search(r'>\s*\S*report\S*\.md', c) for c in bash),
      # Touched the developer's checkout, build dirs or prebuilt cache: the
      # run did not behave like a first-contact user and should be discarded.
      'contaminated':
          any(re.search(CONTAMINATION_RE, c) for c in bash),
      'is_error':
          u['is_error'],
      'final_chars':
          len(tr['final']),
  }


# ----------------------------------------------------------------------------
# Graders
# ----------------------------------------------------------------------------


def _target_text(g: Dict[str, Any], tr: Dict[str, Any], ws: Path) -> str:
  target = g.get('target', 'last_message')
  if target == 'last_message':
    return tr['final']
  if target == 'bash_commands':
    return '\n'.join(tu['input'].get('command', '')
                     for tu in tr['tool_uses']
                     if tu['name'] == 'Bash')
  if target == 'all_text':
    return '\n'.join(tr['texts'] + [tr['final']])
  if isinstance(target, dict) and target.get('source') == 'file':
    p = ws / target['path']
    return p.read_text() if p.exists() else ''
  raise ValueError(f'unknown grader target {target!r}')


def grade_regex(g, tr, ws):
  text = _target_text(g, tr, ws)
  flags = re.I if 'i' in g.get('flags', '') else 0
  if 's' in g.get('flags', ''):
    flags |= re.S
  n = len(re.findall(g['pattern'], text, flags))
  mode = g.get('match', 'contains')
  if mode == 'contains':
    ok = n > 0
  elif mode == 'not_contains':
    ok = n == 0
  elif str(mode).startswith('count:'):
    ok = n == int(mode.split(':')[1])
  else:
    raise ValueError(mode)
  return ok, (f'{n} match(es) for /{g["pattern"]}/ in '
              f'{g.get("target", "last_message")}')


def grade_tool_used(g, tr, ws):
  tool, pat = g['tool'], g.get('input_match')
  n = 0
  for tu in tr['tool_uses']:
    if tu['name'] != tool:
      continue
    if pat and not re.search(pat, json.dumps(tu['input'])):
      continue
    n += 1
  lo, hi = g.get('min', 1), g.get('max')
  ok = n >= lo and (hi is None or n <= hi)
  return ok, f'{tool} called {n} time(s)' + (f' matching /{pat}/'
                                             if pat else '')


def grade_bash(g, tr, ws):
  return grade_tool_used(
      {
          'tool': 'Bash',
          'input_match': g['pattern'],
          'min': g.get('min', 1),
          'max': g.get('max')
      }, tr, ws)


def grade_file_exists(g, tr, ws):
  matches = glob.glob(str(ws / g['path']), recursive=True)
  return bool(matches), f'{len(matches)} file(s) match {g["path"]}'


JUDGE_SCHEMA = json.dumps({
    'type': 'object',
    'properties': {
        'pass': {
            'type': 'boolean'
        },
        'evidence': {
            'type': 'string'
        }
    },
    'required': ['pass', 'evidence'],
})


def grade_llm(g, tr, ws, judge_model='haiku'):
  """LLM judge. Always runs through Claude Code regardless of the harness
  under test, so verdicts are comparable across harnesses."""
  answer = _target_text(g, tr, ws)
  if not answer.strip():
    return False, 'empty answer'
  prompt = (
      'You are grading the final answer an AI coding agent gave to a user. '
      'Decide whether the answer satisfies the criterion. Require concrete '
      'evidence from the answer for a PASS; do not give the benefit of the '
      'doubt.\n\n'
      f'CRITERION:\n{g["criteria"].strip()}\n\n' +
      (f'FOCUS:\n{g["focus"].strip()}\n\n' if g.get('focus') else '') +
      f'ANSWER:\n<<<\n{answer[:20000]}\n>>>\n')
  cmd = [
      'claude', '-p', prompt, '--output-format', 'json', '--json-schema',
      JUDGE_SCHEMA, '--model', judge_model, '--setting-sources', '',
      '--max-turns', '1', '--tools', '', '--disable-slash-commands'
  ]
  env = {
      k: v for k, v in os.environ.items() if not k.startswith('CLAUDE_CODE_')
  }
  env.pop('CLAUDECODE', None)
  r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=env)
  try:
    res = json.loads(r.stdout)
    if isinstance(res, list):  # some builds emit the event list, not the result
      res = next((e for e in res if e.get('type') == 'result'), {})
    so = res.get('structured_output') or json.loads(res.get('result', '{}'))
    return bool(so.get('pass')), 'judge: ' + str(so.get('evidence', ''))[:400]
  except Exception as ex:  # pylint: disable=broad-except
    return False, f'judge failed: {ex}: {r.stdout[:200]} {r.stderr[:200]}'


GRADERS = {
    'regex': grade_regex,
    'tool_used': grade_tool_used,
    'bash': grade_bash,
    'file_exists': grade_file_exists,
    'llm': grade_llm,
}


def grade_run(harness,
              case: Dict[str, Any],
              run_dir: Path,
              judge_model: str,
              reuse: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
  tr = harness.parse(run_dir / 'transcript.jsonl')
  wp = run_dir / 'workspace_path.txt'
  ws = Path(wp.read_text().strip()) if wp.exists() else run_dir / 'workspace'
  meta_path = run_dir / 'meta.json'
  meta = json.loads(meta_path.read_text()) if meta_path.exists() else {}
  (run_dir / 'final_answer.md').write_text(tr['final'])
  indicators = compute_indicators(tr, meta.get('wall_seconds', 0))
  results = []
  for g in case['graders']:
    if reuse and g['name'] in reuse:
      results.append(reuse[g['name']])
      continue
    fn = GRADERS[g['type']]
    try:
      if g['type'] == 'llm':
        ok, evidence = fn(g, tr, ws, judge_model)
      else:
        ok, evidence = fn(g, tr, ws)
    except Exception as ex:  # pylint: disable=broad-except
      ok, evidence = False, f'grader error: {ex}'
    results.append({
        'name': g['name'],
        'type': g['type'],
        'scored': g.get('scored', True),
        'passed': ok,
        'evidence': evidence
    })
  scored = [r for r in results if r['scored']]
  score = (sum(r['passed'] for r in scored) / len(scored)) if scored else None
  grading = {
      'graders': results,
      'score': score,
      'all_passed': all(r['passed'] for r in scored) if scored else None,
      'indicators': indicators
  }
  (run_dir / 'grading.json').write_text(json.dumps(grading, indent=1))
  return grading


# ----------------------------------------------------------------------------
# Aggregation / reports
# ----------------------------------------------------------------------------


def _mean(xs):
  xs = [x for x in xs if isinstance(x, (int, float))]
  return round(statistics.mean(xs), 3) if xs else None


def _rate(xs):
  xs = [x for x in xs if x is not None]
  return round(sum(bool(x) for x in xs) / len(xs), 2) if xs else None


RATE_KEYS = [
    ('skill_invoked_rate', 'skill_invoked'),
    ('read_setup_rate', 'read_setup_md'),
    ('read_querying_rate', 'read_querying_md'),
    ('warm_session_rate', 'used_warm_session'),
    ('downloaded_tp_rate', 'downloaded_tp'),
    ('python_api_rate', 'used_python_api'),
    ('fetched_docs_rate', 'fetched_perfetto_dev'),
    ('ran_help_rate', 'ran_help'),
    ('ran_help_agent_rate', 'ran_help_agent'),
    ('wrote_report_rate', 'wrote_report_file'),
    ('error_rate', 'is_error'),
    ('contaminated_rate', 'contaminated'),
]
MEAN_KEYS = [
    ('cost_mean', 'cost_usd'),
    ('duration_mean_s', 'duration_s'),
    ('turns_mean', 'turns'),
    ('tp_invocations_mean', 'tp_invocations'),
    ('sql_bash_calls_mean', 'sql_bash_calls'),
    ('sql_errors_mean', 'sql_errors'),
    ('missing_name_errors_mean', 'missing_name_errors'),
]


def aggregate(out_dir: Path) -> Dict[str, Any]:
  rows: Dict[str, Dict[str, list]] = {}
  for gpath in sorted(out_dir.glob('*/*/run-*/grading.json')):
    cond, case = gpath.parts[-4], gpath.parts[-3]
    rows.setdefault(cond,
                    {}).setdefault(case,
                                   []).append(json.loads(gpath.read_text()))
  agg: Dict[str, Any] = {'conditions': {}}
  for cond, cases in rows.items():
    agg['conditions'][cond] = {}
    for case, runs in cases.items():
      ind = [r['indicators'] for r in runs]
      per_grader: Dict[str, list] = {}
      for r in runs:
        for gr in r['graders']:
          per_grader.setdefault(gr['name'], []).append(gr['passed'])
      row = {
          'runs': len(runs),
          'score_mean': _mean([r['score'] for r in runs]),
          'all_passed_rate': _rate([r['all_passed'] for r in runs]),
          'graders': {
              k: _rate(v) for k, v in per_grader.items()
          },
      }
      for key, ik in MEAN_KEYS:
        row[key] = _mean([i.get(ik) for i in ind])
      for key, ik in RATE_KEYS:
        row[key] = _rate([i.get(ik) for i in ind])
      total_missing = sum(i.get('missing_name_errors', 0) for i in ind)
      row['missing_name_recovery_rate'] = (
          round(
              sum(i.get('missing_name_recovered_next', 0) for i in ind) /
              total_missing, 2) if total_missing else None)
      agg['conditions'][cond][case] = row
  (out_dir / 'aggregate.json').write_text(json.dumps(agg, indent=1))
  return agg


def _fmt(v, pct=False):
  if v is None:
    return '-'
  if pct:
    return f'{int(round(v * 100))}%'
  return f'{v:.2f}' if isinstance(v, float) else str(v)


METRICS = [
    ('cost_mean', 'cost $', False),
    ('duration_mean_s', 'duration s', False),
    ('turns_mean', 'turns', False),
    ('tp_invocations_mean', 'tp invocations', False),
    ('sql_bash_calls_mean', 'shell calls with SQL', False),
    ('sql_errors_mean', 'sql errors', False),
    ('missing_name_errors_mean', 'missing table/col errors', False),
    ('missing_name_recovery_rate', 'fixed on next call', True),
    ('skill_invoked_rate', 'skill invoked', True),
    ('read_setup_rate', 'read setup.md', True),
    ('read_querying_rate', 'read querying.md', True),
    ('warm_session_rate', 'warm session', True),
    ('downloaded_tp_rate', 'downloaded tp', True),
    ('python_api_rate', 'python api', True),
    ('fetched_docs_rate', 'fetched docs', True),
    ('ran_help_rate', 'ran --help', True),
    ('ran_help_agent_rate', 'ran help agent', True),
    ('wrote_report_rate', 'wrote report file', True),
    ('error_rate', 'run errored', True),
    ('contaminated_rate', 'contaminated', True),
]


def _table(header: List[str], rows: List[List[str]]) -> List[str]:
  return (
      ['| ' + ' | '.join(header) + ' |', '|---|' + '---|' *
       (len(header) - 1)] + ['| ' + ' | '.join(r) + ' |' for r in rows])


def report(out_dir: Path) -> str:
  agg = aggregate(out_dir)
  conds = list(agg['conditions'])
  cases = sorted({c for cond in conds for c in agg['conditions'][cond]})

  def cell(cond, case):
    return agg['conditions'][cond].get(case)

  lines = [
      f'# Eval report: {out_dir.name}', '',
      '## Score (mean fraction of scored graders passed) and all-pass rate', ''
  ]
  lines += _table(
      ['case'] + conds,
      [[case] +
       [('-' if not cell(c, case) else f'{_fmt(cell(c, case)["score_mean"])} / '
         f'{_fmt(cell(c, case)["all_passed_rate"], True)} '
         f'(n={cell(c, case)["runs"]})') for c in conds] for case in cases])
  lines += ['', '## Per-grader pass rate', '']
  for case in cases:
    names: List[str] = []
    for c in conds:
      for g in (cell(c, case) or {}).get('graders', {}):
        if g not in names:
          names.append(g)
    lines += [f'### {case}', ''] + _table(
        ['grader'] + conds, [[g] + [('-' if not cell(c, case) else _fmt(
            cell(c, case)['graders'].get(g), True))
                                    for c in conds]
                             for g in names]) + ['']
  lines += ['## Process indicators (mean over runs)', '']
  for case in cases:
    lines += [f'### {case}', ''] + _table(['metric'] + conds, [
        [label] + [('-' if not cell(c, case) else _fmt(cell(c, case)[key], pct))
                   for c in conds]
        for key, label, pct in METRICS
    ]) + ['']
  text = '\n'.join(lines)
  (out_dir / 'report.md').write_text(text)
  return text


def compare(results_dirs: List[str], conditions: Optional[str],
            out: Optional[str]) -> str:
  """One matrix across several result dirs. Later dirs win for a
  (condition, case) pair, so a clean rerun replaces an earlier one."""
  merged: Dict[str, Dict[str, Any]] = {}
  for d in results_dirs:
    for cond, cases in aggregate(Path(d))['conditions'].items():
      for case, r in cases.items():
        merged.setdefault(cond, {})[case] = dict(r, source=Path(d).name)
  conds = conditions.split(',') if conditions else sorted(merged)
  cases = sorted({c for cond in conds for c in merged.get(cond, {})})
  lines = [
      f'# Comparison: {", ".join(results_dirs)}', '',
      'Cell: mean score / mean $ / mean s / warm-session rate (n runs)', ''
  ]
  rows = []
  for case in cases:
    cells = []
    for cond in conds:
      r = merged.get(cond, {}).get(case)
      cells.append('-' if not r else (
          f'{_fmt(r["score_mean"])} / ${_fmt(r["cost_mean"])} / '
          f'{_fmt(r["duration_mean_s"])}s / '
          f'{_fmt(r["warm_session_rate"], True)} (n={r["runs"]})'))
    rows.append([case] + cells)
  lines += _table(['case'] + conds, rows)
  # Means only over cases every displayed condition has, so columns are
  # comparable; the negative control is excluded as it never uses Perfetto.
  common = [
      c for c in cases if all(c in merged.get(cond, {})
                              for cond in conds) and c != 'negative-python'
  ]
  lines += [
      '', f'## Means over the {len(common)} cases all conditions share', ''
  ]
  rows = []
  for key, label, pct in [('score_mean', 'score', False),
                          ('cost_mean', 'cost $', False),
                          ('duration_mean_s', 'duration s', False),
                          ('sql_bash_calls_mean', 'shell calls with SQL',
                           False), ('sql_errors_mean', 'sql errors', False),
                          ('warm_session_rate', 'warm session', True),
                          ('skill_invoked_rate', 'skill invoked', True),
                          ('wrote_report_rate', 'wrote report file', True)]:
    rows.append([label] + [
        _fmt(
            _mean([
                merged[cond][c][key]
                for c in common
                if merged[cond][c].get(key) is not None
            ]), pct)
        for cond in conds
    ])
  lines += _table(['metric'] + conds, rows)
  text = '\n'.join(lines)
  if out:
    Path(out).write_text(text)
  return text


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------


def cmd_run(args):
  harness = HARNESSES[args.agent]
  conditions = load_conditions(Path(args.conditions_file))
  vars_ = {'repo': str(REPO), 'evals': str(ROOT)}
  vars_.update(conditions.get('vars', {}))
  vars_ = {k: expand(v, vars_) for k, v in vars_.items()}
  model = args.model or (conditions.get('model')
                         if args.agent == 'claude' else None)
  want = args.conditions.split(',') if args.conditions else list(
      conditions['conditions'])
  for c in want:
    if c not in conditions['conditions']:
      sys.exit(
          f'unknown condition {c!r}; known: {list(conditions["conditions"])}')
  cases = load_cases(args.cases)
  if args.tag:
    cases = [c for c in cases if set(args.tag) & set(c['tags'])]
  if args.exclude_tag:
    cases = [c for c in cases if not (set(args.exclude_tag) & set(c['tags']))]
  if not cases:
    sys.exit(f'no cases match {args.cases!r} / tags {args.tag} / '
             f'-tags {args.exclude_tag}')
  out_dir = Path(args.out) if args.out else (
      ROOT / 'results' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
  out_dir = out_dir.resolve()  # trials run with a different cwd
  out_dir.mkdir(parents=True, exist_ok=True)
  if args.hide_path:
    hide_subtrees, hide_listing = sorted(set(args.hide_path)), []
  else:
    hide_subtrees, hide_listing = default_hidden_paths()
  (out_dir / 'run_config.json').write_text(
      json.dumps(
          {
              'agent': args.agent,
              'model': model,
              'conditions': {
                  c: conditions['conditions'][c] for c in want
              },
              'cases': [c['id'] for c in cases],
              'runs': args.runs,
              'vars': vars_,
              'timeout': args.timeout,
              'budget': args.budget,
              'hidden': {
                  'subtrees': hide_subtrees,
                  'listing': hide_listing
              },
          },
          indent=1))

  jobs = []
  for cond_name in want:
    for case in cases:
      for i in range(args.runs or case.get('runs') or 3):
        jobs.append((case, cond_name, i))
  print(
      f'{len(jobs)} trials -> {out_dir}  (agent={args.agent}, jobs={args.jobs}, '
      f'model={model or harness.default_model}, hidden={hide_subtrees})',
      flush=True)
  if args.dry_run:
    for case, cond_name, i in jobs:
      cmd = harness.build_cmd(case['prompt'][:60] + '...',
                              conditions['conditions'][cond_name], model, vars_,
                              args.budget)
      print(' '.join(cmd), f'[{cond_name}/{case["id"]}/run-{i}]')
    return

  def one(job):
    case, cond_name, i = job
    cond = conditions['conditions'][cond_name]
    meta = run_trial(harness, case, cond_name, cond, i, out_dir, model, vars_,
                     case.get('timeout_seconds') or args.timeout, args.budget,
                     args.keep_inputs, hide_subtrees, hide_listing)
    g = grade_run(harness, case, out_dir / cond_name / case['id'] / f'run-{i}',
                  args.judge_model)
    ind = g['indicators']
    print(
        f'[{cond_name}/{case["id"]}/run-{i}] score={_fmt(g["score"])} '
        f'cost=${ind["cost_usd"] or 0:.2f} dur={ind["duration_s"]}s '
        f'turns={ind["turns"]} tp={ind["tp_invocations"]} '
        f'warm={int(ind["used_warm_session"])} '
        f'skill={int(ind["skill_invoked"])}' +
        (' TIMEOUT' if meta['timed_out'] else '') +
        (' CONTAMINATED' if ind.get('contaminated') else ''),
        flush=True)
    return meta

  with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
    list(ex.map(one, jobs))
  print(report(out_dir))


def cmd_grade(args):
  out_dir = Path(args.results_dir)
  cfg_path = out_dir / 'run_config.json'
  cfg = json.loads(cfg_path.read_text()) if cfg_path.exists() else {}
  harness = HARNESSES[cfg.get('agent', 'claude')]
  cases = {c['id']: c for c in load_cases('*')}
  for run_dir in sorted(out_dir.glob('*/*/run-*')):
    case = cases.get(run_dir.parts[-2])
    if not case or not (run_dir / 'transcript.jsonl').exists():
      continue
    previous = {}
    if args.skip_llm and (run_dir / 'grading.json').exists():
      # Keep the earlier LLM verdicts instead of paying for the judge again.
      for gr in json.loads((run_dir / 'grading.json').read_text())['graders']:
        if gr['type'] == 'llm':
          previous[gr['name']] = gr
    g = grade_run(harness, case, run_dir, args.judge_model, reuse=previous)
    print(f'{run_dir.relative_to(out_dir)}: score={_fmt(g["score"])}')
  print(report(out_dir))


def main():
  ap = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  sub = ap.add_subparsers(dest='cmd', required=True)
  r = sub.add_parser('run')
  r.add_argument('--agent', choices=sorted(HARNESSES), default='claude')
  r.add_argument(
      '--conditions', help='comma-separated condition names (default: all)')
  r.add_argument('--conditions-file', default=str(ROOT / 'conditions.json'))
  r.add_argument('--cases', default='*', help='glob over case ids')
  r.add_argument(
      '--tag', action='append', help='only cases with one of these tags')
  r.add_argument(
      '--exclude-tag',
      action='append',
      help='skip cases with any of these tags')
  r.add_argument(
      '--runs', type=int, help='trials per case (default: case.runs or 3)')
  r.add_argument('--jobs', type=int, default=3)
  r.add_argument(
      '--model',
      help="model for the agent (default: conditions.json's for claude, "
      'the harness default otherwise)')
  r.add_argument(
      '--judge-model', default='haiku', help='Claude model for llm graders')
  r.add_argument('--timeout', type=int, default=1500, help='seconds per trial')
  r.add_argument('--budget', type=float, help='per-trial USD cap (claude only)')
  r.add_argument('--out')
  r.add_argument(
      '--keep-inputs', action='store_true', help='keep staged trace files')
  r.add_argument(
      '--hide-path',
      action='append',
      help='path hidden from the agent (default: this repo, its main '
      "checkout, and the wrapper's prebuilt cache)")
  r.add_argument('--dry-run', action='store_true')
  r.set_defaults(fn=cmd_run)
  g = sub.add_parser('grade')
  g.add_argument('results_dir')
  g.add_argument('--judge-model', default='haiku')
  g.add_argument(
      '--skip-llm',
      action='store_true',
      help='reuse existing LLM verdicts; recompute everything else')
  g.set_defaults(fn=cmd_grade)
  p = sub.add_parser('report')
  p.add_argument('results_dir')
  p.set_defaults(fn=lambda a: print(report(Path(a.results_dir))))
  c = sub.add_parser('compare')
  c.add_argument('results_dirs', nargs='+')
  c.add_argument('--conditions', help='comma-separated, in display order')
  c.add_argument('--out', help='write the markdown here too')
  c.set_defaults(
      fn=lambda a: print(compare(a.results_dirs, a.conditions, a.out)))
  args = ap.parse_args()
  args.fn(args)


if __name__ == '__main__':
  main()
