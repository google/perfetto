# Testing Perfetto

The testing strategy for Perfetto is rather complex considering the wide variety
of build configurations and places it is expected to be integrated. We discuss
all testing related information in this doc.

# Test runners
Perfetto can be tested in a variety of locations:
* Travis: CI for Perfetto only. Found at https://perfetto-ci.appspot.com/
* APCT: see go/apct and go/apct-guide
* Treehugger: Android's presubmit system. Run before submission of every CL.
* CTS: Android test suite used run to ensure API compatibility. Rolling runs
internally.

Note that Travis uses the standalone build system and the others build as
part of the Android tree.

# Unit tests
Unit tests exist for most of the code in Perfetto on the class level. They
ensure that each class broadly works as expected.

Unit tests are run on Travis, APCT (pending) and Treehugger (pending).

# Integration tests
Integration tests ensure that subsystems (importantly ftrace) and Perfetto
as a whole (end to end testing) is working correctly. Note that we do not
test integration with any third parties, just within Perfetto.

There are two configurations in which integration tests can be run:
1. Assuming that the daemons (the service and ftrace) already running and
   checking the test is able to interact correctly with them. This is usually
   the way in which we test things on Android.
2. Starting up the daemons in the test itself and then testing against them.
   This is usually how standalone builds are tested.

Integration tests are run on Travis (excluding ftrace) in mode 2, APCT (pending)
in mode 1, Treehugger (pending) in mode 1 and CTS in mode 1.

# CTS tests
CTS tests ensure that any vendors who modify Android remain compliant with the
platform API.

These tests include a subset of the integration tests above as well as adding
more complex tests which ensure interaction between platform (e.g. Android apps
etc.) and Perfetto is not broken.

CTS tests currently are not run anywhere as these tests are not included in
the suite.
