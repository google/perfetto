# Instrumenting the Linux kernel with ftrace

## Outline

- Intro and explain what we're doing and why we'd like to do it.

A good way of intrumenting the kernel is to emit ftrace events. Perfetto can
natively record these and convert the events them as tracks which can be viewed
on the UI. A good example of this is the scheduling and thread state tracks,
which are generated from sched\_\* ftrace events.

In order to emit diagnostics from the kernel which can be viewed in perfetto,
you can use ftrace events. Perfetto records ftrace events and trace processor
interprets them, converting them into tracks which can be viewed in the the UI
and analysed using SQL queries.

On this page, you'll learn how to instrument your kernel subsystem using ftrace
events, record those events in a trace using `tracebox`, interpret those events
into tracks in `trace_processor`, and finally view the raw events and the
interpreted tracks in the Perfetto UI.

The general process looks like this:

- Instrument you kernel module by adding tracepoints to it. Each tracepoint can
  emit a number of arguments.
- The ftrace tracer can bind to these tracepoints and emit an ftrace event when
  you call these in your code.
- Configure traced_probes with the format of your ftrace event, so that it can
  encode all the argument data properly in the trace's proto file.
- Configure trace_processor to interpret these events as appropriate, generally
  creating slice and/or counter tracks from the events.

## Instrumenting the kernel

For this example we are going to create a kernel module called `ticker` which
emits an ftrace event called `ticker_tick` every second with an incrementing
counter as its only argument. Then we are going to configure trace processor to
convert these events to a counter track, visible in the UI.

Create a new directory and the following files:

```c
// ticker.c

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>

#define CREATE_TRACE_POINTS
#include "trace/events/ticker.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Perfetto");
MODULE_DESCRIPTION("Ticker: A kernel module emitting example static tracepoint events.");
MODULE_VERSION("0.1");

static struct timer_list my_timer;
static unsigned int tick_count = 0;
static unsigned long timer_interval_ms = 1000;

static void my_timer_callback(struct timer_list *timer)
{
    // Fire the tracepoint, incrementing the tick count every time.
    // The function name is trace_<event_name> from the header file.
    trace_ticker_tick(tick_count++);

    // Re-arm the timer.
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(timer_interval_ms));
}

static int __init ticker_init(void)
{
    pr_info("Ticker: Initializing...\n");

    timer_setup(&my_timer, my_timer_callback, 0);
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(timer_interval_ms));

    pr_info("Ticker: Timer started.\n");
    pr_info("Ticker: View events under /sys/kernel/tracing/events/ticker/\n");

    return 0;
}

static void __exit ticker_exit(void)
{
    pr_info("Ticker: Exiting...\n");
    del_timer_sync(&my_timer);
    pr_info("Ticker: Timer stopped and module unloaded.\n");
}

module_init(ticker_init);
module_exit(ticker_exit);
```

```h
// trace/events/ticker.h

#undef TRACE_SYSTEM
#define TRACE_SYSTEM ticker

#if !defined(_TRACE_KEVIN_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_KEVIN_H

#include <linux/tracepoint.h>

TRACE_EVENT(ticker_tick,

    TP_PROTO(unsigned int count),

    TP_ARGS(count),

    TP_STRUCT__entry(
        __field(unsigned int, id)
    ),

    TP_fast_assign(
        __entry->id = count;
    ),

    TP_printk("count=%u",
        __entry->id
    )
);

#endif /* _TRACE_KEVIN_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
```

```makefile
# Makefile

obj-m += ticker.o
ccflags-y += -I$(src)
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
```

Make sure your kernel headers are installed and run `make` to build the kernel
module.

You can now install the kernel module with:

```bash
sudo insmod ticker.ko
```

You can always uninstall the kernel module with:

```bash
sudo rmmod ticker
```

In order to check if everything is working, the kernel should have installed

```bash
cd /sys/kernel/tracing
ls events/ticker/ticker_tick
```

You can enable these events with:

