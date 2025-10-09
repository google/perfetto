# Extension Server Authentication

**Authors:** @LalitMaganti

**Status:** Draft

## Problem

RFC-0005 (Perfetto UI Server Extensions) defines a system where users can
configure HTTP(S) endpoints to serve Perfetto UI extensions (macros, SQL
modules, proto descriptors). These extension servers may require authentication
to access:

1. **Private repositories**: GitHub repos, GCS buckets, S3 buckets with
   restricted access
2. **Corporate servers**: Internal servers behind SSO/OAuth2/OIDC
3. **Mixed access patterns**: Some endpoints public, others requiring auth

The authentication system needs to:

- Work entirely client-side (Perfetto UI has no backend)
- Support multiple authentication mechanisms
- Store credentials securely in the browser
- Handle token refresh automatically
- Support multiple servers with different credentials

## Decision

Implement a flexible authentication system that:

1. **Tries public access first** - No credentials required for public servers
2. **Prompts on-demand** - Only request credentials when needed (401/403)
3. **Supports multiple mechanisms** - GitHub OAuth, GCS, S3, Bearer tokens,
   Basic auth
4. **Stores credentials per-server** - Each server has independent credentials
5. **Auto-refreshes OAuth tokens** - No manual token management required

## Design

### Authentication Flow

**High-level flow for all servers:**

1. User adds extension server URL in Settings
2. UI attempts to fetch `/manifest.json` without credentials
3. If **200 OK** → Server is public, proceed
4. If **401/403** → Prompt user for credentials via provider-specific flow
5. Store credentials in localStorage (separate from settings)
6. Retry request with credentials
7. On subsequent requests, include stored credentials automatically
8. Auto-refresh OAuth tokens when they expire

### Storage Schema

**Extension Server configuration** (in Perfetto settings localStorage):

```typescript
interface ServerSettings {
  servers: {
    [server_key: string]: {
      // Normalized server key (see RFC-0005 Appendix A)
      enabled: boolean;
      selected_modules: string[];
      // Display name from manifest
      display_name?: string;
    };
  };
}
```

**Credentials** (separate localStorage key `perfetto_server_credentials`):

```typescript
interface ServerCredentials {
  [server_key: string]: {
    // Normalized server key
    type:
      | 'github_oauth'
      | 'bearer_token'
      | 'basic_auth'
      | 'gcs_oauth'
      | 's3_keys';

    // GitHub OAuth (Device Flow)
    access_token?: string;
    refresh_token?: string;
    expires_at?: number;

    // Bearer token
    bearer_token?: string;

    // Basic auth
    username?: string;
    password?: string;

    // S3
    access_key_id?: string;
    secret_access_key?: string;
    session_token?: string;

    // Metadata
    last_used_at?: number;
    created_at?: number;
  };
}
```

### GitHub OAuth (Device Flow)

**Why Device Flow?**

- Fully client-side (no backend required)
- Users authorize via github.com (trusted domain)
- Refresh tokens for long-lived access
- Standard OAuth 2.0 flow (RFC 8628)

**Prerequisites:**

- Perfetto GitHub App registered with GitHub
- App permissions: Repository Contents (read-only)
- Client ID hardcoded in UI (public, no secret needed)

**Flow:**

1. User adds `github://owner/repo/ref` server in Settings
2. UI attempts fetch without auth → receives 401/403
3. UI requests device code:
   ```
   POST https://github.com/login/device/code
   {
     "client_id": "PERFETTO_GITHUB_CLIENT_ID"
   }
   ```
4. GitHub returns:
   ```json
   {
     "device_code": "3584d83...",
     "user_code": "ABCD-1234",
     "verification_uri": "https://github.com/login/device",
     "expires_in": 900,
     "interval": 5
   }
   ```
5. UI shows modal with instructions:

   ```
   To authorize Perfetto to access GitHub:

   1. Visit: github.com/login/device
   2. Enter code: ABCD-1234

   [Copy Code]  [Open GitHub]

   Waiting for authorization...
   ```

6. User opens GitHub in separate tab/window and enters code
7. UI polls token endpoint every 5 seconds:
   ```
   POST https://github.com/login/oauth/access_token
   {
     "client_id": "PERFETTO_GITHUB_CLIENT_ID",
     "device_code": "3584d83...",
     "grant_type": "urn:ietf:params:oauth:grant-type:device_code"
   }
   ```
8. On success, GitHub returns:
   ```json
   {
     "access_token": "gho_...",
     "token_type": "bearer",
     "scope": "repo",
     "refresh_token": "ghr_...",
     "refresh_token_expires_in": 15552000,
     "expires_in": 28800
   }
   ```
9. Store both tokens in `perfetto_server_credentials`
10. Retry original request with `Authorization: Bearer {access_token}`

**Token Refresh:**

