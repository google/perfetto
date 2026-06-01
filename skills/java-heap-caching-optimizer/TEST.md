# Java Heap Optimizer Skill — Agent E2E Test Plan

## Prerequisites

**Read `SKILL.md` first** to understand instructions, heuristics, SQL queries, and expected outputs.

Ensure you have access to:
1. A Perfetto-formatted Java heap dump file (e.g., `~/traces/app-heap.pb` or a downloadable URL).
2. The local source code workspace corresponding to the heap dump's target package.

--------------------------------------------------------------------------------

## Test 1: Tool Available & Download Setup

**Prompt:** "Set up trace processor and prepare to analyze a heap dump."

**Verify:**
- Agent downloads or locates the `trace_processor` binary using:
  ```bash
  curl -LO https://get.perfetto.dev/trace_processor
  chmod +x trace_processor
  ```
- Agent confirms `trace_processor` is installed and executable.

--------------------------------------------------------------------------------

## Test 2: Analyze Heap Dump - Location Request

**Prompt:** "Can you analyze a heap dump and find caching optimizations for our app?"

**Verify:**
- Agent asks the user for the location of the heap dump (URL or local path).
- Agent asks the user for a specific package name they are interested in optimizing.

--------------------------------------------------------------------------------

## Test 3: Remote Download & High-Level Analysis

**Setup:** Have a Perfetto Java heap dump URL ready (e.g., `https://ui.perfetto.dev/#!/?s=0d259d2b6ef1e5e5970b83c47b5e531aff2d0fd9`).

**Prompt:** "Analyze the heap dump at https://ui.perfetto.dev/#!/?s=0d259d2b6ef1e5e5970b83c47b5e531aff2d0fd9 to find caching opportunities. Focus on package 'com.example.sampleapp'."

**Verify:**
- Agent downloads the heap dump file from the URL.
- Agent executes `query_most_repeated_objects.sql` and `query_size_frequencies.sql` using `trace_processor`.
- Agent filters out primitives and `java.lang.*` classes.
- Agent prioritizes classes matching `com.example.sampleapp`.
- Agent lists potential caching candidates in the prompt, categorized by heuristics:
  - High cumulative size objects that are not very often repeated.
  - Objects that repeat often independent of cumulative size.
  - Stateless objects.

--------------------------------------------------------------------------------

## Test 4: Verify Duplicate References via SQL

**Setup:** Identify candidate classes from Test 3 (e.g., Owner: `MediaItem`, Owned: `MediaMetadata`).

**Prompt:** "I suspect that MediaMetadata objects are duplicated and owned by MediaItem. Can you verify if MediaItem owns MediaMetadata in the heap dump '/home/user/traces/app-heap.pb'?"

**Verify:**
- Agent executes `query_heap_references.sql` using `trace_processor`.
- Agent replaces `<owner_classname>` with `MediaItem` and `<owned_classname>` with `MediaMetadata`.
- Agent interprets the results to confirm or refute the ownership relationship and reports back with details.

--------------------------------------------------------------------------------

## Test 5: Deep Code Analysis

**Setup:** Locate a target class file (e.g., `MediaMetadata.java`) and its referencing parent classes in the workspace.

**Prompt:** "We found a candidate class com.google.android.apps.photos.MediaMetadata which is repeated 15,000 times. The source code is in the workspace under src/com/google/android/apps/photos/MediaMetadata.java. Perform a deep analysis and present the findings."

**Verify:**
- Agent searches for and reads `MediaMetadata.java` and its parent referencing files in the workspace.
- Agent analyzes how the instances are constructed, stored, and whether they are stateless or mutable.
- Agent creates a markdown report listing:
  - Why `MediaMetadata` is a good candidate.
  - Implementation design details (e.g., static deduplication, LruCache).
  - Expected memory savings and file path to the results.

--------------------------------------------------------------------------------

## Test 6: Generate Implementation Plan

**Prompt:** "Create a detailed implementation plan for caching MediaMetadata in MediaItem. Include expected savings."

**Verify:**
- Agent creates or updates `analysis.md` or `implementation_plan.md`.
- Agent provides a detailed estimate of memory savings (detailing upper and lower bounds if an exact estimate isn't possible).
- Agent describes any architectural trade-offs that impact estimated savings.

--------------------------------------------------------------------------------

## Test 7: Apply Caching Optimization (Local Only)

**Prompt:** "I have reviewed the implementation plan. Please apply the caching changes to MediaMetadata.java and its references."

**Verify:**
- Agent implements the implementation plan provided.
- Agent preserves unrelated code structures, comments, and docstrings.
- Agent asks the user to review the local diffs.
