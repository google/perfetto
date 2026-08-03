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
"""Runs end-to-end evals against the assembled Perfetto AI skill.

Each eval case gives an agent (Claude Code in headless mode by default) a
realistic user prompt in a fresh sandbox workspace that has the assembled
skill installed as a project skill. The agent's transcript and any files it
writes are then graded by deterministic checks — does a synthesized
TraceConfig actually parse against trace_config.proto? does it contain the
required data sources? — with an optional LLM rubric judge for prose-only
answers.

The graders are intentionally programmatic-first: parsing a config with
protoc and asserting on the decoded fields is cheap, objective, and does not
drift, so rubric judging is reserved for cases where no artifact can be
checked mechanically.

Usage:
    ai/evals/run_evals.py                          # run all suites once
    ai/evals/run_evals.py --filter recording       # one suite / case regex
    ai/evals/run_evals.py --trials 3 --jobs 4      # reliability (pass^k)
    ai/evals/run_evals.py --baseline               # no-skill control run

Requires: the `claude` CLI on PATH (authenticated), and a built `protoc`
(auto-detected from out/*/protoc, override with --protoc).
"""

import argparse
import concurrent.futures
import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CASES_DIR = Path(__file__).resolve().parent / 'cases'
BUNDLER = REPO_ROOT / 'tools' / 'release' / 'build_ai_agents.py'
MERGED_CONFIG_PROTO = 'protos/perfetto/config/perfetto_config.proto'

JUDGE_PROMPT = """You are grading the output of an AI agent that was asked to
help with a Perfetto tracing task. Judge ONLY against the expectations below;
do not invent extra requirements. Respond with a single JSON object and
nothing else, of the form:
{{"expectations": [{{"met": true, "reason": "..."}}, ...]}}
with one entry per expectation, in order.

<user_prompt>
{prompt}
</user_prompt>

<agent_final_output>
{output}
</agent_final_output>

<files_written_by_agent>
{files}
</files_written_by_agent>

<expectations>
{expectations}
</expectations>
"""


@dataclasses.dataclass
class CheckResult:
  check: dict
  passed: bool
  detail: str


@dataclasses.dataclass
class TrialResult:
  case_name: str
  trial: int
  checks: list
  agent_error: str = ''
  cost_usd: float = 0.0
  num_turns: int = 0
  sandbox: str = ''

  @property
  def passed(self):
    return not self.agent_error and all(c.passed for c in self.checks)


def find_protoc(explicit):
  if explicit:
    return Path(explicit)
  out_dir = REPO_ROOT / 'out'
  if out_dir.is_dir():
    candidates = sorted(
        out_dir.glob('*/protoc'),
        key=lambda p: p.stat().st_mtime,
        reverse=True)
    if candidates:
      return candidates[0]
  found = shutil.which('protoc')
  if found:
    return Path(found)
  sys.exit('error: no protoc found. Build one (tools/ninja -C out/... protoc) '
           'or pass --protoc.')


def build_skill_bundle(tmp_root, skills_src):
  """Assembles the shippable bundle and returns the plugin directory."""
  out = tmp_root / 'bundle'
  cmd = [sys.executable, str(BUNDLER), '--output', str(out)]
  if skills_src:
    cmd += ['--skills-src', str(skills_src)]
  subprocess.run(cmd, check=True, capture_output=True, text=True)
  plugin = out / 'plugins' / 'perfetto'
  if not (plugin / 'skills' / 'perfetto' / 'SKILL.md').is_file():
    sys.exit(f'error: bundler did not produce a skill under {plugin}')
  return plugin


def load_cases(cases_arg):
  paths = []
  p = Path(cases_arg)
  if p.is_dir():
    paths = sorted(p.glob('*.json'))
  else:
    paths = [p]
  cases = []
  for path in paths:
    data = json.loads(path.read_text())
    for case in data['cases']:
      case['suite'] = data.get('suite', path.stem)
      cases.append(case)
  return cases


