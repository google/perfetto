# UI plugins
The Perfetto UI can be extended with plugins. These plugins are shipped part of
Perfetto.

## Create a plugin
The guide below explains how to create a plugin for the Perfetto UI.

### Prepare for UI development
First we need to prepare the UI development environment. You will need to use a
MacOS or Linux machine. Follow the steps below or see the [Getting
Started](./getting-started) guide for more detail.

```sh
git clone https://android.googlesource.com/platform/external/perfetto/
cd perfetto
./tools/install-build-deps --ui
```

### Copy the plugin skeleton
```sh
cp -r ui/src/plugins/com.example.Skeleton ui/src/plugins/<your-plugin-name>
```
Now edit `ui/src/plugins/<your-plugin-name>/index.ts`. Search for all instances
of `SKELETON: <instruction>` in the file and follow the instructions.

Notes on naming:
- Don't name the directory `XyzPlugin` just `Xyz`.
- The `pluginId` and directory name must match.
- Plugins should be prefixed with the reversed components of a domain name you
  control. For example if `example.com` is your domain your plugin should be
  named `com.example.Foo`.
- Core plugins maintained by the Perfetto team should use `dev.perfetto.Foo`.
- Commands should have ids with the pattern `example.com#DoSomething`
- Command's ids should be prefixed with the id of the plugin which provides
  them.
- Command names should have the form "Verb something something", and should be
  in normal sentence case. I.e. don't capitalize the first letter of each word.
  - Good: "Pin janky frame timeline tracks"
  - Bad: "Tracks are Displayed if Janky"

