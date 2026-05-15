# TEST for perfetto-workflow-android-heap-dump

## Tier 1 — Build & Smoke Tests

### Test 1: File Existence
Verify that the skill files exist.
```bash
ls ai/skills/perfetto-workflow-android-heap-dump/SKILL.md
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

### Test 3: Ask about heap dump analysis
**Prompt:** "I have a trace with a heap dump. How do I start analyzing it?"
**Verify:**
- Agent recommends checking `android_heap_graph_stats` first to orient.
- Agent explains how to find top retainers using `android_heap_graph_class_summary_tree`.
- Agent mentions sorting by `cumulative_size`.

### Test 4: Ask about finding leak cause
**Prompt:** "I know `com.example.MyActivity` is leaking. How do I find what retains it?"
**Verify:**
- Agent recommends using the dominator tree (`heap_graph_dominator_tree`).
- Agent explains how to query it for the specific class and walk up `idom_id`.
