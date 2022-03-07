# Perfetto UI

## Quick Start

```bash
$ git clone https://android.googlesource.com/platform/external/perfetto/
$ cd perfetto

# Will build into ./out/ui by default. Can be changed with --out path/
# The final bundle will be available at ./ui/out/dist/.
# The build script creates a symlink from ./ui/out to $OUT_PATH/ui/.
ui/build

# This will automatically build the UI. There is no need to manually run
# ui/build before running ui/run-dev-server.
ui/run-dev-server
```

Then navigate to `http://localhost:10000`.

See also https://perfetto.dev/docs/contributing/build-instructions#ui-development

## Unit tests

```bash
ui/run-unittests  # Add --watch to run them in watch mode.
```

## Integration tests (browser screenshot difftests)

```bash
run-integrationtests
```

To rebaseline screenshots after a UI change

```bash
ui/run-integrationtests --rebaseline

tools/test_data upload

git add -A

git commit
```

See also https://perfetto.dev/docs/contributing/testing#ui-pixel-diff-tests
