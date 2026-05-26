---
name: perfetto-workflow-android-heap-dump-cluster
description:
  Use when the user has multiple Android Java heap dump traces (or a batch of
  extracted heap dump paths) for a specific process and wants to cluster them to
  identify common memory leaks, group matching attribution chains, or eliminate
  parent-child dominator noise.
---

# Clustering Android Java Heap Dumps

This skill teaches an AI agent how to process, cluster, and summarize large sets
of Android Java heap dump traces (or pre-extracted dominator class paths) for a
specific process. It is designed to eliminate noise from parent-child
attribution chains (e.g., where `Captions`, `SubtitleWindow`, and
`SubtitleWindowTextTimeline` appear separately but share identical dominated
memory sizes).

If the user has not yet retrieved or extracted dominator paths from their
traces, guide them to use your environment's trace querying or batch extraction
mechanics first.

## Mental Model

When analyzing memory issues across a fleet or multiple timestamps of a process,
individual heap dumps produce overwhelming noise due to slight variations in
class paths and deeply nested dominator trees. To extract clear, actionable leak
signals:

1.  **Preprocess the Attribution Paths:** Strip out dynamic instance counts
    (e.g., `[104]`) and root type annotations (e.g., `[ROOT_JAVA_FRAME]`),
    leaving normalized, canonical class reference chains
    (`ClassA -> ClassB -> ClassC`).
2.  **Vectorize and Cluster:** Convert the normalized paths into feature vectors
    using TF-IDF (Term Frequency-Inverse Document Frequency). Use K-Means
    clustering to group structurally similar paths together.
3.  **Determine Optimal Clusters Dynamically:** Rather than hardcoding a fixed
    number of clusters ($K$), evaluate cluster quality across a range of $K$
    values using **Silhouette Scores** to identify the most natural mathematical
    grouping.
4.  **Collapse Parent-Child Chains:** Within each cluster, identify linear
    parent-child dominator relationships where the parent and child retain
    nearly identical memory sizes, collapsing them into a single logical
    attribution root.

## Step 1 — Prepare and Normalize Input Data

Ensure you have a CSV containing the extracted heap dump paths across your
traces. The expected CSV columns are
`trace_uuid,process_name,path,class_name,self_size`.

If the user provides raw traces, you can extract these columns by running the
standard dominator tree query from the `perfetto_workflow_android_heap_dump`
skill across each trace.

## Step 2 — Execute the Clustering Script

Invoke the provided standalone Python clustering script
(`scripts/cluster_paths.py`) on the CSV data. The script performs TF-IDF
vectorization, computes Silhouette scores to find the optimal $K$, and assigns
cluster IDs.

```bash
python3 scripts/cluster_paths.py --process_name <process_name> --input_file <path_to_input.csv> --output_file <path_to_clustered_output.csv>
```

> [!TIP] If you are running in an isolated environment, ensure `pandas` and
> `scikit-learn` are installed in your Python virtual environment
> (`pip install pandas scikit-learn`).

## Step 3 — Generate the Cluster Summary Report

Once the paths are clustered, invoke the summarization script
(`scripts/summarize_clusters.py`) to aggregate metrics and identify dominant
leak signatures per cluster.

```bash
python3 scripts/summarize_clusters.py --process_name <process_name> --input_file <path_to_clustered_output.csv> --output_file <path_to_summary_report.txt>
```

## What to Report Back

A high-quality cluster analysis summary for the user must include:

1.  **Fleet Overview:** The target process name, total number of traces
    analyzed, and the optimal number of clusters discovered ($K$).
2.  **Top Cluster Signatures:** For each major cluster, report the primary
    retaining class chain, the average dominated memory size, and the percentage
    of total traces belonging to that cluster.
3.  **Collapsed Attribution:** Explicitly highlight where parent-child
    components (e.g., inner listeners or UI wrapper classes) were collapsed into
    a single root cause.
4.  **Actionable Hypothesis:** Formulate clear hypotheses for the top clusters
    (e.g., "Cluster 1 represents a leaked Activity retained via a static
    singleton Handler; Cluster 2 represents a JNI global reference leak holding
    a large byte buffer").

## Reference

- Java heap profiler documentation:
  <https://perfetto.dev/docs/data-sources/java-heap-profiler>
- Perfetto SQL standard library (`android.memory.heap_graph`):
  <https://perfetto.dev/docs/analysis/stdlib-docs>
