# Trace Processor Warm Sessions

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

A caller that runs many queries against one trace re-parses it on every
`tp query <trace>` invocation. Large traces take seconds to minutes to parse, so
the common AI-agent pattern of repeated one-shot queries is slow.

`tp server http` and `tp server stdio` already hold a trace warm, but two things
are missing for an agent driving the CLI:

1. No self-managing lifetime. A server runs until killed, so an agent that forgets
   to stop one, or whose session ends, leaves it resident.
2. No name-based discovery. http needs the client to know a port; stdio needs the
   client to hold the pipe for the whole session.

This RFC adds both. Single trace per server; multi-trace is a separate API.

## Decision

Three additions:

1. A self-managing lifecycle (idle-timeout plus owner-aware reaping), wired into
   the `http` and `unix` server modes.
2. A `unix` transport addressed by name, plus a `--remote` flag on every
   trace-consuming subcommand (`query`, `metrics`, `summarize`, `interactive`), so
   an agent can start a warm session in the background and run any of them against
   it by name.
3. A `tp ctl` management command, starting with `tp ctl kill-server
   <name|host:port>` to stop a running server by address.

Transport and lifecycle are independent: the lifecycle flags apply to `http` as
well as `unix`. `unix` is added because it makes name-based discovery clean: a name
maps to a fixed socket path, whereas a TCP port cannot be derived from a name.

## Design

### Lifecycle (transport-agnostic)

Applies to the `http` and `unix` modes (stdio is bound to its pipe). A server
starts only when explicitly created (no auto-spawn) and is a single process: the
query engine exits with the server, with no detached child that can outlive it.
Two flags, both defaulting to `auto`:

* `--idle-timeout auto|<dur>`: idle duration before the server exits. `auto` is 30
  minutes. `--daemonize` uses the same default; an explicit value, including
  disabling the timeout, is honored, with no enforced minimum.
* `--idle-start auto|orphaned|last-query`: when the idle clock applies. With `auto`
  the server does not idle-reap while it has a live owning parent, since the parent
  is expected to reap it. Once orphaned (the parent exits, or it was started with
  `--daemonize`) it arms the clock. An owned foreground server is therefore not
  reaped during a long pause, and an orphaned one, including one backgrounded with
  `nohup` or `&`, still exits when idle.

### `unix` transport and naming

```
tp server unix <trace> [--name N | --path PATH] [--daemonize] [--idle-timeout ...] [--idle-start ...]   # new
tp server http <trace> [--port P]               [--idle-timeout ...] [--idle-start ...]   # flags now apply here too
tp query     --remote <addr> 'SELECT ...'    # --remote (new) goes on every trace-consuming
tp metrics   --remote <addr> <metric...>     #   subcommand: query, metrics, summarize, interactive.
tp summarize --remote <addr> ...             #   <addr> = name | socket-path | host:port.
tp query <trace> 'SELECT ...'                # existing one-shot (no --remote) unchanged
tp ctl kill-server <name|socket-path|host:port>   # new: stop a running server by address
```

A session is a running server addressed by name. There is no registry; the name
maps to a socket path by convention:

```
<name> -> $XDG_RUNTIME_DIR/perfetto/<name>.sock   (Linux)
          %LOCALAPPDATA%\perfetto\<name>.sock      (Windows)
```

If `--name` is omitted the server generates a three-word name such as
`calm-blue-otter`. Liveness is whether the socket accepts a connection. A stale
socket from a crashed server is removed on the next bind: try to connect, and
unlink if the connection fails. There are no descriptor files and no garbage
collector.

`--path PATH` binds an explicit socket path instead of the convention path: for
sandboxed environments whose writable directory is elsewhere, or when the
convention path would exceed the `AF_UNIX` limit. The server validates the
assembled path and, if it would overflow, errors and asks for an explicit
`--path`. A client reaches such a server with `tp query --remote <path>`.

On startup the server prints a `key=value` record on stdout and human-readable
guidance on stderr:

```
pid=1234 session=calm-blue-otter socket-path=.../calm-blue-otter.sock idle-timeout=300s
```

### Agent guidance

The skill doc tells the agent to run `tp server unix <trace>` in the background and
then query it, and that the session cleans itself up. It does not mention
`--daemonize`. Testing showed that exposing the foreground-versus-daemonize choice
causes agents to start the server several times before settling. With the choice
removed, each agent backgrounds the server with whatever mechanism it has, and the
server adapts to being owned or orphaned on its own.

