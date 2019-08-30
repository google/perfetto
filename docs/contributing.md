# Contributing to Pefetto
This project uses [Android AOSP Gerrit][perfetto-gerrit] for code reviews and
uses the [Google C++ style][google-cpp-style] and targets `-std=c++11`.

`git cl upload` from [Chromium depot tools][depot-tools] is the preferred
workflow to upload patches, as it supports presubmits and code formatting via
`git cl format`.

See https://source.android.com/source/contributing for more details about
external contributions and CLA signing.


### Continuous integration

Continuous build and test coverage is available at
[ci.perfetto.dev](https://ci.perfetto.dev).

**Trybots**:  
CLs uploaded to gerrit are automatically submitted to the CI and
and available on the CI page.
If the label `Presubmit-Ready: +1` is set, the CI will also publish a comment
like [this][ci-example] on the CL.

[perfetto-gerrit]: https://android-review.googlesource.com/q/project:platform%252Fexternal%252Fperfetto+status:open
[google-cpp-style]: https://google.github.io/styleguide/cppguide.html
[depot-tools]: https://dev.chromium.org/developers/how-tos/depottools
[ci-example]: https://android-review.googlesource.com/c/platform/external/perfetto/+/1108253/3#message-09fd27fb92ca8357abade3ec725919ac3445f3af