def parse_cli_result(stdout):
  """Extracts the result event from claude --output-format json output.

  Depending on CLI version the output is either a single result object or an
  array of events ending with a {"type": "result", ...} entry.
  """
  data = json.loads(stdout)
  if isinstance(data, list):
    results = [d for d in data if d.get('type') == 'result']
    if not results:
      raise json.JSONDecodeError('no result event', stdout, 0)
    return results[-1]
  return data


def run_agent(claude_bin, prompt, sandbox, plugin_dir, model, max_turns,
              timeout):
  """Runs claude -p in the sandbox. Returns (final_text, err, cost, turns).

  --bare gives a reproducible run (no user hooks/skills/CLAUDE.md leak in);
  the skill under test is loaded explicitly via --plugin-dir, exactly as the
  shipped plugin. plugin_dir=None runs the no-skill baseline.
  """
  cmd = [
      claude_bin, '-p', prompt, '--output-format', 'json', '--bare',
      '--dangerously-skip-permissions', '--max-turns', str(max_turns)
  ]
  if plugin_dir:
    cmd += ['--plugin-dir', str(plugin_dir)]
  if model:
    cmd += ['--model', model]
  env = dict(os.environ)
  # Never inherit the invoking session's project/user settings.
  env.pop('CLAUDE_PROJECT_DIR', None)
  try:
    proc = subprocess.run(
        cmd, cwd=sandbox, capture_output=True, text=True, timeout=timeout,
        env=env)
  except subprocess.TimeoutExpired:
    return '', f'agent timed out after {timeout}s', 0.0, 0
  if proc.returncode != 0:
    return '', f'claude exited {proc.returncode}: {proc.stderr[-500:]}', 0.0, 0
  try:
    result = parse_cli_result(proc.stdout)
  except json.JSONDecodeError:
    return proc.stdout, 'unparseable claude --output-format json', 0.0, 0
  if result.get('is_error'):
    return '', f'agent error: {result.get("result", "")[:500]}', 0.0, 0
  return (result.get('result', ''), '',
          result.get('total_cost_usd', 0.0), result.get('num_turns', 0))


def decode_config(protoc, path):
  """Parses a text-format TraceConfig; returns (canonical_text, error)."""
  encode = subprocess.run(
      [str(protoc), f'--encode=perfetto.protos.TraceConfig', '-I', 'protos',
       MERGED_CONFIG_PROTO],
      cwd=REPO_ROOT, stdin=open(path, 'rb'), capture_output=True)
  if encode.returncode != 0:
    return None, encode.stderr.decode(errors='replace').strip()
  decode = subprocess.run(
      [str(protoc), f'--decode=perfetto.protos.TraceConfig', '-I', 'protos',
       MERGED_CONFIG_PROTO],
      cwd=REPO_ROOT, input=encode.stdout, capture_output=True)
  if decode.returncode != 0:
    return None, decode.stderr.decode(errors='replace').strip()
  return decode.stdout.decode(errors='replace'), None


def sandbox_files(sandbox):
  files = []
  for root, dirs, names in os.walk(sandbox):
    dirs[:] = [d for d in dirs if d != '.claude']
    for name in names:
      files.append(str(Path(root, name).relative_to(sandbox)))
  return sorted(files)


def resolve_artifact(sandbox, path_glob):
  """Finds the file a check refers to; supports glob patterns."""
  matches = sorted(Path(sandbox).glob(path_glob))
  matches = [m for m in matches if '.claude' not in m.parts]
  return matches[0] if matches else None


def run_judge(claude_bin, case, output, sandbox, expectations, judge_model,
              timeout):
  file_dump = []
  for rel in sandbox_files(sandbox)[:10]:
    content = (Path(sandbox) / rel).read_text(errors='replace')[:4000]
    file_dump.append(f'--- {rel} ---\n{content}')
  prompt = JUDGE_PROMPT.format(
      prompt=case['prompt'],
      output=output[:20000],
      files='\n'.join(file_dump) or '(none)',
      expectations='\n'.join(f'{i+1}. {e}' for i, e in enumerate(expectations)))
  cmd = [
      claude_bin, '-p', prompt, '--output-format', 'json', '--bare',
      '--max-turns', '1', '--model', judge_model
  ]
  try:
    proc = subprocess.run(
        cmd, capture_output=True, text=True, timeout=timeout,
        cwd=tempfile.gettempdir())
  except subprocess.TimeoutExpired:
    return None, 'judge timed out'
  if proc.returncode != 0:
    return None, f'judge exited {proc.returncode}: {proc.stderr[-300:]}'
  try:
    text = parse_cli_result(proc.stdout).get('result', '')
    match = re.search(r'\{.*\}', text, re.DOTALL)
    verdict = json.loads(match.group(0))
    return verdict['expectations'], None
  except (json.JSONDecodeError, AttributeError, KeyError) as e:
    return None, f'unparseable judge verdict: {e}'


