# TEST for perfetto-infra-getting-trace-processor

## Tier 1 — Build & Smoke Tests

### Test 1: File Existence
Verify that the skill files exist.
```bash
ls ai/skills/perfetto-infra-getting-trace-processor/SKILL.md
```
**Verify:** File exists.

## Tier 2 — Smoke Tests

### Test 2: Verify instructions work
Follow the instructions to download `trace_processor` and verify version.
```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x trace_processor
./trace_processor --version
```
**Verify:** `trace_processor` runs and prints version.

## Tier 3 — Functional Tests

### Test 3: Ask how to get trace_processor
**Prompt:** "I need to install trace_processor. What should I do?"
**Verify:**
- Agent recommends fetching from `get.perfetto.dev/trace_processor`.
- Agent explains it is hash-idempotent.
- Agent mentions making it executable.

### Test 4: Ask how to setup Python client
**Prompt:** "How do I set up the Perfetto Python client?"
**Verify:**
- Agent recommends a dedicated venv.
- Agent mentions installing `perfetto` and `protobuf`.
