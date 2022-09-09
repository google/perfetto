# Batch Trace Processor
This document describes the overall design of Batch Trace Processor and
aids in integrating it into other systems.

![BTP Overview](/docs/images/perfetto-btp-overview.svg)

## Motivation
The Perfetto trace processor is the de-facto way to perform analysis on a
single trace. Using the
[trace processor Python API](/docs/analysis/trace-processor#python-api),
traces can be queried interactively, plots made from those results etc.

While queries on a single trace are useful when debugging a specific problem
in that trace or in the very early stages of understanding a domain, it soon
becomes limiting. One trace is unlikely to be representative
of the entire population and it's easy to overfit queries i.e. spend a
lot of effort on breaking down a problem in that trace while neglecting
other, more common issues in the population.

Because of this, what we actually want is to be able to query many traces
(usually on the order of 250-10000+) and identify the patterns which show
up in a significant fraction of them. This ensures that time is being spent
on issues which are affecting user experience instead of just a random
problem which happened to show up in the trace.

One low-effort option for solving this problem is simply to ask people to use
utilities like [Executors](https://docs.python.org/3/library/concurrent.futures.html#executor-objects)
with the Python API to load multiple traces and query them in parallel.
Unfortunately, there are several downsides to this approach:
* Every user has to reinvent the wheel every time they want to query multiple
  traces. Over time, there would likely be a proliferation of slightly modified
  code which is copied from each place.
* While the basics of parallelising queries on multiple traces on a single
  machine is straightforward, one day, we may want to shard trace processing
  across multiple machines. Once this happens, the complexity of the code would
  rise significantly to the point where a central implementation becomes a
  necessity. Because of this, it's better to have the API first before engineers
  start building their own custom solutions.
* A big aim for the Perfetto team these days is to make trace analysis more
  accessible to reduce the number of places where we need to be in the loop.
  Having a well supported API for an important usecase like bulk trace analysis
  directly helps with this.

While we've discussed querying traces so far, the experience for loading traces
from different traces should be just as good. This has historically been a big
reason why the Python API has not gained as much adoption as we would have
liked.

Especially internally in Google, we should not be relying on engineers
knowing where traces live on the network filesystem and the directory layout.
Instead, they should be able to simply be able to specify the data source (i.e.
lab, testing population) and some parameters (e.g. build id, date, kernel
version) that traces should match should match and traces meeting these criteria
should found and loaded.

Putting all this together, we want to build a library which can:
* Interactively query ~1000+ traces in O(s) (for simple queries)
* Expose full SQL expressiveness from trace processor
* Load traces from many sources with minimal ceremony. This should  include
  Google-internal sources: e.g. lab runs and internal testing populations
* Integrate with data analysis libraries for easy charting and visulazation

## Design Highlights
In this section, we briefly discuss some of the most impactful design decisions
taken when building batch trace processor and the reasons behind them.

### Language
The choice of langugage is pretty straightforward. Python is already the go-to
langugage for data analysis in a wide variety of domains and our problem
is not unique enough to warrant making a different decision. Moreover, another
point in favour is the existence of the Python API for trace processor. This
further eases the implementation as we do not have to start from scratch.

The main downside of choosing Python is performance but given that that all
the data crunching happens in C++ inside TP,  this is not a big factor.

### Trace URIs and Resolvers
[Trace URIs](/docs/analysis/batch-trace-processor#trace-uris)
are an elegant solution to the problem of loading traces from a diverse range
of public and internal sources. As with web URIs, the idea with trace URIs is
to describe both the protocol (i.e. the source) from which traces should be
fetched and the arguments (i.e. query parameters) which the traces should match.

Batch trace processor should integrate tightly with trace URIs and their
resolvers. Users should be able to pass either just the URI (whcih is really
just a string for maximum flexibility) or a resolver object which can yield a
list of trace file paths.

To handle URI strings, there should be some mecahinsm of "registering" resolvers
to make them eligible to resolve a certain "protocol". By default, we should
provide a resolver to handle filesystem. We should ensure that the resolver
design is such that resolvers can be closed soruce while the rest of batch trace
processor is open.

Along with the job of yielding a list of traces, resolvers should also be
responsible for creating metadata for each trace these are different pieces
of information about the trace that the user might be interested in e.g. OS
version, device name, collected date etc. The metadata can then be used when
"flattening" results across many traces as discussed below.

### Persisting loaded traces
Optimizing the loading of traces is critical for the O(s) query performance
we want out of batch trace processor. Traces are often accessed
over the network meaning fetching their contents has a high latency.
Traces also take at least a few seconds to parse, eating up the budget for
O(s) before even getting the running time of queries.

To address this issue, we take the decision to keep all traces fully loaded in
memory in trace processor instances. That way, instead of loading them on every
query/set of queries, we can issue queries directly.

For the moment, we restrict the loading and querying of traces to a
single machine. While querying n traces is "embarassngly parallel" and shards
perfectly across multiple machines, introducing distributed systems to any
solution simply makes everything more complicated. The move to multiple
machines is explored further in the "Future plans" section.

### Flattening query results
The naive way to return the result of querying n traces is a list
of n elements, with each element being result for a single trace. However,
after performing several case-study performance investigations using BTP, it
became obvious that this obvious answer was not the most convienent for the end
user.

Instead, a pattern which proved very useful was to "flatten" the results into
a single table, containing the results from all the traces. However,
simply flattening causes us to lose the information about which trace a row
originated from. We can deal with this by allowing resolvers to silently add
columns with the metadata for each trace.


So suppose we query three traces with:

```SELECT ts, dur FROM slice```

Then in the flattening operation might do something like this behind the scenes:
![BTP Flattening](/docs/images/perfetto-btp-flattening.svg)


## Integration points
Batch trace processor needs to be both open source yet allow deep integration
with Google internal tooling. Because of this, there are various integration
points built design to allow closed compoentns to be slotted in place of the
default, open source ones.

The first point is the formalization of the idea "platform" code. Even since the
begining of the Python API, there was always a need for code internally to be
run slightly different to open source code. For example, Google internal Python
distrubution does not use Pip, instead packaging dependencies into a single
binary. The notion of a "platform" loosely existed to abstract this sort of
differences but this was very ad-hoc. As part of batch trace processor
implementation, this has been retroactively formalized.

Resolvers are another big point of pluggability. By allowing registration of
a "protocol" for each internal trace source (e.g. lab, testing population), we
allow for trace loading to be neatly abstracted.

Finally, for batch trace processor specifically, we abstract the creation of
thread pools for loading traces and running queries. The parallelism and memory
available to programs internally is often does not 1:1 correspond with the
available CPUs/memory on the system: internal APIs need to be accessed to find
out this information.

## Future plans
One common problem when running batch trace processor is that we are
constrained by a single machine and so can only load O(1000) traces.
For rare problems, there might only be a handful of traces matching a given
pattern even in such a large sample.

A way around this would be to build a "no trace limit" mode. The idea here
is that you would develop queries like usual with batch trace processor
operating on a O(1000) traces with O(s) performance. Once the queries are
relatively finalized, we could then "switch" the mode of batch trace processor
to opeate closer to a "MapReduce" style pipeline which operates over O(10000)+
traces loading O(n cpus) traces at any one time.

This allows us to retain both the quick iteration speed while developing queries
while also allowing for large scale analysis without needing to move code
to a pipeline model. However, this approach does not really resolve the root
cause of the problem which is that we are restricted to a single machine.

The "ideal" solution here is to, as mentioned above, shard batch trace processor
across >1 machine. When querying traces, each trace is entirely independent of
any other so paralleising across multiple machines yields very close to perfect
gains in performance at little cost.

This is would be however quite a complex undertaking. We would need to design
the API in such a way that allows for pluggable integration with various compute
platforms (e.g. GCP, Google internal, your custom infra). Even restricting to
just Google infra and leaving others as open for contribution, internal infra's
ideal workload does not match the approach of "have a bunch of machines tied to
one user waiting for their input". There would need to be significiant research
and design work before going here but it would likely be wortwhile.
