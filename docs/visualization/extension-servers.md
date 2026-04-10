# Extension Servers

Extension servers are HTTP(S) endpoints that distribute shared
[macros](/docs/visualization/ui-automation.md), SQL modules, and proto
descriptors to the Perfetto UI. They are the recommended way for teams and
organizations to share reusable trace analysis workflows.

## Why extension servers?

Without extension servers, sharing macros or SQL modules means copy-pasting JSON
between teammates. This doesn't scale: definitions get out of date, new team
members miss them, and there's no single source of truth. Extension servers solve
this by letting you host extensions in one place and have everyone load them
automatically.

## Key properties

Extension servers are **optional and never load-bearing**. The Perfetto UI works
fully without any servers configured — servers only provide optional
enhancements.

All extensions are **declarative and safe**:

- **Macros** are sequences of UI commands — no JavaScript execution.
- **SQL modules** run inside trace processor's existing sandbox.
- **Proto descriptors** are binary type definitions (data only).

## What extension servers provide

### Macros

Macros loaded from extension servers appear in the command palette
(`Ctrl+Shift+P`) alongside locally-defined macros. They show the server and
module name as a source label so you can tell where each macro comes from.

See [Commands and Macros](/docs/visualization/ui-automation.md) for how macros
work.

### SQL modules

SQL modules become available for use in the query editor and in
[startup commands](/docs/visualization/ui-automation.md). Use them with:

```sql
INCLUDE PERFETTO MODULE <namespace>.<module_name>;
```

For example, if a server with namespace `com.acme` provides a module called
`com.acme.startup`, you would write:

```sql
INCLUDE PERFETTO MODULE com.acme.startup;
```

See
[PerfettoSQL Getting Started](/docs/analysis/perfetto-sql-getting-started.md)
for more on SQL modules.

### Proto descriptors

Proto descriptors allow the UI to decode and display custom protobuf messages
embedded in traces without needing the `.proto` files compiled into the UI.
These are registered automatically when the extension server loads.

## How extension servers work

Each extension server hosts a **manifest** that declares:

- The server's **name** and **namespace** (e.g., `com.acme`)
- Which **features** it supports (macros, SQL modules, proto descriptors)
- Which **modules** are available (named collections of extensions)

When the Perfetto UI starts, it fetches the manifest from each configured server,
then loads extensions from the enabled modules. If a server is unreachable or
returns errors, it is skipped — other servers and the rest of the UI are
unaffected.

### Modules

Extension servers organize content into **modules**. For example, a corporate
server might offer:

- `default` — General-purpose macros and SQL modules
- `android` — Android-specific analysis workflows
- `chrome` — Chrome rendering performance tools

When you add a server, the `default` module is selected automatically. Other
modules are opt-in.

### Namespacing

All macro IDs and SQL module names from a server must start with the server's
namespace (e.g., `com.acme.`). This prevents naming conflicts when multiple
extension servers are configured. The UI enforces this and rejects extensions
that violate the convention.

### Lifecycle

Extensions are loaded **once at UI startup**. If you change your extension
server configuration (add/remove servers, change modules), you need to reload
the page for changes to take effect.

## Server types and authentication

Extension servers come in two types:

- **GitHub** — Extensions hosted in a GitHub repository. The UI fetches files
  directly from `raw.githubusercontent.com` (public repos) or the GitHub API
  (private repos). This is the easiest option — no server infrastructure needed.
