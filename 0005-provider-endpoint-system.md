# Perfetto Provider Endpoint System

**Authors:** @LalitMaganti

**Status:** Complete - Ready for Implementation

## Summary

This RFC proposes a provider-based system for distributing Perfetto UI
extensions and integrating with external services via standard HTTP(S)
endpoints.

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

## Decision

Use a provider-based architecture where users configure HTTP(S) endpoints that
serve Perfetto extensions. The browser UI queries configured providers to load
macros, SQL modules, and proto descriptors.

## Scope

**In Scope:**

- Browser UI provider configuration and loading
- Remote providers (HTTPS, GitHub, GCS, S3)
- UI extensions: macros, SQL modules, proto descriptors
- Authentication for browser access to providers
- Team-based organization and filtering
- Manual provider management via Settings (Provider Store/one-click install are
  post-MVP followups)

**Out of Scope (Future RFCs):**

- **Symbolization and deobfuscation implementation** (separate RFC will cover
  browser integration, caching, mode selection, etc.)
- CLI tools (`traceconv`, standalone scripts)
- Local filesystem access via `trace_processor --httpd`
- Command-line configuration files

**Note:** Symbolization and deobfuscation endpoints are mentioned for
completeness and to establish the overall provider architecture, but all
implementation details are deferred to a separate RFC.

## Key Principles

1. **Standard protocols** - Use HTTP/HTTPS, not custom schemes
2. **Leverage existing systems** - Use provider's existing auth (GitHub, GCS
   IAM, corporate SSO), not custom Perfetto ACL
3. **Simple capability detection** - No manifest files or service discovery;
   rely on HTTP status codes (404 = not supported, 200 = supported)
4. **Safe extensions** - Only declarative extensions (macros, SQL, proto
   descriptors), no JavaScript code execution

## Architecture

### Provider Configuration

Users configure providers in Settings → Providers. Configuration stored in
localStorage using existing Perfetto settings system. Credentials stored
separately in localStorage.

**Two ways to add providers in MVP:**

1. **User-configured** - Manually add provider URL in Settings
2. **Default Google provider** - Auto-added for Googlers on first load

Provider Store entries and URL-parameter-based one-click install remain goals
for a V1 followup (separate RFC covering catalog UX + distribution) and are
tracked in Phase 2 future work.

The default Google provider (internal-only) is auto-added on first load with the
`perfetto` team preselected. Every other team remains opt-in via Settings so
that users explicitly enable the content sets they care about.

The hosted UI is intentionally client-distributed: there is no first-party way
to centrally provision provider lists or credentials into every user profile.
This follows directly from the “static client + no backend” architecture, so
org-level bootstrap and managed policy distribution are intentionally out of
scope. Organizations that require locked-down bootstrap flows must fork/deploy
their own Perfetto UI bundle and pair it with server-side automation that
preloads the desired provider endpoints.

Providers are configured with HTTPS URLs. For convenience, the UI also supports
shorthand aliases that resolve to standard HTTPS endpoints:

- `github://owner/repo/ref` resolves to GitHub raw.githubusercontent.com
- `gs://bucket/path` resolves to GCS public HTTPS API
- `s3://bucket/path` resolves to S3 HTTPS API

Normalization rules, alias expansion details, and deterministic ordering live in
Appendix A (Endpoint Specification).

### Standard Endpoints

Providers implement these HTTP(S) endpoints (structure explained below):

```
# Team discovery (optional)
{base_url}/teams                            (GET)

# UI Extensions (team-scoped)
{base_url}/teams/{team}/macros              (GET)
{base_url}/teams/{team}/sql_modules         (GET)
{base_url}/teams/{team}/proto_descriptors   (GET)

# UI Extensions (no teams - fallback)
{base_url}/macros                           (GET)
{base_url}/sql_modules                      (GET)
{base_url}/proto_descriptors                (GET)

# Symbolization (optional - details in separate RFC)
{base_url}/symbolize/frames           (POST)
{base_url}/symbolize/elf/{build_id}   (GET)

# Deobfuscation (optional - details in separate RFC)
{base_url}/deobfuscate/symbols        (POST)
{base_url}/deobfuscate/mapping/{id}   (GET)
```

### Request Strategy

The UI uses different request patterns for different endpoint types:

- **UI extensions (macros, SQL modules, proto descriptors):** Queries all
  enabled provider/team combinations and aggregates responses. Partial failures
  are acceptable - failed providers are skipped and logged.
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
providers.

