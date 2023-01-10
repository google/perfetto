# Quickstart: SQL-based analysis and trace-based metrics

_This quickstart explains how to use `trace_processor` as well as its Python API to 
programmatically query the trace contents through SQL and compute trace-based metrics._

## Trace Processor

TraceProcessor is a multi-format trace importing and query engine based on
SQLite. It comes both as a C++ library and as a standalone executable:
`trace_processor_shell` (or just `trace_processor`).

### Setup

```bash
# Download prebuilts (Linux and Mac only)
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor

# Start the interactive shell
./trace_processor trace.perfetto-trace

# Start a local trace processor instance to replace wasm module in the UI
./trace_processor trace.perfetto-trace --httpd
```

NOTE: In HTTP mode the trace will be loaded into the `trace_processor` and
      the UI will connect and issue queries over TCP. This can allow
      arbitrary sized traces to be loaded since there are no memory
      constraints, unlike the WASM module. In addition, this can improve
      performance in the UI as it issues SQL queries.

See [Trace Processor docs](/docs/analysis/trace-processor.md) for the full
TraceProcessor guide.

### Sample queries

For more exhaustive examples see the _SQL_ section of the various _Data sources_
docs.

#### Slices

Slices are stackable events which have name and span some duration of time.

![](/docs/images/slices.png "Example of slices in the UI")

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

![](/docs/images/counters.png "Example of counters in the UI")

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

![](/docs/images/sched-slices.png "Example of scheduler slices in the UI")

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

### Trace-based metrics

Trace Processor offers also a higher-level query interface that allows to run
pre-baked queries, herein called "metrics". Metrics are generally curated by
domain experts, often the same people who add the instrumentation points in the
first place, and output structured JSON/Protobuf/text.
Metrics allow to get a summarized view of the trace without having to type any
SQL or having to load the trace in the UI.

The metrics` schema files live in the
[/protos/perfetto/metrics](/protos/perfetto/metrics/) directory.
The corresponding SQL queries live in
[/src/trace_processor/metrics](/src/trace_processor/metrics/).

#### Run a single metric

Let's run the [`android_cpu`](/protos/perfetto/metrics/android/cpu_metric.proto)
metric. This metrics computes the total CPU time and the total cycles
(CPU frequency * time spent running at that frequency) for each process in the
trace, breaking it down by CPU (_core_) number.

```protobuf
./trace_processor --run-metrics android_cpu trace.perfetto-trace

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
```

#### Running multiple metrics

Multiple metrics can be flagged using comma separators to the `--run-metrics`
flag. This will output a text proto with the combined result of running both
metrics.

```protobuf
$ ./trace_processor --run-metrics android_mem,android_cpu trace.perfetto-trace

android_mem {
  process_metrics {
    process_name: ".dataservices"
    total_counters {
      anon_rss {
        min: 19451904
        max: 19890176
        avg: 19837548.157829277
      }
      file_rss {
        min: 25804800
        max: 25829376
        avg: 25827909.957489081
      }
      swap {
        min: 9289728
        max: 9728000
        avg: 9342355.8421707246
      }
      anon_and_swap {
        min: 29179904
        max: 29179904
        avg: 29179904
      }
    }
    ...
  }
  ...
}
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
      ...
    }
    ...
  }
  ...
}
```

#### JSON and binary output

The trace processor also supports binary protobuf and JSON as alternative output
formats. This is useful when the intended reader is an offline tool.

Both single and multiple metrics are supported as with proto text output.

```
./trace_processor --run-metrics android_mem --metrics-output=binary trace.perfetto-trace
<binary protobuf output>

./trace_processor --run-metrics android_mem,android_cpu --metrics-output=json trace.perfetto-trace
{
  "android_mem": {
    "process_metrics": [
      {
        "process_name": ".dataservices",
        "total_counters": {
          "anon_rss": {
            "min": 19451904.000000,
            "max": 19890176.000000,
            "avg": 19837548.157829
          },
          "file_rss": {
            "min": 25804800.000000,
            "max": 25829376.000000,
            "avg": 25827909.957489
          },
          "swap": {
            "min": 9289728.000000,
            "max": 9728000.000000,
            "avg": 9342355.842171
          },
          "anon_and_swap": {
            "min": 29179904.000000,
            "max": 29179904.000000,
            "avg": 29179904.000000
          }
        },
        ...
      },
      ...
    ]
  }
  "android_cpu": {
    "process_info": [
      {
        "name": "\/system\/bin\/init",
        "threads": [
          {
            "name": "init",
            "core": [
              {
                "id": 1,
                "metrics": {
                  "mcycles": 1,
                  "runtime_ns": 570365,
                  "min_freq_khz": 1900800,
                  "max_freq_khz": 1900800,
                  "avg_freq_khz": 1902017
                }
              },
              ...
            ]
            ...
          }
          ...
        ]
        ...
      },
      ...
    ]
    ...
  }
}
```

## Python API

The API can be run without requiring the `trace_processor` binary to be
downloaded or installed.

### Setup
```
$ pip install perfetto
```
NOTE: The API is only compatible with Python3.

### Example functions
See the Python API section of
[Trace Processor (SQL)](/docs/analysis/trace-processor.md) to get
more details on all available functions.

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
#### Metric
```python
from perfetto.trace_processor import TraceProcessor
tp = TraceProcessor(trace='trace.perfetto-trace')

cpu_metrics = tp.metric(['android_cpu'])
print(cpu_metrics)
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
    ...
  }
}
```

## Next steps

There are several options for exploring more of the trace analysis features
Perfetto provides:

* The [trace conversion quickstart](/docs/quickstart/traceconv.md) gives an
  overview on how to convert Perfetto traces to legacy formats to integrate with
  existing tooling.
* The [Trace Processor documentation](/docs/analysis/trace-processor.md) gives
  more information about how to work with trace processor including details on
  how to write queries and how tables in trace processor are organized.
* The [metrics documentation](/docs/analysis/metrics.md) gives a more in-depth
  look into metrics including a short walkthrough on how to build an
  experimental metric from scratch.
* The [SQL table reference](/docs/analysis/sql-tables.autogen) gives a
  comprehensive guide to the all the available tables in trace processor.
* The [common tasks](/docs/contributing/common-tasks.md) page gives a list of
  steps on how new metrics can be added to the trace processor.
