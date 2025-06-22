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