**Symbolization & Deobfuscation:**

- **Symbolization** (`/symbolize/*`) - Convert addresses to function names,
  files, line numbers
- **Deobfuscation** (`/deobfuscate/*`) - Convert obfuscated names to original
  names

Details for these endpoints deferred to separate RFC.

### Team-Based Organization

**Why teams exist:**

In large organizations, different teams build different extensions:

- Android team creates startup analysis macros
- Chrome team creates rendering performance macros
- Infrastructure team creates general profiling utilities

Without teams, all extensions from a provider would load for everyone, creating
noise. Teams let users choose which sets of extensions they want.

**How teams work:**

- Provider implements `GET /teams` returning:
  `{"teams": ["android", "chrome", "infra"]}`
- When adding provider, UI calls `/teams` and shows team checkboxes in Settings
- Users explicitly select which teams to load (none selected by default)
- UI loads extensions only from selected teams
- Each team's extensions in separate files: `/teams/android/macros`,
  `/teams/chrome/macros`, etc.

**Default bootstrap behavior:** when the internal Google provider is auto-added,
the UI selects the `perfetto` team by default and leaves all other teams
unchecked so users can opt in explicitly.

**Providers without teams:**

If `/teams` returns 404, provider serves a single set of extensions. UI loads
directly from `{base_url}/macros`, `{base_url}/sql_modules`, etc. Good for
simple providers.

**Implementation notes:**

- **Flat structure only**: Use dashes for hierarchy (e.g., `android-frameworks`)
- **No team-level ACL**: Access control at provider level (GitHub repo
  permissions, GCS IAM, etc.)

### Resource Naming and Conflicts

**Macros: Automatic Namespacing**

Macros are automatically namespaced to prevent conflicts.

Format: `[provider_key team] macro_name`

Examples:

- `[google android] Startup Analysis`
- `[acme mobile] Memory Snapshot`

`provider_key` references the normalized slug defined in Appendix A. Duplicate
handling specifics are documented in Appendix B.

**SQL Modules & Proto Descriptors: Conflict Handling**

Not automatically namespaced. Deterministic ordering and conflict logging rules
are covered in Appendix A and Appendix B. Future enhancement: Settings UI for
conflict resolution.

### Extension Lifecycle

**Extensions loaded ONCE on UI startup. Configuration changes require page
reload.**

Load flow:

1. UI initializes
2. Load provider configuration from localStorage
3. Fetch extensions from all enabled providers/teams in deterministic
   alphabetical order
4. Register macros, SQL modules, proto descriptors
5. UI ready

**Configuration changes:**

- Add/remove provider or change team selection → Settings shows "Reload page to
  apply changes"
- User clicks **[Reload Page]** button
- No automatic reload, no hot-reload logic

**Caching:**

- Extensions kept in memory during session
- UI respects standard HTTP caching semantics (provider-set `Cache-Control`,
  `ETag`, etc.)
- Providers control cache behavior via HTTP headers
- No localStorage cache for extensions

**Failure handling:**

- If all providers fail (timeout, network error, auth failure, etc.), the UI
  still initializes successfully with an empty extension set
- Console logs show which providers failed and why
- UI remains fully functional for local trace analysis

### Authentication

All providers follow same pattern:

1. Check if credentials stored for this provider
2. If yes → Use stored credentials
3. If no → Try request without credentials (may be public)
4. On 401/403 → Prompt user for credentials
5. Store credentials in localStorage (separate from settings)
6. Retry with credentials

For OAuth-backed providers, the UI owns refresh handling: tokens are refreshed
via the provider's standard flow (e.g., GitHub Device Flow refresh tokens) and
no manual rotation is required for the default deployment.

**GitHub OAuth:** Uses GitHub App with Device Flow (client-side only, no backend
needed). User visits github.com/login/device and enters code. Access and refresh
tokens stored in localStorage.

**GCS/S3/HTTPS:** User enters credentials (API tokens, access keys, etc.) when
prompted. Stored per-provider in localStorage.

See Appendix C for detailed authentication flows.

## Examples

### GitHub Static Hosting

**Repo structure:**

```
acme-corp/perfetto-resources/
├── teams/
│   ├── android/
│   │   ├── macros
│   │   ├── sql_modules
│   │   └── proto_descriptors
│   └── chrome/
│       ├── macros
│       └── sql_modules
└── teams              (team list: ["android", "chrome"])
```

**Configuration:**

