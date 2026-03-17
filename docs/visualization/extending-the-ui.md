# Extending the Perfetto UI

Perfetto offers several ways to extend and customize the UI. The right choice
depends on what you want to do and who you want to share it with.

## Which approach should I use?

```mermaid
graph TD
    Start["How do I extend the Perfetto UI?"]
    Q1{"Do you want simple things?<br>(queries, pinning tracks,<br>selecting events, etc.)"}

    Start --> Q1

    Q2{"For yourself or<br>to share with others?"}
    Q3{"Are you OK contributing<br>your changes upstream?"}

    Q1 -->|Yes| Q2
    Q1 -->|No| Q3

    Out1["Use Macros +<br>Startup Commands"]
    Out2["Use Extension Servers"]

    Q2 -->|For myself| Out1
    Q2 -->|To share| Out2

    Out3["Contribute a<br>UI Plugin Upstream"]
    Q4{"Do you work<br>at Google?"}

    Q3 -->|Yes| Out3
    Q3 -->|No| Q4

    Out4["Please speak to us"]
    Out5["Fork Perfetto &<br>maintain your own instance"]

    Q4 -->|Yes| Out4
    Q4 -->|No| Out5

    Out6["Use Plugins for<br>UI-affecting changes"]
    Out7["Use Embedder for<br>central infra<br>(analytics, branding, etc.)"]

    Out5 --> Out6
    Out5 --> Out7

    click Out1 "/docs/visualization/ui-automation" "Commands and Macros"
    click Out2 "/docs/visualization/extension-servers" "Extension Server Setup"
    click Out3 "/docs/contributing/ui-plugins" "UI Plugins"
    click Out4 "https://github.com/google/perfetto/issues" "Open an issue"
    click Out6 "/docs/contributing/ui-plugins" "UI Plugins"

    %% Styling
    style Start fill:#6b9ae8,color:#fff,stroke:none

    style Q1 fill:#ece5ff,stroke:#d0c4eb,color:#333
    style Q2 fill:#ece5ff,stroke:#d0c4eb,color:#333
    style Q3 fill:#ece5ff,stroke:#d0c4eb,color:#333
    style Q4 fill:#ece5ff,stroke:#d0c4eb,color:#333

    style Out1 fill:#36a265,color:#fff,stroke:none
    style Out2 fill:#36a265,color:#fff,stroke:none
    style Out3 fill:#36a265,color:#fff,stroke:none

    style Out4 fill:#ece5ff,stroke:#d0c4eb,color:#333
    style Out5 fill:#ece5ff,stroke:#d0c4eb,color:#333

    style Out6 fill:#ef991c,color:#fff,stroke:none
    style Out7 fill:#ef991c,color:#fff,stroke:none
```

## Commands, startup commands, and macros

**Commands** are individual UI actions (pin a track, run a query, create a debug
track). **Startup commands** run automatically every time you open a trace.
**Macros** are named sequences of commands you trigger manually from the command
palette.

These are configured locally in Settings and are the simplest way to customize
your own workflow. No server or sharing infrastructure needed.

See [Commands and Macros](/docs/visualization/ui-automation.md) for how to set
these up, and the
[Commands Automation Reference](/docs/visualization/commands-automation-reference.md)
for the full list of available commands.

## Extension servers

**Extension servers** are HTTP(S) endpoints that distribute macros, SQL modules,
and proto descriptors to the Perfetto UI. They are the recommended way for teams
to share reusable trace analysis workflows — instead of everyone copy-pasting
JSON, you host extensions on a server and anyone with access can load them.

The easiest way to get started is to fork a GitHub template repository and push
your extensions there. The Perfetto UI can load directly from GitHub repos.

See [Extension Servers](/docs/visualization/extension-servers.md) for how they
work and how to set one up.

## Plugins

**Plugins** are TypeScript modules that run inside the Perfetto UI and can add
new tracks, tabs, commands, and visualizations. Unlike macros and extension
servers (which are declarative), plugins can execute code and deeply integrate
with the UI.

If you want to contribute a plugin upstream, see
[UI Plugins](/docs/contributing/ui-plugins.md).

## Forking Perfetto

If you need changes that go beyond what plugins, macros, and extension servers
offer — such as custom branding, analytics integration, or deep infrastructure
changes — you can fork Perfetto and maintain your own instance. Within a fork,
you can use the **embedder API** for central infrastructure concerns (analytics,
branding) and **plugins** for UI-affecting changes.