Access tokens expire after 8 hours. When a request returns 401:

```
POST https://github.com/login/oauth/access_token
{
  "client_id": "PERFETTO_GITHUB_CLIENT_ID",
  "grant_type": "refresh_token",
  "refresh_token": "ghr_..."
}
```

GitHub returns new access token and refresh token. Update stored credentials.

**Note:** GitHub Device Flow refresh tokens do NOT require client secret.

**Per-Server Authentication:**

Each `github://` URL gets independent credentials. This supports multiple
accounts (e.g., personal `github://user1/repo` and work `github://org/repo` with
different auth).

**URL Resolution:**

- `github://owner/repo/ref` → `https://raw.githubusercontent.com/owner/repo/ref`
- Credentials stored under normalized server key (e.g.,
  `raw-githubusercontent-com-owner-repo-ref`)

### Google Cloud Storage (GCS)

**Flow:**

1. User adds `gs://bucket/path` server
2. UI attempts fetch without auth → receives 401/403
3. UI opens Google OAuth Sign-In popup:
   ```typescript
   const auth = gapi.auth2.getAuthInstance();
   await auth.signIn({
     scope: 'https://www.googleapis.com/auth/devstorage.read_only',
   });
   ```
4. User authorizes in popup
5. Get OAuth token: `auth.currentUser.get().getAuthResponse().access_token`
6. Store token in `perfetto_server_credentials`
7. Retry request with `Authorization: Bearer {token}`

**Token Refresh:**

Use Google Identity Services library to handle automatic token refresh:

```typescript
const token = await auth.currentUser.get().reloadAuthResponse();
```

**URL Resolution:**

- `gs://bucket/path` → `https://storage.googleapis.com/bucket/path`

### AWS S3

**Flow:**

1. User adds `s3://bucket/path` server
2. UI attempts fetch without auth → receives 401/403
3. UI shows credential input dialog:

   ```
   AWS Credentials Required

   Access Key ID: [_____________]
   Secret Access Key: [_____________]
   Session Token (optional): [_____________]

   [Cancel]  [Save]
   ```

4. User enters credentials
5. Store credentials in `perfetto_server_credentials`
6. Sign requests using AWS Signature Version 4 (SigV4)
7. Retry request with signed headers

**SigV4 Signing:**

Use aws4fetch library for client-side SigV4 signing:

```typescript
import {AwsClient} from 'aws4fetch';

const aws = new AwsClient({
  accessKeyId: credentials.access_key_id,
  secretAccessKey: credentials.secret_access_key,
  sessionToken: credentials.session_token,
});

const response = await aws.fetch(url);
```

**URL Resolution:**

- `s3://bucket/path` → `https://bucket.s3.amazonaws.com/path`

### HTTP/HTTPS (Generic)

**Flow:**

1. User adds `https://server.example.com` server
2. UI attempts fetch without auth → receives 401/403
3. Check `WWW-Authenticate` response header:
   - **`Bearer realm="..."`** → Prompt for Bearer token
   - **`Basic realm="..."`** → Prompt for username/password
4. Show appropriate credential input dialog
5. Store credentials in `perfetto_server_credentials`
6. Retry request with appropriate `Authorization` header

**Bearer Token:**

```
Authorization: Bearer {token}
```

**Basic Auth:**

```
Authorization: Basic {base64(username:password)}
```

### Server Addition Flow (Settings UI)

**Step-by-step:**

1. User clicks "Add Extension Server" in Settings
2. Enter URL (HTTPS or alias like `github://`, `gs://`, `s3://`)
3. UI normalizes URL and generates server key
4. UI fetches `/manifest.json`:
   - **Public (200 OK):**
     - Parse manifest (name, modules, csp_allow)
     - Show module checkboxes
     - Server added in disabled state
   - **Auth required (401/403):**
     - Detect auth type from URL scheme or WWW-Authenticate header
     - Show appropriate auth flow (GitHub Device Flow, GCS popup, credentials
       form)
     - Store credentials on success
     - Retry manifest fetch
     - Show module checkboxes on success
   - **Other error:**
     - Show error message inline
     - Allow retry or cancel
5. User selects modules to enable
6. Click "Save" → Server added to settings
7. Reload page to load extensions

### Credential Management UI

**Settings → Extension Servers → [Server] → Credentials:**

- **View**: Show auth type (e.g., "GitHub OAuth", "Bearer Token", "Basic Auth")
- **Reveal**: Show masked credentials with confirmation
  - Access tokens: `gho_••••••••••••••••`
  - Passwords: `••••••••••`
  - Click "Reveal" → Confirm → Show plaintext for 10 seconds
- **Delete**: Remove stored credentials
  - Next request will prompt for re-auth
- **Re-authenticate**: Force new auth flow
  - GitHub: Open Device Flow modal again
  - GCS: Open OAuth popup again
  - S3/HTTPS: Show credential form again

