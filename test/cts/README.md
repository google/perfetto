This folder contains the CTS tests for the Perfetto library.

# Background
For information about what CTS is, please go to
https://source.android.com/compatibility/cts/ where you will find information
on the purpose of CTS and how to run these tests.

# Structure of folder
There are two targets in this folder: a mock producer app and a GTest
suite.

The GTest suite is both the consumer of data as well as the driver
the actual tests we wish to run. This target drives the tracing system by
requesting tracing to start/stop and ensures the data it receives is correct.
This mimics the role of real consumers acting as the driver for tracing on
device. This suite is compiled and pushed to device and subsequently run
using a shell account which gives us permissions to access the perfetto 
consumer socket.

The mock producer is an Android app with a thin Java wrapping around the C++
library interfaced using JNI. The purpose of this target is to ensure that the
TraceProto received from the consumer is valid and and then push some fake data.
This ensures that any arbitary app can push data to the Perfetto socket which
can then be decoded by the GTest consumer. This app is simply installed before
the GTest suite is run.

# Notes
The AndroidTest.xml file which is associated with these tests can be found
in $ANDROID/cts/tests/perfetto. This is to allow for other users of CTS to
be aware that Perfetto has CTS tests.

The code is located inside the Perfetto repository as we anticipate fast
development on Perfetto and moreover, we wish to retain the ability to
run these tests independently of the Android tree if required.
