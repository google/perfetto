# Community Maintained Code

This directory contains code maintained by the Perfetto community,
not the core Perfetto team.

Code in this directory is:
- **Community maintained**: Owned and supported by contributors listed in 
    OWNERS.github (please stick to this naming to avoid internal rollout
    complications).
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
4. Submit PR with initial code and OWNERS.github file

See [Contributing Guide](https://perfetto.dev/docs/contributing/getting-started)
for general contribution guidelines.