def run_checks(case, output, sandbox, protoc, claude_bin, judge_model,
               timeout):
  results = []
  for check in case['checks']:
    kind = check['type']
    if kind == 'file_exists':
      artifact = resolve_artifact(sandbox, check['path'])
      results.append(
          CheckResult(check, artifact is not None,
                      str(artifact) if artifact else
                      f'no file matching {check["path"]}; '
                      f'files: {sandbox_files(sandbox)}'))
    elif kind == 'trace_config_parses':
      artifact = resolve_artifact(sandbox, check['path'])
      if not artifact:
        results.append(CheckResult(check, False, 'file missing'))
        continue
      _, err = decode_config(protoc, artifact)
      results.append(CheckResult(check, err is None, err or 'parses'))
    elif kind == 'config_contains':
      artifact = resolve_artifact(sandbox, check['path'])
      if not artifact:
        results.append(CheckResult(check, False, 'file missing'))
        continue
      decoded, err = decode_config(protoc, artifact)
      if err:
        results.append(CheckResult(check, False, f'unparseable: {err}'))
        continue
      missing = [p for p in check.get('patterns', [])
                 if not re.search(p, decoded)]
      unwanted = [p for p in check.get('absent_patterns', [])
                  if re.search(p, decoded)]
      detail = []
      if missing:
        detail.append(f'missing patterns: {missing}')
      if unwanted:
        detail.append(f'unwanted patterns present: {unwanted}')
      results.append(
          CheckResult(check, not missing and not unwanted,
                      '; '.join(detail) or 'ok'))
    elif kind == 'output_matches':
      ok = re.search(check['pattern'], output, re.IGNORECASE) is not None
      results.append(CheckResult(check, ok, check['pattern']))
    elif kind == 'output_not_matches':
      ok = re.search(check['pattern'], output, re.IGNORECASE) is None
      results.append(CheckResult(check, ok, check['pattern']))
    elif kind == 'rubric':
      verdict, err = run_judge(claude_bin, case, output, sandbox,
                               check['expectations'], judge_model, timeout)
      if verdict is None:
        results.append(CheckResult(check, False, err))
        continue
      failed = [
          f'{e}: {v.get("reason", "")}'
          for e, v in zip(check['expectations'], verdict)
          if not v.get('met')
      ]
      results.append(
          CheckResult(check, not failed,
                      '; '.join(failed) if failed else 'all expectations met'))
    else:
      results.append(CheckResult(check, False, f'unknown check type {kind}'))
  return results


def run_trial(case, trial, plugin_dir, work_root, args, protoc):
  sandbox = Path(
      tempfile.mkdtemp(prefix=f'{case["name"]}-t{trial}-', dir=work_root))
  output, err, cost, turns = run_agent(
      args.claude_bin, case['prompt'], sandbox, plugin_dir, args.model,
      case.get('max_turns', args.max_turns), args.timeout)
  (sandbox / 'agent_output.md').write_text(output)
  checks = [] if err else run_checks(case, output, sandbox, protoc,
                                     args.claude_bin, args.judge_model,
                                     args.timeout)
  return TrialResult(case['name'], trial, checks, err, cost, turns,
                     str(sandbox))