**Credential List:**

```
Extension Servers
├─ Google Internal Extensions
│  ├─ Status: Enabled
│  ├─ Modules: default, android
│  ├─ Auth: GitHub OAuth
│  └─ [View Credentials] [Re-authenticate] [Delete Credentials]
├─ Acme Corp Extensions
│  ├─ Status: Disabled
│  ├─ Modules: (none selected)
│  ├─ Auth: Bearer Token
│  └─ [View Credentials] [Re-authenticate] [Delete Credentials]
```

### Server Removal

When user removes a server from Settings:

1. Confirm removal (show warning about data loss)
2. Delete server config from Perfetto settings
3. Delete associated credentials from `perfetto_server_credentials`
4. Removal is permanent
5. Re-adding requires reconfiguring modules and re-authenticating

### Error Handling

**Common error scenarios:**

1. **Expired token (401):**

   - Try token refresh (OAuth flows)
   - If refresh fails → Prompt for re-auth
   - Show notification: "Credentials expired for [Server]. Please
     re-authenticate."

2. **Invalid credentials (401/403):**

   - Show error: "Authentication failed. Please check credentials."
   - Prompt for re-auth

3. **Network error:**

   - Retry with exponential backoff (3 attempts)
   - Log to console
   - Skip server (don't block UI startup)

4. **CORS error:**
   - Log to console: "CORS error for [Server]. Check server CORS configuration."
   - Skip server
   - Show in Settings UI: "⚠️ CORS configuration issue"

### Security Considerations

**Credential Storage:**

- Stored in localStorage in **plaintext**
- Protected by browser same-origin policy
- Separate from Perfetto settings (not exported/synced)
- Rationale: Perfetto runs entirely in-browser without privileged services.
  Browser's same-origin protection is the security boundary.

**No credential encryption** because:

- No secure key storage available in browser without backend
- Browser process memory is the security boundary
- Same approach as VS Code, GitHub Desktop, other Electron apps

**User controls:**

- View/delete credentials in Settings
- Clear credentials on sign-out (if implemented)
- Credentials never sent to perfetto.dev servers

**OAuth security:**

- GitHub Device Flow uses user's GitHub session (2FA supported)
- Access tokens scoped to read-only repository access
- Refresh tokens have 180-day expiration
- Users can revoke via GitHub Settings → Applications

**CORS requirements:**

All HTTPS servers must set CORS headers:

```
Access-Control-Allow-Origin: https://ui.perfetto.dev
Access-Control-Allow-Methods: GET, POST
Access-Control-Allow-Headers: Authorization, Content-Type
```

### Implementation Timeline

**Phase 1: Core Auth Infrastructure**

- Credential storage schema
- Auth header injection
- Error handling for 401/403
- Credential management UI (view/delete/re-auth)

**Phase 2: GitHub OAuth**

- Register Perfetto GitHub App
- Implement Device Flow
- Token refresh logic
- Test with private GitHub repos

**Phase 3: Cloud Storage**

- GCS OAuth integration (Google Identity Services)
- S3 SigV4 signing (aws4fetch library)
- Test with private buckets

**Phase 4: Generic HTTPS**

- Bearer token flow
- Basic auth flow
- WWW-Authenticate header parsing

**Phase 5: Polish**

- Credential reveal with confirmation
- Better error messages
- Auth status indicators in Settings

## Alternatives Considered

### Alternative 1: Proxy Server for Auth

**Approach:** Run a Perfetto-managed proxy server that handles auth and forwards
requests.

**Pros:**

- Centralized credential storage
- Could implement OAuth flows with client secrets
- Users don't manage credentials

**Cons:**

- Single point of failure
- Privacy concerns (Perfetto sees all extension requests)
- Doesn't work for corporate internal servers
- Requires running/maintaining proxy infrastructure
- **Rejected** - Goes against "no backend" principle

### Alternative 2: Browser Extension for Credentials

**Approach:** Build a browser extension to manage credentials with OS keychain
integration.

**Pros:**

- More secure credential storage (OS keychain)
- Could inject credentials without CORS issues

**Cons:**

- Requires installing extension (barrier to adoption)
- Only works in Chrome/Firefox (not mobile)
- Adds complexity for minimal security gain
- **Rejected** - localStorage with same-origin is sufficient

### Alternative 3: Service Worker as Credential Manager

**Approach:** Use service worker to intercept requests and inject credentials.

**Pros:**

- Can handle requests before CORS checks
- Credentials never exposed to main UI thread

**Cons:**

- Service worker lifecycle is complex
- Doesn't solve credential storage problem
- Still uses localStorage or IndexedDB
- **Rejected** - Adds complexity without meaningful security improvement

### Alternative 4: Server-Side Session Tokens

**Approach:** Perfetto backend issues session tokens, stores credentials
server-side.

**Pros:**

- Credentials never in browser
- Centralized token revocation

**Cons:**

- Requires Perfetto backend (violates "no backend" principle)
- Users must create Perfetto accounts
- Doesn't work for corporate internal servers
- **Hard Rejected** - Fundamentally incompatible with architecture

## Open Questions

### Q1: Should we support OAuth client secrets for GitHub?

**Context:** GitHub Device Flow doesn't require client secrets, but traditional
OAuth flows do.

**Current Decision:** No. Device Flow is sufficient and simpler.

**Rationale:** Client secrets can't be kept secret in client-side JavaScript
anyway. Device Flow is the recommended approach for native/mobile apps and works
well here.

### Q2: Should we support multiple GitHub accounts per server?

**Context:** User might want to access same repo with different GitHub accounts
depending on context.

**Current Decision:** No for MVP. Each server URL gets one credential set.

**Workaround:** Add the same repo twice with different URL paths (e.g.,
`github://org/repo/main` and `github://org/repo/main/` with trailing slash maps
to different server key).

**Future:** Could add "account switcher" in Settings if users request it.

### Q3: Should credentials have expiration/TTL beyond OAuth token expiration?

**Context:** Security best practice is to expire credentials periodically.

**Current Decision:** No explicit TTL beyond OAuth token expiration.

**Rationale:**

- OAuth tokens already expire (8 hours for GitHub)
- Users can delete credentials manually
- Adding TTL adds complexity without clear benefit
- If users want to rotate, they can delete and re-add

### Q4: How to handle credential conflicts when adding same server twice?

**Context:** User adds `github://org/repo/main` twice (maybe deleted and
re-added).

**Current Decision:** Overwrite existing credentials for the same server key.

**UI Flow:**

1. Detect server key already exists
2. Show warning: "This server already exists. Re-authenticate to update
   credentials?"
3. On confirm → Delete old credentials → Run auth flow → Store new credentials

### Q5: Should we support password managers (autofill)?

**Context:** Browser password managers can autofill username/password for Basic
auth.

**Current Decision:** Yes, use standard HTML input elements with appropriate
`autocomplete` attributes.

**Implementation:**

```html
<input type="text" name="username" autocomplete="username" />
<input type="password" name="password" autocomplete="current-password" />
```

This allows password managers to recognize and autofill credentials.

---

## Appendix A: GitHub App Registration

**Settings for Perfetto GitHub App:**

- **App name:** Perfetto UI Extension Loader
- **Homepage URL:** https://perfetto.dev
- **Callback URL:** Not needed (Device Flow)
- **Request user authorization (OAuth) during installation:** No
- **Webhook:** Disabled
- **Permissions:**
  - Repository permissions:
    - Contents: Read-only
  - Account permissions: None
- **Where can this GitHub App be installed?:** Any account

**After registration:**

- Note Client ID (will be hardcoded in UI)
- No client secret needed for Device Flow

## Appendix B: Testing Strategy

**Unit Tests:**

- Credential storage/retrieval
- Auth header injection
- Token expiration detection
- Server key normalization
- OAuth response parsing

**Integration Tests:**

- GitHub Device Flow (mock OAuth endpoints)
- Token refresh (mock expired token scenario)
- GCS OAuth (mock Google Identity Services)
- S3 signing (verify SigV4 signature)
- Basic auth (verify Base64 encoding)

**E2E Tests:**

- Add private GitHub repo → Device Flow → Extensions load
- Add public server → No auth → Extensions load
- Token expires → Auto-refresh → Request succeeds
- Invalid credentials → Error message → Re-auth prompt
- Delete credentials → Next request prompts for auth

**Manual Testing:**

- Test with real private GitHub repo
- Test with real GCS bucket (corporate account)
- Test with real S3 bucket
- Test with corporate HTTPS server (SSO/OAuth2)
- Test credential management UI (view/delete/re-auth)
- Test CORS errors
- Test network errors

## Appendix C: Libraries and Dependencies

**GitHub OAuth:**

- **Built-in fetch API** - For all OAuth requests
- No external library needed (Device Flow is simple HTTP requests)

**GCS OAuth:**

- **Google Identity Services** - `https://accounts.google.com/gsi/client`
- Loaded dynamically when needed

**S3 Signing:**

- **aws4fetch** - Client-side AWS SigV4 signing library
- Lightweight (5KB gzipped)
- Pure JavaScript, no AWS SDK needed
- NPM: `aws4fetch`

**Basic Auth:**

- Built-in `btoa()` for Base64 encoding
- No external library needed

**TypeScript Types:**

```typescript
// Add to package.json devDependencies
"@types/gapi": "^0.0.44",
"@types/gapi.auth2": "^0.0.57"
```
