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
To add your first command edit either the `commands()` or `traceCommands()`
methods.

```typescript
class MyPlugin implements Plugin {
  // ...

  commands(ctx: PluginContext): Command[] {
    return [
       {
         id: 'dev.perfetto.ExampleSimpleCommand#LogHelloWorld',
         name: 'Log hello world',
         callback: () => console.log('Hello, world!'),
       },
    ];
  }

  traceCommands(ctx: TracePluginContext): Command[] {
    return [
       {
         id: 'dev.perfetto.ExampleSimpleTraceCommand#LogHelloWorld',
         name: 'Log hello trace',
         callback: () => console.log('Hello, trace!'),
       },
    ];
  }
}
```

Commands are polled whenever the command list must be updated. When no trace is
loaded, only the `commands()` method is called, whereas when a trace is loaded,
both the `commands()` and the `traceCommands()` methods are called, and their
outputs are concatenated.

The difference between the two is that commands defined in `commands()` only
have access to the viewer API, whereas commands defined in `traceCommands()` may
access the store and the engine in addition to the viewer API.

The tradeoff is that commands defined in `traceCommands()` are only available
when a trace is loaded, whereas commands defined in `commands()` are available
all the time.

Here `id` is a unique string which identifies this command.
The `id` should be prefixed with the plugin id followed by a `#`.
`name` is a human readable name for the command.
Finally `callback()` is the callback which actually performs the
action.

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

This interface will be used as type parameter to the `Plugin` and
`TracePluginContext` interfaces.
```typescript
class MyPlugin implements Plugin<MyState> {

  migrate(initialState: unknown): MyState {
    // ...
  }

  async onTraceLoad(ctx: TracePluginContext<MyState>): Promise<void> {
    // You can access the store on ctx.store
  }

  async onTraceUnload(ctx: TracePluginContext<MyState>): Promise<void> {
    // You can access the store on ctx.store
  }

  // ...
}
```

`migrate()` is called after `onActivate()` just before `onTraceLoad()`. There
are two cases to consider:
- Loading a new trace
- Loading from a permalink

In case of a new trace `migrate()` is called with `undefined`. In this
case you should return a default version of `MyState`:
```typescript
class MyPlugin implements Plugin<MyState> {

  migrate(initialState: unknown): MyState {
    if (initialState === undefined) {
      return {
        favouriteSlices: [];
      };
    }
    // ...
  }

  // ...
}
```

In the permalink case `migrate()` is called with the state of the plugin
store at the time the permalink was generated. This may be from a
older or newer version of the plugin.
**Plugin's must not make assumptions about the contents of `initialState`**.

In this case you need to carefully validate the state object.

TODO: Add validation example.

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