def main():
  ap = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('--cases', default=str(CASES_DIR),
                  help='Case file or directory of *.json case files.')
  ap.add_argument('--filter', default='',
                  help='Regex matched against suite.case_name.')
  ap.add_argument('--trials', type=int, default=1)
  ap.add_argument('--jobs', type=int, default=4)
  ap.add_argument('--model', default='',
                  help='Model for the agent under test (claude CLI --model). '
                  'Empty uses the CLI default.')
  ap.add_argument('--judge-model', default='opus')
  ap.add_argument('--claude-bin', default='claude',
                  help='Path to the claude CLI (or a compatible stub, '
                  'useful for testing the harness itself).')
  ap.add_argument('--max-turns', type=int, default=40)
  ap.add_argument('--timeout', type=int, default=900,
                  help='Per-trial agent timeout in seconds.')
  ap.add_argument('--protoc', default='')
  ap.add_argument('--plugin-dir', default='',
                  help='Use an already-assembled plugin dir instead of '
                  'bundling ai/skills via tools/release/build_ai_agents.py.')
  ap.add_argument('--skills-src', default='',
                  help='ai/skills tree to bundle (default: this checkout).')
  ap.add_argument('--baseline', action='store_true',
                  help='Run without the skill installed (no-skill baseline, '
                  'for measuring the uplift the skill provides).')
  ap.add_argument('--keep-sandboxes', action='store_true')
  ap.add_argument('--results', default='',
                  help='Write full results JSON to this path.')
  args = ap.parse_args()

  protoc = find_protoc(args.protoc)
  cases = load_cases(args.cases)
  if args.filter:
    cases = [c for c in cases
             if re.search(args.filter, f'{c["suite"]}.{c["name"]}')]
  if not cases:
    sys.exit('error: no cases matched')

  work_root = Path(tempfile.mkdtemp(prefix='perfetto-skill-eval-'))
  if args.baseline:
    plugin_dir = None
  elif args.plugin_dir:
    plugin_dir = Path(args.plugin_dir)
  else:
    plugin_dir = build_skill_bundle(work_root, args.skills_src or None)

  print(f'Running {len(cases)} case(s) x {args.trials} trial(s) '
        f'[{"BASELINE (no skill)" if args.baseline else plugin_dir}]')

  jobs = []
  with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for case in cases:
      for trial in range(args.trials):
        jobs.append(pool.submit(run_trial, case, trial, plugin_dir, work_root,
                                args, protoc))
    results = [j.result() for j in jobs]

  by_case = {}
  for r in results:
    by_case.setdefault(r.case_name, []).append(r)

  total_cost = sum(r.cost_usd for r in results)
  any_fail = False
  print()
  for case in cases:
    trials = by_case[case['name']]
    passed = sum(1 for t in trials if t.passed)
    status = 'PASS' if passed == len(trials) else 'FAIL'
    if passed != len(trials):
      any_fail = True
    print(f'[{status}] {case["suite"]}.{case["name"]}: '
          f'{passed}/{len(trials)} trials')
    for t in trials:
      if t.agent_error:
        print(f'    trial {t.trial}: AGENT ERROR: {t.agent_error}')
      for c in t.checks:
        if not c.passed:
          print(f'    trial {t.trial}: check {c.check["type"]} failed: '
                f'{c.detail}')
      if not t.passed:
        print(f'    trial {t.trial}: sandbox: {t.sandbox}')
  print(f'\nTotal agent cost: ${total_cost:.2f}')

  if args.results:
    payload = [{
        'case': r.case_name,
        'trial': r.trial,
        'passed': r.passed,
        'agent_error': r.agent_error,
        'cost_usd': r.cost_usd,
        'num_turns': r.num_turns,
        'sandbox': r.sandbox,
        'checks': [{
            'type': c.check['type'],
            'passed': c.passed,
            'detail': c.detail
        } for c in r.checks],
    } for r in results]
    Path(args.results).write_text(json.dumps(payload, indent=2) + '\n')
    print(f'Results written to {args.results}')

  if not args.keep_sandboxes and not any_fail:
    shutil.rmtree(work_root, ignore_errors=True)
  elif any_fail:
    print(f'Sandboxes kept for inspection under {work_root}')

  return 1 if any_fail else 0


if __name__ == '__main__':
  sys.exit(main())
