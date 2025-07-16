# Support for Multiple Open Traces

## Overview

This document describes the support for keeping multiple open traces open simultaneously in the Perfetto UI.
There are three core concepts implementing this capability:

- pushing the contextual `Trace` object down into all UI and other components that need it, rather than relying on pulling the currently active trace from the `AppImpl` instance.
This includes core components such as `UiMain` and `Sidebar`, allowing embedder applications to instantiate the main UI multiple times to present multiple traces to the user
- hierarchical registries and managers: The `Registry` API supports delegation of look-up to a parent registry. The various `XyzManager` objects instantiated in the `TraceImpl` make use of this to establish trace-level registrations of commands, pages, and more
- keeping track of multiple loaded (and loading) `TraceImpl`s  in the `AppImpl`, with one of them at any time designated as the "current trace" on the assumption that applications supporting multiple traces will have at most one "in focuse" at any time

## Hierarchical Registries

The `Registry<T>` class supports the creation of any number of children, each of which inherits registrations from their parent.
The `createChild()` method creates a new child registry that

- uses the same key function to extract keys from registered services as does its parent
- on look-up via `get()` or `tryGet()`, delegates to the parent for any keys not registered in itself
- permits registration of the services under the same keys as services registered in the parent, thereby shadowing (overriding) those inherited services
- iteration over the child registry yields all services in the parent that are not overridden in the child, as well as all services registered in the child
- has an optional `id: string` property distinguishing it from its parent and from other child registries

**Note** that, although a child registry can register a service under the same key as a service registered in its parent, registrations in the child registry are still unique: attempting a second overriding registration will fail with an error.

## Trace-scoped Manager Services

All of the core UI managers such as `CommandManager` and `SidebarManager` that are created by the `TraceImpl` are trace-scoped by instantiation as a child of the corresponding `AppImpl` manager instance.
These manager classes have `createChild()` methods like the `Registry`, which in fact just instantiate a new manager instance with a child of their own `Registry`.

## AppImpl Traces

The `AppImpl` instance maintains information about what traces are currently loaded and being loaded.
However, the API exposes only queries for loading state and tracking of the currently active trace:

- `isTraceLoading(source: TraceSource): boolean` answers whether a trace is currently being loaded from some source
- `readonly trace: TraceImpl | undefined` is the currently active trace.
What it means for a trace to be the active trace is application-specific, although there are still dependencies on it within the UI.
For Perfetto UI, it is the trace that is currently presented to the user (only one trace)
- `setActiveTrace(traceImpl: TraceImpl): void` assigns what is the currently active trace
- `closeTrace(trace: Trace): void` closes a trace if it is open and removes all bookkeeping about it from the `AppImpl`
- `readonly onActiveTraceChanged: Evt<Trace | undefined>` is an event source notifying when the active trace changes

The `AppContext` class has a `multiTraceEnabled: boolean` property that lets embedding applications turn on the support for `AppImpl` maintaining multiple open traces.
When this property is `false`, which it is in the Perfetto UI application, then the current trace is always closed when a new trace is loaded.

> _When Perfetto UI itself supports multiple simultaneous open traces, perhaps this would be recast as a_ feature flag _instead of a property of the context object._

### Dependencies on the Active Trace

There are several occurrences in the Perfetto UI where the currently active trace is accessed from the `AppImpl.instance`.
Some of these are fall-backs for when a `Trace` is not specified as the `App` for a component to use.
Mostly these support the Perfetto UI app as is; embedder applications that present multiple traces simultaneously are expected to be diligent about supplying the appropriate trace to these components.

Other occurrences are in code that intentionally operates on whatever happens to be the current trace.
For example, the Error Dialog assumes that an error would have occurred in the active trace.

The remaining category are cases where it is expected that embedding applications would not be using the component in question anyways, such as the plugins page or the trace URL handler.
If these assumptions prove to be invalid then some future refactorings may be warranted.
