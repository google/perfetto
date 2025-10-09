# Perfetto UI Server Extensions

**Authors:** @LalitMaganti

**Status:** In Review - Addressing Feedback

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
3. **Simple discovery** - Required manifest file (`manifest.json`) provides
   server metadata and module list
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

Extension servers are configured with HTTPS URLs. For convenience, the UI also
supports shorthand aliases that resolve to standard HTTPS endpoints:

- `github://owner/repo/ref` resolves to GitHub raw.githubusercontent.com
- `gs://bucket/path` resolves to GCS public HTTPS API
- `s3://bucket/path` resolves to S3 HTTPS API

Normalization rules, alias expansion details, and deterministic ordering live in
Appendix A (Endpoint Specification).

### Standard Endpoints

Extension servers implement these HTTP(S) endpoints:

```
# Server metadata (required)
{base_url}/manifest.json                          (GET)

# UI Extensions (module-scoped)
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

**Manifest File:**

The `manifest.json` file contains server metadata and module list:

```json
{
  "name": "Google Internal Extensions",
  "modules": ["default", "android", "chrome"],
  "csp_allow": ["https://symbolserver.corp.google.com"]
}
```

**Fields:**
- `name` (required): Human-readable server name shown in Settings
- `modules` (required): List of available modules. Use `["default"]` for
  single-module servers.
- `csp_allow` (optional): URLs to add to Content Security Policy for
  symbolization/deobfuscation endpoints

### Request Strategy

The UI uses different request patterns for different endpoint types:

- **UI extensions (macros, SQL modules, proto descriptors):** Queries all
  enabled server/module combinations and aggregates responses. Partial failures
  are acceptable - failed servers are skipped and logged.
- **Symbolization/deobfuscation:** Try-until-success pattern (first successful
  response ends the search).

Detailed orchestration behavior (ordering, timeouts, rate limiting, retry logic)
is specified in Appendix A.

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

- Server's `manifest.json` lists available modules:
  `{"name": "...", "modules": ["default", "android", "chrome"]}`
- When adding server, UI fetches manifest and shows module checkboxes in
  Settings
- Users explicitly select which modules to load (none selected by default except
  for installation-configured servers)
- UI loads extensions only from selected modules
- Each module's extensions in separate paths: `/modules/android/macros`,
  `/modules/chrome/macros`, etc.

**Default bootstrap behavior:** when an installation-configured server (like
Google's internal server) is auto-added, the UI selects the `default` module
automatically. Other modules remain unchecked so users can opt in explicitly.

**Implementation notes:**

- **Flat structure only**: Use dashes for hierarchy (e.g., `android-frameworks`)
- **No module-level ACL**: Access control at server level (GitHub repo
  permissions, GCS IAM, etc.)

### Resource Naming and Conflicts

**Macros: Automatic Namespacing**

Macros are automatically namespaced to prevent conflicts.

Format: `[server_key module] macro_name`

Examples:

- `[google-internal default] Startup Analysis`
- `[acme-corp android] Memory Snapshot`

`server_key` references the normalized slug defined in Appendix A. Duplicate
handling specifics are documented in Appendix B.

**Note:** Macros are registered with their full namespaced name. When invoking
macros programmatically with `RunCommand`, use the full namespaced name (e.g.,
`RunCommand("[google-internal default] Startup Analysis")`).

**SQL Modules & Proto Descriptors: Conflict Handling**

Not automatically namespaced. Deterministic ordering and conflict logging rules
are covered in Appendix A and Appendix B. Future enhancement: Settings UI for
conflict resolution.

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
- **GitHub OAuth**: Device Flow for client-side authentication without backend
- **Cloud storage**: GCS and S3 native authentication
- **Standard HTTPS auth**: Bearer tokens or Basic authentication

**Authentication flow:**

1. Try request without credentials (works for public servers)
2. On 401/403 → Prompt user for credentials via provider-specific flow
3. Store credentials in localStorage (separate from settings)
4. Retry request with credentials
5. Auto-refresh OAuth tokens as needed

**Full specification:** See RFC-0006 for storage schemas, credential management
UI, OAuth flows, token refresh logic, and security considerations. Appendix C
below provides minimal context for understanding this RFC.

## Examples

### GitHub Static Hosting

**Repo structure:**

```
acme-corp/perfetto-resources/
├── manifest.json
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

