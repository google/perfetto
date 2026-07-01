# Android Heap Dump Caching Optimizer

This workflow walks an AI agent through analyzing a perfetto java heap dump to identify memory savings by finding duplicated objects, objects that could be consolidated into a single object instance, or objects that are highly repeated and propose a caching strategy along with potential memory savings. If the agent has access to the codebase, then it would also propose a concrete implementation plan to implement caching and improve memory usage.

If the user has not yet loaded a heap dump into `trace_processor`, follow
`$SKILL_ROOT/infra-references/querying.md` first, then come back here to analyze it.

## Instructions

1. Ask the user if there's a specific package name they want to focus when exploring potential optimizations, if none provided, then consider classes for every package in your analysis. If one is provided, use it to prioritize and filter candidates.

2. Ask them if they want to focus on optimizing a specific class of objects they can specify a regex that matches the class name, if they specify one, then focus on optimizing objects of that class or any objects that are retained by that class in the heap dump, otherwise, be open to optimize any object in the heap dump if it seems like a good candidate for optimization.

3. Retrieve the location of the heap dump if the user has not yet provided it, ask the user to provide the location either via a URL or a local file path, if it is a URL then download it to the local filesystem. then once the heap dump file is downloaded and available in a local path, then follow `$SKILL_ROOT/infra-references/querying.md` to load it into trace processor and then come back here to analyze it.

4. Ask the user if they want to provide a codebase location either a URL, a custom local path,current workspace or only look at heap dump without codebase. If the codebase is unavailable let the user know that the analysis will be performed purely based on the heap dump data as code base was not found.

5. Now that you have the heap dump location, execute the following PerfettoSQL queries (shipped alongside this workflow under `$SKILL_ROOT/workflows/android_memory/scripts/`) using trace processor on the heap dump file, e.g. `trace_processor query --query-file $SKILL_ROOT/workflows/android_memory/scripts/query_most_repeated_objects.sql HEAP_DUMP_FILE`.
 * Execute `$SKILL_ROOT/workflows/android_memory/scripts/query_most_repeated_objects.sql` to find the most repeated objects and their classes.
 * Execute `$SKILL_ROOT/workflows/android_memory/scripts/query_size_frequencies.sql` to find the objects whose sizes repeat the most.

6. After you execute the queries, you will have two sets of results. You may use following information to understand the results, but also you are encouraged to execute any other queries that you think might help you find caching opportunities:

The output of `$SKILL_ROOT/workflows/android_memory/scripts/query_most_repeated_objects.sql` will contain the following important pieces of data:
* total_unique_paths_to_gc_root: This value tells you how many unique paths there are from the root of the heap to the objects in the heap. It is a good representation of how many different code paths converge into the allocated objects of a certain class.
* class_name: This value tells you the name of the class.
* total_objects: This value tells you how many instances of a class are present in the heap dump.
* total_objects_memory_consumption: This value tells you how much memory the objects of a certain class are consuming (all instances combined).
* single_object_self_size: This value tells you the self size of a single object of a certain class. 
* single_object_cumulative_size: This value tells you the cumulative size of a single object of a certain class. Which includes the object itself and all of its children. Removing an object would free the cumulative size in memory.

The output of `$SKILL_ROOT/workflows/android_memory/scripts/query_size_frequencies.sql` will contain the following important pieces of data:
* class_name: This value tells you the name of the class.
* single_object_self_size: This value tells you the size of an object in bytes.
* occurrence_count: This value tells you how many objects of that size are present in the heap dump.

Some other useful tables to query in heap dumps are:
heap_graph_object: Information about individual objects in the heap dump.
heap_graph_class: Information about classes in the heap dump.
heap_graph_reference: Information about references between objects in the heap dump.

7. Once you understand the data, think about what kind of objects could be good candidates for caching. Filter out any objects that are primitives objects or objects that are part of java.lang.*, if the user provided a package name, prioritize objects that are part of that package name.

Use the following set of heuristics to find opportinities but don't limit yourself to them, if you find another heuristic that leads to a good caching candidate, go for it.

* Highly repeated objects may have more potential for caching as the ratio of potential saved memory is higher.
* Stateless objects tend to be easy to cache as they do not have any state that needs to be managed.
* Objects that repeat and many different root paths converge to them are good candidates for caching as they are likely being reused in multiple different parts of the codebase.
* Objects with a high cumulative size are good candidates for caching as removing them would free a significant amount of memory.
* Objects whose size is similar may indicate potential duplicates that can be consolidated into fewer objects via caching.

Note: Not every heuristic has to be optimized, you may find one opportunity that falls into just one heuristic but still is a good candidate for caching. The goal is to find the top memory saving objects or memory leaks but more about the easy to cache objects that would yield savings with low implementation complexity. Ideally you will identify several different opportunities that fall into different heuristics

8. Now that you have a list of potential candidates. Perform a deep analysis both on heap dump and codebase (if available), make sure to analyze the object reference hierarchy to identify the optimal location in the call stack to cache objects. You can run more queries after looking at the code base if required to improve your analysis. To confirm that one class owns (dominates) duplicated instances of another — e.g. verifying a suspected duplicate is retained by a particular owner — run `$SKILL_ROOT/workflows/android_memory/scripts/query_heap_references.sql`, replacing the `<owner_classname>` and `<owned_classname>` placeholders with the two class names.

9. Present your findings to the user both in a markdown file as well as in the prompt. List every opportunity you identified along with a brief explanation of why it might be a good candidate for caching, how to implement it and an estimate of how much memory you estimate that it would save. Provide the file path to the file containing the results of the analysis.

10. If more than one opportunity was identified, ask the user from the provided opportunities which one they would like you to proceed with. Once user selected an option to move forward with. Create the following artifacts:
* If code was available to the agent: Provide an implementation plan which consists of step-by-step instructions on how to implement the caching solution, including any code changes required.
* If code was not available to the agent: Provide a list of suggestions on what to look for in the codebase to implement the caching solution to the best of your ability.

* Always provide an in-detail explanation doc called `analysis.md` which shows the in-depth reasoning of savings, why you are suggesting to cache, potential trade-offs you considered, any other details and considerations which you think are important. In case trade off decisions were made, ensure the memory savings are properly updated in `analysis.md`.

11. Once the user has agreed to the implementation plan, proceed with the implementation and apply the changes locally, do not commit them, let the user handle the code submission process.

