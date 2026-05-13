# Extension Server Protocol Reference

This page documents the HTTP protocol that extension servers must implement. Use
this if you are building a custom extension server rather than using the
[GitHub template approach](/docs/visualization/extension-servers.md).

## Endpoints

Extension servers implement these HTTP(S) endpoints:

```
{base_url}/manifest                              GET  (required)
{base_url}/modules/{module_id}/macros            GET  (optional)
{base_url}/modules/{module_id}/sql_modules       GET  (optional)
{base_url}/modules/{module_id}/proto_descriptors GET  (optional)
```

The UI only fetches optional endpoints for features declared in the manifest.

## Manifest

**Endpoint:** `GET {base_url}/manifest`

Returns server metadata, supported features, and available modules.

```json
{
  "name": "Acme Corp Extensions",
  "namespace": "com.acme",
  "features": [
    {"name": "macros"},
    {"name": "sql_modules"},
    {"name": "proto_descriptors"}
  ],
  "modules": [
    {"id": "default", "name": "Default"},
    {"id": "android", "name": "Android"},
    {"id": "chrome", "name": "Chrome"}
  ]
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | Yes | Human-readable server name, shown in Settings and command palette source chips. |
| `namespace` | string | Yes | Unique identifier in reverse-domain notation (e.g., `com.acme`). Used to enforce naming constraints on macros and SQL modules. |
| `features` | array | Yes | Features this server supports. Each entry has a `name` field. Valid names: `macros`, `sql_modules`, `proto_descriptors`. |
| `modules` | array | Yes | Available modules. Each entry has an `id` (used in URL paths and settings) and a `name` (human-readable display name). |

For single-module servers, use `[{"id": "default", "name": "Default"}]`.

## Macros

**Endpoint:** `GET {base_url}/modules/{module_id}/macros`

Only fetched if `features` includes `{"name": "macros"}`.

```json
{
  "macros": [
    {
      "id": "com.acme.StartupAnalysis",
      "name": "Startup Analysis",
      "run": [
        {"id": "dev.perfetto.RunQuery", "args": ["SELECT 1"]},
        {"id": "dev.perfetto.PinTracksByRegex", "args": [".*CPU.*"]}
      ]
    }
  ]
}
```

### Macro fields

| Field | Type | Description |
|-------|------|-------------|
| `id` | string | Unique identifier. Must start with the server's namespace followed by `.` (e.g., `com.acme.StartupAnalysis`). |
| `name` | string | Display name shown in the command palette. |
| `run` | array | Commands to execute in sequence. Each entry has a command `id` and an `args` array. |

See the
[Commands Automation Reference](/docs/visualization/commands-automation-reference.md)
for the full list of available command IDs and their arguments.

## SQL modules

**Endpoint:** `GET {base_url}/modules/{module_id}/sql_modules`

Only fetched if `features` includes `{"name": "sql_modules"}`.

```json
{
  "sql_modules": [
    {
      "name": "com.acme.startup",
      "sql": "CREATE PERFETTO TABLE _startup_events AS SELECT ts, dur, name FROM slice WHERE name GLOB 'startup*';"
    },
    {
      "name": "com.acme.memory",
      "sql": "CREATE PERFETTO FUNCTION com_acme_rss_mb(upid INT) RETURNS FLOAT AS SELECT CAST(value AS FLOAT) / 1048576 FROM counter WHERE track_id IN (SELECT id FROM process_counter_track WHERE upid = $upid AND name = 'mem.rss') ORDER BY ts DESC LIMIT 1;"
    }
  ]
}
```

### SQL module fields

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Module name. Must start with the server's namespace followed by `.` (e.g., `com.acme.startup`). Users reference this with `INCLUDE PERFETTO MODULE com.acme.startup;`. |
| `sql` | string | SQL text. Can contain `CREATE PERFETTO TABLE`, `CREATE PERFETTO FUNCTION`, `CREATE PERFETTO VIEW`, or any valid PerfettoSQL. |

## Proto descriptors

**Endpoint:** `GET {base_url}/modules/{module_id}/proto_descriptors`

Only fetched if `features` includes `{"name": "proto_descriptors"}`.

```json
{
  "proto_descriptors": [
    "CgdteV9wcm90bxIHbXlwcm90byI...",
    "Cghhbm90aGVyEghhbm90aGVyIi..."
  ]
}
```

### Proto descriptor fields

| Field | Type | Description |
|-------|------|-------------|
| `proto_descriptors` | array of strings | Base64-encoded `FileDescriptorSet` protocol buffer messages. These allow the UI to decode custom protobuf messages in traces. |

Proto descriptors are not subject to namespace enforcement since protobuf
messages have their own package-based namespacing.

## Namespace enforcement

All macro IDs and SQL module names must start with the server's `namespace`
value from the manifest, followed by a `.`. For example, a server with namespace
`com.acme` can only serve:

- Macro IDs like `com.acme.StartupAnalysis`, `com.acme.MemoryCheck`
- SQL module names like `com.acme.startup`, `com.acme.memory.helpers`

The UI validates this and rejects extensions that violate the convention. This
prevents naming conflicts when users configure multiple extension servers.

## CORS requirements

All HTTPS extension servers must set CORS headers to allow the Perfetto UI to
make cross-origin requests:

```
Access-Control-Allow-Origin: https://ui.perfetto.dev
Access-Control-Allow-Methods: GET
Access-Control-Allow-Headers: Authorization, Content-Type
```

If your server supports multiple Perfetto UI deployments, you can reflect the
`Origin` request header instead of hardcoding a single origin.

GitHub-hosted servers do not need CORS configuration —
`raw.githubusercontent.com` and the GitHub API already set appropriate headers.

CORS failures appear as network errors in the browser console. The affected
server is skipped and other servers continue to load normally.

## Authentication headers

The Perfetto UI constructs authentication headers based on the server's
configured auth type:

| Auth type | Header |
|-----------|--------|
| `none` | No auth headers |
| `github_pat` | `Authorization: token <pat>` (via GitHub API) |
| `https_basic` | `Authorization: Basic <base64(username:password)>` |
| `https_apikey` (bearer) | `Authorization: Bearer <key>` |
| `https_apikey` (x_api_key) | `X-API-Key: <key>` |
| `https_apikey` (custom) | `<custom_header_name>: <key>` |
| `https_sso` | No header; request sent with `credentials: 'include'` |

For SSO authentication, if a request returns HTTP 403, the UI loads the server's
base URL in a hidden iframe to refresh the SSO session cookie, then retries the
request once.

## GitHub server URL construction

For GitHub-hosted extension servers, the UI constructs fetch URLs automatically:

- **Unauthenticated (public repos):** Uses `raw.githubusercontent.com` to avoid
  GitHub API rate limits.
  ```
  https://raw.githubusercontent.com/{repo}/{ref}/{path}/manifest
  ```
- **Authenticated (private repos):** Uses the GitHub Contents API.
  ```
  https://api.github.com/repos/{repo}/contents/{path}/manifest?ref={ref}
  ```
  With header: `Accept: application/vnd.github.raw+json`

## Example: minimal static server

A complete extension server can be a set of static JSON files:

```
my-extensions/
  manifest
  modules/
    default/
      macros
      sql_modules