- **HTTPS** — Extensions hosted on any HTTPS endpoint: a static file host
  (GCS, S3, nginx), a dynamic server (Flask, Express), or corporate
  infrastructure. The server must set
  [CORS headers](/docs/visualization/extension-server-protocol.md#cors-requirements)
  to allow the Perfetto UI to fetch from it.

Each server type supports different authentication methods:

| Server type | Auth method | When to use |
|-------------|-------------|-------------|
| GitHub | None | Public repositories |
| GitHub | Personal Access Token | Private repositories |
| HTTPS | None | Publicly accessible servers |
| HTTPS | Basic Auth | Username/password protected servers |
| HTTPS | API Key | Bearer token, X-API-Key, or custom header |
| HTTPS | SSO | Corporate SSO with cookie-based authentication |

For most use cases (public GitHub repos + link sharing), no authentication is
needed. See the
[protocol reference](/docs/visualization/extension-server-protocol.md#authentication-headers)
for the exact headers the UI sends for each auth type.

## Creating extensions with GitHub

The easiest way to create an extension server is to use a GitHub repository — no
custom server infrastructure needed.

### Fork the template repository

Start by forking (or importing, for a private copy) the
[perfetto-test-extensions](https://github.com/LalitMaganti/perfetto-test-extensions)
template repository on GitHub. This gives you a ready-made structure with a
GitHub Action that automatically builds the endpoint files when you push.

### Configure your server

Edit `config.yaml` to set your extension's name and namespace:

```yaml
name: My Team Extensions
namespace: com.example.myteam
```

The **namespace** must be unique to your organization and follows reverse-domain
notation. All macro IDs and SQL module names must start with this namespace.

You can also configure which modules your server offers. By default, the template
includes a `default` module. Add more if you want to organize extensions by team
or topic:

```yaml
name: My Team Extensions
namespace: com.example.myteam
modules:
  - id: default
    name: Default
  - id: android
    name: Android
  - id: chrome
    name: Chrome
```

### Add SQL modules

Add `.sql` files under `src/{module}/sql_modules/`. The filename determines the
SQL module name based on your namespace:

- `src/default/sql_modules/helpers.sql` becomes
  `INCLUDE PERFETTO MODULE com.example.myteam.helpers;`
- `src/default/sql_modules/foo/bar.sql` becomes
  `INCLUDE PERFETTO MODULE com.example.myteam.foo.bar;`

You can organize SQL files into subdirectories — each path component becomes a
dot-separated part of the module name.

See
[PerfettoSQL Getting Started](/docs/analysis/perfetto-sql-getting-started.md)
for how to write SQL modules.

### Add macros

Add `.yaml` or `.json` files under `src/{module}/macros/`. Each macro has an
`id`, a display `name`, and a `run` list of commands.

YAML example (`src/default/macros/show_long_tasks.yaml`):

```yaml
id: com.example.myteam.ShowLongTasks
name: Show Long Tasks
run:
  - id: dev.perfetto.RunQueryAndShowTab
    args:
      - "SELECT * FROM slice WHERE dur > 50000000"
```

Equivalent JSON (`src/default/macros/show_long_tasks.json`):

```json
{
  "id": "com.example.myteam.ShowLongTasks",
  "name": "Show Long Tasks",
  "run": [
    {
      "id": "dev.perfetto.RunQueryAndShowTab",
      "args": ["SELECT * FROM slice WHERE dur > 50000000"]
    }
  ]
}
```

Macro IDs must start with your namespace (e.g., `com.example.myteam.`). See the
[Commands Automation Reference](/docs/visualization/commands-automation-reference.md)
for the full list of available commands to use in macros.

### Push and deploy

Push your changes to `main`. The included GitHub Action builds the generated
endpoint files (`manifest`, `modules/*/macros`, etc.) and commits them
automatically.

## Adding an extension server in the Perfetto UI

### Add a GitHub server

1. Go to **Settings** (gear icon in the sidebar) and scroll to **Extension
   Servers**, or open
   [the settings directly](https://ui.perfetto.dev/#!/settings/dev.perfetto.ExtensionServers).
2. Click **Add Server** and select **GitHub**.
3. Enter the repository in `owner/repo` format (e.g.,
   `my-org/perfetto-extensions`).
4. Enter the branch or tag in the **Ref** field (e.g., `main`).
5. The UI fetches the manifest and shows available modules. The `default` module
   is selected automatically; enable others as needed.
6. Click **Save** and reload the page.

For **private repositories**, select **Personal Access Token (PAT)** under
authentication:

1. Go to
   [GitHub personal access tokens](https://github.com/settings/personal-access-tokens)
   and click **Generate new token**.
2. Under **Repository access**, select **Only select repositories** and choose
   your extension repo.
3. Under **Permissions > Repository permissions**, set **Contents** to
   **Read-only**.
4. Generate the token and enter it in the Perfetto UI when adding the server.

### Add an HTTPS server

1. Click **Add Server** and select **HTTPS**.
2. Enter the server URL (e.g., `https://perfetto-ext.corp.example.com`). The
   `https://` prefix is added automatically if omitted.
3. Select modules and configure authentication (see below).
4. Click **Save** and reload the page.

## Sharing extension servers

Click the **Share** button on any server in Settings to copy a shareable URL.
When someone opens the link:

- If they don't have the server configured, the **Add Server** dialog opens
  pre-populated with the shared configuration.
- If they already have the server, the **Edit** dialog opens with the shared
  modules merged in.

Secrets (PATs, passwords, API keys) are automatically stripped from shared URLs.
Recipients enter their own credentials if the server requires authentication.

## Managing servers

In the Extension Servers settings section, use the action buttons to:

- **Toggle** — Enable or disable a server without removing it.
- **Edit** — Change modules, authentication, or other settings.
- **Share** — Copy a shareable URL to the clipboard.
- **Delete** — Remove the server.

Changes require a **page reload** to take effect.

## Creating an HTTPS extension server

If you need more control than a GitHub repository provides — dynamic content,
corporate SSO, or integration with internal systems — you can host an extension
server on any HTTPS endpoint.

An extension server is a set of JSON endpoints. At minimum you need:

```
https://your-server.example.com/manifest          → server metadata
https://your-server.example.com/modules/default/macros      → macros (optional)
https://your-server.example.com/modules/default/sql_modules → SQL modules (optional)
```

You can serve these as static files (nginx, GCS, S3) or from a dynamic server
(Flask, Express, etc.). The server must set CORS headers to allow the Perfetto
UI to make cross-origin requests.

See the
[Extension Server Protocol Reference](/docs/visualization/extension-server-protocol.md)
for the full endpoint specification, JSON schemas, CORS requirements, and
examples including a minimal Python/Flask server.

## Troubleshooting

If extensions fail to load, the UI shows an error dialog listing what went wrong.
Errors are non-blocking — the UI works normally and extensions from other servers
that loaded successfully remain available.

Below are the error messages you may see and how to fix them.

### "Failed to fetch \<url\>: \<error\>"

The UI could not reach the server at all. This is usually a network or CORS
issue.

- **Check the URL.** Open the URL in a new browser tab. If you can't reach it,
  the server may be down, the URL may be wrong, or you may need to be on a VPN.
- **Check CORS headers.** If the URL loads in a tab but the UI shows a fetch
  error, the server likely isn't setting CORS headers. Open the browser console
  (`F12`) and look for "CORS policy" errors. See the
  [CORS requirements](/docs/visualization/extension-server-protocol.md#cors-requirements)
  for what headers to set. GitHub-hosted servers don't need CORS configuration.
- **Check for mixed content.** The Perfetto UI is served over HTTPS, so it
  cannot fetch from `http://` URLs. Use `https://` for your server.

### "Fetch failed: \<url\> returned \<status\>"

The server was reachable but returned an HTTP error.

- **401 or 403** — Authentication failed. For GitHub PATs, check that the token
  hasn't expired and has read access to the repository (Settings > Personal
  access tokens). For HTTPS servers with SSO, try logging into the server
  directly in a new tab to refresh your session, then reload the Perfetto UI.
- **404** — The endpoint doesn't exist. Check that the server implements the
  expected [endpoint paths](/docs/visualization/extension-server-protocol.md#endpoints)
  and that the repository/branch/path are correct.

### "Failed to parse JSON from \<url\>: \<error\>"

The server returned a response that isn't valid JSON.

- Check that the endpoint returns `Content-Type: application/json` and valid
  JSON. If using the GitHub template, make sure the GitHub Action has run
  successfully after your last push — check the Actions tab in your repository.

### "Invalid response from \<url\>: \<error\>"

The server returned valid JSON but it doesn't match the expected schema.

- Compare your response against the expected format in the
  [protocol reference](/docs/visualization/extension-server-protocol.md). Common
  mistakes include missing required fields (`name`, `namespace`, `features`,
  `modules` in the manifest) or using the wrong field names.

### "Module '\<name\>' not found on server"

You have a module enabled in settings that the server's manifest doesn't list.

- Edit the server in Settings and check which modules are enabled. If a module
  was renamed or removed from the server, deselect it and save.

### "Macro ID '\<id\>' must start with namespace '\<ns\>.'"

A macro's `id` field doesn't match the server's namespace.

- If you maintain the server: update the macro ID to start with your namespace
  (e.g., change `MyMacro` to `com.acme.MyMacro`). See
  [namespace enforcement](/docs/visualization/extension-server-protocol.md#namespace-enforcement).
- If you don't maintain the server: contact the server maintainer.

### "SQL module name '\<name\>' must start with namespace '\<ns\>.'"

Same as above, but for SQL modules. The module's `name` field must start with
the server's namespace.

## See also

- [Extending the UI](/docs/visualization/extending-the-ui.md) —
  Overview of all Perfetto UI extension mechanisms
- [Extension Server Protocol Reference](/docs/visualization/extension-server-protocol.md) —
  Full specification for building custom servers
- [Commands and Macros](/docs/visualization/ui-automation.md) — How to create
  macros and startup commands
