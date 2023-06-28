# Android Version Notes
This page documents all the caveats of using Perfetto on older versions of
Android. Because of the yearly release cycle, it's often the case that a
feature is shipped but we later discover a bug/problem which makes the feature
not usable or needs to be used in a very specific way.

## U-
### New features
* The CLONE_SNAPSHOT trigger mode was introduced.
* String field filtering in traces was introduced.

### Caveats
* On the CLONE_SNAPSHOT codepath, the trace UUID gets rewritten when the session
  is clone but a statsd atom linking the two is not emitted. This means we
  should be careful to exclude any "clone-only" sessions as being "failed"
  sessions.

## T-
### New features
* Reporting traces through the framework was introduced.

### Caveats
* CLONE_SNAPSHOT does not exist in T- but there is a subtle edge case
  (b/274931668) which means it's quite dangerous to specify CLONE_SNAPSHOT on
  configs being sent to T- devices.

## P
### New features
* Perfetto was included in the system image!

### Caveats
* --txt option is not supported so configs must be binary encoded.
