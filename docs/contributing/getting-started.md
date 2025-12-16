# Contributing to Perfetto

## Quickstart

Follow those steps if you are new to contributing to Perfetto.

### Setup

**Prerequisites:** git and python3.

```sh
# Clone the Perfetto repo and enter the directory
git clone https://github.com/google/perfetto.git
cd perfetto

# Install dependencies
# Add --android to pull the Android NDK and emulator
tools/install-build-deps

# Setup all build configs
# Add --android to generate Android build configs
tools/setup_all_configs.py
```

### Building

_On Linux_

```sh
# Production build
tools/ninja -C out/linux_clang_release

# Debug build
tools/ninja -C out/linux_clang_debug
```

_On Mac_

```sh
# Production build
tools/ninja -C out/mac_release

# Debug build
tools/ninja -C out/mac_debug
```

_For Android (cross-compiled on desktop OS)_

```sh
# Production build (arm64)
tools/ninja -C out/android_release_arm64

# Debug build (arm64)
tools/ninja -C out/android_debug_arm64
```

_UI_

```sh
# Build the UI
ui/build

# Run the dev server
ui/run-dev-server
```

For more information on building Perfetto go to [build instructions](build-instructions).

### Contributing

NOTE: In March 2025 our team has moved the primary development of Perfetto
to GitHub (previously on Android Gerrit).

#### Googlers

NOTE: Follow the instructions at [go/perfetto-github-instructions](http://go/perfetto-github-instructions).

1. Make sure you/your organization has signed the Google CLA at [cla.developers.google.com](https://cla.developers.google.com/)
2. Create a branch with the change:

```sh
git checkout -b first-contribution
```

3. Make change in the repo.
4. Add, commit and upload the change:

```sh
git add .
git commit -m "My first contribution"
gh pr create  # Requires cli.github.com
```

Please note our project follows the [Google C++ style](https://google.github.io/styleguide/cppguide.html), and targets `-std=c++17`.

#### External contributors

Please contribute the same way as you would to any other Github repository.
A good explanation of how to do it can be found [here](https://docs.github.com/en/get-started/exploring-projects-on-github/contributing-to-a-project).

### Testing

As Perfetto has a rather complicated testing strategy, we will automatically run our presubmit on each push into the repo.
For manual run: `tools/run_presubmit`.

For more information on testing Perfetto go to [testing page](testing).

## What's next?

You might want to contribute to the UI, Trace Processor, SDK or various data importers.

- If you want to add a new functionality to the UI, most likely the next step is the [UI getting started](ui-getting-started).
- If you want to edit the core functionality of the UI: it's a much bigger change which would require in depth understanding of Perfetto UI. Most requests/bugs now are related to various plugins, not the core.
- If you want to add a new ftrace event take a look at [common tasks page](common-tasks).
- If you want to add a new table/view/function to Perfetto SQL standard library you need to first undestand [the Perfetto SQL syntax](/docs/analysis/perfetto-sql-syntax.md), and then read the details of updating the standard library at [common tasks page](common-tasks).
- If you want to add a support of a new file type into Perfetto, you need to add a new `importer` to Trace Processor C++ code.

## {#community} Communication

### Contact

Our main communication channel is our mailing list: https://groups.google.com/forum/#!forum/perfetto-dev.

You can also reach us on our [Discord channel](https://discord.gg/35ShE3A) but our support there is best effort only.

This project follows
[Google's Open Source Community Guidelines](https://opensource.google/conduct/).

### Bugs

For bugs affecting Android or the tracing internals:

- **Googlers**: use the internal bug tracker [go/perfetto-bugs](http://goto.google.com/perfetto-bugs)
- **Non-Googlers**: use [GitHub issues](https://github.com/google/perfetto/issues).

For bugs affecting Chrome Tracing:

- Use http://crbug.com `Component:Speed>Tracing label:Perfetto`.

## Contributor License Agreement

Contributions to this project must be accompanied by a Contributor License
Agreement. You (or your employer) retain the copyright to your contribution;
this simply gives us permission to use and redistribute your contributions as
part of the project. Head over to <https://cla.developers.google.com/> to see
your current agreements on file or to sign a new one.

You generally only need to submit a CLA once, so if you've already submitted one
(even if it was for a different project), you probably don't need to do it
again.
