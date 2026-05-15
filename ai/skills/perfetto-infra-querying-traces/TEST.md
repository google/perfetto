# TEST for perfetto-infra-querying-traces

## Tier 1 — Build & Smoke Tests

### Test 1: File Existence
Verify that the skill files exist.
```bash
ls ai/skills/perfetto-infra-querying-traces/SKILL.md
```
**Verify:** File exists.

## Tier 2 — Smoke Tests

### Test 2: Verify quickstart query
Run a simple query using `trace_processor`.
```bash
trace_processor query dummy.pftrace "SELECT 1"
```
**Verify:** Command runs (might fail if dummy.pftrace not found, but checks binary existence).

## Tier 3 — Functional Tests

### Test 3: Ask how to query trace
**Prompt:** "I have a trace file called `my_trace.pftrace`. How do I find all slices longer than 1 second?"
**Verify:**
- Agent recommends using `trace_processor`.
- Agent suggests a query filtering on `dur > 1e9` (or `dur > 1000000000`).
- Agent suggests using `slices.with_context` or `thread_or_process_slice` for better context.

### Test 4: Ask about long-running mode
**Prompt:** "I need to run many queries on a large trace. What's the best way?"
**Verify:**
- Agent recommends the long-running RPC mode.
- Agent explains how to start the server on a random port.
- Agent shows Python snippet to connect.
