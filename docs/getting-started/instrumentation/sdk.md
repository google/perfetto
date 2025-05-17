# Intstrumenting code with the Perfetto SDK

_In this page, you'll TODO_

- Perfetto has C SDK for instrumenting your program
- Instrumentation gives insight into what your app is doing
- Can be collected inside your app through C APIs or combined with other system
  level information.

Link to collection page if you don't already have infra setup to collect traces
with Perfetto.

Directly take data from the sdk/track-event pages

## Track Event

Perferred source

### Slices

Timestamped data with duration Designed for representing execution over time
Good for function calls, async operations spanning some length of time (RPCs,
IPCs, network requests etc).

### Instants

Timestamped data at a single point. Good for "fire and forget" style operations

### Flows

Used to causally link slices together A caused B to happen

### Counters

Value over time

Good for any numerical value which changes but always has some value

## Data sources

For more complex data, needs custom protos etc.

## Next Steps

If you want to see traces alongside system data, go to system page.
