# Community Maintained Code

**Authors:** @primiano @LalitMaganti

**Status:** Draft

## Problem

The Perfetto project has traditionally been maintained primarily by Google
engineers, with strict review requirements and infrastructure integration
standards. However, the community has expressed interest in contributing
substantial features and extensions that may not align with Google's immediate
priorities or infrastructure requirements.

The key use-case that is driving this RFC is
[#3330 Rust SDK implementation by @dreveman](https://github.com/google/perfetto/pull/3330)

The current model creates friction for such contributions:

1. **Review bottleneck**: All PRs require 2 Google employee approvals, even for
    community-driven features.
2. **Maintenance expectations**: Code in the main tree implies Google ownership
    and long-term support.
3. **Infrastructure coupling**: All code must work in Android and
    Google-internal repositories, limiting experimentation.
4. **Contributor frustration**: Community members cannot fully own features they
    build and maintain.

We need a mechanism to:

- Enable community ownership of specific Perfetto components.
- Reduce review burden on core maintainers for community features.
- Set clear expectations about maintenance and support.
- Allow experimentation without blocking on infrastructure compatibility.

### Out of scope

While desirable, this problem cannot be easily solved for the general case:

- It is not trivial to have a contrib/ table in TraceProcessor
- It is not trivial to have a contrib/ data source in traced_probes.

Because in both cases the code gets bundled in our binaries and it's non-trivial
to handle optional build dependencies in our other repos.

For now this RFC limits the scope of contrib/ to projects that are technically
decoupled from the official distributions of Perfetto (e.g., conversion tools,
scripts, language bindings).

We might reconsider contrib/ extension points in future (having /contrib TP
modules could be very nice) but that requires further discussion out of scope
here.

## Decision

Create a `contrib/` directory for community-maintained code with the
following properties:

1. **Community ownership**: Code in `contrib/` can be owned and
    maintained by non-Google contributors.
2. **Relaxed review requirements**: PRs touching only `contrib/` do not require
    2 Google employee approvals.
3. **Infrastructure isolation**: Code in `contrib/` is **not** built or exposed
    in Android or Google-internal repositories.
4. **Clear expectations**: Explicit disclaimers that `contrib/` code is
    maintained by the community, not the core Perfetto team.
5. **Quality standards**: Contributions must still meet basic code quality
  standards and have at least 2 dedicated maintainers.

## Design

### Directory Structure

```txt
.github/
  CODEOWNERS
perfetto/
├── contrib/
│   ├── README.md                    # Disclaimer and contribution guide
│   ├── rust-sdk/                    # Example: Rust SDK
│   │   ├── OWNERS                   # Community maintainers
│   │   ├── README.md
│   │   └── src/
│   └── <other-community-projects>/
```

Note that the OWNERS file for now is only for human consumption.
The source of truth is `.github/CODEOWNERS` which will need to be kept in
sync manually. In future we should invest in automation to allow decentralized
OWNERS files.


## Decision

Create a `contrib/` directory for community-maintained code with the
following properties:

1. **Community ownership**: Code in `contrib/` can be owned and
    maintained by non-Google contributors.
2. **Relaxed review requirements**: PRs touching only `contrib/` do not require
    2 Google employee approvals.
3. **Infrastructure isolation**: Code in `contrib/` is **not** built or exposed
    in Android or Google-internal repositories.
4. **Clear expectations**: Explicit disclaimers that `contrib/` code is
    maintained by the community, not the core Perfetto team.
5. **Quality standards**: Contributions must still meet basic code quality
  standards and have at least 2 dedicated maintainers.


### Repository Integration

**Perfetto main repository (`perfetto`):**

- `contrib/` directory exists and is fully functional
- Code can be built and tested via standalone Perfetto build.
- There is no requirement that the code builds in other environments such as
  Android.bp or Bazel.
- CI bots (if present) are FYI only and don't block PRs. Contrib maintainers
  are responsible for keeping it green. The core team will put an effort to
  avoid breaking it, but some breakages might be unavoidable (e.g. time-critical
  changes when cutting a release)

**Android repository:**

- `contrib/` directory **exists** to keep GitHub and Android repo in sync to
  allow the team to do git merges into release branches.
- **No `Android.bp` build rules** expose `contrib/` code.
- Code is present but not compiled or linked into Android builds.

**Google-internal repository:**

- `contrib/` directory is **completely removed** via Copybara transformations.
- Prevents accidental dependencies or visibility.

### Disclaimer and Documentation

**[`contrib/README.md`](contrib/README.md) (to be created):**

```markdown
# Community Maintained Code

This directory contains code maintained by the Perfetto community, 
not the core Perfetto team.

Code in this directory is:
- **Community maintained**: Owned and supported by contributors listed in OWNERS
- **Not officially supported**: The Perfetto core team does not guarantee
    maintenance
- **Not part of Android/Google builds**: Not included in Android or
    Google-internal repositories
- **Experimental**: May have different stability and compatibility guarantees
  than core Perfetto.

## Using contrib/ code

If you depend on code from `contrib/`, understand that:
- Breaking changes may occur without the same deprecation periods as core APIs.
- Bugs may take longer to fix depending on maintainer availability.
- Features may be removed if maintainers are no longer active.

## Contributing to contrib/

To add a new project to `contrib/`:
1. Open a GitHub issue proposing the addition
2. Demonstrate community need and maintainer commitment
3. Get approval from Perfetto maintainers
4. Submit PR with initial code and OWNERS file

See [Contributing Guide](https://perfetto.dev/docs/contributing/getting-started) 
for general contribution guidelines.
```

## Alternatives Considered

### Alternative 1: Separate repository

**Approach:** Create `perfetto-contrib` organization with separate repos for
each community project.

**Pros:**

- Complete independence for community projects
- No impact on main Perfetto repository
- Easier to archive/remove dead projects

**Cons:**

- Discoverability problem (community extensions not visible to Perfetto users)
- Harder to share build infrastructure and CI
- Harder to stay in sync with .proto changes.
- **Rejected**: Too much separation reduces collaboration and visibility.

## Implementation Plan

- [ ] Create `contrib/` directory structure
- [ ] Add [`contrib/README.md`](contrib/README.md) with disclaimer/guidelines
- [ ] Update [`.github/workflows/require-two-reviewers-for-fork-prs.yml`](.github/workflows/require-two-reviewers-for-fork-prs.yml)
- [ ] Update [`copybara.sky`](copybara.sky) to exclude `contrib/` from
    Google-internal imports
- [ ] Update [Contributing Guide](https://perfetto.dev/docs/contributing/getting-started)
    to mention `contrib/`
- [ ] Add section to docs about finding and using community extensions
- [ ] Update [Committer Guide](https://perfetto.dev/docs/contributing/become-a-committer)
    to mention `contrib/` ownership
