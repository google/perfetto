# Extension Server Authentication

**Authors:** @LalitMaganti

**Status:** Adopted

## Problem

RFC-0005 (Perfetto UI Server Extensions) defines a system where users can
configure HTTP(S) endpoints to serve Perfetto UI extensions (macros, SQL
modules, proto descriptors). These extension servers may require authentication
to access:

1. **Private repositories**: GitHub repos with restricted access
2. **Corporate servers**: Internal servers behind SSO
3. **API-protected servers**: Servers requiring API keys or tokens

The authentication system needs to:

- Work entirely client-side (Perfetto UI has no backend)
- Support multiple authentication mechanisms
- Store credentials alongside server configuration
- Support multiple servers with different credentials

## Decision

Implement authentication as part of the server configuration itself, using
per-server-type discriminated unions for auth options. Credentials are stored
inline with the server settings in localStorage, with secret fields annotated
via Zod metadata so they can be stripped when sharing server configurations.

## Design

### Authentication Per Server Type

Authentication is configured when adding or editing a server in Settings. Each
server type (GitHub, HTTPS) has its own set of supported auth mechanisms,
modeled as discriminated unions.

**GitHub servers:**

| Auth type    | Description                          |
|--------------|--------------------------------------|
| `none`       | Public repos (no auth required)      |
| `github_pat` | Personal Access Token for private repos |

**HTTPS servers:**

| Auth type      | Description                                      |
|----------------|--------------------------------------------------|
| `none`         | Public servers (no auth required)                |
| `https_basic`  | Username/password (Basic auth header)            |
| `https_apikey` | API key via Bearer, X-API-Key, or custom header  |
| `https_sso`    | Cookie-based SSO with automatic iframe refresh   |

### Storage

Credentials are stored **inline** with the server configuration in the Perfetto
settings system (localStorage). There is no separate credential store.

Secret fields (PAT, passwords, API keys) are annotated with
`.meta({secret: true})` in the Zod schema. This metadata is used to strip
secrets when generating shareable server URLs.

```typescript
// GitHub auth
const githubAuthSchema = z.discriminatedUnion('type', [
  z.object({type: z.literal('none')}),
  z.object({
    type: z.literal('github_pat'),
    pat: z.string().meta({secret: true}).default(''),
  }),
]);

// HTTPS auth
const httpsAuthSchema = z.discriminatedUnion('type', [
  z.object({type: z.literal('none')}),
  z.object({
    type: z.literal('https_basic'),
    username: z.string().meta({secret: true}).default(''),
    password: z.string().meta({secret: true}).default(''),
  }),
  z.object({
    type: z.literal('https_apikey'),
    keyType: z.enum(['bearer', 'x_api_key', 'custom']).default('bearer'),
    key: z.string().meta({secret: true}).default(''),
    customHeaderName: z.string().default(''),
  }),
  z.object({type: z.literal('https_sso')}),
]);
```

### Request Construction

All URL construction and auth header injection happens in a single
`buildFetchRequest()` function that takes a server configuration and a resource
path, producing a URL and `RequestInit`:

**GitHub PAT:**
- Uses the GitHub API (`api.github.com/repos/.../contents/...`) instead of
  `raw.githubusercontent.com`
- Adds `Authorization: token {pat}` header
- Adds `Accept: application/vnd.github.raw+json` header

**GitHub (no auth):**
- Uses `raw.githubusercontent.com` directly (avoids GitHub API rate limits)

**HTTPS Basic:**
- Adds `Authorization: Basic {base64(username:password)}` header

**HTTPS API Key:**
- `bearer` → `Authorization: Bearer {key}`
- `x_api_key` → `X-API-Key: {key}`
- `custom` → `{customHeaderName}: {key}`

**HTTPS SSO:**
- Adds `credentials: 'include'` to the fetch request to send cookies

### SSO Cookie Refresh

For SSO-authenticated servers, a 403 response may indicate an expired session
cookie. The UI handles this automatically:

1. On 403 from an SSO server, load the server's base URL in a hidden iframe
2. The iframe follows SSO redirects; on `onload`, the browser has fresh cookies
3. Retry the original request once with the refreshed cookies
4. If the retry also fails, report the error normally

The iframe has a 10-second timeout. This flow is transparent to the user.

### Server Addition Flow

1. User clicks "Add Server" in Settings → Extension Servers
2. Modal appears with server type selection (GitHub or HTTPS)
3. User fills in connection details and selects auth type
4. UI fetches manifest to discover modules
5. User selects which modules to enable
6. Click "Save" to persist; page reload applies changes

Auth configuration happens upfront during server addition, not on-demand after
a 401/403. This is simpler and avoids the complexity of detecting auth
requirements at runtime.

### Sharing Servers

Servers can be shared via URL (`?addServer=<base64>`). When generating the
share URL, the `stripSecrets()` function removes all fields annotated with
`.meta({secret: true})`, replacing the auth with `{type: 'none'}`. The
recipient must configure their own credentials after importing.

### Locked (Embedder-Configured) Servers

Embedder-configured servers (e.g., Google's internal server auto-added for
Googlers) are marked as `locked: true`. Locked servers:

- Cannot be deleted or have their connection details edited
- Can still be enabled/disabled by the user
- Can have their module selection changed
- Auth type is set by the embedder (e.g., SSO for corporate servers)

## Security Considerations

**Credential Storage:**

- Stored in localStorage in plaintext alongside server configuration
- Protected by browser same-origin policy
- Secret fields annotated with Zod metadata for stripping during export/share
- Rationale: Perfetto runs entirely in-browser without privileged services.
  Browser's same-origin protection is the security boundary. This follows
  industry practice (GitHub, VS Code, etc.).

**User controls:**

- View/edit/delete credentials via the server edit modal
- Credentials never sent to perfetto.dev servers
- Shared URLs have credentials automatically stripped

**CORS requirements:**

All HTTPS extension servers must set CORS headers:

```
Access-Control-Allow-Origin: https://ui.perfetto.dev
Access-Control-Allow-Methods: GET, POST
Access-Control-Allow-Headers: Authorization, Content-Type, X-API-Key
```

SSO servers additionally need `Access-Control-Allow-Credentials: true`.

## Future Work

- **GitHub OAuth Device Flow**: Could replace PAT with proper OAuth for better
  UX (no manual token creation). Requires registering a Perfetto GitHub App.
- **GCS/S3 support**: Cloud storage authentication for extension servers hosted
  on Google Cloud Storage or AWS S3.
- **Credential reveal UI**: Show masked credentials with click-to-reveal.
- **Token refresh**: Automatic refresh for OAuth-based tokens.