### Start the dev server
```sh
./ui/run-dev-server
```
Now navigate to [localhost:10000](http://localhost:10000/)

### Enable your plugin
- Navigate to the plugins page:
  [localhost:10000/#!/plugins](http://localhost:10000/#!/plugins).
- Ctrl-F for your plugin name and enable it.

Later you can request for your plugin to be enabled by default. Follow the
[default plugins](#default-plugins) section for this.

### Upload your plugin for review
- Update `ui/src/plugins/<your-plugin-name>/OWNERS` to include your email.
- Follow the [Contributing](./getting-started#contributing) instructions to
  upload your CL to the codereview tool.
- Once uploaded add `stevegolton@google.com` as a reviewer for your CL.

## Plugin extension points
Plugins can extend a handful of specific places in the UI. The sections below
show these extension points and give examples of how they can be used.

### Commands
Commands are user issuable shortcuts for actions in the UI. They can be accessed
via the omnibox.

Follow the [create a plugin](#create-a-plugin) to get an initial skeleton for
your plugin.

To add your first command, add a call to `ctx.registerCommand()` in either your
`onActivate()` or `onTraceLoad()` hooks. The recommendation is to register
commands in `onActivate()` by default unless they require something from
`PluginContextTrace` which is not available on `PluginContext`.

The tradeoff is that commands registered in `onTraceLoad()` are only available
while a trace is loaded, whereas commands registered in `onActivate()` are
available all the time the plugin is active.

```typescript
class MyPlugin implements PerfettoPlugin {
  onActivate(ctx: PluginContext): void {
    ctx.registerCommand(
       {
         id: 'dev.perfetto.ExampleSimpleCommand#LogHelloPlugin',
         name: 'Log "Hello, plugin!"',
         callback: () => console.log('Hello, plugin!'),
       },
    );
  }

  onTraceLoad(ctx: PluginContextTrace): void {
    ctx.registerCommand(
       {
         id: 'dev.perfetto.ExampleSimpleTraceCommand#LogHelloTrace',
         name: 'Log "Hello, trace!"',
         callback: () => console.log('Hello, trace!'),
       },
    );
  }
}
```

Here `id` is a unique string which identifies this command. The `id` should be
prefixed with the plugin id followed by a `#`. All command `id`s must be unique
system-wide. `name` is a human readable name for the command, which is shown in
the command palette. Finally `callback()` is the callback which actually
performs the action.

Commands are removed automatically when their context disappears. Commands
registered with the `PluginContext` are removed when the plugin is deactivated,
and commands registered with the `PluginContextTrace` are removed when the trace
is unloaded.

Examples:
- [dev.perfetto.ExampleSimpleCommand](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.ExampleSimpleCommand/index.ts).
- [perfetto.CoreCommands](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/core_plugins/commands/index.ts).
- [dev.perfetto.ExampleState](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.ExampleState/index.ts).

#### Hotkeys

A default hotkey may be provided when registering a command.

```typescript
ctx.registerCommand({
  id: 'dev.perfetto.ExampleSimpleCommand#LogHelloWorld',
  name: 'Log "Hello, World!"',
  callback: () => console.log('Hello, World!'),
  defaultHotkey: 'Shift+H',
});
```

Even though the hotkey is a string, it's format checked at compile time using
typescript's [template literal
types](https://www.typescriptlang.org/docs/handbook/2/template-literal-types.html).

See
[hotkey.ts](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/base/hotkeys.ts)
for more details on how the hotkey syntax works, and for the available keys and
modifiers.

### Tracks
#### Defining Tracks
Tracks describe how to render a track and how to respond to mouse interaction.
However, the interface is a WIP and should be considered unstable. This
documentation will be added to over the next few months after the design is
finalised.

#### Reusing Existing Tracks
Creating tracks from scratch is difficult and the API is currently a WIP, so it
is strongly recommended to use one of our existing base classes which do a lot
of the heavy lifting for you. These base classes also provide a more stable
layer between your track and the (currently unstable) track API.

For example, if your track needs to show slices from a given a SQL expression (a
very common pattern), extend the `NamedSliceTrack` abstract base class and
implement `getSqlSource()`, which should return a query with the following
columns:

- `id: INTEGER`: A unique ID for the slice.
- `ts: INTEGER`: The timestamp of the start of the slice.
- `dur: INTEGER`: The duration of the slice.
- `depth: INTEGER`: Integer value defining how deep the slice should be drawn in
    the track, 0 being rendered at the top of the track, and increasing numbers
    being drawn towards the bottom of the track.
- `name: TEXT`: Text to be rendered on the slice and in the popup.

For example, the following track describes a slice track that displays all
slices that begin with the letter 'a'.
```ts
class MyTrack extends NamedSliceTrack {
  getSqlSource(): string {
    return `
    SELECT
      id,
      ts,
      dur,
      depth,
      name
    from slice
    where name like 'a%'
    `;
  }
}
```

#### Registering Tracks
Plugins may register tracks with Perfetto using
`PluginContextTrace.registerTrack()`, usually in their `onTraceLoad` function.

```ts
class MyPlugin implements PerfettoPlugin {
  onTraceLoad(ctx: PluginContextTrace): void {
    ctx.registerTrack({
      uri: 'dev.MyPlugin#ExampleTrack',
      displayName: 'My Example Track',
      trackFactory: ({trackKey}) => {
        return new MyTrack({engine: ctx.engine, trackKey});
      },
    });
  }
}
```

#### Default Tracks
The "default" tracks are a list of tracks that are added to the timeline when a
fresh trace is loaded (i.e. **not** when loading a trace from a permalink). This
list is copied into the timeline after the trace has finished loading, at which
point control is handed over to the user, allowing them add, remove and reorder
tracks as they please. Thus it only makes sense to add default tracks in your
plugin's `onTraceLoad` function, as adding a default track later will have no
effect.

```ts
class MyPlugin implements PerfettoPlugin {
  onTraceLoad(ctx: PluginContextTrace): void {
    ctx.registerTrack({
      // ... as above ...
    });

    ctx.addDefaultTrack({
      uri: 'dev.MyPlugin#ExampleTrack',
      displayName: 'My Example Track',
      sortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
    });
  }
}
```

Registering and adding a default track is such a common pattern that there is a
shortcut for doing both in one go: `PluginContextTrace.registerStaticTrack()`,
which saves having to repeat the URI and display name.

```ts
class MyPlugin implements PerfettoPlugin {
  onTraceLoad(ctx: PluginContextTrace): void {
    ctx.registerStaticTrack({
      uri: 'dev.MyPlugin#ExampleTrack',
      displayName: 'My Example Track',
      trackFactory: ({trackKey}) => {
        return new MyTrack({engine: ctx.engine, trackKey});
      },
      sortKey: PrimaryTrackSortKey.COUNTER_TRACK,
    });
  }
}
```

#### Adding Tracks Directly
Sometimes plugins might want to add a track to the timeline immediately, usually
as a result of a command or on some other user action such as a button click. We
can do this using `PluginContext.timeline.addTrack()`.

```ts
class MyPlugin implements PerfettoPlugin {
  onTraceLoad(ctx: PluginContextTrace): void {
    ctx.registerTrack({
      // ... as above ...
    });

    // Register a command that directly adds a new track to the timeline
    ctx.registerCommand({
      id: 'dev.MyPlugin#AddMyTrack',
      name: 'Add my track',
      callback: () => {
        ctx.timeline.addTrack(
          'dev.MyPlugin#ExampleTrack',
          'My Example Track'
        );
      },
    });
  }
}
```

### Tabs
Tabs are a useful way to display contextual information about the trace, the
current selection, or to show the results of an operation.

To register a tab from a plugin, use the `PluginContextTrace.registerTab`
method.

```ts
import m from 'mithril';
import {Tab, Plugin, PluginContext, PluginContextTrace} from '../../public';

class MyTab implements Tab {
  render(): m.Children {
    return m('div', 'Hello from my tab');
  }

  getTitle(): string {
    return 'My Tab';
  }
}

class MyPlugin implements PerfettoPlugin {
  onActivate(_: PluginContext): void {}
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerTab({
      uri: 'dev.MyPlugin#MyTab',
      content: new MyTab(),
    });
  }
}
```

You'll need to pass in a tab-like object, something that implements the `Tab`
interface. Tabs only need to define their title and a render function which
specifies how to render the tab.

Registered tabs don't appear immediately - we need to show it first. All
registered tabs are displayed in the tab dropdown menu, and can be shown or
hidden by clicking on the entries in the drop down menu.

Tabs can also be hidden by clicking the little x in the top right of their
handle.

Alternatively, tabs may be shown or hidden programmatically using the tabs API.

```ts
ctx.tabs.showTab('dev.MyPlugin#MyTab');
ctx.tabs.hideTab('dev.MyPlugin#MyTab');
```

Tabs have the following properties:
- Each tab has a unique URI.
- Only once instance of the tab may be open at a time. Calling showTab multiple
  times with the same URI will only activate the tab, not add a new instance of
  the tab to the tab bar.

#### Ephemeral Tabs

By default, tabs are registered as 'permanent' tabs. These tabs have the
following additional properties:
- They appear in the tab dropdown.
- They remain once closed. The plugin controls the lifetime of the tab object.

Ephemeral tabs, by contrast, have the following properties:
- They do not appear in the tab dropdown.
- When they are hidden, they will be automatically unregistered.

Ephemeral tabs can be registered by setting the `isEphemeral` flag when
registering the tab.

```ts
ctx.registerTab({
  isEphemeral: true,
  uri: 'dev.MyPlugin#MyTab',
  content: new MyEphemeralTab(),
});
```

Ephemeral tabs are usually added as a result of some user action, such as
running a command. Thus, it's common pattern to register a tab and show the tab
simultaneously.

Motivating example:
```ts
import m from 'mithril';
import {uuidv4} from '../../base/uuid';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
  Tab,
} from '../../public';

class MyNameTab implements Tab {
  constructor(private name: string) {}
  render(): m.Children {
    return m('h1', `Hello, ${this.name}!`);
  }
  getTitle(): string {
    return 'My Name Tab';
  }
}

class MyPlugin implements PerfettoPlugin {
  onActivate(_: PluginContext): void {}
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerCommand({
      id: 'dev.MyPlugin#AddNewEphemeralTab',
      name: 'Add new ephemeral tab',
      callback: () => handleCommand(ctx),
    });
  }
}

function handleCommand(ctx: PluginContextTrace): void {
  const name = prompt('What is your name');
  if (name) {
    const uri = 'dev.MyPlugin#MyName' + uuidv4();
    // This makes the tab available to perfetto
    ctx.registerTab({
      isEphemeral: true,
      uri,
      content: new MyNameTab(name),
    });

    // This opens the tab in the tab bar
    ctx.tabs.showTab(uri);
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'dev.MyPlugin',
  plugin: MyPlugin,
};
```

### Details Panels & The Current Selection Tab
The "Current Selection" tab is a special tab that cannot be hidden. It remains
permanently in the left-most tab position in the tab bar. Its purpose is to
display details about the current selection.

Plugins may register interest in providing content for this tab using the
`PluginContentTrace.registerDetailsPanel()` method.

For example:

```ts
class MyPlugin implements PerfettoPlugin {
  onActivate(_: PluginContext): void {}
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    ctx.registerDetailsPanel({
      render(selection: Selection) {
        if (canHandleSelection(selection)) {
          return m('div', 'Details for selection');
        } else {
          return undefined;
        }
      }
    });
  }
}
```

This function takes an object that implements the `DetailsPanel` interface,
which only requires a render function to be implemented that takes the current
selection object and returns either mithril vnodes or a falsy value.

Every render cycle, render is called on all registered details panels, and the
first registered panel to return a truthy value will be used.

Currently the winning details panel takes complete control over this tab. Also,
the order that these panels are called in is not defined, so if we have multiple
details panels competing for the same selection, the one that actually shows up
is undefined. This is a limitation of the current approach and will be updated
to a more democratic contribution model in the future.

### Metric Visualisations
TBD

Examples:
- [dev.perfetto.AndroidBinderViz](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.AndroidBinderViz/index.ts).

### State
NOTE: It is important to consider version skew when using persistent state.

Plugins can persist information into permalinks. This allows plugins to
gracefully handle permalinking and is an opt-in - not automatic - mechanism.

Persistent plugin state works using a `Store<T>` where `T` is some JSON
serializable object. `Store` is implemented
[here](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/base/store.ts).
`Store` allows for reading and writing `T`. Reading:
```typescript
interface Foo {
  bar: string;
}

const store: Store<Foo> = getFooStoreSomehow();

// store.state is immutable and must not be edited.
const foo = store.state.foo;
const bar = foo.bar;

console.log(bar);
```

Writing:
```typescript
interface Foo {
  bar: string;
}

const store: Store<Foo> = getFooStoreSomehow();

store.edit((draft) => {
  draft.foo.bar = 'Hello, world!';
});

console.log(store.state.foo.bar);
// > Hello, world!
```

First define an interface for your specific plugin state.
```typescript
interface MyState {
  favouriteSlices: MySliceInfo[];
}
```

To access permalink state, call `mountStore()` on your `PluginContextTrace`
object, passing in a migration function.
```typescript
class MyPlugin implements PerfettoPlugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const store = ctx.mountStore(migrate);
  }
}

function migrate(initialState: unknown): MyState {
  // ...
}
```

When it comes to migration, there are two cases to consider:
- Loading a new trace
- Loading from a permalink

In case of a new trace, your migration function is called with `undefined`. In
this case you should return a default version of `MyState`:
```typescript
const DEFAULT = {favouriteSlices: []};

function migrate(initialState: unknown): MyState {
  if (initialState === undefined) {
    // Return default version of MyState.
    return DEFAULT;
  } else {
    // Migrate old version here.
  }
}
```

In the permalink case, your migration function is called with the state of the
plugin store at the time the permalink was generated. This may be from an older
or newer version of the plugin.

**Plugins must not make assumptions about the contents of `initialState`!**

In this case you need to carefully validate the state object. This could be
achieved in several ways, none of which are particularly straight forward. State
migration is difficult!

One brute force way would be to use a version number.

```typescript
interface MyState {
  version: number;
  favouriteSlices: MySliceInfo[];
}

const VERSION = 3;
const DEFAULT = {favouriteSlices: []};

function migrate(initialState: unknown): MyState {
  if (initialState && (initialState as {version: any}).version === VERSION) {
    // Version number checks out, assume the structure is correct.
    return initialState as State;
  } else {
    // Null, undefined, or bad version number - return default value.
    return DEFAULT;
  }
}
```

You'll need to remember to update your version number when making changes!
Migration should be unit-tested to ensure compatibility.

Examples:
- [dev.perfetto.ExampleState](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.ExampleState/index.ts).

## Guide to the plugin API
The plugin interfaces are defined in
[ui/src/public/](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/public).

## Default plugins
Some plugins are enabled by default. These plugins are held to a higher quality
than non-default plugins since changes to those plugins effect all users of the
UI. The list of default plugins is specified at
[ui/src/core/default_plugins.ts](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/core/default_plugins.ts).

## Misc notes
- Plugins must be licensed under
  [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html) the same as all other
  code in the repository.
- Plugins are the responsibility of the OWNERS of that plugin to maintain, not
  the responsibility of the Perfetto team. All efforts will be made to keep the
  plugin API stable and existing plugins working however plugins that remain
  unmaintained for long periods of time will be disabled and ultimately deleted.

