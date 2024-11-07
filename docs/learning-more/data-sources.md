
### Data sources
## Customizing trace collection


<?tabs>

TAB: Android

Android Perfetto is the default tracing system on Android. This means it is
deeply integrated throughout the operating system and there are a wide variety
of tracing data sources available out of the box.

**Linux kernel ftrace**

ftrace is the Linux kernel's tracing system and provides detailed information
about what the kernel is doing. The most widely used data from ftrace is the
*scheduling timeline* which is information about the scheduling state of every
thread on the system. Scheduling often critical to understand why an app
might be slow: is it because the program is doing too much work or because it is
waiting for some other thread and/or process to respond?

Beyond scheduling, ftrace provides a lot of other infomration: the
frequency of the CPUs, whether the device was suspended (i.e. in deep sleep),
the system calls made by threads and much much more.

https://www.kernel.org/doc/Documentation/trace/ftrace.txt

**android.os.Trace instrumentation (atrace)**

[android.os.Trace](https://developer.android.com/reference/android/os/Trace) is
an API which lets apps instrument their codebase with

**Frame timeline**

[](/docs/data-sources/frametimeline)

**logcat**

**/proc and /sys interfaces**

TAB: Linux

**Linux kernel ftrace**

https://www.kernel.org/doc/Documentation/trace/ftrace.txt

**/proc and /sys interfaces**

**Perfetto SDK intrumentation**

</tabs?>
