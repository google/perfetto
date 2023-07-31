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
cp -r ui/plugins/com.example.Skeleton ui/plugins/<your-plugin-name>
```
Now edit `ui/plugins/<your-plugin-name>/index.ts`.
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

### Start the dev server
```sh
./ui/run-dev-server
```
Now navigate to [](http://localhost:10000/settings)

### Upload your plugin for review
- Update `ui/plugins/<your-plugin-name>/OWNERS` to include your email.
- Follow the [Contributing](./getting-started#contributing)
  instructions to upload your CL to the codereview tool.
- Once uploaded add `hjd@google.com` as a reviewer for your CL.

## Plugin extension points
Plugins can extend a handful of specific places in the UI. The sections
below show these extension points and give examples of how they can be
used.

### Commands
TBD

### Tracks
TBD

### Detail tabs
TBD

### Metric Visualisations
TBD

## Guide to the plugin API
TBD

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

