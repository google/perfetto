# TEST for perfetto_workflow_android_heap_dump

## Tier 1 — Build & Smoke Tests

### Test 1: File Existence
Verify that the skill files exist.
```bash
ls ai/skills/perfetto_workflow_android_heap_dump/SKILL.md
```
**Verify:** File exists.

## Tier 2 — Smoke Tests

### Test 2: Verify query parses
Verify that the orientation query parses (requires trace_processor).
```bash
trace_processor query dummy.pftrace "INCLUDE PERFETTO MODULE android.memory.heap_graph.heap_graph_stats; SELECT 1"
```
**Verify:** Command runs and checks query validity.

## Tier 3 — Functional Tests

### Test 3: Ask for quickstart triage
**Prompt:** "I need to analyze an Android OOM heap dump."
**Verify:**
- Agent executes `scripts/triage_dominator_path.sql` and strictly follows the verbatim prompt structure.
- Agent explicitly gates open-ended exploration, only proceeding if the user states triage is insufficient.

### Test 4: Ask about heap dump analysis
**Prompt:** "I have a trace with a heap dump. How do I start analyzing it?"
**Verify:**
- Agent recommends checking `android_heap_graph_stats` first to orient.
- Agent explains how to find top retainers using `android_heap_graph_class_summary_tree`.
- Agent mentions sorting by `cumulative_size`.

### Test 5: Ask about finding leak cause and remediation
**Prompt:** "I know `com.example.MyActivity` is leaking. How do I find what retains it and fix it?"
**Verify:**
- Agent recommends using the dominator tree (`heap_graph_dominator_tree`) and walking up `idom_id`.
- Agent explicitly instructs to search the codebase for matching application source code.
- Agent provides expert philosophical architectural advice and creates a concrete implementation plan referencing specific filenames and line numbers.

