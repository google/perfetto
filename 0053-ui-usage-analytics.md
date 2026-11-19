# UI usage analytics for Google employees

**Authors:** @shawseanyang @primiano

**Status:** Discussion

As the feature set of the Perfetto UI grows, we need a better understanding of
which parts of the UI are used often and which are hardly used at all. This
will help us prioritize our work and keep unused parts from bitrotting.

To address this, we will add a set of extended analytics, which are enabled
when the user is a Google employee. This RFC describes how the code will gate
them.

## How the extended analytics are gated

Four things work together to keep the extended analytics to Google employees:

1. The extended events have their own type, separate from the existing
   analytics events.
2. Only core can send them; plugins can't.
3. The function that sends them checks a setting before every send.
4. That setting is only on when the UI can reach Google's internal network.

Each is described below.

**1. The extended events have their own type.** Every extended event will be
one of a fixed list of shapes, defined together in one type,
`AdvancedAnalyticsEvent`. Each shape is a plain object with a `kind` and a
fixed set of fields:

```ts
// ui/src/core/analytics_impl.ts (planned)
export type AdvancedAnalyticsEvent =
  | {kind: 'trace_loaded'; traceLoadId; traceFormat; traceSizeBucket; loadMs}
  | {kind: 'plugin_loaded'; traceLoadId; plugin; loadMs; hasTracks}
  | {kind: 'plugin_usage'; reportId; traceLoadId; plugin; feature; ...};
```

This isn't an interface that other code can implement: the compiler only
accepts objects that match one of the shapes in the list, so adding a new kind
of extended event means changing this type. The type lives in `core/`, next to
the code that sends the events, and the existing analytics events have a
separate type of their own, so the two can't be mixed up.

**2. Only core can send them.** The extended events will be sent with
`logAdvancedEvent`, a new method on `AnalyticsInternal`. This existing
interface extends the public `Analytics` interface and is only visible to core:

```ts
// ui/src/public/analytics.ts: what plugins see
export interface Analytics {
  logEvent(event: BasicAnalyticsEvent): void;
  ...
}

// ui/src/core/analytics_impl.ts: what only core sees
export interface AnalyticsInternal extends Analytics {
  logAdvancedEvent(event: AdvancedAnalyticsEvent): void; // planned
  ...
}
```

Plugins only receive the public `Analytics` interface, and the existing import
rules (`tools/check_imports`) stop them from importing anything under `core/`.
So no plugin can call `logAdvancedEvent`, or even refer to the
`AdvancedAnalyticsEvent` type.

**3. `logAdvancedEvent` checks the `useAdvancedAnalytics` setting before every
send.** It sends nothing unless the setting is on:

```ts
// ui/src/core/analytics_impl.ts (planned)
logAdvancedEvent(event: AdvancedAnalyticsEvent) {
  if (!this.getPolicy().useAdvancedAnalytics) return;
  ...
}
```

The setting is read on every call, rather than once at startup, because the UI
only learns whether the user is a Google employee shortly after the page
loads. A unit test checks that nothing is sent while the setting is off, so
removing the check would fail CI.

**4. `useAdvancedAnalytics` is only on for Google employees.** This is the
sequence of calls, from page load to the setting:

1. **The UI fetches the internal extension server's manifest.** On startup,
   the core plugin `dev.perfetto.ExtensionServers` fetches the manifest of the
   embedder's extension server
   ([`ExtensionServers/index.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core_plugins/dev.perfetto.ExtensionServers/index.ts#L68)).
   On ui.perfetto.dev, that is Google's internal extension server, configured
   with `authType: 'https_sso'`
   ([`perfetto_ui_embedder.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core/embedder/perfetto_ui_embedder.ts#L21-L24)).
2. **The fetch only succeeds for Google employees.** Because of `https_sso`,
   the request is sent with the browser's cookies
   ([`extension_server.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core_plugins/dev.perfetto.ExtensionServers/extension_server.ts#L101)).
   The server is only reachable from Google's internal network with Google's
   internal SSO cookies, so for everyone else the fetch fails.
3. **The plugin passes the result to the app.** Core plugins receive the app's
   internal object, `AppImpl`, as `ctx`. The plugin calls
   `AppImpl.setIsInternalUser` on it, with whether the fetch succeeded
   ([`ExtensionServers/index.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core_plugins/dev.perfetto.ExtensionServers/index.ts#L73)):

   ```ts
   ctx.setIsInternalUser(embedder.manifest.then((r) => r.ok));
   ```

4. **`AppImpl.setIsInternalUser` uses the result to fill in the active
   policy.** When the result arrives, it rebuilds the policy from it with
   `derivePolicy`, and saves the result for the next visit
   ([`app_impl.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core/app_impl.ts#L218-L225)).
   `derivePolicy` turns each internal-only feature on for internal users and
   off for everyone else, and `useAdvancedAnalytics` will be one more of its
   fields
   ([`app_impl.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core/app_impl.ts#L58-L68)):

   ```ts
   function derivePolicy(isInternalUser: boolean): Policy {
     return {
       ...
       useAdvancedAnalytics: isInternalUser, // planned
     };
   }
   ```

5. **The analytics class reads `useAdvancedAnalytics` from the active
   policy.** `AppImpl` gives the analytics class a function that returns the
   active policy, and `logAdvancedEvent` reads `useAdvancedAnalytics` through
   it on every call (part 3), so it always uses the latest result.
6. **Before the result arrives, the policy uses the previous visit's
   result.** If there is none, every internal-only feature, including
   `useAdvancedAnalytics`, is off
   ([`app_impl.ts`](https://github.com/google/perfetto/blob/b77e5300f18955204b357d5df7cacc205c260227/ui/src/core/app_impl.ts#L205-L214)).

For everyone else, the fetch fails, the setting stays off, and
`logAdvancedEvent` sends nothing.
