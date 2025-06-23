# Trace Analysis Overview

This page is the entrypoint to the world of trace analysis with Perfetto. It
provides an overview of the different tools and concepts you can use to extract
meaningful information from traces, guiding you from interactive exploration to
large-scale automated analysis.

## Why Trace Processing?

Events in a trace are optimized for fast, low-overhead recording. Therefore,
traces need significant data processing to extract meaningful information from
them. This is compounded by the number of legacy formats which are still in use
and need to be supported in trace analysis tools.

## The Trace Processor: The Heart of Perfetto Analysis

At the heart of all trace analysis in Perfetto is the **Trace Processor**, a C++
library that solves this complexity. It does the heavy lifting of parsing,
structuring, and querying trace data.

The Trace Processor abstracts away the underlying trace format by:

- **Parsing traces**: Ingesting a wide variety of trace formats, including
  Perfetto, ftrace, and Chrome JSON.
- **Structuring data**: Massaging the raw trace data into a structured format.
- **Exposing a query interface**: Providing a PerfettoSQL interface for querying
  the structured data.
- **Bundling the standard library**: Including the PerfettoSQL standard library
  for out-of-the-box analysis.

## The Trace Analysis Workflow

Perfetto offers a flexible set of tools that build on each other to support
different analysis needs. The typical workflow progresses from broad,
interactive exploration to narrow, automated analysis.

1.  **Interactive Exploration**: Start by exploring your trace interactively
    using the `trace_processor` shell. This is ideal for ad-hoc investigations,
    debugging, and getting a feel for the data in your trace.

2.  **Programmatic Analysis**: Once you have a better understanding of your
    trace, you can automate your queries and build more complex analysis
    pipelines using the Trace Processor libraries for Python and C++.

3.  **Large-Scale Analysis**: For building robust, automated analysis
    pipelines, Trace Summarization is the recommended approach. It allows you to
    define a stable, structured output for your analysis, making it perfect for
    performance monitoring and regression detection at scale.

## Getting Started with Trace Analysis

You can interact with the Trace Processor in several ways, depending on your
needs.

### Interactive Analysis: The `trace_processor` Shell

For quick, interactive exploration, the `trace_processor` shell is the best
starting point. It's a command-line tool that lets you load a trace and query it
using SQL.

- **[Trace Processor (C++)](trace-processor.md)**: Learn how to use the
  interactive shell and the underlying C++ library.

### Programmatic Analysis: Python and C++

For more complex or automated analysis, you can use the Trace Processor's
libraries.

- **[Trace Processor (Python)](trace-processor-python.md)**: Leverage the Python
  API to combine trace analysis with the rich data science and visualization
  ecosystem.
- **[Trace Processor (C++)](trace-processor.md)**: Integrate trace analysis
  directly into your C++ applications for high-performance, low-level access.

### The Language of Analysis: PerfettoSQL

**PerfettoSQL** is the foundation of trace analysis in Perfetto. It is a dialect
of SQL that allows you to query the contents of your traces as if they were a
database.

- **[PerfettoSQL Syntax](perfetto-sql-syntax.md)**: Learn about the SQL syntax
  supported by Perfetto, including special features for creating functions,
  tables, and views.
- **[Standard Library](stdlib-docs.autogen)**: Explore the rich set of modules
  available in the standard library for analyzing common scenarios like CPU
  usage, memory, and power.

### Large-Scale Analysis: Trace Summarization

**Trace Summarization** builds on top of the PerfettoSQL language, bridging the
gap between ad-hoc SQL analysis and the world of structured, automated data
extraction. It is the key to turning your interactive SQL queries into a
reliable source of structured data for performance monitoring and regression
detection.

- **[Trace Summarization](trace-summary.md)**: Learn how to define and run
  summaries to generate consistent, structured protobuf outputs from your
  traces.