```
URL: github://acme-corp/perfetto-resources/main
Teams: android, chrome
Auth: GitHub OAuth (automatic)
```

### Corporate Server

```python
@app.route('/teams')
def list_teams():
    return jsonify({"teams": ["android", "chrome", "infra"]})

@app.route('/teams/<team>/macros')
def get_team_macros(team):
    return send_file(f'/data/teams/{team}/macros')
```

**Configuration:**

```
URL: https://perfetto.corp.example.com
Teams: android
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

### Provider Integrity Expectations

Perfetto does not attempt to verify that provider-hosted JSON or descriptor
payloads are trusted. Provider operators are solely responsible for reviewing,
signing, or gating their own content before distribution to guard against
compromised repos or buckets. Each organization can apply its own signing or
review pipeline (e.g., proxy servers, checksum validation, internal CI) without
upstream coordination, and this RFC does not prescribe a canonical integrity
mechanism.

### Data Sensitivity

**For UI extensions:** NO trace data sent to providers. Extensions downloaded
once and executed locally.

**Consent model:** Adding a provider = implicit consent to load extensions from
it.

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

All HTTPS providers MUST set CORS headers:

```
Access-Control-Allow-Origin: https://ui.perfetto.dev
Access-Control-Allow-Methods: GET, POST
Access-Control-Allow-Headers: Authorization, Content-Type
```

**CORS failure handling:**

- CORS violations manifest as browser network errors (typically "CORS policy"
  errors in the console)
- Logged to console with provider URL and endpoint
- Provider/team combination is skipped (same as other network failures)
- Future: Settings UI could detect CORS issues and suggest configuration fixes

Request orchestration details (timeouts, rate limiting, ordering) are specified
in Appendix A.

## Alternatives Considered

### Custom `perfetto://` URI Scheme

**Rejected** - Adds parsing complexity for minimal benefit. Standard HTTP(S)
URLs work fine.

### Service Discovery via Manifest Files

**Rejected** - Extra request for every provider. HTTP status codes (404, 401)
work fine for capability detection.

### Perfetto-Hosted Registry

**Rejected** - Single point of failure, doesn't solve private/corporate use
case. Decentralized model is better.

### Build Custom ACL System

**Hard Rejected** - Huge implementation burden, users need another account.
Leverage existing systems (GitHub, GCS, corporate SSO).

## Implementation

### Phase 1: Core Provider Infrastructure

- Provider config schema (localStorage)
- Provider fan-out orchestration (aggregate UI extensions from all enabled
  providers/teams)
- Auth header injection
- Error handling and logging

### Phase 2: UI Extensions

- Team discovery (`/teams`)
- Extension endpoints (macros, SQL modules, proto descriptors)
- GitHub App OAuth (Device Flow)
- Replace `is_internal_user_script_loader.ts`
- Default Google provider bootstrap (auto-added, `perfetto` team selected)

### Phase 2.5: Post-MVP Enhancements

- Provider Store (curated catalog + Settings shortcuts; cover in dedicated RFC)
- URL parameter support (`?add_provider=...`)

### Phase 3: Local HTTP Accelerator (Future)

- Extend `trace_processor --httpd` to serve provider endpoints
- Local filesystem discovery

### Phase 4: Symbolization & Deobfuscation (Separate RFC)

- Endpoint specifications
- Browser integration
- Caching strategies

### Migration Strategy

- Replacing the legacy `is_internal_user_script_loader.ts` assets with providers
  happens centrally: hosted Perfetto deployments will update their default
  provider list and the UI automatically begins fetching from the new endpoints
  on reload without user action.
- Existing inline macros/SQL bundles ship until the provider backend is ready,
  so the switch is transparent to users aside from the new Settings surface.
- Forked or self-hosted deployments can follow the same pattern; they only need
  to update the packaged provider defaults before rolling the refreshed UI
  bundle.

### Prerequisites

1. Register Perfetto GitHub App
2. Set up internal Google provider server
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
  websites work - users get the latest version on page load. Providers are
  responsible for ensuring extensions remain compatible or coordinating updates
  with users. Version pinning, rollback switches, or multi-version compatibility
  matrices would require backend orchestration and are intentionally excluded.

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

- No hard limits on number of providers, extensions per provider, or response
  sizes. The user explicitly adds providers, establishing an implicit trust
  relationship - malicious or misconfigured providers are not an expected attack
  vector. In practice, these limits are not expected to be reached. If
  performance issues arise, limits can be added later.

**Namespace Protection:**

