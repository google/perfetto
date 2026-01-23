# Better Association of Perf Samples with Counters

**Authors:** @LalitMaganti

**Status:** Decided

**PR:** N/A

## Problem

When we collect perf samples today, we always collect them based on a timebase
which tells you "which hardware/software counter should this sample be collected
on". In most cases this is a time/cycle based counter which goes something like
"sample every 1 ms" or "sample every 1 mcycle" etc.

At the same time, the perf subsystem can also lookup a bunch of other counters
as well to determine their values. This are "follower counters" which allow
for tracking other metrics like page faults etc.

The main problem is that, when this data reaches trace processor today, the link
between the sample (mainly the callstack) and the counters is *removed*. That
means if you want to ask the question "what was the value of the perf counter
at sample X", it's very hard to do so. Really it requires joining across the
tables with (perf_sample_id, ts) which is both highly inefficient and
non-intuitive.

## Design

We propose making the following changes:
1. Introducing a new `__intrinsic_perf_counter_set` table with the columns
   `perf_counter_set_id`, `counter_id`. The counter set should be
   a "set id" similar arg_set_id. The counter id points to the counter value in
   the counter table
2. Introduce a new intrinisic function `__intrinsic_perf_counter_for_sample`
   which, given a perf sample id annd a counter name, returns the counter value
   for it.
3. Introduce a new view in the stdlib table `perf_sample_with_counters` in the
   `linux.perf` module which is the join of perf_sample,
   __intrinsic_perf_counter_set and counter table. Basically this is a fully
   denormalized view of things which people can use for filtering/aggregation
   at the cost of performance from having to do the joins.
4. A "reexport" `__intrinsic_perf_counter_for_sample` function in the stdlib
   with a public name as `linux_perf_counter_for_sample(sample_id)`.
5. Add a new column to __intrinsic_perf_sample (not exposed to public API for
   now) called `counter_set_id` which mapes to `__intrinsic_perf_counter_set`.

## Alternatives considered

### Just having the normalized tables

It can be argued the denoramized `perf_sample_with_counters` is a footgun and
has the potential for people to write queries which are inefficient or just give
the data users don't expect (because for each sample it has multiple entries,
one per counter).

Upon discussion within the team, it was decided that the gains from being able
to have a single, join-less table to query out-weighed these considerations.

## Open questions

N/A
