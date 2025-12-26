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

Server URLs are normalized to a canonical form and loaded in deterministic order (installation-configured servers first, then user-configured servers alphabetically).

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

The `server_key` is derived from the normalized server URL. Duplicate macro names from different servers are distinguished by their namespace prefix.

**Note:** Macros are registered with their full namespaced name. When invoking
macros programmatically with `RunCommand`, use the full namespaced name (e.g.,
`RunCommand("[google-internal default] Startup Analysis")`).

**SQL Modules & Proto Descriptors: Conflict Handling**

Not automatically namespaced. Conflicts are resolved via deterministic ordering (first registration wins), with duplicates logged to console. Future enhancement: Settings UI for conflict resolution.

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
UI, OAuth flows, token refresh logic, and security considerations.

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
