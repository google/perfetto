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

To load traces, pass a list of file paths:
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

To load traces from cloud storage or a server, use
[trace URIs](/docs/analysis/batch-trace-processor#trace-uris):
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
[Python API](/docs/analysis/trace-processor-python.md).

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

To combine results from all traces into a single dataframe, use
`query_and_flatten`:
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

[Polars](https://pola.rs/) DataFrames are also supported as an alternative to
Pandas. `query_polars` mirrors `query` and returns a list of Polars DataFrames
(one per trace); `query_and_flatten_polars` mirrors `query_and_flatten` and
concatenates them into a single DataFrame. Polars support requires an optional
dependency:

```shell
pip3 install perfetto[polars]
```

```python
>>> btp.query_polars('select count(1) from slice')
[shape: (1, 1)
┌──────────┐
│ count(1) │
│ ---      │
│ i64      │
╞══════════╡
│  2092592 │
└──────────┘, shape: (1, 1)
┌──────────┐
│ count(1) │
│ ---      │
│ i64      │
╞══════════╡
│   156071 │
└──────────┘, ...]

>>> btp.query_and_flatten_polars('select count(1) from slice')
shape: (3, 1)
┌──────────┐
│ count(1) │
│ ---      │
│ i64      │
╞══════════╡
│  2092592 │
│   156071 │
│   121431 │
└──────────┘
```

## Trace URIs
Trace URIs describe *how* to fetch a trace, rather than where it lives in the
filesystem. For example, a URI can specify an HTTP request to a server or a
location in cloud storage.

The syntax of trace URIs is similar to web
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

NOTE: The `gcs` resolver is *not* included; it is used here as an example.

Batch Trace Processor uses *resolvers* to convert URIs to trace bytes for
parsing and querying. Resolvers are Python classes associated with each
*protocol*. They use the key-value pairs in the URI to look up traces.

By default, Batch Trace Processor ships with a single resolver for filesystem
paths. You can also create and register custom resolvers. See the documentation
on the
[TraceUriResolver class](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/python/perfetto/trace_uri_resolver/resolver.py;l=56?q=resolver.py)
for information on how to do this.

## Memory usage
Every loaded trace lives fully in memory, allowing fast queries (<1s) even on
hundreds of traces.

This also means that the number of traces you can load is heavily limited by the
amount of available memory. As a rule of thumb, if your average trace size is S
and you are trying to load N traces, you will have 2 * S * N memory usage. Note
that this can vary significantly based on the exact contents and sizes of your
trace.

## Advanced features
### Sharing computations between TP and BTP
Sometimes it can be useful to parameterize code to work with either trace
processor or batch trace processor. `execute` or `execute_and_flatten` can be
used for this purpose:
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
