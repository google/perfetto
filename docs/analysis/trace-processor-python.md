# Trace Processor (Python)

The trace processor Python API is built on the trace procesor
[C++ library](/docs/analysis/trace-processor.md). By integrating with Python,
the library allows using Python's rich data analysis ecosystem to process
traces.

## Setup
```
pip install perfetto
```
NOTE: The API is only compatible with Python3.

```python
from perfetto.trace_processor import TraceProcessor
# Initialise TraceProcessor with a trace file
tp = TraceProcessor(trace='trace.perfetto-trace')
```

NOTE: The TraceProcessor can be initialized in a combination of ways including:
      <br> - An address at which there exists a running instance of `trace_processor` with a
      loaded trace (e.g.`TraceProcessor(addr='localhost:9001')`)
      <br> - An address at which there exists a running instance of `trace_processor` and
      needs a trace to be loaded in
      (e.g. `TraceProcessor(trace='trace.perfetto-trace', addr='localhost:9001')`)
      <br> - A path to a `trace_processor` binary and the trace to be loaded in
      (e.g. `TraceProcessor(trace='trace.perfetto-trace', config=TraceProcessorConfig(bin_path='./trace_processor'))`)


## API

The `trace_processor.api` module contains the `TraceProcessor` class which provides various
functions that can be called on the loaded trace. For more information on how to use
these functions, see this [`example`](/python/example.py).

#### Query
The query() function takes an SQL query as input and returns an iterator through the rows
of the result.

```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

qr_it = tp.query('SELECT ts, dur, name FROM slice')
for row in qr_it:
  print(row.ts, row.dur, row.name)
```
**Output**
```
261187017446933 358594 eglSwapBuffersWithDamageKHR
261187017518340 357 onMessageReceived
261187020825163 9948 queueBuffer
261187021345235 642 bufferLoad
261187121345235 153 query
...
```
The QueryResultIterator can also be converted to a Pandas DataFrame, although this
requires you to have both the `NumPy` and `Pandas` modules installed.
```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

qr_it = tp.query('SELECT ts, dur, name FROM slice')
qr_df = qr_it.as_pandas_dataframe()
print(qr_df.to_string())
```
**Output**
```
ts                   dur                  name
-------------------- -------------------- ---------------------------
     261187017446933               358594 eglSwapBuffersWithDamageKHR
     261187017518340                  357 onMessageReceived
     261187020825163                 9948 queueBuffer
     261187021345235                  642 bufferLoad
     261187121345235                  153 query
     ...
```
Furthermore, you can use the query result in a Pandas DataFrame format to easily
make visualisations from the trace data.
```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

qr_it = tp.query('SELECT ts, value FROM counter WHERE track_id=50')
qr_df = qr_it.as_pandas_dataframe()
qr_df = qr_df.replace(np.nan,0)
qr_df = qr_df.set_index('ts')['value'].plot()
```
**Output**

![Graph made from the query results](/docs/images/example_pd_graph.png)


### Metric
The metric() function takes in a list of trace metrics and returns the results as a Protobuf.

```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

ad_cpu_metrics = tp.metric(['android_cpu'])
print(ad_cpu_metrics)
```
**Output**
```
metrics {
  android_cpu {
    process_info {
      name: "/system/bin/init"
      threads {
        name: "init"
        core {
          id: 1
          metrics {
            mcycles: 1
            runtime_ns: 570365
            min_freq_khz: 1900800
            max_freq_khz: 1900800
            avg_freq_khz: 1902017
          }
        }
        core {
          id: 3
          metrics {
            mcycles: 0
            runtime_ns: 366406
            min_freq_khz: 1900800
            max_freq_khz: 1900800
            avg_freq_khz: 1902908
          }
        }
        ...
      }
      ...
    }
    process_info {
      name: "/system/bin/logd"
      threads {
        name: "logd.writer"
        core {
          id: 0
          metrics {
            mcycles: 8
            runtime_ns: 33842357
            min_freq_khz: 595200
            max_freq_khz: 1900800
            avg_freq_khz: 1891825
          }
        }
        core {
          id: 1
          metrics {
            mcycles: 9
            runtime_ns: 36019300
            min_freq_khz: 1171200
            max_freq_khz: 1900800
            avg_freq_khz: 1887969
          }
        }
        ...
      }
      ...
    }
    ...
  }
}
```

## HTTP
The `trace_processor.http` module contains the `TraceProcessorHttp` class which
provides methods to make HTTP requests to an address at which there already
exists a running instance of `trace_processor` with a trace loaded in. All
results are returned in Protobuf format
(see [`trace_processor_proto`](/protos/perfetto/trace_processor/trace_processor.proto)).
Some functions include:
* `execute_query()` - Takes in an SQL query and returns a `QueryResult` Protobuf
  message
* `compute_metric()` - Takes in a list of trace metrics and returns a
  `ComputeMetricResult` Protobuf message
* `status()` - Returns a `StatusResult` Protobuf message