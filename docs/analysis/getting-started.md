# Trace Analysis Overview

This page is the entrypoint to the world of trace analysis with Perfetto. It
provides an overview of the different tools and concepts you can use to extract
meaningful information from traces, guiding you from interactive exploration to
large-scale automated analysis.

## The Trace Analysis Workflow

Perfetto offers a flexible set of tools that build on each other to support
different analysis needs. The typical workflow progresses from broad,
interactive exploration to narrow, automated analysis.

1.  **Interactive Exploration with PerfettoSQL**: Start by exploring your trace
    interactively using PerfettoSQL. This is ideal for ad-hoc investigations,
    debugging, and getting a feel for the data in your trace.

2.  **Programmatic Analysis with Trace Processor**: Once you have a better
    understanding of your trace, you can automate your queries and build more
    complex analysis pipelines using the Trace Processor libraries for Python
    and C++.

3.  **Bridging to Structured Data with Trace Summarization**: For building
    robust, automated analysis pipelines, Trace Summarization is the recommended
    approach. It allows you to define a stable, structured output for your
    analysis, making it perfect for performance monitoring and regression
    detection at scale.

## Core Components

### Trace Processor

At the heart of all trace analysis in Perfetto is the **Trace Processor**, a
powerful library that does the heavy lifting of parsing, structuring, and
querying trace data. It is responsible for:

-   **Parsing traces**: Ingesting a wide variety of trace formats.
-   **Structuring data**: Massaging the raw trace data into a structured format.
-   **Exposing a query interface**: Providing a PerfettoSQL interface for
    querying the structured data.
-   **Bundling the standard library**: Including the PerfettoSQL standard
    library for out-of-the-box analysis.

You can interact with the Trace Processor through its C++ and Python libraries:

-   **[Trace Processor (C++)](trace-processor.md)**: Integrate trace analysis
    directly into your C++ applications for high-performance, low-level access.
-   **[Trace Processor (Python)](trace-processor-python.md)**: Leverage the
    Python API to combine trace analysis with the rich data science and
    visualization ecosystem.

### PerfettoSQL

**PerfettoSQL** is the foundation of trace analysis in Perfetto. It is a dialect
of SQL that allows you to query the contents of your traces as if they were a
database.

-   **[PerfettoSQL Syntax](perfetto-sql-syntax.md)**: Learn about the SQL syntax
    supported by Perfetto, including special features for creating functions,
    tables, and views.
-   **[Standard Library](stdlib-docs.autogen)**: Explore the rich set of modules
    available in the standard library for analyzing common scenarios like CPU
    usage, memory, and power.

### Trace Summarization

**Trace Summarization** builds on top of the PerfettoSQL language, bridging the
gap between ad-hoc SQL analysis and the world of structured, automated data
extraction. It allows you to define a stable schema for your data, making it
easy to build reliable tooling for performance monitoring and regression
detection. It is the key to turning your interactive SQL queries into a
reliable source of structured data.

-   **[Trace Summarization](trace-summary.md)**: Learn how to define and run
    summaries to generate consistent, structured protobuf outputs from your
    traces.