- Overriding built-in Perfetto functionality (commands, SQL modules, etc.) is
  undefined behavior. Providers should use domain-specific names to avoid
  conflicts with current or future built-in features.

**Macro Validation:**

- Macros that reference nonexistent command IDs or use invalid arguments will
  fail at runtime when invoked. No upfront validation is performed. This matches
  how ad-hoc macros work today - the macro simply fails to execute correctly if
  it's malformed or incompatible with the UI version.

**Cross-Provider Dependencies:**

- Extensions cannot declare dependencies on other extensions, especially across
  providers. Loading order is deterministic but dependencies are not enforced.
  If an extension relies on another extension being present, this is undefined
  behavior and will fail at runtime. Users should configure extensions from a
  single provider if dependencies are required.

**Provider Deployment Ownership:**

- Provider hosting, rollout cadence, and monitoring are intentionally delegated
  to each organization. The RFC does not prescribe SLAs, scaling rules, or
  shared infrastructure so that internal and external deployments can evolve
  independently.

**Offline Distribution:**

- Shipping offline or bundled extension archives is out of scope for this
  design. Teams that require offline readiness can fork or self-host the
  Perfetto UI and manage their own distribution channel without upstream
  protocol changes.

**Telemetry Scope:**

- Provider usage or health telemetry is not part of the MVP. Future work can add
  aggregate reporting once core plumbing is proven, but client behavior today is
  limited to console logging.
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

No versioning initially. When breaking changes needed, add `/v2/` prefix:

- `/v2/teams`
- `/v2/symbolize/frames`

Old endpoints continue for backward compatibility.

### Endpoint Aliases & Provider Keys

- Shorthand aliases resolve to canonical HTTPS endpoints before issuing
  requests:
  - `github://owner/repo/ref[/optional/path]` →
    `https://raw.githubusercontent.com/owner/repo/ref[/optional/path]`. When no
    optional path is supplied the repo root is used and Perfetto appends the
    standard endpoint suffix (for example `/teams/android/macros`).
  - `gs://bucket/path` → `https://storage.googleapis.com/bucket/path`
  - `s3://bucket/path` → `https://bucket.s3.amazonaws.com/path`
- Provider keys are normalized from the canonical HTTPS URL:
  1. Lowercase the URL.
  2. Strip the `https://` prefix.
  3. Remove any query string or fragment.
  4. Replace non-alphanumeric ASCII (e.g., `/ . _ :`) with `-`.
  5. Collapse repeated `-` and trim leading/trailing `-`.
- Example mappings:
  - `https://perfetto.acme.com` → `perfetto-acme-com`
  - `https://corp.example.com:8443/teams` → `corp-example-com-8443-teams`
  - `github://acme/perfetto-ext/main` →
    `raw-githubusercontent-com-acme-perfetto-ext-main`
- Display labels shown in Settings can be overridden, but normalized provider
  keys remain stable and are used for deduplication, ordering, and credential
  lookups.

### Ordering Algorithm

Extension loading follows a deterministic ordering to ensure consistent behavior
across sessions and predictable conflict resolution:

1. **Provider ordering:** Iterate providers alphabetically by normalized
   provider key (as defined above).
2. **Team ordering:** For each team-scoped provider, iterate selected teams
   alphabetically. Single-set providers contribute one logical entry at this
   level.
3. **Resource ordering:** Within each resource type (macros, SQL modules, proto
   descriptors), iterate resource keys in lexicographical order.

**Example:** Given providers `corp-example-com` (teams: `chrome`, `android`) and
`raw-githubusercontent-com-acme-ext-main` (single-set), the load order is:

1. `corp-example-com` / `android` / macros (keys sorted)
2. `corp-example-com` / `android` / sql_modules (keys sorted)
3. `corp-example-com` / `android` / proto_descriptors (keys sorted)
4. `corp-example-com` / `chrome` / macros (keys sorted)
5. `corp-example-com` / `chrome` / sql_modules (keys sorted)
6. `corp-example-com` / `chrome` / proto_descriptors (keys sorted)
7. `raw-githubusercontent-com-acme-ext-main` / macros (keys sorted)
8. `raw-githubusercontent-com-acme-ext-main` / sql_modules (keys sorted)
9. `raw-githubusercontent-com-acme-ext-main` / proto_descriptors (keys sorted)

This ordering determines which resource wins in case of conflicts (first
registration wins).

### Request Orchestration & Rate Limiting

**Request ordering:**