**File: `manifest.json`**

```json
{
  "name": "Acme Corp Extensions",
  "modules": ["default", "android", "chrome"]
}
```

**Configuration:**

```
URL: github://acme-corp/perfetto-resources/main
Modules: default, android, chrome
Auth: GitHub OAuth (automatic)
```

### Corporate Server

```python
@app.route('/manifest.json')
def get_manifest():
    return jsonify({
        "name": "Acme Corp Extensions",
        "modules": ["default", "android", "chrome", "infra"]
    })

@app.route('/modules/<module>/macros')
def get_module_macros(module):
    return send_file(f'/data/modules/{module}/macros')
```

**Configuration:**

```
URL: https://perfetto.corp.example.com
Modules: default, android
Auth: Corporate SSO (OAuth2/OIDC)
```

See Appendix D for complete implementation examples.

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

Request orchestration details (timeouts, rate limiting, ordering) are specified
in Appendix A.

## Alternatives Considered

### Custom `perfetto://` URI Scheme

**Rejected** - Adds parsing complexity for minimal benefit. Standard HTTP(S)
URLs work fine.

### Service Discovery via Manifest File

**Adopted** - Manifest file (`manifest.json`) is required and serves multiple
purposes: friendly server name, module list, and CSP configuration for optional
endpoints like symbolization.

### Perfetto-Hosted Registry

**Rejected** - Single point of failure, doesn't solve private/corporate use
case. Decentralized model is better.

### Build Custom ACL System

**Hard Rejected** - Huge implementation burden, users need another account.
Leverage existing systems (GitHub, GCS, corporate SSO).

## Implementation

### Phase 1: Core Extension Server Infrastructure

- Extension server config schema (localStorage)
- Server fan-out orchestration (aggregate UI extensions from all enabled
  servers/modules)
- Manifest fetching and parsing
- Auth header injection
- Error handling and logging

### Phase 2: UI Extensions

- Manifest endpoint (`/manifest.json`)
- Module discovery and selection UI
- Extension endpoints (macros, SQL modules, proto descriptors)
- Replace `is_internal_user_script_loader.ts`
- Installation-configured server bootstrap (auto-added, `default` module
  selected)

### Phase 2.5: Authentication (RFC-0006)

- GitHub App OAuth (Device Flow)
- GCS/S3/HTTPS authentication flows
- Credential management UI
- See RFC-0006: Extension Server Authentication for details

### Phase 3: Post-MVP Enhancements

- Extension Server Store (curated catalog + Settings shortcuts; cover in
  dedicated RFC)
- URL parameter support (`?add_server=...`)
- Service worker caching for offline support

### Phase 4: Local HTTP Accelerator (Future)

- Extend `trace_processor --httpd` to serve extension server endpoints
- Local filesystem discovery

### Phase 5: Symbolization & Deobfuscation (Separate RFC)

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

---

# Appendices

## Appendix A: Complete Endpoint Specification

### HTTP Status Codes

**Request-level status:**

- **200 OK** - Request processed (even if some items failed)
- **400 Bad Request** - Malformed request
- **401 Unauthorized** - Authentication required
- **403 Forbidden** - Insufficient permissions
- **404 Not Found** - Endpoint/resource doesn't exist
- **500 Internal Server Error** - Provider error

**Item-level status:**

For endpoints processing multiple items, use 200 OK with per-item status in
response. Partial success is acceptable.

```json
{
  "symbols": [
    {
      "address": "0x1000",
      "function": "Foo()",
      "file": "foo.cc",
      "line": 42
    },
    {
      "address": "0x2000",
      "status": "not_found",
      "message": "Build ID abc123 not found"
    }
  ]
}
```

**Error responses (4xx/5xx):**

```json
{
  "error": {
    "message": "Human-readable error description",
    "code": "internal_error"
  }
}
```

