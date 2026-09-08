# Perfetto agent evals

Answers one question with data: when a coding agent is asked to debug a
performance problem with a trace, does a given change (to the skill, to
what `trace_processor` prints, to the docs) make it do a better job, and
at what cost? Reading a skill tells you whether it looks right; only
running an agent against it tells you whether it helps. Round-one
findings are in [REPORT.md](REPORT.md).

## How a trial works

A trial is one agent, one prompt, one *condition*:

1. **A fresh workspace** is created under the system temp dir and the
   case's input files (a trace from `test/data`, a script) are copied in.
   Nothing else: no user settings, plugins or memory, so the only Perfetto
   knowledge available is the model's own plus what the condition adds.
2. **The condition** decides what the agent finds: a skill installed or
   not, a particular `trace_processor` build on `PATH` or nothing at all,
   extra environment. Conditions are named in `conditions.json`; the
   things they point at (bundled skills, binaries) live outside the tree
   and are assembled by `setup_assets.py`.
3. **The agent runs non-interactively** through its own CLI, with the
   checkout and the wrapper's prebuilt cache hidden from it (see
   Isolation). Its full event stream is kept.
4. **The transcript is normalised** to a harness-neutral shape (tool
   calls, tool results, answer text, usage), then **graded**: regex and
   tool-call graders first, an LLM judge only for things a regex cannot
   check ("grounded, not fabricated"). A set of **process indicators**
   is computed too: did it invoke the skill, did it keep the trace loaded
   across queries, how many `trace_processor` calls, how many SQL errors,
   cost, duration, and whether it cheated by finding a local build.
5. Each case runs several times per condition, because single runs of a
   non-deterministic agent are noise. Reports aggregate score, cost and
   indicators per case and condition; `compare` puts several result
   directories side by side.

## Why it is built this way

- **With versus without, same prompts.** The only fair measure of a
  skill or a hint is the delta against the agent's default behaviour on
  the same task. The baseline is the control, and it is the reason
  isolation gets so much attention.
- **Outcomes first, process second.** Graders check facts in the answer
  (the thread name, the startup duration, the ANR subject) so an agent
  that takes an unexpected route to the right answer still passes. The
  process indicators are reported next to the score, not folded into
  it, because "used a warm session" is a cost story, not a correctness
  one.
- **Deterministic graders before an LLM judge.** A regex on a known
  number is cheap, reproducible and cannot be talked into a pass. The
  judge is reserved for honesty criteria, always runs on the same model
  regardless of the harness under test, and is told to require evidence.
- **Ground truth from `trace_processor` itself.** Every grader's body
  records the query that produced the expected value, and where two
  readings of a trace are defensible both are accepted. The first round
  showed why: agents reasonably excluded a container track the reference
  script counted, and reported a different but correct top retainer.
- **Cases that can fail.** Capable models solve easy questions with or
  without help, so the set includes a prompt that never says "Perfetto"
  (does the skill trigger?), a trap trace with nothing to find (does the
  agent invent a result?), a negative control (does the skill fire when
  it should not?), and multi-step questions where a wrong method gives
  a plausible wrong number.
- **Cost and time are first-class.** A skill that raises the score but
  triples the bill is a different decision from one that does both. Every
  trial records dollars (where the harness reports them), tokens and
  wall time.
- **Harness-agnostic.** Users run different agents and the same skill can
  behave differently in each, so a harness is one class with three
  methods: launch, install the skill, parse the transcript. Cases are
  plain markdown with YAML frontmatter, tied to no harness.

## Isolation

The control condition is only meaningful if the agent cannot find help
lying around, and in the first round it did: agents walked up from the
workspace into the checkout and used its build, then looked in the
wrapper's prebuilt cache by name. So the runner:

- keeps workspaces outside the repo;
- hides the repo, its main checkout and `~/.local/share/perfetto` from the
  agent: `sandbox-exec` on macOS (the cache is unlistable but files stay
  reachable by exact path, so wrappers keep working), bubblewrap on Linux
  (an empty tmpfs over each hidden path, so downloads vanish with the
  sandbox), a loud warning elsewhere;
- gives each trial a short private `TMPDIR` (warm-session sockets live
  there) and refuses `pip install` outside a virtualenv;
- flags a run as `contaminated` if it still used a hidden binary, so the
  run can be discarded.

## Layout

```
ai/evals/
  run_evals.py          runner, harnesses, graders, aggregation, comparison
  setup_assets.py       builds the out-of-tree assets conditions refer to
  conditions.json       named conditions (plugin dirs, PATH entries, env)
  cases/<id>/prompt.md  frontmatter (files to stage, runs, tags) + prompt
  cases/<id>/graders/   one grader per file: regex | bash | tool_used |
                        file_exists | llm  (scored: false = indicator only)
  results/<name>/       transcripts, answers, grading, reports (gitignored)
```

## Running

Setup once: build `trace_processor_shell`, make sure the test traces are
present (`tools/install-test-deps`), then assemble the assets outside the
checkout and point the runner at them:

```sh
git fetch origin ai-agents
ai/evals/setup_assets.py --out ~/perfetto-eval-assets \
    --tp-binary out/mac_release/trace_processor_shell
export EVAL_ASSETS=~/perfetto-eval-assets
```

Re-run `setup_assets.py` after editing `ai/skills/` or rebuilding. The
runner's one dependency (PyYAML) is declared as inline script metadata,
so `uv run ai/evals/run_evals.py` (or the file's shebang) needs no venv.

```sh
# With vs without the local skill, three runs per case, five in parallel.
ai/evals/run_evals.py run --conditions baseline-tp,skill-local --runs 3 --jobs 5

# Same thing driven by Codex; only the harder cases, on a cheaper model.
ai/evals/run_evals.py run --agent codex --tag hard --conditions baseline-tp,skill-local
ai/evals/run_evals.py run --model sonnet --budget 2 --conditions baseline-tp,skill-local

# Re-grade after editing graders (keeps existing LLM verdicts).
ai/evals/run_evals.py grade ai/evals/results/<name> --skip-llm

# One table across result directories; later dirs win for a cell.
ai/evals/run_evals.py compare ai/evals/results/a ai/evals/results/b \
    --conditions baseline-tp,skill-local
```

Harnesses: `claude` (default, reports cost) and `codex` are tested;
`gemini` is written against its documented interface but untested.
Budget: with Opus a trial costs $0.1 to $3 depending on the case; a full
11-case, 3-run, single-condition matrix is about $30 and 40 minutes at
five parallel jobs. Sonnet is about a fifth of that.

## Adding a case

- Pick a question a real user would ask, with a trace in `test/data`
  that answers it. Compute the answer with `trace_processor_shell` and
  put the query in the grader body.
- One fact per regex grader; an `llm` grader for "grounded, not
  fabricated"; process checks (`bash`, `tool_used`) marked
  `scored: false` unless the process is the point.
- Run it three times in the baseline before trusting it. A case every
  condition passes tells you nothing; a case the baseline cannot pass
  is where a change can show.
- Frontmatter is YAML: double-quote regex patterns and escape
  backslashes (`"\\d+"`), or single-quote them literally (`'\d+'`).
