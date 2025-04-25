# Contributing to Perfetto

## Quickstart

NOTE: In March 2025 our team has moved the primary development of Perfetto
to GitHub (previously on Android Gerrit)

Perfetto can be built on Windows, Mac or Linux. However, setting up the environment on Windows is complicated so is not covered by this quickstart.

Prerequisites: git and python3.

Setup:
```sh
git clone https://github.com/google/perfetto.git
cd perfetto
tools/install-build-deps
tools/setup_all_configs.py
```

### Building

#### On Linux

For production:
```sh
tools/ninja -C out/linux_clang_release
```

For debug:
```sh
tools/ninja -C out/linux_clang_debug
```

#### On Mac

For production:
```sh
tools/ninja -C out/mac_release
```

For debug:
```sh
tools/ninja -C out/mac_debug
```

### Contributing

**Google-employees**: follow instructions at [go/perfetto-github-instructions](http://go/perfetto-github-instructions)

1. Make sure you/your organization has signed the Google CLA at [cla.developers.google.com](https://cla.developers.google.com/)
2. Send pull requests to our project on GitHub.
3. Create a branch with the change:
```sh
git checkout -b first-contribution
```
4. Make change in the repo.
5. Add, commit and upload the change:
```sh
git add .
git commit -m "My first contribution"
gh pr create  # Requires cli.github.com
```

## Repository

This project uses GitHub pull requests for code reviews,
follows the [Google C++ style][google-cpp-style], and targets `-std=c++17`.

Development happens in the GitHub repository: https://github.com/google/perfetto

## Continuous integration

The Perfetto CI on GitHub Actions covers building and testing
on most platforms and toolchains within ~30 mins. Anecdotally most build
failures and bugs are detected at the Perfetto CI level.

You can also
[test a pending Perfetto CL against Chrome's TryBots](testing.md#chromium).

## Community

You can reach us on our [Discord channel](https://discord.gg/35ShE3A).

Mailing list: https://groups.google.com/forum/#!forum/perfetto-dev

This project follows
[Google's Open Source Community Guidelines](https://opensource.google/conduct/).

### Bugs

For bugs affecting Android or the tracing internals:

* **Googlers**: use the internal bug tracker [go/perfetto-bugs](http://goto.google.com/perfetto-bugs)
* **Non-Googlers**: use [GitHub issues](https://github.com/google/perfetto/issues).

For bugs affecting Chrome Tracing:

* Use http://crbug.com `Component:Speed>Tracing label:Perfetto`.

## Contributor License Agreement

Contributions to this project must be accompanied by a Contributor License
Agreement. You (or your employer) retain the copyright to your contribution;
this simply gives us permission to use and redistribute your contributions as
part of the project. Head over to <https://cla.developers.google.com/> to see
your current agreements on file or to sign a new one.

You generally only need to submit a CLA once, so if you've already submitted one
(even if it was for a different project), you probably don't need to do it
again.

[google-cpp-style]: https://google.github.io/styleguide/cppguide.html