### Stopping a server (`tp ctl`)

`tp ctl kill-server <target>` resolves `<target>` the same way as `--remote` (a
bare name to the convention socket path, an absolute path to a socket, anything
else to `host:port`), connects to the server, and sends a shutdown over the RPC.
This works for both `unix` and `http` servers and needs no pid or signal, so it
behaves the same on every OS. Idle-timeout is still the normal way a session ends;
`tp ctl kill-server` is for stopping one early. `ctl` is the home for later
management verbs (e.g. listing live servers).

### Cross-platform

`AF_UNIX` is available on all supported platforms, including Windows since Windows
10 1803, so the naming, liveness check, and stale-socket cleanup are identical
everywhere. Only the lifecycle primitives differ per OS:

| concern | Linux | Windows |
|---|---|---|
| ownership / orphan detection | `getppid()` | open a handle to the parent and wait on it |
| child exits with server | `PR_SET_PDEATHSIG` | Job Object with `KILL_ON_JOB_CLOSE` |
| detach for `--daemonize` | `setsid` + fork | `CreateProcess(DETACHED_PROCESS)` |

### Output

Query results are JSON. SQL can be passed inline, on stdin (`-`), or from a file
(`--file`, already supported). Results stream over the existing RPC, which uses
`is_last_batch` to mark the end.

## The `--remote` flag

`--remote <addr>` makes a trace-consuming subcommand run against a running server
instead of loading a trace itself. It is added to `query`, `metrics`, `summarize`,
and `interactive`. `export` and `convert` are file-to-file operations and keep
taking a trace path; `server` starts a server rather than consuming one. The point
of a warm session is that all of these reuse it, not just `query`.

`--remote` and a positional trace are mutually exclusive: a command either loads a
trace or talks to a server.

### Address resolution

One `<addr>` syntax covers local sockets and TCP, auto-detected in order:

1. Contains `://` or a trailing `:port` -> HTTP (`host:port`, `http://host:port`).
2. Absolute path, or ends in `.sock` -> a unix socket at that path.
3. Otherwise (matches the session-name charset) -> a unix socket at the convention
   path for that name.

The forms are mostly disjoint because a session name cannot contain `:`, `/`, or
`.`. The remaining ambiguity is a bare single-label host such as `localhost`: it
has no `:` and looks like a name, so it resolves as a session, not a host. Use a
port (`localhost:9001`) for HTTP.

### Missing target

If the resolved address has no live server (idle-timed-out, never started, or wrong
name), the subcommand fails fast with a message naming the address and how to start
one. It does not hang, and does not auto-spawn a server. Agents recover by
re-creating the session and retrying; this was reliable across the tested agents.

### Naming

"Remote" is used loosely: the target is a separate server process, usually on the
same host over a unix socket. The flag is named for the client/server split, not
for being on another machine. `--server`, `--session`, and `--attach` were
considered; `--remote` reads best across `query`, `metrics`, and `summarize`.

## Validation

A prototype over the existing RPC was tested against five coding agents: claude,
codex, pi, agy, and opencode. All reused one warm session across queries, restarted
a reaped session by name when a query reported it gone, and left no process behind
under `--idle-start auto`. Agents with a background-task API (claude, agy) ran the
server in the foreground under their own management. Those without it, pi via `&`
and opencode via a detached spawn, left the server orphaned, where the idle backstop
collected it.

## Alternatives considered

### Name-addressed sessions over http instead of a `unix` transport

Pro: no new transport.

Con: a TCP port cannot be derived from a name, so the client must be told the port
out of band or via a registry, which is the discovery problem this is meant to
remove. A unix socket path is derivable from the name.

### Automatic content-addressed sessions

Auto-spawn a daemon per trace and reap it on a TTL.

Con: implicit, easy to leave large daemons resident, and the lifecycle is not
visible to the user.

### MCP server as the primary interface

Con: serves only LLM agents, not scripts, CI, or humans at a terminal. An MCP
server can instead wrap a warm session as a child it owns.

### Harness-tracked foreground with no server-side backstop

Con: pi and opencode have no background-process API, and a foreground server blocks
the turn, so they must launch it detached. A detached server escapes harness
cleanup, so a server-side idle backstop is still required.

## Open questions

* Whether `--remote` should accept explicit `unix:` / `tcp:` scheme prefixes to
  remove the bare-hostname ambiguity.
* Multi-trace and shared-cache sessions, which are out of scope here.
