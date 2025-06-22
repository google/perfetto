# TODO

## Trace Processor (Python)

The trace processor Python API are bindings aroung the C++ trace processor
library and is the recommended way to start analysing traces.

### Setup

```
$ pip install perfetto
```

### Example usage

#### Query

```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

qr_it = tp.query('SELECT name FROM slice')
for row in qr_it:
  print(row.name)
```

**Output**

```
eglSwapBuffersWithDamageKHR
onMessageReceived
queueBuffer
bufferLoad
query
...
```

#### Query as Pandas DataFrame

```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

qr_it = tp.query('SELECT ts, name FROM slice')
qr_df = qr_it.as_pandas_dataframe()
print(qr_df.to_string())
```

**Output**

```
ts                   name
-------------------- ---------------------------
     261187017446933 eglSwapBuffersWithDamageKHR
     261187017518340 onMessageReceived
     261187020825163 queueBuffer
     261187021345235 bufferLoad
     261187121345235 query
     ...
```

## Trace Processor (C++)

As well as a Python API

### Setup

```bash
# Download prebuilts (Linux and Mac only)
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor

# Start the interactive shell
./trace_processor trace.perfetto-trace
```

### Sample queries

#### Slices

Slices are stackable events which have name and span some duration of time.

![](/docs/images/slices.png 'Example of slices in the UI')

```
> SELECT ts, dur, name FROM slice
ts                   dur                  name
-------------------- -------------------- ---------------------------
     261187017446933               358594 eglSwapBuffersWithDamageKHR
     261187017518340                  357 onMessageReceived
     261187020825163                 9948 queueBuffer
     261187021345235                  642 bufferLoad
     261187121345235                  153 query
     ...
```

#### Counters

Counters are events with a value which changes over time.

![](/docs/images/counters.png 'Example of counters in the UI')

```
> SELECT ts, value FROM counter
ts                   value
-------------------- --------------------
     261187012149954          1454.000000
     261187012399172          4232.000000
     261187012447402         14304.000000
     261187012535839         15490.000000
     261187012590890         17490.000000
     261187012590890         16590.000000
...
```

#### Scheduler slices

Scheduler slices indicate which thread was scheduled on which CPU at which time.

![](/docs/images/sched-slices.png 'Example of scheduler slices in the UI')

```
> SELECT ts, dur, cpu, utid FROM sched
ts                   dur                  cpu                  utid
-------------------- -------------------- -------------------- --------------------
     261187012170489               267188                    0                  390
     261187012170995               247153                    1                  767
     261187012418183                12812                    2                 2790
     261187012421099               220000                    6                  683
     261187012430995                72396                    7                 2791
...
```

## Next steps

There are several options for exploring more of the trace analysis features
Perfetto provides:
<!-- 
- The [trace conversion quickstart](/docs/quickstart/traceconv.md) gives an
  overview on how to convert Perfetto traces to legacy formats to integrate with
  existing tooling.
- The [Trace Processor documentation](/docs/analysis/trace-processor.md) gives
  more information about how to work with trace processor including details on
  how to write queries and how tables in trace processor are organized.
- The [metrics documentation](/docs/analysis/metrics.md) gives a more in-depth
  look into metrics including a short walkthrough on how to build an
  experimental metric from scratch.
- The [SQL table reference](/docs/analysis/sql-tables.autogen) gives a
  comprehensive guide to the all the available tables in trace processor.
- The [common tasks](/docs/contributing/common-tasks.md) page gives a list of
  steps on how new metrics can be added to the trace processor. -->
