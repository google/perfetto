---
name: java-heap-optimizer
description: >-
  Find caching optimization opportunities in heap dumps
---

# Heap Optimizer

You are an expert android performance engineer that specializes in analyzing heap dumps and finding caching opportunities.

## When to Use

Use this skill when the user wants to analyze a perfetto java heap dump and find potential caching optimizations that would generate memory savings and reduce memory churn.

## Instructions

1. Retrieve the location of the heap dump if the user has not yet provided it, ask the user to provide the location either via a URL or a local file path, if it is a URL then download it to the local filesystem. Ask the user if there's a specific package name they are interested in optimizing.

2. Now that you have the heap dump location, execute the following PerfettoSQL queries using trace processor on the heap dump file.
 * Execute `query_most_repeated_objects.sql` script to find the most repeated objects and their classes.
 * Execute `query_size_frequencies.sql` script to find the objects whose sizes repeat the most.

3. After you execute the queries, you will have two sets of results. You may use following information to understand the results, but also you are encouraged to execute any other queries that you think might help you find caching opportunities:

The output of `query_most_repeated_objects.sql` will contain the following important pieces of data:
* total_unique_paths_to_gc_root: This value tells you how many unique paths there are from the root of the heap to the objects in the heap. It is a good representation of how many different code paths converge into the allocated objects of a certain class.
* class_name: This value tells you the name of the class.
* total_objects: This value tells you how many instances of a class are present in the heap dump.
* total_objects_memory_consumption: This value tells you how much memory the objects of a certain class are consuming (all instances combined).
* single_object_self_size: This value tells you the self size of a single object of a certain class. 
* single_object_cumulative_size: This value tells you the cumulative size of a single object of a certain class. Which includes the object itself and all of its children. Removing an object would free the cumulative size in memory.

The output of `query_size_frequencies.sql` will contain the following important pieces of data:
* class_name: This value tells you the name of the class.
* single_object_self_size: This value tells you the size of an object in bytes.
* occurrence_count: This value tells you how many objects of that size are present in the heap dump.

Some other useful tables to query in heap dumps are:
heap_graph_object: Information about individual objects in the heap dump.
heap_graph_class: Information about classes in the heap dump.
heap_graph_reference: Information about references between objects in the heap dump.

4. The goal with the data will be to find potential caching opportunities, perform any other queries that you think might help you find them, if these are not sufficient and show them in the output. In order to execute queries you will need to use the trace processor tool which can be downloaded from the Perfetto website. Use following command to download it:

```
curl -LO https://get.perfetto.dev/trace_processor 
```

5. Once you understand the data, think about what kind of objects could be good candidates for caching. Filter out any objects that are primitives objects or objects that are part of java.lang.*, if the user provided a package name, prioritize objects that are part of that package name.

Find opportunities across several different heuristics to find opportinities, don't limit yourself to these, but they are a good place to start:

* Highly repeated objects may have more potential for caching as the ratio of potential saved memory is higher.
* Stateless objects tend to be easy to cache as they do not have any state that needs to be managed.
* Objects that repeat and many different root paths converge to them are good candidates for caching as they are likely being reused in multiple different parts of the codebase.
* Objects with a high cumulative size are good candidates for caching as removing them would free a significant amount of memory.
* Objects whose size is similar may indicate potential duplicates that can be consolidated into fewer objects via caching.

6. Now that you have a list of potential candidates, try to locate the source code in the current workspace, if its not available or if the user has indicated another location for it like a URL, go and search for it. Perform a deep analysis, make sure to analyze the object reference hierarchy to identify the optimal location in the chain to cache objects.

7. Present your findings to the user both in a markdown file as well as in the prompt. List every opportunity you identified along with an explanation of why it might be a good candidate for caching, how to implement it and how much memory you estimate that it would save. Provide the file path to the file containing the results of the analysis.

8. Ask the user from the provided opportunities which one they would like you to proceed with creating an implementation plan for. Once the user selects which one they would like you to proceed with, then create the implementation plan for that specific opportunity. Go into detail about the expected memory savings and if you are unable to estimate an exact number for memory saved, provide an explanation of why and provide the upper and lower bounds for memory saved instead. Provide the in-detail explanation doc called analysis.md in a markdown file. Make sure to update any estimates based on the implementation plan, if you decided to do trade-offs that will impact savings, mention it in the analysis.md file and tell the user about it.

9. Once the user has made the changes requested in the implementation doc, proceed with the implementation and just apply it locally, let the user handle the code submission process.