### JSON Naming Convention

All JSON fields use **snake_case** (following GitHub, Stripe):

- Examples: `build_id`, `user_code`, `frame_address`, `team_name`

### Compression

- Providers MAY compress using gzip, brotli, or deflate
- Browsers automatically handle Accept-Encoding and decompression
- No special handling in Perfetto UI
- Recommendation: Compress large files (ELF files, proto descriptors)

### Error Reporting

- Failed requests logged to console with provider URL, endpoint, status code
- Malformed JSON or schema validation failures (via Zod) logged to console with
  provider/endpoint/error details, then that provider/team combination is
  skipped for that resource type
- No user-facing error notifications for MVP
- Future: Error UI with retry options

### Endpoint Versioning

**Proto-style versioning:** Avoid monolithic version increments. Instead:

1. **Adding fields/features** - Just add them. Worst case: clients get 404 for
   new optional endpoints, which is handled gracefully.
2. **Changing semantics** - Define new types/endpoints alongside old ones. For
   example, if symbolization behavior changes fundamentally, the manifest can
   specify:
   ```json
   {
     "symbolizers": [
       {
         "type": "on_demand_symbolizer_v2",
         "path": "/symbolize/v2/frames"
       }
     ]
   }
   ```
   Old `on_demand_symbolizer` continues working at its original path. UI
   supports both, preferring newer versions when available.

**No monolithic `/v2/` prefixes** - These force unnecessary version bumps.
Version individual features only when semantics change, not when adding new
capabilities.

### Endpoint Aliases & Server Keys

- Shorthand aliases resolve to canonical HTTPS endpoints before issuing
  requests:
  - `github://owner/repo/ref[/optional/path]` →
    `https://raw.githubusercontent.com/owner/repo/ref[/optional/path]`. When no
    optional path is supplied the repo root is used and Perfetto appends the
    standard endpoint suffix (for example `/modules/android/macros`).
  - `gs://bucket/path` → `https://storage.googleapis.com/bucket/path`
  - `s3://bucket/path` → `https://bucket.s3.amazonaws.com/path`
- Server keys are normalized from the canonical HTTPS URL:
  1. Lowercase the URL.
  2. Strip the `https://` prefix.
  3. Remove any query string or fragment.
  4. Replace non-alphanumeric ASCII (e.g., `/ . _ :`) with `-`.
  5. Collapse repeated `-` and trim leading/trailing `-`.
- Example mappings:
  - `https://perfetto.acme.com` → `perfetto-acme-com`
  - `https://corp.example.com:8443/modules` → `corp-example-com-8443-modules`
  - `github://acme/perfetto-ext/main` →
    `raw-githubusercontent-com-acme-perfetto-ext-main`
- Display labels shown in Settings can be overridden, but normalized server keys
  remain stable and are used for deduplication, ordering, and credential
  lookups.

### Ordering Algorithm

Extension loading follows a deterministic ordering to ensure consistent behavior
across sessions and predictable conflict resolution:

1. **Server ordering:**
   - Installation-configured servers load first (in the order they're defined in
     the UI bundle)
   - Then user-configured servers, alphabetically by normalized server key (as
     defined above)
2. **Module ordering:** For each server, iterate selected modules alphabetically
3. **Resource ordering:** Within each resource type (macros, SQL modules, proto
   descriptors), iterate resource keys in lexicographical order

**Example:** Given installation-configured server `google-internal` (modules:
`default`) and user-configured servers `corp-example-com` (modules: `chrome`,
`android`) and `raw-githubusercontent-com-acme-ext-main` (modules: `default`),
the load order is:

