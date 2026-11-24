# Perfetto standalone Bazel config

This directory contains the default Bazel configuration of Perfetto. The
`perfetto_cfg_ext` module extension (see `MODULE.bazel`) exposes it as the
`@perfetto_cfg` repository.

Bazel-based embedders that need a different configuration (e.g. to map
Perfetto's dependencies to their own third_party targets) are supposed to:

### 1. Have a (modified) copy of perfetto_cfg.bzl in their repo

```
myproject/
  build/
    perfetto_overrides/
      BUILD.bazel  (empty)
      perfetto_cfg.bzl
```

### 2. Replace @perfetto_cfg with that directory

E.g. in myproject/MODULE.bazel (requires Bazel 7.4 or newer):

```
bazel_dep(name = "perfetto", version = "...")

local_repository = use_repo_rule(
    "@bazel_tools//tools/build_defs/repo:local.bzl",
    "local_repository",
)

local_repository(
    name = "my_perfetto_cfg",
    path = "build/perfetto_overrides",
)

perfetto_cfg_ext = use_extension(
    "@perfetto//bazel:perfetto_cfg_ext.bzl",
    "perfetto_cfg_ext",
)
override_repo(perfetto_cfg_ext, perfetto_cfg = "my_perfetto_cfg")
```

`override_repo` only has an effect in the root module.

Embedders that still use a WORKSPACE file instead map the directory to
`@perfetto_cfg` directly:

```
local_repository(
    name = "perfetto_cfg",
    path = "build/perfetto_overrides",
)
```
