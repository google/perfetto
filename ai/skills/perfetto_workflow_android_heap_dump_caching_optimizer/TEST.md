# Java Heap Optimizer Skill — Agent E2E Test Plan

## Prerequisites

**Read `SKILL.md` first** to understand instructions, heuristics, SQL queries, and expected outputs.

Ensure you have access to:
1. A Perfetto-formatted Java heap dump file (e.g., `~/traces/app-heap.pb` or a downloadable URL).
2. The local source code workspace corresponding to the heap dump's target package.

--------------------------------------------------------------------------------

## Test 1: Analyze Heap Dump - Initial Requests

**Prompt:** "Can you analyze a heap dump and find caching optimizations for our app?"

**Verify:**
- Agent asks the user for the location of the heap dump (URL or local path).
- Agent asks the user if there is a specific package name they are interested in optimizing.
- Agent asks the user if they want to focus on a specific class name regex.
- Agent asks the user for the codebase location.

--------------------------------------------------------------------------------

## Test 2: Remote Download & High-Level Analysis

**Setup:** Have a Perfetto Java heap dump URL ready (e.g., `https://ui.perfetto.dev/#!/?s=0d259d2b6ef1e5e5970b83c47b5e531aff2d0fd9`).

**Prompt:** "Analyze the heap dump at https://ui.perfetto.dev/#!/?s=0d259d2b6ef1e5e5970b83c47b5e531aff2d0fd9 to find caching opportunities. Focus on package 'com.example.sampleapp'."

**Verify:**
- Agent downloads the heap dump file from the URL.
- Agent asks for class name regex and codebase location.
- Agent loads the heap dump (following `perfetto_infra_querying_traces` skill).
- Agent executes `query_most_repeated_objects.sql` and `query_size_frequencies.sql` using `trace_processor`.
- Agent filters out primitives and `java.lang.*` classes.
- Agent prioritizes classes matching `com.example.sampleapp`.
- Agent lists potential caching candidates in the prompt and in a markdown file, categorized by heuristics.

--------------------------------------------------------------------------------

## Test 3: Verify Duplicate References via SQL

**Setup:** Identify candidate classes from Test 2 (e.g., Owner: `MediaItem`, Owned: `MediaMetadata`).

**Prompt:** "I suspect that MediaMetadata objects are duplicated and owned by MediaItem. Can you verify if MediaItem owns MediaMetadata in the heap dump '/home/user/traces/app-heap.pb'?"

**Verify:**
- Agent executes `query_heap_references.sql` using `trace_processor`.
- Agent replaces `<owner_classname>` with `MediaItem` and `<owned_classname>` with `MediaMetadata`.
- Agent interprets the results to confirm or refute the ownership relationship and reports back with details.

--------------------------------------------------------------------------------

## Test 4: Deep Code Analysis

**Setup:** Locate a target class file (e.g., `MediaMetadata.java`) and its referencing parent classes in the workspace.

**Prompt:** "We found a candidate class com.google.android.apps.photos.MediaMetadata which is repeated 15,000 times. The source code is in the workspace under src/com/google/android/apps/photos/MediaMetadata.java. Perform a deep analysis of references and present findings."

**Verify:**
- Agent searches for and reads `MediaMetadata.java` and its parent referencing files in the workspace.
- Agent analyzes the reference hierarchy to identify the optimal location in the call stack to cache.
- Agent creates a markdown report listing:
  - Why `MediaMetadata` is a good candidate.
  - Implementation design details.
  - Expected memory savings and file path to the results.

--------------------------------------------------------------------------------

## Test 5: Generate Implementation Plan (With Code)

**Setup:** Codebase is available in the workspace.

**Prompt:** "Create a detailed implementation plan for caching MediaMetadata in MediaItem. The codebase is in the workspace."

**Verify:**
- Agent creates a detailed implementation plan with step-by-step instructions and code changes.
- Agent creates `analysis.md` showing in-depth reasoning, why to cache, and trade-offs.
- Agent provides detailed estimate of memory savings (with bounds if needed).

--------------------------------------------------------------------------------

## Test 6: Generate Implementation Plan (No Code)

**Setup:** Codebase is NOT available.

**Prompt:** "Create a detailed implementation plan for caching MediaMetadata. We do not have access to the codebase."

**Verify:**
- Agent creates `analysis.md` with in-depth reasoning and trade-offs.
- Agent provides a list of suggestions on what to look for in the codebase to implement caching.

--------------------------------------------------------------------------------

## Test 7: Apply Caching Optimization (Local Only)

**Prompt:** "I have reviewed the implementation plan. Please apply the caching changes to MediaMetadata.java and its references."

**Verify:**
- Agent implements the caching changes locally.
- Agent preserves unrelated code structures, comments, and docstrings.
- Agent asks the user to review the local diffs (does not commit).
