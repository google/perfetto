# Perfetto UI Server Extensions

**Authors:** @LalitMaganti

**Status:** Adopted

## Summary

This RFC proposes a system for distributing Perfetto UI extensions (macros, SQL
modules, proto descriptors) and integrating with external services via standard
HTTP(S) endpoints. Extension servers are optional and never load-bearing - the
UI remains fully functional without them for local trace analysis.

## Problem

Perfetto has several pain points around extensibility:

1. **UI Extensions are hardcoded**: The `is_internal_user_script_loader.ts` file
   contains hardcoded URLs to internal Google resources. There's no standard way
   for companies or individuals to distribute UI extensions.

2. **No sharing mechanism**: Users want to share custom macros and SQL modules
   within their team without everyone manually copy-pasting JSON
   ([#3085](https://github.com/google/perfetto/issues/3085)).

3. **Symbolization is complex**: Requires magic environment variables, manual
   symbol paths, etc. No unified way to query symbol servers.

4. **No ACL story**: We don't want to build and maintain our own
   authentication/authorization system.

5. **Local filesystem access problem**: Browser-based UI cannot directly access
   local filesystem for symbols, mappings, or extensions.

## Terminology

- **Extension Server**: An HTTP(S) endpoint that serves Perfetto UI extensions.
  Can be a static file host (GitHub, GCS, S3) or a dynamic server (corporate
  infrastructure).
- **Extension**: Declarative content that extends the Perfetto UI without code
  execution. Three types:
  - **Macros**: Named sequences of UI commands
  - **SQL Modules**: Reusable SQL code (tables, views, functions)
  - **Proto Descriptors**: Type definitions for custom protobuf messages
- **Module**: A named collection of extensions (e.g., `android`, `chrome`,
  `default`). Allows users to selectively load relevant extension sets.

## Decision

Users can optionally configure HTTP(S) endpoints (extension servers) that serve
Perfetto UI extensions. The browser UI queries configured servers and aggregates
extensions from all enabled servers/modules.

**Extension servers are optional and never load-bearing.** The UI remains fully
functional without any servers configured - users can always perform local trace
analysis. Servers only provide optional enhancements like team-shared macros or
symbol resolution.

## Scope

**In Scope:**

- Browser UI server configuration and loading
- Remote servers (HTTPS, GitHub, GCS, S3)
- UI extensions: macros, SQL modules, proto descriptors
- Module-based organization and filtering
- Manual server management via Settings (Extension Server Store/one-click
  install are post-MVP followups)

**Out of Scope (Separate RFCs):**

- **Authentication** (RFC-0006: Extension Server Authentication)
- **Symbolization and deobfuscation implementation** (separate RFC will cover
  browser integration, caching, mode selection, etc.)
- CLI tools (`traceconv`, standalone scripts)
- Local filesystem access via `trace_processor --httpd`
- Command-line configuration files

**Note:** Symbolization and deobfuscation endpoints are mentioned for
completeness and to establish the overall server architecture, but all
implementation details are deferred to a separate RFC.

## Key Principles

1. **Standard protocols** - Use HTTP/HTTPS, not custom schemes
2. **Leverage existing systems** - Use server's existing auth (GitHub, GCS IAM,
   corporate SSO), not custom Perfetto ACL
3. **Simple discovery** - Required manifest endpoint (`/manifest`) provides
   server metadata, supported features, and module list
4. **Safe extensions** - Only declarative extensions (macros, SQL, proto
   descriptors), no JavaScript code execution

## Architecture

### Extension Server Configuration

Users configure extension servers in Settings → Extension Servers. Configuration
stored in localStorage using existing Perfetto settings system. Credentials
stored separately in localStorage.

**Two ways to add servers in MVP:**

1. **User-configured** - Manually add server URL in Settings
2. **Installation-configured** - Packaged with the UI deployment (e.g., Google's
   internal server auto-added for Googlers)

Extension Server Store entries and URL-parameter-based one-click install remain
goals for a V1 followup (separate RFC covering catalog UX + distribution) and
are tracked in Phase 2 future work.

Installation-configured servers (like Google's internal server) are added on
first load with the `default` module selected. Other modules remain opt-in via
Settings so users explicitly choose which content sets to enable.

**Important:** The Perfetto UI is a static client with no backend. Each UI
deployment (e.g., ui.perfetto.dev) can package default servers into the client
bundle, but cannot remotely provision servers or credentials into existing user
profiles. This is intentional - it follows from the "static client + no backend"
architecture. Organizations needing centralized provisioning must fork/deploy
their own UI bundle with the desired servers pre-configured.

Extension servers come in two types, configured via a discriminated union:

- **GitHub**: Configured with `repo` (e.g., `"owner/repo"`), `ref` (branch or
  tag), and optional `path` (subdirectory). The UI constructs the fetch URL
  internally, using `raw.githubusercontent.com` for unauthenticated requests or
  the GitHub API for authenticated requests (to support private repos).
- **HTTPS**: Configured with a direct URL. The URL is automatically normalized
  to use `https://` if no scheme is provided. `http://` URLs are rejected —
  browsers don't allow mixed-content fetch.

Each server also stores: `enabledModules` (array of module IDs selected by the
user), `enabled` (on/off toggle), `auth` (authentication configuration), and
`locked` (whether the server can be edited/deleted by the user — used for
embedder-configured servers).

### Standard Endpoints

Extension servers implement these HTTP(S) endpoints:

```
# Server metadata (required)
{base_url}/manifest                               (GET)

# UI Extensions (module-scoped, optional based on features)
{base_url}/modules/{module}/macros                (GET)
{base_url}/modules/{module}/sql_modules           (GET)
{base_url}/modules/{module}/proto_descriptors     (GET)

# Symbolization (optional - details in separate RFC)
{base_url}/symbolize/frames                       (POST)
{base_url}/symbolize/elf/{build_id}               (GET)

# Deobfuscation (optional - details in separate RFC)
{base_url}/deobfuscate/symbols                    (POST)
{base_url}/deobfuscate/mapping/{id}               (GET)
```

**Manifest Endpoint:**

The `manifest` endpoint returns server metadata, features, and module list:

```json
{
  "name": "Google Internal Extensions",
  "namespace": "com.google",
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

**Fields:**
- `name` (required): Human-readable server name shown in Settings and in
  command palette source chips.
- `namespace` (required): Unique identifier for this server, following reverse
  domain notation (e.g., `"com.google"`, `"dev.perfetto"`). Used to prevent
  naming conflicts when multiple extension servers are configured:
  - All SQL module names must start with the namespace (e.g., SQL modules from
    a server with namespace `"com.google"` must be named `"com.google.startup"`,
    `"com.google.memory"`, etc.)
  - All macro IDs must start with the namespace (e.g.,
    `"com.google.StartupAnalysis"`)
  - The UI validates these constraints and rejects extensions that don't follow
    the namespacing convention.
- `features` (required): Array of feature objects, each with a `name` field.
  Valid feature names: `"macros"`, `"sql_modules"`, `"proto_descriptors"`. The
  UI only fetches endpoints for declared features - if a feature is not listed,
  the corresponding endpoint is not queried.
- `modules` (required): Array of module objects, each with:
  - `id` (required): Identifier used in URL paths (`/modules/{id}/macros`) and
    in the `enabledModules` setting array.
  - `name` (required): Human-readable display name shown in the UI (module
    selection, command palette source chips, etc.).
  Use `[{"id": "default", "name": "Default"}]` for single-module servers.

**Extension Response Formats:**

The extension endpoints return JSON with the following structures:

`/modules/{module}/macros`:
```json
{
  "macros": [
    {
      "id": "com.google.StartupAnalysis",
      "name": "Startup Analysis",
      "run": [
        {"id": "dev.perfetto.RunQuery", "args": ["SELECT 1"]}
      ]
    }
  ]
}
```

Note: The macro `id` must start with the server's namespace (e.g.,
`com.google.`). The `name` is human-readable and shown in the command palette.
The `run` field is an array of command invocations, each with a command `id` and
`args` array.

`/modules/{module}/sql_modules`:
```json
{
  "sql_modules": [
    {"name": "com.google.startup", "sql": "CREATE PERFETTO TABLE..."},
    {"name": "com.google.memory", "sql": "CREATE PERFETTO FUNCTION..."}
  ]
}
```

Note: Each SQL module `name` must start with the server's namespace (e.g.,
`com.google.`).

`/modules/{module}/proto_descriptors`:
```json
{
  "proto_descriptors": [
    "base64-encoded-descriptor-1",
    "base64-encoded-descriptor-2"
  ]
}
```

### Request Strategy

The UI uses different request patterns for different endpoint types:

- **UI extensions (macros, SQL modules, proto descriptors):** Queries all
  enabled server/module combinations and aggregates responses. Partial failures
  are acceptable - failed servers are skipped and logged.
- **Symbolization/deobfuscation:** Try-until-success pattern (first successful
  response ends the search).

Requests use fail-fast timeouts (5-30s depending on endpoint type), respect HTTP 429/503 rate limiting, and log failures to console.

### What These Endpoints Provide

**UI Extensions:**

- **Macros** (`/macros`) - Named sequences of UI commands that users invoke via
  command palette. Example: "Android Startup Analysis" macro that pins specific
  tracks and runs startup queries.

- **SQL Modules** (`/sql_modules`) - Reusable SQL code that creates tables,
  views, or functions. Example: `android.startup` module with common startup
  analysis queries.

- **Proto Descriptors** (`/proto_descriptors`) - Type definitions for custom
  protobuf messages in traces. Example: Android system_server proto definitions.

All extensions are declarative and execute locally - no trace data sent to
extension servers.

**Symbolization & Deobfuscation:**

- **Symbolization** (`/symbolize/*`) - Convert addresses to function names,
  files, line numbers
- **Deobfuscation** (`/deobfuscate/*`) - Convert obfuscated names to original
  names

Details for these endpoints deferred to separate RFC.

### Module-Based Organization

**Why modules exist:**

In large organizations, different teams build different extensions:

- Android team creates startup analysis macros
- Chrome team creates rendering performance macros
- Infrastructure team creates general profiling utilities

Without modules, all extensions from a server would load for everyone, creating
noise. Modules let users choose which sets of extensions they want.

**How modules work:**

- Server's `/manifest` endpoint lists available modules and features:
  `{"name": "...", "namespace": "...", "features": [{...}], "modules": [{...}]}`
- Each module has an `id` (used in URL paths and settings) and a `name`
  (human-readable label shown in the UI)
- When adding server, UI fetches manifest and shows module selection in Settings
- The `default` module (if available) is automatically selected when manifest is
  fetched. Other modules remain unselected so users can opt in explicitly.
- UI loads extensions only from selected modules
- Each module's extensions in separate paths: `/modules/{module_id}/macros`,
  `/modules/{module_id}/sql_modules`, etc.
- UI only fetches endpoints for features declared in the manifest

**Implementation notes:**

- **Flat structure only**: Use dashes for hierarchy (e.g., `android-frameworks`)
- **No module-level ACL**: Access control at server level (GitHub repo
  permissions, GCS IAM, etc.)

### Resource Naming and Conflicts

**Namespace Enforcement**

All resources from an extension server must be prefixed with the server's
`namespace` (from the manifest). The UI validates this constraint and rejects
resources that don't follow the convention:

- Macro IDs must start with `{namespace}.` (e.g., `com.google.StartupAnalysis`)
- SQL module names must start with `{namespace}.` (e.g., `com.google.startup`)

This prevents naming conflicts when multiple extension servers are configured.
Each organization controls their own namespace, ensuring resources from
different servers never collide.

**Proto Descriptors**

Proto descriptors are not namespaced since they define protobuf message types
which have their own package-based namespacing in the proto schema itself.

### Extension Lifecycle

**Extensions loaded ONCE on UI startup. Configuration changes require page
reload.**

Load flow:

1. UI initializes
2. Load installation-configured servers (from UI bundle)
3. Load user-configured servers (from localStorage)
4. Fetch extensions from all enabled servers/modules in deterministic order
   (installation-configured servers first, then user-configured servers
   alphabetically)
5. Register macros, SQL modules, proto descriptors
6. UI ready

**Configuration changes:**

- Add/remove server or change module selection → Settings shows "Reload page to
  apply changes"
- User clicks **[Reload Page]** button
- No automatic reload, no hot-reload logic

**Caching:**

- Extensions kept in memory during session
- UI respects standard HTTP caching semantics (server-set `Cache-Control`,
  `ETag`, etc.)
- Servers control cache behavior via HTTP headers
- No localStorage cache for extensions in MVP
- **Future consideration (V2):** Service worker caching for offline support.
  This would allow extensions to work offline after initial load. Requires
  separate design for cache invalidation and version management.

**Failure handling:**

- If all servers fail (timeout, network error, auth failure, etc.), the UI still
  initializes successfully with an empty extension set
- Console logs show which servers failed and why
- UI remains fully functional for local trace analysis

### Authentication

**Note:** Authentication implementation is covered in **RFC-0006: Extension
Server Authentication**. This section provides a high-level overview for context.

Extension servers support standard authentication mechanisms:

- **Public access**: Servers can be publicly accessible (no auth required)
- **GitHub PAT**: Personal Access Token for private GitHub repos
- **HTTPS Basic auth**: Username/password
- **HTTPS API key**: Bearer token, X-API-Key header, or custom header
- **HTTPS SSO**: Cookie-based SSO with automatic iframe-based refresh on 403

Authentication is configured per-server in the Settings UI when adding or
editing a server. Credentials are stored in localStorage alongside the server
configuration. Secret fields (PAT, passwords, API keys) are marked with
metadata so they can be stripped when sharing server configurations via URL.

**SSO flow:** When a request returns 403 and the server uses SSO auth, the UI
loads the server's base URL in a hidden iframe to refresh session cookies, then
retries the request once.

## Examples

### GitHub Static Hosting

**Repo structure:**

```
acme-corp/perfetto-resources/
├── manifest
├── modules/
│   ├── default/
│   │   ├── macros
│   │   ├── sql_modules
│   │   └── proto_descriptors
│   ├── android/
│   │   ├── macros
│   │   ├── sql_modules
│   │   └── proto_descriptors
│   └── chrome/
│       ├── macros
│       └── sql_modules
```

**File: `manifest`**

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

**File: `modules/default/macros`**

```json
{
  "macros": [
    {
      "id": "com.acme.StartupAnalysis",
      "name": "Startup Analysis",
      "run": [{"id": "dev.perfetto.RunQuery", "args": ["SELECT 1"]}]
    }
  ]
}
```

**File: `modules/default/sql_modules`**

```json
{
  "sql_modules": [
    {"name": "com.acme.startup", "sql": "CREATE PERFETTO TABLE..."}
  ]
}
```

**Configuration:**

```
Type: GitHub
Repo: acme-corp/perfetto-resources
Ref: main
Path: /
Modules: default, android, chrome
Auth: None (public repo) or PAT (private repo)
```

### Corporate Server

```python
@app.route('/manifest')
def get_manifest():
    return jsonify({
        "name": "Acme Corp Extensions",
        "namespace": "com.acme",
        "features": [{"name": "macros"}, {"name": "sql_modules"}],
        "modules": [
            {"id": "default", "name": "Default"},
            {"id": "android", "name": "Android"},
            {"id": "chrome", "name": "Chrome"},
            {"id": "infra", "name": "Infrastructure"},
        ]
    })

@app.route('/modules/<module>/macros')
def get_module_macros(module):
    return send_file(f'/data/modules/{module}/macros')

@app.route('/modules/<module>/sql_modules')
def get_module_sql_modules(module):
    return send_file(f'/data/modules/{module}/sql_modules')
```

**Configuration:**

```
Type: HTTPS
URL: https://perfetto.corp.example.com
Modules: default, android
Auth: SSO (cookie-based)
```

## Security Considerations

### Extension Safety

All extensions are **declarative and safe** - no code execution:

1. **Macros**: UI command sequences, cannot execute JavaScript
2. **SQL Modules**: SQL text in trace processor sandbox, no browser access
3. **Proto Descriptors**: Binary type definitions (data only)

JavaScript plugins are NOT supported. Future executable plugins would require
separate RFC with sandboxing.

### Server Integrity Expectations

Perfetto does not attempt to verify that server-hosted JSON or descriptor
payloads are trusted. Server operators are solely responsible for reviewing,
signing, or gating their own content before distribution to guard against
compromised repos or buckets. Each organization can apply its own signing or
review pipeline (e.g., proxy servers, checksum validation, internal CI) without
upstream coordination, and this RFC does not prescribe a canonical integrity
mechanism.

### Data Sensitivity

**For UI extensions:** NO trace data sent to extension servers. Extensions
downloaded once and executed locally.

**Consent model:** Adding an extension server = implicit consent to load
extensions from it.

**For symbolization (future RFC):** Will send trace data (addresses, build IDs).
Future RFC will specify consent flow and warnings.

### Credential Storage

- Stored in localStorage in plaintext
- Protected by browser same-origin policy
- Separate from Perfetto settings (not exported/synced)
- User can view/delete in Settings
- Follows industry practice (GitHub, VS Code, etc.)
- Rationale: Perfetto runs entirely in-browser without privileged services, so
  leveraging the browser's same-origin protections for credentials is an
  intentional design choice rather than a placeholder for future keychain work.

### CORS Requirements

All HTTPS extension servers MUST set CORS headers:

```
Access-Control-Allow-Origin: https://ui.perfetto.dev
Access-Control-Allow-Methods: GET, POST
Access-Control-Allow-Headers: Authorization, Content-Type
```

**CORS failure handling:**

- CORS violations manifest as browser network errors (typically "CORS policy"
  errors in the console)
- Logged to console with server URL and endpoint
- Server/module combination is skipped (same as other network failures)
- Future: Settings UI could detect CORS issues and suggest configuration fixes

## Alternatives Considered

### Custom `perfetto://` URI Scheme

**Rejected** - Adds parsing complexity for minimal benefit. Standard HTTP(S)
URLs work fine.

### Perfetto-Hosted Registry

**Rejected** - Single point of failure, doesn't solve private/corporate use
case. Decentralized model is better.

### Build Custom ACL System

**Hard Rejected** - Huge implementation burden, users need another account.
Leverage existing systems (GitHub, GCS, corporate SSO).

## Implementation

### Phase 1: Core Extension Server Infrastructure (Done)

- Extension server config schema (localStorage via Settings)
- Server fan-out orchestration (aggregate UI extensions from all enabled
  servers/modules)
- Manifest fetching and parsing
- Auth header injection (PAT, Basic, API key, SSO)
- Error handling, logging, and error modal
- GitHub and HTTPS server types
- Add/edit/delete/share server UI in Settings
- Module discovery and selection UI
- Extension endpoints (macros, SQL modules, proto descriptors)
- Embedder-configured server bootstrap (auto-added, `default` module selected,
  locked)
- URL-based server sharing (`?addServer=<base64>`)
- SSO cookie refresh via hidden iframe

### Phase 2: Post-MVP Enhancements

- Extension Server Store (curated catalog + Settings shortcuts; cover in
  dedicated RFC)
- Replace `is_internal_user_script_loader.ts`
- Service worker caching for offline support
- Command palette source chips showing server/module origin

### Phase 3: Local HTTP Accelerator (Future)

- Extend `trace_processor --httpd` to serve extension server endpoints
- Local filesystem discovery

### Phase 4: Symbolization & Deobfuscation (Separate RFC)

- Endpoint specifications
- Browser integration
- Caching strategies

### Migration Strategy

- Replacing the legacy `is_internal_user_script_loader.ts` assets with extension
  servers happens centrally: hosted Perfetto deployments will update their
  installation-configured server list and the UI automatically begins fetching
  from the new endpoints on reload without user action.
- Existing inline macros/SQL bundles ship until the extension server backend is
  ready, so the switch is transparent to users aside from the new Settings
  surface.
- Forked or self-hosted deployments can follow the same pattern; they only need
  to update the packaged server defaults before rolling the refreshed UI bundle.

### Prerequisites

1. Register Perfetto GitHub App
2. Set up internal Google extension server
3. Create example GitHub repo with test extensions

## Design Decisions & Non-Goals

This section documents intentional scope boundaries and architectural choices to
prevent future confusion about what is explicitly out of scope.

**SQL Module Safety:**

- SQL modules execute in trace processor's existing sandbox with current
  resource limits and protections. No additional sandboxing is introduced by
  this system. This matches how SQL modules work today.

**Extension Versioning:**

- No version negotiation or compatibility checks between UI and extensions.
  Extensions are always fetched fresh on reload. This matches how normal
  websites work - users get the latest version on page load. Extension servers
  are responsible for ensuring extensions remain compatible or coordinating
  updates with users. Version pinning, rollback switches, or multi-version
  compatibility matrices would require backend orchestration and are
  intentionally excluded.

**Proto Descriptor Validation:**

- Proto descriptors are validated by trace processor using existing protobuf
  parsing. Malformed descriptors are rejected and logged. No additional
  validation layer is introduced.

**Extension Update Notifications:**

- No mechanism to notify users of extension updates. Users reload the page to
  get latest extensions. Providers should ensure extensions are rolled out
  safely, same as any distributed system. This is not different from how web
  applications handle updates.

**Resource Quotas:**

- No hard limits on number of servers, extensions per server, or response sizes.
  The user explicitly adds servers, establishing an implicit trust relationship -
  malicious or misconfigured servers are not an expected attack vector. In
  practice, these limits are not expected to be reached. If performance issues
  arise, limits can be added later.

**Namespace Protection:**

- Overriding built-in Perfetto functionality (commands, SQL modules, etc.) is
  undefined behavior. Extension servers should use domain-specific names to
  avoid conflicts with current or future built-in features.

**Macro Validation:**

- Macros that reference nonexistent command IDs or use invalid arguments will
  fail at runtime when invoked. No upfront validation is performed. This matches
  how ad-hoc macros work today - the macro simply fails to execute correctly if
  it's malformed or incompatible with the UI version.

**Cross-Server Dependencies:**

- Extensions cannot declare dependencies on other extensions, especially across
  servers. Loading order is deterministic but dependencies are not enforced. If
  an extension relies on another extension being present, this is undefined
  behavior and will fail at runtime. Users should configure extensions from a
  single server if dependencies are required.

**Extension Server Deployment Ownership:**

- Server hosting, rollout cadence, and monitoring are intentionally delegated to
  each organization. The RFC does not prescribe SLAs, scaling rules, or shared
  infrastructure so that internal and external deployments can evolve
  independently.

**Offline Distribution:**

- Shipping offline or bundled extension archives is out of scope for this
  design. Teams that require offline readiness can fork or self-host the
  Perfetto UI and manage their own distribution channel without upstream
  protocol changes.

**Telemetry Scope:**

- Extension server usage or health telemetry is not part of the MVP. Future work
  can add aggregate reporting once core plumbing is proven, but client behavior
  today is limited to console logging.
- Post-MVP telemetry requirements will arrive through a follow-up RFC that
  defines event schemas, privacy review, and rollout expectations before any
  data collection is enabled. Until that happens, console logging remains the
  only supported observability surface.