- Extension loading follows the deterministic ordering specified in the
  "Ordering Algorithm" subsection above (provider key → team → resource type →
  resource key).
- Extension requests issue serially per provider to limit rate spikes; the UI
  remains interactive and surfaces loading spinners plus console logs while
  providers load.
- Symbolization/deobfuscation continue the try-until-success pattern (first
  provider returning success ends the iteration).

**Timeouts (fail-fast):**

Per-endpoint timeout values:

- Team discovery: 5s
- Extension loading: 10s
- Symbolization: 15s
- Bulk downloads: 30s

Timeout handling:

- For UI extensions (macros, SQL modules, proto descriptors): timeout skips that
  provider/team combination and continues with others. Partial data still loads.
- For symbolization/deobfuscation: timeout triggers immediate fallback to next
  provider (try-until-success pattern).

**Rate limiting:**

- HTTP 429/503 responses:
  - If a `Retry-After` header is present, further requests to that provider/team
    are deferred until the header expires.
  - Without `Retry-After`, the provider/team is skipped for the remainder of the
    session and retried on next reload.

## Appendix B: Resource Format Specifications

### Macros Format

Macros are named sequences of UI commands following
[UI Automation](https://perfetto.dev/docs/visualization/ui-automation) format.

**Endpoints:**

- Team-scoped: `GET /teams/{team}/macros`
- Single-set (no teams): `GET /macros`

Both endpoints use the same response format.

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

- Registered macro names follow `[provider_key team] macro_name`, where
  `provider_key` uses the normalization rules in Appendix A.
- Duplicate keys in JSON are handled by the parser (typically last-wins); no
  additional deduplication is performed within a single provider/team payload.

### SQL Modules Format

**Endpoints:**

- Team-scoped: `GET /teams/{team}/sql_modules`
- Single-set (no teams): `GET /sql_modules`

Both endpoints use the same response format.

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

- SQL modules register in deterministic order (provider key → team →
  module_path). "Successfully registered" means the module was registered in the
  trace processor engine. The first successfully registered definition stays
  active; subsequent duplicates are skipped and logged, including both providers
  and the conflicting module path.

### Proto Descriptors Format

**Endpoints:**

- Team-scoped: `GET /teams/{team}/proto_descriptors`
- Single-set (no teams): `GET /proto_descriptors`

Both endpoints use the same response format.

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
  provider/team/id that was retained vs dropped.

## Appendix C: Authentication Details

### Storage Schema

**Provider configuration** (in Perfetto settings):

```typescript
interface ProviderSettings {
  providers: {
    [provider_key: string]: {
      // Normalized provider key (see Appendix A)
      enabled: boolean;
      selected_teams: string[];
      team_mode: 'team_scoped' | 'single_set';
    };
  };
}

// Defaults:
// - User-added providers: enabled = false, selected_teams = [], team_mode inferred from discovery
// - Providers without /teams responses use team_mode='single_set' and treat selected_teams as []
// - Auto-added Google provider (Googlers only): enabled = true, selected_teams = ['perfetto'], team_mode='team_scoped'
```

**Credentials** (separate localStorage key `perfetto_provider_credentials`):

```typescript
interface ProviderCredentials {
  [provider_key: string]: {
    // Normalized provider key (see Appendix A)
    type:
      | 'github_oauth'
      | 'bearer_token'
      | 'basic_auth'
      | 'gcs_oauth'
      | 's3_keys';
    access_token?: string;
    refresh_token?: string;
    expires_at?: number;
    bearer_token?: string;
    username?: string;
    password?: string;
    access_key_id?: string;
    secret_access_key?: string;
    session_token?: string;
    last_used_at?: number;
  };
}
```

### Configuration Defaults & Bootstrap

- Providers whose `/teams` request returns 404 are marked
  `team_mode = 'single_set'` and expose a single toggle in Settings that
  controls access to `/macros`, `/sql_modules`, and `/proto_descriptors`.
- User-added providers are created disabled with `selected_teams: []`. For
  `single_set` providers that toggle simply tracks whether the provider is
  enabled.

### Provider Addition Flow (Settings UI)

1. User enters an HTTPS URL or supported alias.
2. Client normalizes and validates the input (including alias expansion).
3. `/teams` is fetched immediately:
   - `200` populates the returned team list and sets `team_mode='team_scoped'`.
   - `404` marks `team_mode='single_set'`.
   - Other failures add the provider in a disabled state and surface inline
     errors in the Settings UI.
4. On 401/403 the UI prompts for credentials before exposing team toggles.
5. Providers remain disabled until the user toggles `enabled` and selects teams.

### Credential Management UI

- Settings lists each provider credential record with the auth type label.
- Sensitive fields render masked with a "Reveal" action (confirmation required).
- `Delete credentials` clears stored secrets and forces re-auth on next use.
- `Force re-auth` replays the provider-specific authentication flow (GitHub
  Device Flow modal, OAuth popup, AWS key dialog, etc.).
- Credentials are scoped to the normalized provider key to avoid reuse across
  distinct providers.

### Provider Removal

When a user removes a provider from Settings:

- The provider configuration entry is deleted from the Perfetto settings in
  localStorage
- Associated credentials are deleted from `perfetto_provider_credentials` in
  localStorage
- The removal is permanent and cannot be undone
- Re-adding the same provider requires reconfiguring team selections and
  re-authenticating

### GitHub OAuth (Device Flow)

Uses GitHub App with Device Flow
([RFC 8628](https://datatracker.ietf.org/doc/html/rfc8628)) for fully
client-side authentication.

**Flow:**

1. User adds `github://` provider
2. UI tries to fetch without auth → receives 401/403
3. UI requests device code: `POST https://github.com/login/device/code`
   - `client_id`: Perfetto GitHub App ID (hardcoded in UI)
4. GitHub returns: `device_code`, `user_code`, `verification_uri`
5. UI shows modal:
   ```
   To authorize Perfetto:
   1. Visit: github.com/login/device
   2. Enter code: ABCD-1234
   [Copy Code] [Open GitHub]
   ```
6. User authorizes in separate browser tab/window
7. UI polls `POST https://github.com/login/oauth/access_token` until complete
8. Receives access token AND refresh token
9. Both tokens stored in localStorage
10. All requests use: `Authorization: Bearer {access_token}`

**Token Refresh:**

Access tokens expire after 8 hours. On 401 response:

```
POST https://github.com/login/oauth/access_token
  client_id: PERFETTO_GITHUB_CLIENT_ID
  grant_type: refresh_token
  refresh_token: {stored_refresh_token}
```

Refresh tokens do NOT require client secret for GitHub App Device Flow. If
refresh fails, re-run Device Flow.

**Per-Provider Authentication:**

Each `github://` provider URL gets own token. Supports multiple accounts (e.g.,
work + personal).

**Required GitHub App permissions:**

- Repository permissions: Contents (read-only)
- No account permissions

**URL Resolution:**

- `github://owner/repo/ref/path` →
  `https://raw.githubusercontent.com/owner/repo/ref/path`

### Google Cloud Storage

1. Try public access
2. On 401/403 → Initiate Google OAuth Sign-In in browser
3. Store OAuth token in localStorage
4. Retry with signed request

### AWS S3

1. Try public access
2. On 401/403 → Prompt for Access Key ID, Secret Access Key, optional Session
   Token
3. Store credentials in localStorage
4. Retry with SigV4-signed request

### HTTP/HTTPS

1. Try without auth
2. On 401/403 → Check `WWW-Authenticate` header
   - For Bearer: Prompt "Enter API token"
   - For Basic: Prompt "Enter username and password"
3. Store credentials in localStorage
4. Retry with `Authorization` header

## Appendix D: Provider Implementation Examples

### GitHub Static Hosting

**Repository structure:**

```
acme-corp/perfetto-resources/
├── teams/
│   ├── android/
│   │   ├── macros              (all android macros in one JSON file)
│   │   ├── sql_modules         (all android SQL modules in one JSON file)
│   │   └── proto_descriptors   (all android proto descriptors in one JSON file)
│   └── chrome/
│       ├── macros
│       └── sql_modules
└── teams                       (JSON file with team list)
```

**File: `teams`**

```json
{
  "teams": ["android", "chrome"]
}
```

**File: `teams/android/macros`**

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

`github://acme-corp/perfetto-resources/main/teams/android/macros`

→
`https://raw.githubusercontent.com/acme-corp/perfetto-resources/main/teams/android/macros`

**Authentication:** Automatic via GitHub OAuth if repo is private.

### Corporate Symbol Server (Flask)

```python
from flask import Flask, send_file, jsonify
from flask_cors import CORS

app = Flask(__name__)
CORS(app, origins=["https://ui.perfetto.dev"])

@app.route('/teams')
def list_teams():
    return jsonify({"teams": ["android", "chrome", "infra"]})

@app.route('/teams/<team>/macros')
def get_team_macros(team):
    macro_file = f'/data/teams/{team}/macros'
    if os.path.exists(macro_file):
        return send_file(macro_file, mimetype='application/json')
    return '', 404

@app.route('/teams/<team>/sql_modules')
def get_team_sql_modules(team):
    sql_file = f'/data/teams/{team}/sql_modules'
    if os.path.exists(sql_file):
        return send_file(sql_file, mimetype='application/json')
    return '', 404

@app.route('/teams/<team>/proto_descriptors')
def get_team_proto_descriptors(team):
    proto_file = f'/data/teams/{team}/proto_descriptors'
    if os.path.exists(proto_file):
        return send_file(proto_file, mimetype='application/json')
    return '', 404

if __name__ == '__main__':
    # Production: use proper SSL certificates
    # app.run(host='0.0.0.0', port=443, ssl_context=('/path/to/cert.pem', '/path/to/key.pem'))

    # Development only: adhoc creates self-signed certificates
    app.run(host='0.0.0.0', port=443, ssl_context='adhoc')
```

**Authentication:** Corporate SSO/OAuth2/OIDC. Provider handles auth, Perfetto
sends tokens in `Authorization` header.

### UI Integration Example

```typescript
class ProviderService {
  private providers: Map<string, ProviderConfig>;

  async discoverTeams(provider: string): Promise<string[] | null> {
    try {
      const response = await fetch(`${provider}/teams`);
      if (response.ok) {
        const data = await response.json();
        return data.teams;
      }
      if (response.status === 404) {
        return null; // No team support
      }
    } catch (e) {
      console.error('Failed to discover teams', e);
    }
    return null;
  }

  async loadResources(
    provider: string,
    team: string | null,
    resourceType: 'macros' | 'sql_modules' | 'proto_descriptors',
  ): Promise<Record<string, any> | null> {
    const url = team
      ? `${provider}/teams/${team}/${resourceType}`
      : `${provider}/${resourceType}`;

    try {
      const headers = await this.getAuthHeaders(provider);
      const response = await fetch(url, {headers});

      if (response.status === 401 || response.status === 403) {
        await this.handleAuthRequired(provider);
        return this.loadResources(provider, team, resourceType); // Retry
      }

      if (response.ok) {
        return await response.json();
      }
    } catch (e) {
      console.error(`Failed to load ${resourceType}`, e);
    }
    return null;
  }

  private async getAuthHeaders(provider: string): Promise<Headers> {
    const creds = this.getStoredCredentials(provider);
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

  private getStoredCredentials(provider: string): any {
    const credsJson = localStorage.getItem('perfetto_provider_credentials');
    if (!credsJson) return null;
    const allCreds = JSON.parse(credsJson);
    return allCreds[provider];
  }

  private async handleAuthRequired(provider: string): Promise<void> {
    // Trigger provider-specific auth flow (GitHub OAuth, token prompt, etc.)
    // Implementation depends on provider type
  }

  async loadAllExtensions(): Promise<void> {
    for (const [providerUrl, config] of this.providers) {
      if (config.selectedTeams && config.selectedTeams.length > 0) {
        for (const team of config.selectedTeams) {
          await this.loadAndRegister(providerUrl, team);
        }
      } else {
        await this.loadAndRegister(providerUrl, null);
      }
    }
  }

  private async loadAndRegister(
    provider: string,
    team: string | null,
  ): Promise<void> {
    const macros = await this.loadResources(provider, team, 'macros');
    const sqlModules = await this.loadResources(provider, team, 'sql_modules');
    const protoDescs = await this.loadResources(
      provider,
      team,
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
- Provider-based: users configure URLs, UI aggregates extensions from all
  enabled providers
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

**Endpoint Specification:** 5. ✅ Versioning: No versioning initially, add
`/v2/` when needed 6. ✅ Partial success: UI extensions aggregate across all
selected providers; symbolization/deobfuscation fall back to first-success
semantics in the followup RFC 7. ✅ JSON format: snake_case 8. ✅ Compression:
Optional, browser-handled

**Symbolization & Deobfuscation:** 9. ✅ All deferred to separate RFC

**Performance & Reliability:** 13. ✅ Timeouts: Fixed (5s/10s/15s/30s),
fail-fast 14. ✅ Retry: No retry, try next provider immediately 15. ✅ Provider
queries: Serial (one at a time) 16. ✅ Rate limiting: Respect Retry-After
headers

**Security & Privacy:** 17. ✅ Token storage: Plaintext in localStorage
(separate from settings) 18. ✅ Data sensitivity: Implicit consent for
extensions (no trace data sent) 19. ✅ Code execution: No JavaScript plugins,
only declarative

**User Experience:** 20. ✅ Error reporting: Console logging only for MVP 21. ✅
Health monitoring: No monitoring for MVP 22. ✅ Migration: One-shot when
provider server ready 23. ✅ Discovery: Settings instructions (Provider Store
catalog deferred)

**Teams & Filtering:** 24. ✅ Team discovery: Automatic when adding provider 25.
✅ Team hierarchy: Flat only (use dashes for organization) 26. ✅ Team
permissions: No team-level ACL (provider-level only) 27. ✅ Team wildcards: No
wildcards 28. ✅ No teams support: Fallback to root paths

**GitHub App OAuth:** 29. ✅ Scopes: Repository Contents (read-only) only 30. ✅
Token refresh: Use refresh tokens, auto-refresh 31. ✅ Hosting: Client-side only
(Device Flow) 32. ✅ Multiple accounts: Per-provider auth (natural support)

### Implementation Checklist

**Phase 1: Core Provider Infrastructure**

- [ ] Provider config schema (localStorage)
- [ ] Provider fan-out orchestration (aggregate UI extensions from all enabled
      providers/teams)
- [ ] Alias resolution + provider key normalization helpers
- [ ] Auth header injection
- [ ] Error handling and logging

**Phase 2: UI Extensions**

- [ ] Team discovery endpoint (`/teams`)
- [ ] Extension endpoints (macros, SQL modules, proto descriptors)
- [ ] Extension loading with team filtering
- [ ] GitHub App OAuth (Device Flow)
- [ ] Replace `is_internal_user_script_loader.ts`
- [ ] Default Google provider bootstrap (auto-add + perfetto team selection
      guidance)

**Phase 2.5: Post-MVP Enhancements**

- [ ] Provider Store (requires dedicated Store UX RFC)
- [ ] URL parameter support (`?add_provider=...`)

**Phase 3: Local HTTP Accelerator (Future)**

- [ ] Extend `trace_processor --httpd`
- [ ] Local filesystem discovery

**Phase 4: Symbolization & Deobfuscation (Separate RFC)**

- [ ] Write separate RFC
- [ ] Implement endpoints
- [ ] Browser integration
- [ ] Caching strategies

### Prerequisites

**Before Phase 1:**

1. Register Perfetto GitHub App with GitHub
2. Set up internal Google provider server

**Before Phase 2:** 3. Create example GitHub repo with test extensions 4.
Finalize localStorage settings schema

### Files to Modify/Create

**New files:**

- `ui/src/core/providers/provider_service.ts` - Core provider logic
- `ui/src/core/providers/provider_config.ts` - Config schema

**Files to modify:**

- `ui/src/frontend/is_internal_user_script_loader.ts` - Remove/replace

**Deferred new files (post-MVP):**

- `ui/src/core/providers/store.ts` - Provider Store (curated list + shortcuts)

### Testing Strategy

**Unit tests:**

- Provider config parsing
- Auth header injection
- Extension aggregation logic with mocks
- Error handling
- Conflict filtering/logging for duplicate SQL modules and proto descriptors
- Deterministic ordering across provider/team/resource aggregation
- Alias resolution and provider key normalization helpers

**Integration tests:**

- Real GitHub repo with test extensions
- Mock providers with various response patterns
- Auth with test tokens

**E2E tests:**

- Add provider → select teams → reload → extensions loaded
- Install macro from GitHub → run macro → verify behavior

### Key Technical Challenges

1. **Browser filesystem access** - Solved via local httpd (Phase 3)
2. **CORS** - All providers must support CORS
3. **GitHub OAuth** - Device Flow in browser without backend
4. **Team discovery UX** - Immediate discovery, Settings UI for selection
5. **Credential security** - localStorage with same-origin protection

### Success Criteria

**MVP (Phase 1-2):**

- Users can configure providers via Settings
- UI loads extensions from GitHub or HTTPS
- Replace `is_internal_user_script_loader.ts`
- Team selection works
- GitHub OAuth works

**Future (Phase 3-4):**

- Local httpd for filesystem access
- Symbolization/deobfuscation (separate RFC)

### Important Constraints

- **No custom auth** - Hard requirement
- **Standard protocols** - HTTP/HTTPS only
- **No code execution** - Declarative extensions only
- **Simple** - Avoid complexity
