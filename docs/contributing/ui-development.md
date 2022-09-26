# UI development

Some tips to get started with the UI development:

## Development environment

If you're looking for an IDE to write the TypeScript code, Visual Studio Code
works well out of the box. WebStorm or IntelliJ Idea Ultimate (Community does
not have JavaScript/TypeScript support) also work really well. The code is
located in the `ui` folder.

## Working with devserver

See [Build Instructions](build-instructions.md) page for the details about
starting the local development server.

The devserver has a live reload functionality: once you make a change in
TypeScript files, the resulting code will be recompiled and the page is going to
reload automatically. By default, this logic uses a timeout in order to prevent
successive reloads on rapid changes. This logic can be disabled via
development-only "Rapid live reload" flag in the UI. Disabling it will reload
the page earlier, at the cost of sometimes making multiple reloads in a row.

## Mithril components

Perfetto UI uses the [Mithril](https://mithril.js.org/) library for rendering
the interface. The majority of the components in the codebase use
[class components](https://mithril.js.org/components.html#classes). When Mithril
is imported via `m` alias (as it is usually done in the codebase), the class
component should extend `m.ClassComponent`, which has an optional generic
parameter allowing the component to take inputs. The entry point of class
components is a `view` method, returning a tree of virtual DOM elements to be
rendered when the component is present on the page.

## Component state

Local state of components can reside in class members and accessed directly in
methods via accessing `this`. State that is shared across different components
is stored in the `State` class definition, and should be modified via
implementing a new action in `src/common/actions.ts`. A new field added to
`State` should be initialized in `src/common/empty_state.ts`.

There are restrictions on whan can be used in the global state: plain JS objects
are OK, but class instances are not (this limitation is due to state
serialization: the state should be a valid JSON object). If storing class
instances (like `Map` and `Set` data structures) is necessary, these can be
stored in the `NonSerializableState` portion of the state, that is omitted from
saving into JSON objects.
