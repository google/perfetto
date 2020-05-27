# Contributing to Perfetto

## Repository

This project uses [Android AOSP Gerrit][perfetto-gerrit] for code reviews,
follows the [Google C++ style][google-cpp-style], and targets `-std=c++11`.

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

## Community

You can reach us on our [Discord channel](https://discord.gg/35ShE3A).
If you prefer using IRC we have an experimental Discord <> IRC bridge
synced with `#perfetto-dev` on [Freenode](https://webchat.freenode.net/).

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
