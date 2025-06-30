# Trace Analysis Overview

This page is the entrypoint to the world of trace analysis with Perfetto. It
provides an overview of the different tools and concepts you can use to extract
meaningful information from traces, guiding you from interactive exploration to
large-scale automated analysis.

## The Challenge: Making Sense of Raw Traces

Events in a trace are optimized for fast, low-overhead recording. Therefore,
traces need significant data processing to extract meaningful information from
them. This is compounded by the number of legacy formats which are still in use
and need to be supported in trace analysis tools.

## The Solution: The Trace Processor and PerfettoSQL

At the heart of all trace analysis in Perfetto is the **Trace Processor**, a C++
library that solves this complexity. It does the heavy lifting of parsing,
structuring, and querying trace data.

The Trace Processor abstracts away the underlying trace format and exposes the
data through **PerfettoSQL**, a dialect of SQL that allows you to query the
contents of your traces as if they were a database.

The Trace Processor is responsible for:

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
    using the Perfetto UI or the `trace_processor` shell. This is ideal for
    ad-hoc investigations, debugging, and getting a feel for the data in your
    trace.

2.  **Programmatic Analysis**: Once you have a better understanding of your
    trace, you can automate your queries and build more complex analysis
    pipelines using the Trace Processor libraries for Python and C++.

3.  **Large-Scale Analysis**: For building robust, automated analysis pipelines,
    Trace Summarization is the recommended approach. It allows you to define a
    stable, structured output for your analysis, making it perfect for
    performance monitoring and regression detection at scale.

## Where to Go Next

### Learn the Language: PerfettoSQL

Before diving into the tools, it's helpful to have a foundational understanding
of PerfettoSQL.

- **[Getting Started with PerfettoSQL](perfetto-sql-getting-started.md)**: Learn
  the core concepts of PerfettoSQL and how to write queries.
- **[PerfettoSQL Syntax](perfetto-sql-syntax.md)**: Learn about the SQL syntax
  supported by Perfetto, including special features for creating functions,
  tables, and views.
- **[Standard Library](stdlib-docs.autogen)**: Explore the rich set of modules
  available in the standard library for analyzing common scenarios like CPU
  usage, memory, and power.

### Explore the Tools

Once you're comfortable with the basics of PerfettoSQL, you can explore the
different ways to use the Trace Processor.

- **[Trace Processor (C++)](trace-processor.md)**: Learn how to use the
  interactive shell and the underlying C++ library.
- **[Trace Processor (Python)](trace-processor-python.md)**: Leverage the Python
  API to combine trace analysis with the rich data science and visualization
  ecosystem.

### Automate Your Analysis

For large-scale or automated analysis, Trace Summarization is the recommended
approach.

- **[Trace Summarization](trace-summary.md)**: Learn how to define and run
  summaries to generate consistent, structured protobuf outputs from your
  traces.