```

Serve these with any static file server (nginx, Caddy, GCS, S3) that sets the
required CORS headers. No dynamic server logic is needed for basic use cases.

## Example: dynamic server (Python/Flask)

```python
from flask import Flask, jsonify

app = Flask(__name__)

@app.route('/manifest')
def manifest():
    return jsonify({
        "name": "My Extensions",
        "namespace": "com.example",
        "features": [{"name": "macros"}, {"name": "sql_modules"}],
        "modules": [{"id": "default", "name": "Default"}],
    })

@app.route('/modules/<module>/macros')
def macros(module):
    return jsonify({
        "macros": [
            {
                "id": "com.example.ShowLongSlices",
                "name": "Show Long Slices",
                "run": [
                    {
                        "id": "dev.perfetto.RunQueryAndShowTab",
                        "args": ["SELECT * FROM slice ORDER BY dur DESC LIMIT 20"]
                    }
                ]
            }
        ]
    })

@app.route('/modules/<module>/sql_modules')
def sql_modules(module):
    return jsonify({
        "sql_modules": [
            {"name": "com.example.helpers", "sql": "CREATE PERFETTO TABLE ...;"}
        ]
    })

@app.after_request
def add_cors(response):
    response.headers['Access-Control-Allow-Origin'] = 'https://ui.perfetto.dev'
    response.headers['Access-Control-Allow-Methods'] = 'GET'
    response.headers['Access-Control-Allow-Headers'] = 'Authorization, Content-Type'
    return response
```

## See also

- [Extension Server Setup Guide](/docs/visualization/extension-servers.md) —
  Step-by-step setup using the GitHub template
- [Extension Servers](/docs/visualization/extension-servers.md) —
  What extension servers are and how they work
- [Commands Automation Reference](/docs/visualization/commands-automation-reference.md) —
  Available command IDs for macros
