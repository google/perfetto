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
[perfetto-ci.appspot.com](https://perfetto-ci.appspot.com).

**Trybots**: CLs uploaded to gerrit are automatically submitted to TravisCI
within one minute and available on the CI page.

[perfetto-gerrit]: https://android-review.googlesource.com/q/project:platform%252Fexternal%252Fperfetto+status:open
[google-cpp-style]: https://google.github.io/styleguide/cppguide.html
[depot-tools]: https://dev.chromium.org/developers/how-tos/depottools