```bash
echo 1 > events/ticker/ticker_tick/enable
echo 1 > tracing_on
```

Finally, listen to the stream of ftrace events with:

```bash
echo trace_pipe
```

## Recording a Trace

You need to create a recording config like below - its is configured to only
record ticker events.

```
# ticker.cfg

# One buffer allocated within the central tracing binary for the entire trace,
# shared by the two data sources below.
buffers {
  size_kb: 20480
  fill_policy: DISCARD
}

# Ftrace data from the kernel, mainly the process scheduling events.
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 0
    ftrace_config {
      ftrace_events: "ticker/ticker_tick"
    }
  }
}

# 10s trace, but can be stopped prematurely.
duration_ms: 10000
```

On Linux, obtain tracebox (see instructions todo) and run the following:

```bash
tracebox -c ticker.cfg --txt -o ticker.pftrace
```

Tracebox will take care of enabling ftrace tracing and enabling the ticker
events (as we did above manually).

This will create a perfetto trace at `ticker.pftrace` in the current directory.
You should be able to view this trace immediately.

## Viewing your trace

TODO video

## Turning tracepoints into slices and counters

Before we can add the logic into trace processor to convert these events into
slice and counter tracks, we first need to teach traced_probes the format of our
new ftrace event so that it can store the event and its arguments properly in
the trace proto.

Find the format of our event

```bash
cat /sys/kernel/tracing/events/ticker/ticker_tick/format
```

You should see something like this:

```
name: ticker_tick
ID: 1783
format:
        field:unsigned short common_type;       offset:0;       size:2; signed:0;
        field:unsigned char common_flags;       offset:2;       size:1; signed:0;
        field:unsigned char common_preempt_count;       offset:3;       size:1; signed:0;
        field:int common_pid;   offset:4;       size:4; signed:1;

        field:unsigned int id;  offset:8;       size:4; signed:0;

print fmt: "count=%u", REC->id
```

Copy and paste that into a file called
`src/traced/probes/ftrace/test/data/synthetic/events/ticker/ticker_tick/format`,
and add the event name to the file `src/tools/ftrace_proto_gen/event_list`

Then run the following command:

```bash
tools/run_ftrace_proto_gen
```

This will update `protos/perfetto/trace/ftrace/ftrace_event.proto` and
`protos/perfetto/trace/ftrace/ticker.proto`.

THen run:

```bash
tools/gen_all out/YOUR_BUILD_DIRECTORY
```

This will update src/traced/probes/ftrace/event_info.cc and
protos/perfetto/trace/perfetto_trace.proto.

Now we can add some special handling to `ftrace_parser.cc`. Find the enormous
switch-case in ParseFtraceEvent and add a new case for our new ftrace event
type.

```c++
// ftrace_parser.cc
static constexpr auto kTickerCountBlueprint = tracks::CounterBlueprint(
      "ticker",
      tracks::UnknownUnitBlueprint(),
      tracks::DimensionBlueprints(tracks::StringDimensionBlueprint("ticker")),
      tracks::FnNameBlueprint([](base::StringView key) {
        return base::StackString<1024>("%.*s", int(key.size()), key.data());
      }));

// ~~ snip ~~

      case FtraceEvent::kTickerEventFieldNumber: {
        ParseTickerEvent(cpu, ts, fld_bytes);
        break;
      }

// ~~ snip ~~

void FtraceParser::ParseTickerEvent(uint32_t cpu,
                                    int64_t timestamp,
                                    protozero::ConstBytes data) {
  protos::pbzero::TickerEventFtraceEvent::Decoder ticker_event(data);

  PERFETTO_LOG("Parsing ticker event: %" PRId64 ", %u, %d",
               timestamp,
               cpu,
               ticker_event.count());

  // Push the global counter.
  TrackId track = context_->track_tracker->InternTrack(
      kTickerCountBlueprint, tracks::Dimensions("Ticker Count"));
  context_->event_tracker->PushCounter(
      timestamp, static_cast<double>(ticker_event.count()), track);
}
```

