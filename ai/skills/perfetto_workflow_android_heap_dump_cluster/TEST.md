# TEST for perfetto_workflow_android_heap_dump_cluster

## Tier 1 — Build & Smoke Tests

### Test 1: File Existence

Verify that the skill files exist.

```bash
ls ai/skills/perfetto_workflow_android_heap_dump_cluster/SKILL.md
ls ai/skills/perfetto_workflow_android_heap_dump_cluster/scripts/cluster_paths.py
ls ai/skills/perfetto_workflow_android_heap_dump_cluster/scripts/summarize_clusters.py
```

**Verify:** Files exist.

## Tier 2 — Functional Tests

### Test 2: Ask about clustering heap dumps

**Prompt:** "I have extracted dominator paths from multiple Android heap dumps.
How do I cluster them?" **Verify:**

-   Agent explains the mental model of normalizing paths and using TF-IDF with
    K-Means clustering.
-   Agent recommends invoking `scripts/cluster_paths.py`.

### Test 3: Ask about generating report

**Prompt:** "I have the clustered CSV output. How do I see the summary report?"
**Verify:**

-   Agent recommends invoking `scripts/summarize_clusters.py`.
-   Agent describes the expected report structure (top clusters, collapsed
    attribution chains).
