# Googler features & 3rd-party cookies

When using the [Perfetto UI](https://ui.perfetto.dev), Googlers have access to
internal features that extend the UI with Google-specific workflows, including:

- **Internal macros** — Pre-configured command sequences and queries for
  analyzing internal traces, available in the command palette (`Ctrl+Shift+P` /
  `Cmd+Shift+P`).
- **Internal SQL modules** — Shared PerfettoSQL modules packaged and served
  internally, accessible via `INCLUDE PERFETTO MODULE`.
- **Proto descriptors** — Automatic decoding of internal protobuf messages
  embedded in traces.
- **Trace sharing** — Internal trace uploading and sharing features integrated
  directly into the UI.

These features are provided by connecting to the internal Google extension
server at `https://perfetto-gae-internal.googleplex.com`.

## Why 3rd-party cookies are required

The Perfetto UI is hosted on `https://ui.perfetto.dev`, while the internal
extension server is hosted on `https://perfetto-gae-internal.googleplex.com`.
Because these origins have different domains, browser requests from the Perfetto
UI to the extension server are **cross-site (third-party)** requests.

Authentication to the internal extension server relies on corporate single
sign-on (SSO) session cookies. If your browser blocks 3rd-party cookies, these
authentication cookies will not be sent, causing requests to fail with HTTP 403
or CORS errors. As a result:

1. The internal extension server cannot load its manifest, macros, or SQL
   modules.
2. The UI cannot verify your internal user status, disabling Googler-specific
   features such as internal trace sharing.

## How to enable 3rd-party cookies in Chrome

To enable access to internal features, you need to allow third-party cookies for
the internal extension server.

### Option 1: add a site exception in Chrome settings (recommended)

You do not need to allow 3rd-party cookies globally. You can add specific site
exceptions:

1. Open Chrome settings by navigating to `chrome://settings/cookies` or
   `chrome://settings/trackingProtection` (or go to **Settings** → **Privacy and
   security** → **Third-party cookies** / **Tracking Protection**).
2. Under **Sites allowed to use third-party cookies** (or **Allowed to use
   third-party cookies**), click **Add**.
3. Add `https://ui.perfetto.dev` and click **Add**.
4. Reload [https://ui.perfetto.dev](https://ui.perfetto.dev).

### Option 2: allow via the Chrome address bar

1. Navigate to [https://ui.perfetto.dev](https://ui.perfetto.dev).
2. Look for the **eye icon** (or the site controls icon next to the address bar)
   indicating blocked third-party cookies.
3. Click the icon and toggle **Third-party cookies** to **Allowed** for
   `ui.perfetto.dev`.
4. Reload the page when prompted.

## Verifying your setup

Once 3rd-party cookies are enabled:

1. Open [https://ui.perfetto.dev](https://ui.perfetto.dev).
2. Open **Settings** (gear icon in the sidebar) and navigate to **Extension
   Servers**.
3. You should see the internal extension server listed as enabled, with
   available modules loaded (e.g. `default`).
4. Press `Ctrl+Shift+P` (or `Cmd+Shift+P` on Mac) to open the command palette.
   Google-internal macros should appear in the command list.
5. Internal trace sharing options will now be available when viewing traces.

## Troubleshooting

- **Server still shows an error:** Navigate directly to
  `https://perfetto-gae-internal.googleplex.com` in a separate tab to ensure your
  SSO session is active, then reload `https://ui.perfetto.dev`.
- **Still blocked on corp network/VPN:** Ensure you are connected to the
  corporate network or VPN so that `googleplex.com` endpoints are reachable.
- See [Extension Servers](/docs/visualization/extension-servers.md) for more
  details on extension server mechanics and troubleshooting.