1. `google-internal` / `default` / macros (keys sorted)
2. `google-internal` / `default` / sql_modules (keys sorted)
3. `google-internal` / `default` / proto_descriptors (keys sorted)
4. `corp-example-com` / `android` / macros (keys sorted)
5. `corp-example-com` / `android` / sql_modules (keys sorted)
6. `corp-example-com` / `android` / proto_descriptors (keys sorted)
7. `corp-example-com` / `chrome` / macros (keys sorted)
8. `corp-example-com` / `chrome` / sql_modules (keys sorted)
9. `corp-example-com` / `chrome` / proto_descriptors (keys sorted)
10. `raw-githubusercontent-com-acme-ext-main` / `default` / macros (keys sorted)
11. `raw-githubusercontent-com-acme-ext-main` / `default` / sql_modules (keys sorted)
12. `raw-githubusercontent-com-acme-ext-main` / `default` / proto_descriptors (keys sorted)

This ordering determines which resource wins in case of conflicts (first
registration wins).

### Request Orchestration & Rate Limiting

**Request ordering:**

- Extension loading follows the deterministic ordering specified in the
  "Ordering Algorithm" subsection above (server key → module → resource type →
  resource key).
- Extension requests issue serially per server to limit rate spikes; the UI
  remains interactive and surfaces loading spinners plus console logs while
  servers load.
- Symbolization/deobfuscation continue the try-until-success pattern (first
  server returning success ends the iteration).

**Timeouts (fail-fast):**

Per-endpoint timeout values:

- Manifest discovery: 5s
- Extension loading: 10s
- Symbolization: 15s
- Bulk downloads: 30s

Timeout handling:

- For UI extensions (macros, SQL modules, proto descriptors): timeout skips that
  server/module combination and continues with others. Partial data still loads.
- For symbolization/deobfuscation: timeout triggers immediate fallback to next
  server (try-until-success pattern).

**Rate limiting:**

- HTTP 429/503 responses:
  - If a `Retry-After` header is present, further requests to that server/module
    are deferred until the header expires.
  - Without `Retry-After`, the server/module is skipped for the remainder of the
    session and retried on next reload.

## Appendix B: Resource Format Specifications

### Macros Format

