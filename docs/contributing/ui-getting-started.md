# UI development

## Getting started

This command pulls the UI-related dependencies (notably, the NodeJS binary)
and installs the `node_modules` in `ui/node_modules`:

```bash
tools/install-build-deps --ui
```

On a fresh Debian/Ubuntu install (including WSL 2), first install the system
packages the build depends on:

```bash
sudo apt install curl python3-venv build-essential
```

Building the UI from Windows is not supported natively, but works from Windows
via [WSL 2](https://learn.microsoft.com/en-us/windows/wsl/about).

### Build the UI

```bash
# Will build into ./out/ui by default. Can be changed with --out path/
# The final bundle will be available at ./ui/out/dist/.
# The build script creates a symlink from ./ui/out to $OUT_PATH/ui/.
ui/build
```

### Run the devserver

The devserver recompiles TypeScript files and reloads the page automatically
when you make changes. By default, it uses a timeout to prevent successive
reloads during rapid changes. Enable the development-only "Rapid live reload"
flag in the UI to disable this timeout. The page will reload sooner, but may
reload multiple times in a row.

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

This command builds the UI first. If you already have a development server
running, use the following command to skip the build steps, avoid interfering
with the server's rebuild, and get results faster:

```bash
ui/run-unittests --no-build
```

The `ui/run-unittests` script also supports the `--watch` parameter, which
reruns tests when source files change. You can use it with `--no-build` or on
its own.

## Development environment

If you're looking for an IDE to write the TypeScript code, Visual Studio Code
works well out of the box. WebStorm or IntelliJ Idea Ultimate (Community does
not have JavaScript/TypeScript support) also work well. The code is located in
the `ui` folder.

For VSCode users, we recommend using the eslint & prettier extensions to handle
this entirely from within the IDE. See the
[Formatting & Linting](#formatting-linting) section below on how to set this up.

### Formatting & Linting

We use `eslint` to lint TypeScript and JavaScript, and `prettier` to format
TypeScript, JavaScript, and SCSS.

To auto-format all source files, run ui/format-sources, which takes care of
running both prettier and eslint on the changed files:

```bash
# By default it formats only files that changed from the upstream Git branch
# (typically origin/main).
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

Component-local state can reside in class members and be accessed directly in
methods through `this`. State that needs to be persisted (e.g. into permalinks)
is kept in a `Store` mounted via `trace.mountStore()`, see
[UI plugins](ui-plugins#state).

There are restrictions on what can be used in a store: plain JS objects
are OK, but class instances are not (this limitation is due to state
serialization: the state should be a valid JSON object).
