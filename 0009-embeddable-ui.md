# Embeddable UI

**Authors:** @stevegolton

**Status:** Draft

## Problem

This doc outlines the process of making the UI embeddable - i.e. make it
possible to instantiate one or more instances of the UI within another web-based
application and control the instances from the host application, without the use
of iframes or other isolation techniques.

Perfetto currently expects to be the only thing in the web page and thus
directly accesses things like the DOM, the URL, and postmessage channels. What's
more, it is entirely written to be self-contained, and doesn't offer any
interface through which it can be configured programmatically.

### Compelling Example

A web app that embeds two instances of the perfetto UI, loads two traces from
some arbitrary source and injects them into each instance. Traces are managed
entirely from the host application. The two instances should look and feel like
the host application as much as possible.

## Requirements

- **Avoid Collisions**: Allow instantiating the UI directly within another web
  app, avoiding collisions and conflicts with the host app also other instances
  of the Perfetto UI.
  - Control mount point (don't just mount to document.main).
  - Contain Router & anchors (don't allow anchors to change the URL & control
    router logic).
  - Avoid CSS naming collisions.
  - Avoid localstorage naming collisions and add prefixes for certain frameworks
    restrictions?
  - Disable listening on the postmessage interface.
- **Host Integration**: Allow the embedder to programmatically control and
  customize the UI's behavior and appearance.
  - Allow complete control of trace management up to the embedder.
  - Customize theme colors & other settings.
  - Control page / route.
  - Configure CSP.
  - Install custom error handlers.
  - Routing hooks - intercept or override route handling.
  - Delegate app state persistence to the host app.
  - Inject appinitargs (e.g what would usually come from the URL on initial
    load).
  - Control analytics.
  - Themability - allow the embedder to inject styles to make Perfetto
    look more like their own app.

## Design

The design outlined in this doc revolves around the idea of splitting Perfetto
into two parts:

1. An isolated and configurable central core packaged as a JS library with TS
   bindings. This is the reusable core which can be used in the standalone and
   embedded contexts. This idea is that this library will be configurable,
   conservative, and avoid touching the DOM or too many browser APIs, and won't
   have any import time.
2. A standalone application which uses this library and hooks it up to the
   browser and mounts the root mithril node, etc. This is the UI that will be
   hosted at ui.perfetto.dev.

This separation actually somewhat exists right now with `frontend/index.ts` +
`common.scss` being the standalone part, and the rest of the code being the
library. The separation however is not perfect. The core of the app definitely
directly accesses a lot of DOM and browser APIs but the general outline is
already there, we just need to lean into this architecture a little more.

### Building and Importing

How would an embedder actually import and use the generated bundle?

If we imported the current frontend bundle directly, we would end up importing
all of the `frontend/index.ts` code as well which would immediately run on
startup, which is not what we want. Ideally we should have two builds - one for
the library that doesn't run anything and simply exports the classes and
functions required by the embedding application.

We can configure rollup to build us two bundles - a library and a standalone
webapp:

```js
export default [
  // 1. Standalone App Config
  {
    input: "src/main.ts",
    output: {
      file: "dist/app.bundle.js",
      format: "iife", // or umd
      name: "MyApp",
    },
    // ... plugins for webapp
  },
  // 2. Library Config
  {
    input: "src/lib.ts",
    output: [
      { file: "dist/lib.esm.js", format: "es" },
      { file: "dist/lib.cjs.js", format: "cjs" },
    ],
    // Ensure you don't bundle peer dependencies (like mithril) in the lib build
    external: ["mithril"],
  },
];
```

Then in the embedder or the standalone we can use the bundle like so:

```ts
import {AppImpl} from 'perfetto'; // Import from the library.

// Configure hooks and callbacks so that Perfetto can talk to the DOM + browser APIs, or stub them out.
AppImpl.initialize({...config});

// Render (or mount) the UI into an element on the page.
m.render(targetElem, m(UiMain, {...}));
```

This will allow us to ship the library to 3rd parties and package the standalone
version for us. It's crucial that the library doesn't have any side effects at
the root level (e.g. when importing).

### Shared app vs separate apps

If we can have multiple instances of Perfetto in a single page, this raises the
question of whether we have one `app` or many?

#### Shared 'App' (Preferred)

```ts
// Initialize a common, global app - this function must be called first.
AppImpl.initialize(config);

// Load the first trace and inject it into the first UI instance
const trace1 = app.openTraceFromFile(traceFile1);
m.render(elem1, m(UiMain, { trace1 }));

// Load the second trace and inject it into the second UI instance
const trace2 = app.openTraceFromFile(traceFile2);
m.render(elem2, m(UiMain, { trace2 }));
```

Pros:

- Very similar to the current design - limited refactoring. We can leave the
  globals in place.
- Plugins currently assume there is only one app, and plugins often use the
  global state to store app related entities, such as settings, in their
  `onActivate` function.
- No need to prop drill the app through multiple layers of mithril components,
  we can access the global instance `AppImpl.instance`.

Cons:

- All instances of the UI share the same app state settings (not trace state, as
  this is stored in the trace object).
- The app can no longer contain a reference to the trace e.g. `app.trace`,
  because one app can now be used with many traces. We would need to invert the
  dependencies, and also separate the shared registries such as commands,
  sidebar entries, and pages so that the trace object has its own set of
  registries.

#### Separate Apps

```ts
const app1 = new AppImpl({ ...config1 });
app1.openTraceFromFile(traceFileHandle1);
m.render(elem1, m(UiMain, { app1 }));

const app2 = new AppImpl({ ...config2 });
app2.openTraceFromFile(traceFileHandle2);
m.render(elem2, m(UiMain, { app2 }));
```

Pros:

- We can have different settings and configuration per app.
- We can keep the notion of one trace per app (e.g. app.trace).

Cons:

- We need to be very careful with mutable global state. Plugins tend to write
  settings references registered in onActivate for instance to global storage so
  that it can be used inside onTraceLoad().
- Large refactor removing all globals including:
  - AppImpl.instance
  - Router
  - Flags
- We need to prop-drill app through potentially many layers of mithril
  components.

### Multi-trace Tenancy

Given that we have one app and many traces, we must remove any state from any
given trace from the app.

#### Trace-scoped Entities

Right now, when commands et al are registered with a trace, they are actually
added to the app's registry and removed when the trace is disposed. This won't
work if we can have multiple traces loaded simultaneously with a shared single
app instance.

Instead we should:

- Store the commands et al on the trace object itself inside its own registries.
- When a command is requested on a trace object, the trace's own registry is
  searched first, followed by the app's registry.
- To avoid accidentally shadowing commands et al in the app with the same id, when an
  entity is added to the trace, it first checks for the existence of that entity
  in the app and throws if there exists a duplicate.

Example implementation:

```ts
class CommandManager {
  constructor(private readonly app: AppImpl) {}

  registerCommand(cmd: Command) {
    if (app.commands.has(cmd.id)) {
      throw new Error(`Duplicate command ${cmd.id}`);
    }
    this.registry.set(cmd.id, cmd);
  }

  runCommand(id: string, ...args: unknown[]) {
    const cmd = this.registry.get(id) ?? app.registry.get(id);
    return cmd?.cb(args);
  }
}
```

This decoupling also makes it much easier to dispose of a trace, as all state
related to a trace is contained within the trace object itself. We can simply
drop all references to a given trace object when we're done with it, we don't
need an explicit dispose step (with the exception of stopping the web worker
perhaps).

#### App-scoped Entities

Right now, the app stores a reference to the currently loaded trace. This is
useful for app-registered entities (e.g. commands) as they can simply call
`app.trace` to look up which trace they're working on.

Only one trace can be loaded at a time within the current environment so that
makes perfect sense. However, if we're now saying that the embedder takes
control of the trace, and the app has no concept of which traces are available,
what are the legitimate uses of the trace. Which trace do app-registered
entities work with?

In order to do this, we must now inject the trace into the command's callback
via some attribute. This trace object must originate from the calling context.

E.g.

```ts
// MyPlugin.ts
app.commands.registerCommand({
  id: "myCommand",
  callback: (ctx) => {
    console.log(ctx.trace);
  },
});

// Calling the command from somewhere else
trace.commands.runCommand("myCommand"); // ctx.trace === trace
app.commands.runCommand("myCommand"); /// ctx.trace === undefined
```

#### Trace Management

Right now, plugins et al can simply open a trace by calling openTrace on the
app, however in this new model we are taking the stance that the currently
loaded trace is controlled by the embedder and injected into the UI. However, it
can still be useful to allow plugins to be able to ask the embedder to open a
trace from within a plugin etc.

E.g. how do we manage this:

```ts
// MyPlugin.ts
app.sidebar.register({
  name: 'Open My Custom Trace',
  callback: () => {
    // Open a trace from somewhere.... (e.g. cloud / gdrive / multi-trace / etc).
    const mytrace: Trace = ...;

    // How does this work? The app has no concept of the current trace anymore.
    app.setCurrentTrace(mytrace);
  },
})
```

The most logical solution here is to defer this to the embedder.

```ts
let currentTrace: Trace | undefined;

AppImpl.initialize({
  onOpenTraceRequested: (trace) => {
    currentTrace = trace;
  },
});
```

In our embedded scenario:

```ts
AppImpl.initialize({
  onOpenTraceRequested: () => {
    throw new Error("Not possible to switch traces!");
  },
});
```

The embedder in this scenario would simply avoid adding any plugins that open
traces to avoid this error from ever occurring.

#### Trace loading statuses

When loading a trace `load_trace.ts` currently calls into the app to set the
omnibox text when the trace is loading in order to provide feedback to the user
about the trace loading process - now this state needs to be stored in the
individual trace object that's doing the loading. This way, the UiMain object
that's looking at the trace object can just glean the status from that instead.

### WASM and HTTP-RPC Engines

When we call `app.openTraceFromFile()`, this will kick off a new independent
background thread and WASM instance per trace, rather than attempting to keep
the same background worker instance.

Cons: Could produce memory spikes when switching traces.

### CSS

CSS has been largely fixed by the introduction of the pf-\* prefix to avoid
collisions with classes in the parent page. We still do use some generic
selectors in common.scss such as h1, h2, h3 and similar, but in the standalone
example these are very reasonable. We could either include them in the
standalone build and omit for the embedded, or we could move them behind
specific style classes.

See: https://github.com/google/perfetto/pull/2436

Within the embedded application, Perfetto's bundled CSS may be imported
dynamically or just added to the HTML header.

There still exists a lot of element styling - CSS rules targeting element types
directly e.g. h1, html, body, in `common.scss`. These styles belong squarely in
the domain of the standalone UI so we should bundle them separately (in the same
vein as we will bundle the JS bundles separately) and only include these
dedicated styles in the standalone build.

### Theming

Similarly to the above, theming is supported via standalone CSS variables.

In the standalone application, we inject this in using the `ThemeProvider`
mithril component. This injects the CSS variables depending on the theme
setting.

```ts
AppImpl.initialize({...config});

const themeSetting = AppImpl.instance.settings.register({
  id: 'theme',
  name: '[Experimental] UI Theme',
  description: 'Warning: Dark mode is not fully supported yet.',
  schema: z.enum(['dark', 'light']),
  defaultValue: 'light',
} as const);

raf.mount(target, {
  view: () => {
    return m(ThemeProvider, {theme: themeSetting.get()}, m(UiMain));
  },
});
```

Within an embedded application, this ThemeProvider can be omitted assuming the
variables are injected via some other method.

### Hotkey Capturing

Currently hotkeys are bound to the document. We currently use a `HotkeyContext`
mithril component which wraps most of the UI, and provides a focusable wrapper
which, when clicked on, becomes the focused element on the page and can capture
all following keyboard events. This is used in the current standalone UI like
so:

```ts
raf.mount(target, {
  view: () => {
    return m(
      HotkeyContext,
      {
        ...hotkeys,
        autoFocus: true, // Automatically focus on creation
      },
      m(UiMain),
    );
  },
});
```

However, this is largely disabled in the current UI as it causes focus loss
issues. It doesn't work well with programmatic `element.blur()` as used in the
omnibox for example. When this function is called, the user agent moves the
focus back to the root, not the nearest focusable ancestor. We would need to
implement something to handle this for us - possibly using a custom event.

```ts
// Omnibox.ts
// Don't call omnibox.blur()!
const focusEvent = new CustomEvent("delegatefocus", {
  bubbles: true,
});
omniboxElem.dispatchEvent(focusEvent);

// HotkeyContext.ts
// Capture the delegatefocus event if any of our children want to give up focus
// we can capture it!
hotkeyContextElem.addEventListener("delegatefocus", (e) => {
  hotkeyContextElem.focus();
  e.stopPropagation(); // Stop any ancestor HotkeyContexts from receiving focus
});
```

We would have to scour the codebase for all .blur() calls and instead dispatch
one of these delegatefocus events. Any embedder won't have any idea
what this even is and won't capture it, so it's unlikely to cause any issues
with the embedder.

This mechanism can actually even be used quite effectively to define hotkeys for
specific subpages - e.g. the explore page - by wrapping the page in a
`HotkeyContext` and providing a custom set of hotkeys/callbacks for that page.

### Error Handling

We configure error handlers in `frontend/index.ts` currently. This can remain
as-is and these will not be configured in the embedded build. It's up to the
embedder to register root level error handlers to catch Perfetto crashes by
registering handlers for uncaught errors that propagate to the window:

```ts
window.addEventListener("error", (e) => reportError(e));
window.addEventListener("unhandledrejection", (e) => reportError(e));
```

#### Reporting errors from workers

Currently logging.reportError is called directly when errors occur inside
traceconv worker and/or the service worker. TODO: Needs more work to understand
exactly how these should behave. Ideally they should invoke an error that gets
thrown on the window's context so that it can propagate up and get handled at
the root level as above.

### Modals

`showModal()` uses a global to store the current modal. This causes problems
when we have multiple UI instances in the same page - that one modal triggered
from one UI will show up in both UIs. We could fix this by storing the modal
state at the root of the UIMain instead, seeing as it's squarely a property of
the UI.

### Plugins

The embedder should be in control of which plugins are
initiated. We can make this an option when initializing Perfetto.

```ts
import CORE_PLUGINS from "../gen/all_core_plugins";
import NON_CORE_PLUGINS from "../gen/all_plugins";
import defaultPlugins from "./core/default_plugins.ts";

AppImpl.initialize({
  // Core plugins might have special treatment in the future (such as not
  // allowing them to be disabled) - for now they're just concatenated with the
  // regular plugins.
  corePlugins: CORE_PLUGINS,
  plugins: NON_CORE_PLUGINS,
  defaultEnabledPlugins: defaultPlugins,
});
```

The standalone app will naturally import all of them and enable those only in
the default list.

Because of the global app state, we don't have to separate out the plugin
enabled/disabled settings (which are currently stored in the `perfettoFlag`
setting in localstorage). This does mean however, that if a plugin is enabled in
one instance, it'll also be enabled in another instance and vice versa. I think
this isn't the end of the world and is an understandable tradeoff.

It makes sense that the embedder should omit plugins that don't make any sense
in the embedded case - e.g. the example traces or the 'Open trace from file'
button. We will have to go through the plugins we have and perhaps separate them
out into smaller reusable chunks so that the embedder may pick and choose the
ones they need vs the ones that make no sense for the app.

### Storage

In order to avoid collisions with local and cache storage keys between Perfetto
and the embedder, we should make it so that the key can be prefixed.

```ts
AppImpl.initialize({
  localStorageKeyPrefix: "perfetto-",
  cacheKeyPrefix: "perfetto-",
});
```

### Routing

It makes sense that the embedder should have complete control over the route of
the UI so that it can keep the UI focused on a given page and change it
programmatically.

```ts
AppImpl.initialize({
  router: {
    navigate: () => {}, // Stubbed out
    currentRoute: () => {
      return "#!/viewer";
    },
  },
});
```

### Anchors

Anchors that change the hash route are now just never going to work as they will
change the embedder's route. This could be quite complex. We could try and leave
the anchors in place and override them in embedders, though that goes against
the rest of the ethos in this doc of doing nothing by default.

We should probably introduce a new API on router so anchors can now link to new
pages and change the route like this:

```ts
app.router.navigate("query"); // Links to the query page
app.router.navigate("viewer"); // Links to the timeline page
app.router.navigate("settings/mysetting"); // Links to a specific setting on the settings page
```

We could even wrap this up behind the `Anchor` widget, and grey out the widget
if the embedder has not injected the correct link.

```ts
m(Anchor, { internalHref: "viewer" });
```

The href attribute is still supported for external links.

### RAF Scheduler

While it would be nice to isolate refreshes to only the UI instance that needs
it - the most practical solution to dealing with the raf would be to just keep
it as a global object. Indeed the 'm' (mithril object) is global anyway, so we'd
have the same problem if we were to use m.mount() rather than raf.mount().

The ramifications of this mean that if one UI instance triggers an update, then
all UI instances will refresh, but that's a reasonable tradeoff for the
complexity that attempting to separate them would involve.

### Analytics

Analytics would be configured by the standalone app, we can inject an analytics
handler into Perfetto at configuration time.

```ts
const analytics = initializeAnalytics();

AppImpl.initialize({
  onAnalyticsEvent: (e) => analytics.logEvent(),
});
```

By default `logEvent()` will just be a no-op.

### Other

The following functionality will be entirely handled by the standalone app or the
embedder. Perfetto doesn't need to get involved at all.

- Loading traces via postmessage.
- Service worker.
- CSP configuration.

### API Surface

The `AppImpl.initialize()` function is the primary entry point for both standalone and
embedded applications. It initializes the global app instance and configures all
the hooks that allow Perfetto to interact with its hosting environment.

#### Complete TypeScript Interface

```ts
/**
 * Configuration for initializing Perfetto.
 *
 * In standalone mode, most of these options will use their default values.
 * In embedded mode, the embedder has full control over how Perfetto interacts
 * with the host application.
 */
interface InitPerfettoConfig {
  /**
   * Initial route arguments, typically parsed from the URL in standalone mode.
   * In embedded mode, these can be used to control initial state (e.g., which
   * page to show, UI theme preferences, etc.)
   *
   * Default: Empty object {}
   */
  initialRouteArgs?: RouteArgs;

  /**
   * Core plugins that are fundamental to Perfetto's operation.
   * These may receive special treatment in the future (e.g., not allowing
   * them to be disabled).
   *
   * Default: All core plugins from gen/all_core_plugins
   */
  corePlugins?: PluginDescriptor[];

  /**
   * Additional plugins beyond the core set.
   *
   * Default: All plugins from gen/all_plugins
   */
  plugins?: PluginDescriptor[];

  /**
   * List of plugin IDs that should be enabled by default.
   * Users can still enable/disable plugins via settings, but this controls
   * the initial state.
   *
   * Default: Platform-specific defaults (see core/default_plugins.ts)
   */
  defaultEnabledPlugins?: string[];

  /**
   * Callback invoked when Perfetto (or a plugin) requests opening a new trace.
   * This allows the embedder to control trace loading behavior.
   *
   * In standalone mode: Switches the current trace and updates the UI
   * In embedded mode: Can throw an error to prevent trace switching, or
   *                   delegate to the embedder's trace management system
   *
   * Default: Switches the global app trace
   */
  onOpenTraceRequested?: (trace: Trace) => void;

  /**
   * Router configuration. Allows the embedder to control navigation behavior.
   *
   * In standalone mode: Uses the browser's URL hash for routing
   * In embedded mode: Embedder controls routing, can stub out navigation
   */
  router?: RouterConfig;

  /**
   * Prefix for localStorage keys to avoid collisions with the host application.
   * All Perfetto localStorage keys will be prefixed with this string.
   *
   * Default: '' (no prefix)
   */
  localStorageKeyPrefix?: string;

  /**
   * Prefix for cache storage keys (for service worker caches, IndexedDB, etc.)
   * to avoid collisions with the host application.
   *
   * Default: '' (no prefix)
   */
  cacheKeyPrefix?: string;

  /**
   * Callback invoked when Perfetto logs an analytics event.
   * This allows the embedder to implement custom analytics handling.
   *
   * In standalone mode: Logs to Google Analytics (if enabled)
   * In embedded mode: Typically a no-op or custom analytics handler
   *
   * Default: No-op function
   */
  onAnalyticsEvent?: (event: AnalyticsEvent) => void;
}

/**
 * Router configuration for controlling navigation in Perfetto.
 */
interface RouterConfig {
  /**
   * Called when Perfetto wants to navigate to a new route.
   * The embedder can handle this however they want (e.g., update their own
   * routing state, show a different view, or ignore it entirely).
   *
   * @param route - The route string (e.g., 'viewer', 'query', 'settings/theme')
   */
  navigate: (route: string) => void;

  /**
   * Returns the current route that Perfetto should display.
   * This is called frequently to determine which page to render.
   *
   * @returns Route string (e.g., '#!/viewer', '/query', etc.)
   */
  currentRoute: () => string;
}
```

## Risks

- Some of these design decisions will involve an ongoing cost to maintain,
  essentially making sure that all new code doesn't assume that the standalone
  application is the only deployment context. This will introduce an additional
  review burden.
- There is guaranteed to be additional issues and roadblocks that haven't been
  addressed in this document.
- Designing for the UI to be purposely used as a library exposes an API surface
  that is difficult to change in the future. Is this something we want to agree
  to and maintain going forward? There will be some additional overhead to test
  for regressions?