Macros are named sequences of UI commands following
[UI Automation](https://perfetto.dev/docs/visualization/ui-automation) format.

**Endpoints:**

- `GET /modules/{module}/macros`

All servers use the same response format regardless of how many modules they
have.

**Response:**

```json
{
  "Android Startup Analysis": [
    {
      "id": "dev.perfetto.CreateWorkspace",
      "args": ["Startup Analysis"]
    },
    {
      "id": "dev.perfetto.PinTracksByRegex",
      "args": [".*ActivityStart.*", "name"]
    },
    {
      "id": "dev.perfetto.RunQueryAndShowTab",
      "args": [
        "SELECT ts, dur, name FROM slice WHERE name LIKE '%ActivityStart%' ORDER BY ts"
      ]
    }
  ],
  "Memory Snapshot": [
    {
      "id": "dev.perfetto.AddDebugSliceTrack",
      "args": [
        "SELECT ts, dur, CAST(size AS text) as name FROM heap_profile_allocation WHERE size > 1000000 ORDER BY size DESC LIMIT 100",
        "Large Allocations (>1MB)"
      ]
    }
  ]
}
```

**Schema:**

```typescript
{
  [macro_name: string]: Command[];
}

interface Command {
  id: string;      // Command ID
  args: any[];     // Arguments (format varies by command)
}
```

**Common command IDs:**

- `dev.perfetto.RunQueryAndShowTab`
- `dev.perfetto.CreateWorkspace`
- `dev.perfetto.PinTracksByRegex`
- `dev.perfetto.AddDebugSliceTrack`

See
[Commands Reference](https://perfetto.dev/docs/visualization/commands-automation-reference)
for complete list.

**Namespacing & duplicates:**

- Registered macro names follow `[server_key module] macro_name`, where
  `server_key` uses the normalization rules in Appendix A.
- Duplicate keys in JSON are handled by the parser (typically last-wins); no
  additional deduplication is performed within a single server/module payload.

### SQL Modules Format

**Endpoints:**

- `GET /modules/{module}/sql_modules`

All servers use the same response format regardless of how many modules they
have.

**Response:**

```json
{
  "android.startup": "CREATE PERFETTO TABLE android_startup_events AS\nSELECT ts, dur, name FROM slice WHERE name GLOB 'Startup*';",
  "android.memory": "CREATE PERFETTO VIEW android_memory_stats AS\nSELECT ts, SUM(size) as total_bytes FROM heap_profile_allocation GROUP BY ts;",
  "common.helpers": "CREATE PERFETTO FUNCTION format_bytes(bytes LONG) RETURNS STRING AS\n  SELECT CASE\n    WHEN $bytes < 1024 THEN $bytes || ' B'\n    WHEN $bytes < 1048576 THEN ($bytes / 1024) || ' KB'\n    ELSE ($bytes / 1048576) || ' MB'\n  END;"
}
```

**Schema:**

```typescript
{
  [module_path: string]: string;  // module_path → SQL content
}
```

**Module path format:**

- Dot-separated: `category.subcategory.module_name`
- Alphanumeric and dots only: `[a-zA-Z0-9.]+`

**Conflict handling:**

- SQL modules register in deterministic order (server key → module →
  module_path). "Successfully registered" means the module was registered in the
  trace processor engine. The first successfully registered definition stays
  active; subsequent duplicates are skipped and logged, including both servers
  and the conflicting module path.

### Proto Descriptors Format

**Endpoints:**

- `GET /modules/{module}/proto_descriptors`

All servers use the same response format regardless of how many modules they
have.

**Response:**

```json
{
  "android-system-protos": {
    "name": "Android System Server Protos",
    "description": "Protos for Android system_server traces",
    "descriptor": "CpcBChd0cmFjZV9wcm90by9hbmRyb2lkLnByb3RvEhBwZXJmZXR0by5wcm90..."
  },
  "chrome-browser-protos": {
    "name": "Chrome Browser Protos",
    "descriptor": "CpQCChZjaHJvbWVfYnJvd3Nlci5wcm90bxIQcGVyZmV0dG8ucHJvdG9zGg9..."
  }
}
```

**Schema:**

```typescript
{
  [id: string]: {
    name: string;              // Required: Display name
    description?: string;      // Optional
    descriptor: string;        // Required: Base64-encoded FileDescriptorSet
  }
}
```

**Creating a FileDescriptorSet:**

```bash
# Compile .proto files
protoc --descriptor_set_out=protos.desc \
       --include_imports \
       your_proto_file.proto

# Base64 encode
base64 protos.desc > protos.desc.b64
```

**Conflict handling:**

- Proto descriptor entries follow the same deterministic ordering and first-win
  semantics as SQL modules. Duplicate IDs yield console warnings listing the
  server/module/id that was retained vs dropped.

## Appendix C: Authentication Details

**Note:** Authentication implementation is fully specified in **RFC-0006:
Extension Server Authentication**. This appendix provides minimal context for
understanding the overall system architecture of RFC-0005.

### High-Level Overview

Extension servers support standard authentication methods:

- **GitHub OAuth**: Device Flow for client-side authentication
- **GCS/S3**: Native cloud provider authentication
- **HTTPS**: Bearer tokens or Basic auth

### Storage

- Server configuration stored in Perfetto settings (localStorage)
- Credentials stored separately in `perfetto_server_credentials` (localStorage)
- Credentials scoped per-server to support multiple accounts

### Key Flows

1. **Server Addition**: User adds server URL → manifest fetched → modules
   displayed → user selects modules
2. **Authentication**: On 401/403 → prompt for credentials → store → retry
3. **Token Refresh**: OAuth tokens auto-refresh using standard flows

**Full authentication specification:** See RFC-0006 for storage schemas,
credential management UI, OAuth flows, token refresh logic, security
considerations, and implementation details.

## Appendix D: Extension Server Implementation Examples

### GitHub Static Hosting

**Repository structure:**

```
acme-corp/perfetto-resources/
├── manifest.json
├── modules/
│   ├── default/
│   │   ├── macros
│   │   ├── sql_modules
│   │   └── proto_descriptors
│   ├── android/
│   │   ├── macros              (all android macros in one JSON file)
│   │   ├── sql_modules         (all android SQL modules in one JSON file)
│   │   └── proto_descriptors   (all android proto descriptors in one JSON file)
│   └── chrome/
│       ├── macros
│       └── sql_modules
```

**File: `manifest.json`**

```json
{
  "name": "Acme Corp Extensions",
  "modules": ["default", "android", "chrome"]
}
```

**File: `modules/android/macros`**

```json
{
  "Startup Analysis": [
    {"id": "dev.perfetto.CreateWorkspace", "args": ["Startup"]},
    {
      "id": "dev.perfetto.PinTracksByRegex",
      "args": [".*ActivityStart.*", "name"]
    }
  ],
  "Memory Snapshot": [
    {
      "id": "dev.perfetto.AddDebugSliceTrack",
      "args": ["SELECT ...", "Allocations"]
    }
  ]
}
```

**URL Resolution:**

`github://acme-corp/perfetto-resources/main/modules/android/macros`

→
`https://raw.githubusercontent.com/acme-corp/perfetto-resources/main/modules/android/macros`

**Authentication:** Automatic via GitHub OAuth if repo is private.

### Corporate Extension Server (Flask)

```python
from flask import Flask, send_file, jsonify
from flask_cors import CORS

app = Flask(__name__)
CORS(app, origins=["https://ui.perfetto.dev"])

@app.route('/manifest.json')
def get_manifest():
    return jsonify({
        "name": "Acme Corp Extensions",
        "modules": ["default", "android", "chrome", "infra"]
    })

@app.route('/modules/<module>/macros')
def get_module_macros(module):
    macro_file = f'/data/modules/{module}/macros'
    if os.path.exists(macro_file):
        return send_file(macro_file, mimetype='application/json')
    return '', 404

@app.route('/modules/<module>/sql_modules')
def get_module_sql_modules(module):
    sql_file = f'/data/modules/{module}/sql_modules'
    if os.path.exists(sql_file):
        return send_file(sql_file, mimetype='application/json')
    return '', 404

@app.route('/modules/<module>/proto_descriptors')
def get_module_proto_descriptors(module):
    proto_file = f'/data/modules/{module}/proto_descriptors'
    if os.path.exists(proto_file):
        return send_file(proto_file, mimetype='application/json')
    return '', 404

if __name__ == '__main__':
    # Production: use proper SSL certificates
    # app.run(host='0.0.0.0', port=443, ssl_context=('/path/to/cert.pem', '/path/to/key.pem'))

    # Development only: adhoc creates self-signed certificates
    app.run(host='0.0.0.0', port=443, ssl_context='adhoc')
```

**Authentication:** Corporate SSO/OAuth2/OIDC. Extension server handles auth,
Perfetto sends tokens in `Authorization` header.

### UI Integration Example

```typescript
class ServerService {
  private servers: Map<string, ServerConfig>;

  async discoverManifest(server: string): Promise<Manifest | null> {
    try {
      const response = await fetch(`${server}/manifest.json`);
      if (response.ok) {
        const data = await response.json();
        return data;
      }
    } catch (e) {
      console.error('Failed to discover manifest', e);
    }
    return null;
  }

  async loadResources(
    server: string,
    module: string,
    resourceType: 'macros' | 'sql_modules' | 'proto_descriptors',
  ): Promise<Record<string, any> | null> {
    const url = `${server}/modules/${module}/${resourceType}`;

    try {
      const headers = await this.getAuthHeaders(server);
      const response = await fetch(url, {headers});

      if (response.status === 401 || response.status === 403) {
        await this.handleAuthRequired(server);
        return this.loadResources(server, module, resourceType); // Retry
      }

      if (response.ok) {
        return await response.json();
      }
    } catch (e) {
      console.error(`Failed to load ${resourceType}`, e);
    }
    return null;
  }

  private async getAuthHeaders(server: string): Promise<Headers> {
    const creds = this.getStoredCredentials(server);
    const headers = new Headers();

    if (creds?.type === 'github_oauth' && creds.access_token) {
      headers.set('Authorization', `Bearer ${creds.access_token}`);
    } else if (creds?.type === 'bearer_token' && creds.bearer_token) {
      headers.set('Authorization', `Bearer ${creds.bearer_token}`);
    } else if (creds?.type === 'basic_auth') {
      const encoded = btoa(`${creds.username}:${creds.password}`);
      headers.set('Authorization', `Basic ${encoded}`);
    }

    return headers;
  }

  private getStoredCredentials(server: string): any {
    const credsJson = localStorage.getItem('perfetto_server_credentials');
    if (!credsJson) return null;
    const allCreds = JSON.parse(credsJson);
    return allCreds[server];
  }

  private async handleAuthRequired(server: string): Promise<void> {
    // Trigger server-specific auth flow (GitHub OAuth, token prompt, etc.)
    // Implementation depends on server type
  }

  async loadAllExtensions(): Promise<void> {
    for (const [serverUrl, config] of this.servers) {
      if (config.selectedModules && config.selectedModules.length > 0) {
        for (const module of config.selectedModules) {
          await this.loadAndRegister(serverUrl, module);
        }
      }
    }
  }

  private async loadAndRegister(
    server: string,
    module: string,
  ): Promise<void> {
    const macros = await this.loadResources(server, module, 'macros');
    const sqlModules = await this.loadResources(server, module, 'sql_modules');
    const protoDescs = await this.loadResources(
      server,
      module,
      'proto_descriptors',
    );

    if (macros) this.registerMacros(macros);
    if (sqlModules) this.registerSqlModules(sqlModules);
    if (protoDescs) this.registerProtoDescriptors(protoDescs);
  }
}
```

## Appendix E: Context for LLM Recovery

**This section contains implementation details, resolved questions, and context
for resuming work. Human readers can stop here.**

### Background

**Problem we're solving:**

- Users want to share Perfetto extensions within teams without copy-paste
- Symbolization/deobfuscation setup is too complex
- No standard way to integrate with symbol servers
- Don't want to build custom ACL/auth systems

**Key design decision:**

- Standard HTTP(S) endpoints with well-defined paths
- Server-based: users configure URLs, UI aggregates extensions from all enabled
  servers
- Piggyback on existing ACL (GitHub, GCS, corporate SSO)

**Related GitHub issues:** #3085

### All Open Questions - RESOLVED ✅

**RFC Status:** Complete, all questions resolved, ready for implementation.

**Architecture & Scope:**

1. ✅ Config distribution: Settings UI + default Google provider (Provider Store
   & URL params followup)
2. ✅ Config location: localStorage (settings system)
3. ✅ URL normalization: Behind the scenes, preserve user input for display
4. ✅ CORS: Required, documented in Security section

**Endpoint Specification:** 5. ✅ Versioning: Proto-style (field additions ok,
semantic changes = new types) 6. ✅ Partial success: UI extensions aggregate
across all selected servers; symbolization/deobfuscation fall back to
first-success semantics in the followup RFC 7. ✅ JSON format: snake_case 8. ✅
Compression: Optional, browser-handled

**Symbolization & Deobfuscation:** 9. ✅ All deferred to separate RFC

**Performance & Reliability:** 13. ✅ Timeouts: Fixed (5s/10s/15s/30s),
fail-fast 14. ✅ Retry: No retry, try next server immediately 15. ✅ Server
queries: Serial (one at a time) 16. ✅ Rate limiting: Respect Retry-After
headers

**Security & Privacy:** 17. ✅ Token storage: Plaintext in localStorage
(separate from settings) 18. ✅ Data sensitivity: Implicit consent for
extensions (no trace data sent) 19. ✅ Code execution: No JavaScript plugins,
only declarative

**User Experience:** 20. ✅ Error reporting: Console logging only for MVP 21. ✅
Health monitoring: No monitoring for MVP 22. ✅ Migration: One-shot when
extension server ready 23. ✅ Discovery: Settings instructions (Extension Server
Store catalog deferred)

**Modules & Filtering:** 24. ✅ Module discovery: Via manifest.json when adding
server 25. ✅ Module hierarchy: Flat only (use dashes for organization) 26. ✅
Module permissions: No module-level ACL (server-level only) 27. ✅ Module
wildcards: No wildcards

**GitHub App OAuth:** 29. ✅ Scopes: Repository Contents (read-only) only 30. ✅
Token refresh: Use refresh tokens, auto-refresh 31. ✅ Hosting: Client-side only
(Device Flow) 32. ✅ Multiple accounts: Per-server auth (natural support)

### Implementation Checklist

**Phase 1: Core Extension Server Infrastructure**

- [ ] Extension server config schema (localStorage)
- [ ] Server fan-out orchestration (aggregate UI extensions from all enabled
      servers/modules)
- [ ] Alias resolution + server key normalization helpers
- [ ] Manifest fetching and parsing
- [ ] Auth header injection
- [ ] Error handling and logging

**Phase 2: UI Extensions**

- [ ] Manifest endpoint (`/manifest.json`)
- [ ] Module discovery and selection UI
- [ ] Extension endpoints (macros, SQL modules, proto descriptors)
- [ ] Extension loading with module filtering
- [ ] Replace `is_internal_user_script_loader.ts`
- [ ] Installation-configured server bootstrap (auto-add + default module
      selection)

**Phase 2.5: Authentication (Separate RFC)**

- [ ] GitHub App OAuth (Device Flow)
- [ ] GCS/S3/HTTPS authentication flows
- [ ] Credential management UI

**Phase 3: Post-MVP Enhancements**

- [ ] Extension Server Store (requires dedicated Store UX RFC)
- [ ] URL parameter support (`?add_server=...`)
- [ ] Service worker caching for offline support

**Phase 4: Local HTTP Accelerator (Future)**

- [ ] Extend `trace_processor --httpd`
- [ ] Local filesystem discovery

**Phase 5: Symbolization & Deobfuscation (Separate RFC)**

- [ ] Write separate RFC
- [ ] Implement endpoints
- [ ] Browser integration
- [ ] Caching strategies

### Prerequisites

**Before Phase 1:**

1. Register Perfetto GitHub App with GitHub
2. Set up internal Google extension server

**Before Phase 2:** 3. Create example GitHub repo with test extensions 4.
Finalize localStorage settings schema

### Files to Modify/Create

**New files:**

- `ui/src/core/servers/server_service.ts` - Core server logic
- `ui/src/core/servers/server_config.ts` - Config schema

**Files to modify:**

- `ui/src/frontend/is_internal_user_script_loader.ts` - Remove/replace

**Deferred new files (post-MVP):**

- `ui/src/core/servers/store.ts` - Extension Server Store (curated list +
  shortcuts)

### Testing Strategy

**Unit tests:**

- Extension server config parsing
- Auth header injection
- Extension aggregation logic with mocks
- Error handling
- Conflict filtering/logging for duplicate SQL modules and proto descriptors
- Deterministic ordering across server/module/resource aggregation
- Alias resolution and server key normalization helpers
- Manifest fetching and parsing

**Integration tests:**

- Real GitHub repo with test extensions
- Mock extension servers with various response patterns
- Auth with test tokens

**E2E tests:**

- Add server → select modules → reload → extensions loaded
- Install macro from GitHub → run macro → verify behavior

### Key Technical Challenges

1. **Browser filesystem access** - Solved via local httpd (Phase 4)
2. **CORS** - All extension servers must support CORS
3. **GitHub OAuth** - Device Flow in browser without backend
4. **Manifest discovery UX** - Immediate discovery, Settings UI for module
   selection
5. **Credential security** - localStorage with same-origin protection

### Success Criteria

**MVP (Phase 1-2):**

- Users can configure extension servers via Settings
- UI loads extensions from GitHub or HTTPS
- Replace `is_internal_user_script_loader.ts`
- Module selection works
- Manifest discovery works

**Future (Phase 3-5):**

- GitHub OAuth (separate authentication RFC)
- Extension Server Store (separate RFC)
- Local httpd for filesystem access
- Symbolization/deobfuscation (separate RFC)

### Important Constraints

- **No custom auth** - Hard requirement
- **Standard protocols** - HTTP/HTTPS only
- **No code execution** - Declarative extensions only
- **Simple** - Avoid complexity
