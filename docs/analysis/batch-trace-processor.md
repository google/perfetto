# Batch Trace Processor

_The Batch Trace Processor is a Python library wrapping the
[Trace Processor](/docs/analysis/trace-processor.md): it allows fast (<1s)
interactive queries on large sets (up to ~1000) of traces._

## Installation

Batch Trace Processor is part of the `perfetto` Python library and can be
installed by running:

```shell
pip3 install pandas       # prerequisite for Batch Trace Processor
pip3 install perfetto
```

## Loading traces
NOTE: if you are a Googler, have a look at
[go/perfetto-btp-load-internal](http://goto.corp.google.com/perfetto-btp-load-internal) for how to load traces from Google-internal sources.

The simplest way to load traces in is by passing a list of file paths to load:
```python
from perfetto.batch_trace_processor.api import BatchTraceProcessor

files = [
  'traces/slow-start.pftrace',
  'traces/oom.pftrace',
  'traces/high-battery-drain.pftrace',
]
with BatchTraceProcessor(files) as btp:
  btp.query('...')
```

[glob](https://docs.python.org/3/library/glob.html) can be used to load
all traces in a directory:
```python
from perfetto.batch_trace_processor.api import BatchTraceProcessor

files = glob.glob('traces/*.pftrace')
with BatchTraceProcessor(files) as btp:
  btp.query('...')
```

NOTE: loading too many traces can cause out-of-memory issues: see
[this](/docs/analysis/batch-trace-processor#memory-usage) section for details.

A common requirement is to load traces located in the cloud or by sending
a request to a server. To support this usecase, traces can also be loaded
using [trace URIs](/docs/analysis/batch-trace-processor#trace-uris):
```python
from perfetto.batch_trace_processor.api import BatchTraceProcessor
from perfetto.batch_trace_processor.api import BatchTraceProcessorConfig
from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.trace_uri_resolver.registry import ResolverRegistry
from perfetto.trace_uri_resolver.resolver import TraceUriResolver

class FooResolver(TraceUriResolver):
  # See "Trace URIs" section below for how to implement a URI resolver.

config = BatchTraceProcessorConfig(
  # See "Trace URIs" below
)
with BatchTraceProcessor('foo:bar=1,baz=abc', config=config) as btp:
  btp.query('...')
```

## Writing queries
Writing queries with batch trace processor works very similarly to the
[Python API](/docs/analysis/batch-trace-processor#python-api).

For example, to get a count of the number of userspace slices:
```python
>>> btp.query('select count(1) from slice')
[  count(1)
0  2092592,   count(1)
0   156071,   count(1)
0   121431]
```
The return value of `query` is a list of [Pandas](https://pandas.pydata.org/)
dataframes, one for each trace loaded.

A common requirement is for all of the traces to be flattened into a
single dataframe instead of getting one dataframe per-trace. To support this,
the `query_and_flatten` function can be used:
```python
>>> btp.query_and_flatten('select count(1) from slice')
  count(1)
0  2092592
1   156071
2   121431
```

`query_and_flatten` also implicitly adds columns indicating the originating
trace. The exact columns added depend on the resolver being used: consult your
resolver's documentation for more information.

## Trace URIs
Trace URIs are a powerful feature of the batch trace processor. URIs decouple
the notion of "paths" to traces from the filesystem. Instead, the URI
describes *how* a trace should be fetched (i.e. by sending a HTTP request
to a server, from cloud storage etc).

The syntax of trace URIs are similar to web
[URLs](https://en.wikipedia.org/wiki/URL). Formally a trace URI has the
structure:
```
Trace URI = protocol:key1=val1(;keyn=valn)*
```

As an example:
```
gcs:bucket=foo;path=bar
```
would indicate that traces should be fetched using the protocol `gcs`
([Google Cloud Storage](https://cloud.google.com/storage)) with traces
located at bucket `foo` and path `bar` in the bucket.

NOTE: the `gcs` resolver is *not* actually included: it's simply given as its
an easy to understand example.

URIs are only a part of the puzzle: ultimately batch trace processor still needs
the bytes of the traces to be able to parse and query them. The job of
converting URIs to trace bytes is left to *resolvers* - Python
classes associated to each *protocol* and use the key-value pairs in the URI
to lookup the traces to be parsed.

By default, batch trace processor only ships with a single resolver which knows
how to lookup filesystem paths: however, custom resolvers can be easily
created and registered. See the documentation on the
[TraceUriResolver class](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/python/perfetto/trace_uri_resolver/resolver.py;l=56?q=resolver.py)
for information on how to do this.

## Memory usage
Memory usage is a very important thing to pay attention to working with batch
trace processor. Every trace loaded lives fully in memory: this is magic behind
making queries fast (<1s) even on hundreds of traces.

This also means that the number of traces you can load is heavily limited by
the amount of memory available available. As a rule of thumb, if your
average trace size is S and you are trying to load N traces, you will have
2 * S * N memory usage. Note that this can vary significantly based on the
exact contents and sizes of your trace.

## Advanced features
### Sharing computations between TP and BTP
Sometimes it can be useful to parameterise code to work with either trace
processor or batch trace processor. `execute` or `execute_and_flatten`
can be used for this purpose:
```python
def some_complex_calculation(tp):
  res = tp.query('...').as_pandas_dataframe()
  # ... do some calculations with res
  return res

# |some_complex_calculation| can be called with a [TraceProcessor] object:
tp = TraceProcessor('/foo/bar.pftrace')
some_complex_calculation(tp)

# |some_complex_calculation| can also be passed to |execute| or
# |execute_and_flatten|
btp = BatchTraceProcessor(['...', '...', '...'])

# Like |query|, |execute| returns one result per trace. Note that the returned
# value *does not* have to be a Pandas dataframe.
[a, b, c] = btp.execute(some_complex_calculation)

# Like |query_and_flatten|, |execute_and_flatten| merges the Pandas dataframes
# returned per trace into a single dataframe, adding any columns requested by
# the resolver.
flattened_res = btp.execute_and_flatten(some_complex_calculation)
```