You'll also need to add the ParseTickerEvent to the header file ftrace_parser.h

Rebuild trace_processor_shell and run the trace again. You should now see a
ticker track in the tracks table and ticker counter samples in the counter
table.

TODO Screenshots showing these tracks in the UI.

- Instrumenting code with ftrace events:
  - Include very basic C example (ticker) of how to add ftrace events
    (tracepoints) to your kernel module.
  - Reference instructions for this.
    - https://docs.kernel.org/trace/tracepoints.html
    - https://lwn.net/Articles/379903/
    - https://lwn.net/Articles/381064/
    - https://lwn.net/Articles/383362/
- Recording a trace
  - Include examples of how to record a trace containing this new ftrace event:
    - Just include the config and a short bash script to demo how to record
      using tracebox, and link to system tracing for information on how to
      actually do the recording
    - `tracebox -c ticker.cfg --txt -o ticker.pftrace`
    - Android using record_android_trace??
    - Links to system tracing page (where?) for full instructions on how to do
      tracing.
- Viewing recorded trace in the UI:
  - What you will see (ftrace tracks) - show a video of navigating around the
    trace and seeing the ftrace events.
  - How to convert these events into a (counter?) track using debug tracks.
    - `select *, extract_arg(arg_set_id, 'count') as value from ftrace_event where name = ticker_tick`
    - Create debug track
    - Show screenshots or video of the counter track.
- Adding a conversion to trace processor
  - Explain that you need to build the knowledge of the event format into
    traced_probes as well as trace processor for the event to be handled
    properly.
  - Converting and making tracks from the ftrace events - add to the ftrace
    parser.
  - Rebuild everything and re-record the trace.
  - Import into the UI and see the result.

## Intro

On this page, you'll learn how to instrument the Linux kernel to emit static
tracepoints, how to record those events in a trace, and how to analyze them
using perfetto.

This mode of tracing is high performance (especially if no tracers are active).
Perfetto has deep support recording and processing ftrace events.

## Instrumenting code with ftrace

There are several ways to instrument code with ftrace events. Typically you'll
want to use static tracepoints to define your ftrace events for your kernel
module / code. Then you'll want to emit events whenever anything interesting
happens in your code.

See the following links for more details:\

- https://docs.kernel.org/trace/tracepoints.html
- https://lwn.net/Articles/379903/
- https://lwn.net/Articles/381064/
- https://lwn.net/Articles/383362/

## Recording your ftrace trace

<?tabs>

TAB: Android

Use record_android_trace script

TAB: Linux

Make sure you have tracebox available (see todo).

Recording config:

```
# One buffer allocated within the central tracing binary for the entire trace,
# shared by the two data sources below.
buffers {
  size_kb: 20480
  fill_policy: DISCARD
}

# Ftrace data from the kernel
data_sources {
  config {
    name: "linux.ftrace"
    target_buffer: 0
    ftrace_config {
      # Add your custom events here - perfetto will enable them and record
      ftrace_events: "kevin/kevin_event"
    }
  }
}

# 10s trace, but can be stopped prematurely.
duration_ms: 10000
```

Run it with the following:

```bash
tracebox -c config.cfg --txt -o mytrace
```

</tabs?>

## Viewing your recorded trace

We can now explore the recorded trace in the perfetto UI. Navigate to
ui.perfetto.dev and drag and drop your file into the window (or press Ctrl/Cmd+O
to bring up the file selector dialog.).

Perfetto doesn't know how to interpret these events currently so they are
displayed in the 'raw' ftrace tracks only.

TODO a video on the UI.

Alternatively, you can explore the trace contents issuing SQL queries through
the [trace processor](/docs/analysis/trace-processor).

TODO: Video: open trace in Perfetto UI, navigate around

## Turning tracepoints into slices and counters

## Next Steps

ftrace with other data sources? some
