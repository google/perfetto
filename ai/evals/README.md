# Perfetto AI Skill Evals

End-to-end evals for the consolidated `perfetto` skill in `ai/skills/`.
The goal is to measure whether a change to the skill actually improves
agent behavior, instead of editing markdown and hoping.

## How it works

`run_evals.py` runs each case as a real headless agent session:

1. The skill is assembled with `tools/release/build_ai_agents.py` — the
   eval always tests the artifact users install, not the source tree.
2. Each trial gets a fresh empty sandbox directory. The agent (Claude
   Code, `claude -p --bare`) runs there with the assembled plugin loaded
   via `--plugin-dir`. `--bare` keeps the run reproducible: no user
   hooks, settings, or other skills leak in.
3. The agent's final output and any files it wrote are graded.

Grading is deterministic-first:

| Check | What it does |
| --- | --- |
| `file_exists` | A file matching the glob was written to the sandbox. |
| `trace_config_parses` | The file parses as a text-format `TraceConfig` (via `protoc --encode` against `protos/perfetto/config/perfetto_config.proto`). |
| `config_contains` | Regexes over the protoc-normalized decode of the config (`patterns` must appear, `absent_patterns` must not). |
| `output_matches` / `output_not_matches` | Regex over the agent's final message. |
| `rubric` | LLM judge (a separate `claude -p` call) grades natural-language expectations; use only where no artifact can be checked mechanically. |

A trial passes only if every check passes. With `--trials N` the report
shows per-case pass rates (agent runs are high-variance; N=3 is a good
default for anything you intend to act on).

## Running

Requires an authenticated `claude` CLI and a built `protoc`
(`tools/ninja -C out/<config> protoc`, auto-detected from `out/`).

```sh
# Everything, once
ai/evals/run_evals.py

# Recording suite, 3 trials each, save results
ai/evals/run_evals.py --filter recording --trials 3 --results /tmp/skill.json

# No-skill control run: the uplift over this is what the skill is worth.
# A case that passes both with and without the skill isn't testing the
# skill at all.
ai/evals/run_evals.py --baseline --results /tmp/baseline.json

# Test a modified skill tree without committing
ai/evals/run_evals.py --skills-src /path/to/ai/skills
```

Failed trials keep their sandbox on disk (path is printed) so you can
read `agent_output.md` and the files the agent wrote.

## Writing cases

Cases live in `cases/*.json`, one file per suite. Guidelines, following
Anthropic's skill-eval guidance:

- **Write the case before the skill change.** Run it, watch it fail,
  then write the minimal skill content that makes it pass.
- **Two experts must agree on pass/fail.** If a check needs judgement,
  it's a `rubric` expectation; everything else should be programmatic.
- **Prompts are realistic user asks**, self-contained, and must tell the
  agent no device is attached (recording cases) so it produces the
  artifact instead of trying to run `adb`.
- **Include should-NOT behavior** (`output_not_matches`,
  `absent_patterns`) so the eval can't be gamed by doing everything.
- Keep suites small (5–20 cases). Each trial is a full agent session and
  costs real money; the summary prints the total spend.
