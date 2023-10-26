# UI plugins
The Perfetto UI can be extended with plugins. These plugins are shipped
part of Perfetto.

## Create a plugin
The guide below explains how to create a plugin for the Perfetto UI.

### Prepare for UI development
First we need to prepare the UI development environment.
You will need to use a MacOS or Linux machine.
Follow the steps below or see the
[Getting Started](./getting-started) guide for more detail.

```sh
git clone https://android.googlesource.com/platform/external/perfetto/
cd perfetto
./tool/install-build-deps --ui
```

### Copy the plugin skeleton
```sh
cp -r ui/src/plugins/com.example.Skeleton ui/src/plugins/<your-plugin-name>
```
Now edit `ui/src/plugins/<your-plugin-name>/index.ts`.
Search for all instances of `SKELETON: <instruction>` in the file and
follow the instructions.

Notes on naming:
- Don't name the directory `XyzPlugin` just `Xyz`.
- The `pluginId` and directory name must match.
- Plugins should be prefixed with the reversed components of a domain
  name you control. For example if `example.com` is your domain your
  plugin should be named `com.example.Foo`.
- Core plugins maintained by the Perfetto team should use
  `dev.perfetto.Foo`.
- Commands should have ids with the pattern `example.com#DoSomething`
- Command's ids should be prefixed with the id of the plugin which
  provides them.
- Commands names should have the form "Verb something something".
  Good: "Pin janky frame timeline tracks"
  Bad: "Tracks are Displayed if Janky"

### Start the dev server
```sh
./ui/run-dev-server
```
Now navigate to [](http://localhost:10000/settings)

### Upload your plugin for review
- Update `ui/src/plugins/<your-plugin-name>/OWNERS` to include your email.
- Follow the [Contributing](./getting-started#contributing)
  instructions to upload your CL to the codereview tool.
- Once uploaded add `hjd@google.com` as a reviewer for your CL.

## Plugin extension points
Plugins can extend a handful of specific places in the UI. The sections
below show these extension points and give examples of how they can be
used.

### Commands
Commands are user issuable shortcuts for actions in the UI.
They can be accessed via the omnibox.

Follow the [create a plugin](#create-a-plugin) to get an initial
skeleton for your plugin.

To add your first command, add a call to `ctx.addCommand()` in either your
`onActivate()` or `onTraceLoad()` hooks. The recommendation is to register
commands in `onActivate()` by default unless they require something from
`TracePluginContext` which is not available on `PluginContext`.

The tradeoff is that commands registered in `onTraceLoad()` are only available
while a trace is loaded, whereas commands registered in `onActivate()` are
available all the time the plugin is active.

```typescript
class MyPlugin implements Plugin {
  onActivate(ctx: PluginContext): void {
    ctx.addCommand(
       {
         id: 'dev.perfetto.ExampleSimpleCommand#LogHelloPlugin',
         name: 'Log "Hello, plugin!"',
         callback: () => console.log('Hello, plugin!'),
       },
    );
  }

  onTraceLoad(ctx: TracePluginContext): void {
    ctx.addCommand(
       {
         id: 'dev.perfetto.ExampleSimpleTraceCommand#LogHelloTrace',
         name: 'Log "Hello, trace!"',
         callback: () => console.log('Hello, trace!'),
       },
    );
  }
}
```

Here `id` is a unique string which identifies this command.
The `id` should be prefixed with the plugin id followed by a `#`. All command
`id`s must be unique system-wide.
`name` is a human readable name for the command, which is shown in the command
palette.
Finally `callback()` is the callback which actually performs the
action.

Commands are removed automatically when their context disappears. Commands
registered with the `PluginContext` are removed when the plugin is deactivated,
and commands registered with the `TracePluginContext` are removed when the trace
is unloaded.

Examples:
- [dev.perfetto.ExampleSimpleCommand](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.ExampleSimpleCommand/index.ts).
- [dev.perfetto.CoreCommands](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.CoreCommands/index.ts).
- [dev.perfetto.ExampleState](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.ExampleState/index.ts).

### Tracks
TBD

### Tabs
TBD

### Metric Visualisations
TBD

Examples:
- [dev.perfetto.AndroidBinderViz](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/plugins/dev.perfetto.AndroidBinderViz/index.ts).

### State
NOTE: It is important to consider version skew when using persistent state.

Plugins can persist information into permalinks. This allows plugins
to gracefully handle permalinking and is an opt-in - not automatic -
mechanism.

Persistent plugin state works using a `Store<T>` where `T` is some JSON
serializable object.
`Store` is implemented [here](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/frontend/store.ts).
`Store` allows for reading and writing `T`.
Reading:
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

To access permalink state, call `mountStore()` on your `TracePluginContext`
object, passing in a migration function.
```typescript
class MyPlugin implements Plugin {
  async onTraceLoad(ctx: TracePluginContext): Promise<void> {
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
The plugin interfaces are defined in [ui/src/public/index.ts](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/ui/src/public/index.ts).


## Default plugins
TBD

## Misc notes
- Plugins must be licensed under
  [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html)
  the same as all other code in the repository.
- Plugins are the responsibility of the OWNERS of that plugin to
  maintain, not the responsibility of the Perfetto team. All
  efforts will be made to keep the plugin API stable and existing
  plugins working however plugins that remain unmaintained for long
  periods of time will be disabled and ultimately deleted.

