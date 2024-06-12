# Contributing to Perfetto

## Quickstart

NOTE: Perfetto can be built on Windows, Mac or Linux. However, setting up the environment on Windows is complicated so is not covered by this quickstart.

Prerequisites: git and python3.

Setup:
```sh
git clone https://android.googlesource.com/platform/external/perfetto/
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

1. Create an account at [android.googlesource.com](https://android.googlesource.com/).
2. (if you are a Googler) Follow go/sync#get-credentials to allow uploading to
Android Gerrit.
3. Download `depot_tools`, a collection of helper scripts which make uploading changes
to Android Gerrit easier.
```sh
cd perfetto
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
```
4. Add `depot_tools` to your PATH (you may want to add this to your .bashrc/.zshrc):
```sh
depot_path="$(realpath depot_tools)"
export PATH=$depot_path:$PATH
```
5. Create a branch with the change:
```sh
git new-branch first-contribution
```
6. Make change in the repo.
7. Add, commit and upload the change:
```sh
git add .
git commit -m "My first contribution"
git cl upload
```

## Repository

This project uses [Android AOSP Gerrit][perfetto-gerrit] for code reviews,
follows the [Google C++ style][google-cpp-style], and targets `-std=c++17`.

Development happens in the AOSP repository:
https://android.googlesource.com/platform/external/perfetto/

https://github.com/google/perfetto is an up-to-date and actively maintained
read-only mirror of the above. Pull requests through GitHub are not accepted.

## Code Reviews

All submissions, including submissions by project members, require review.
We use [Android AOSP Gerrit][perfetto-gerrit] for this purpose.

`git cl upload` from [Chromium depot tools][depot-tools] is the preferred
workflow to upload patches, as it takes care of runing presubmit tests,
build-file generators and code formatting.

If you submit code directly through `repo` and your CL touches build files or
.proto files, it's very likely that it will fail in the CI because the
aforementioned generators are bypassed.

## Continuous integration

There are two levels of CI / TryBots involved when submitting a Perfetto CL:

- [ci.perfetto.dev](https://ci.perfetto.dev): it covers building and testing
  on most platforms and toolchains within ~15 mins. Anecdotally most build
  failures and bugs are detected at the Perfetto CI level.

- The [Android CI](https://ci.android.com) (also known as TreeHugger) builds a
  full system image and runs full integration tests within ~2-4 hours. This can
  shake a number of more rare integration bugs, often related with SELinux,
  initrc files or similar.

Both CIs are kicked in when the `Presubmit-Ready: +1` is set and will publish a
comment like [this][ci-example] on the CL.

You need to wait for both CIs to go green before submitting. The only
exceptions are UI-only, docs-only or GN-only changes, for which the Android CI
can be bypassed, as those are not built as part of the Android tree.

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

[perfetto-gerrit]: https://android-review.googlesource.com/q/project:platform%252Fexternal%252Fperfetto+status:open
[google-cpp-style]: https://google.github.io/styleguide/cppguide.html
[depot-tools]: https://dev.chromium.org/developers/how-tos/depottools
[ci-example]: https://android-review.googlesource.com/c/platform/external/perfetto/+/1108253/3#message-09fd27fb92ca8357abade3ec725919ac3445f3af
