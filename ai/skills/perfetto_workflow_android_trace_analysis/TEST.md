# TEST for perfetto_workflow_android_trace_analysis

## Tier 1 — Build & Smoke Tests

### Test 1: File Existence
Verify that the skill files and all domain hints exist.
```bash
ls ai/skills/perfetto_workflow_android_trace_analysis/SKILL.md
ls ai/skills/perfetto_workflow_android_trace_analysis/references/hints_cpu.md
ls ai/skills/perfetto_workflow_android_trace_analysis/references/hints_graphics.md
ls ai/skills/perfetto_workflow_android_trace_analysis/references/hints_io.md
ls ai/skills/perfetto_workflow_android_trace_analysis/references/hints_ipc.md
ls ai/skills/perfetto_workflow_android_trace_analysis/references/hints_memory.md
ls ai/skills/perfetto_workflow_android_trace_analysis/references/hints_power.md
```
**Verify:** All files exist.

## Tier 2 — Smoke Tests

### Test 2: Verify standard queries parse
Verify that standard analysis queries parse successfully (requires
trace_processor).
```bash
trace_processor query dummy.pftrace "SELECT ts, dur, state, blocked_function FROM thread_state LIMIT 1"
```
**Verify:** Command runs and checks query validity.

## Tier 3 — Functional Tests

### Test 3: Ask about trace analysis setup
**Prompt:** "I want to investigate a slow app startup using a trace. What's the
setup?"
**Verify:**
- Agent recommends initializing a local scratchpad file
  `[trace_name]_analysis.md` first.
- Agent emphasizes keeping a strict, fact-based Chain of Evidence (no premature
  hypotheses).
- Agent targets the active package name using `android.startup.startups` if not
  provided.

### Test 4: Ask about CPU contention analysis using hints
**Prompt:** "My main thread seems to be stalled during startup. How do I verify
if it's CPU contention?"
**Verify:**
- Agent attributes its analysis strategy to `hints_cpu.md`.
- Agent warns against assuming a long-running slice is actively computing (Wall
  time vs CPU time).
- Agent recommends measuring scheduling latency (Runnable duration) and
  comparing 'Runnable' vs 'Running' times in `thread_state`.

### Test 5: Ask about blocked thread and blocker tracking
**Prompt:** "A thread is stuck in Uninterruptible Sleep ('D' state). How do I
find what it's waiting for?"
**Verify:**
- Agent recommends querying the `blocked_function` in `thread_state`.
- Agent explains it must follow the dependency to the blocker (e.g., kworker
  for I/O, or another thread holding a lock) by finding who woke it up.
- Agent explains it cannot conclude the investigation early without identifying
  the blocker.
