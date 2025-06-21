# UI development

## Getting started

This command pulls the UI-related dependencies (notably, the NodeJS binary)
and installs the `node_modules` in `ui/node_modules`:

```bash
tools/install-build-deps --ui
```

### Build the UI

```bash
# Will build into ./out/ui by default. Can be changed with --out path/
# The final bundle will be available at ./ui/out/dist/.
# The build script creates a symlink from ./ui/out to $OUT_PATH/ui/.
ui/build
```

### Run the devserver

The devserver has a live reload functionality: once you make a change in
TypeScript files, the resulting code will be recompiled and the page is going to
reload automatically. By default, this logic uses a timeout in order to prevent
successive reloads on rapid changes. This logic can be disabled via
development-only "Rapid live reload" flag in the UI. Disabling it will reload
the page earlier, at the cost of sometimes making multiple reloads in a row.

```bash
# This will automatically build the UI. There is no need to manually run
# ui/build before running ui/run-dev-server.
ui/run-dev-server
```

Navigate to http://localhost:10000/ to see the changes.

NOTE: If you made changes to Trace Processor you need to restart the server.

### Test the change

UI unit tests are located next to the functionality being tested, and have
`_unittest.ts` or `_jsdomtest.ts` suffixes. The following command runs all unit
tests:

```bash
ui/run-unittests
```

This command will perform the build first; which is not necessary if you
already have a development server running. In this case, to avoid interference
with the rebuild done by development server and to get the results faster, you
can use

```bash
ui/run-unittests --no-build
```

to skip the build steps.

Script `ui/run-unittests` also supports `--watch` parameter, which would
restart the testing when the underlying source files are changed. This can be
used in conjunction with `--no-build`, and on its own as well.

## Development environment

If you're looking for an IDE to write the TypeScript code, Visual Studio Code
works well out of the box. WebStorm or IntelliJ Idea Ultimate (Community does
not have JavaScript/TypeScript support) also work really well. The code is
located in the `ui` folder.

For VSCode users, we recommend using the eslint & prettier extensions to handle
this entirely from within the IDE. See the
[Useful Extensions](#useful-extensions) section on how to set this up.

### Formatting & Linting

We use `eslint` to lint TypeScript and JavaScript, and `prettier` to format
TypeScript, JavaScript, and SCSS.

To auto-format all source files, run ui/format-sources, which takes care of
running both prettier and eslint on the changed files:

```bash
# By default it formats only files that changed from the upstream Git branch
# (typicaly origin/main).
# Pass --all for formatting all files under ui/src
ui/format-sources
```

Presubmit checks require no formatting or linting issues, so fix all issues
using the commands above before submitting a patch.

## Mithril components

Perfetto UI uses the [Mithril](https://mithril.js.org/) library for rendering
the interface. The majority of the components in the codebase use
[class components](https://mithril.js.org/components.html#classes). When Mithril
is imported via `m` alias (as it is usually done in the codebase), the class
component should extend `m.ClassComponent`, which has an optional generic
parameter allowing the component to take inputs. The entry point of class
components is a `view` method, returning a tree of virtual DOM elements to be
rendered when the component is present on the page.

## Hints

### Component state

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
