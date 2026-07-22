#!/usr/bin/env python3
from os import sys
import synth_common

trace = synth_common.create_trace()
trace.add_packet()

trace.add_process(1, 0, 'init')
trace.add_process(2, 1, 'system_server')

trace.add_process(10, 1, 'com.google.android.deskclock', 10010)
trace.add_package_list(
    ts=1, name='com.google.android.deskclock', uid=10010, version_code=1)
trace.add_thread(
    tid=10,
    tgid=10,
    cmdline='com.google.android.deskclock',
    name='com.google.android.deskclock')
trace.add_process(11, 1, 'com.google.android.deskclock', 10011)
trace.add_package_list(
    ts=1, name='com.google.android.deskclock', uid=10011, version_code=1)
trace.add_thread(
    tid=11,
    tgid=11,
    cmdline='com.google.android.deskclock',
    name='com.google.android.deskclock')
trace.add_process(12, 1, 'com.google.android.dialer', 10012)
trace.add_package_list(
    ts=1, name='com.google.android.dialer', uid=10012, version_code=1)
trace.add_thread(
    tid=12,
    tgid=12,
    cmdline='com.google.android.dialer',
    name='com.google.android.dialer')
trace.add_process(13, 1, 'com.google.android.dialer', 10013)
trace.add_package_list(
    ts=1, name='com.google.android.dialer', uid=10013, version_code=1)
trace.add_thread(
    tid=13,
    tgid=13,
    cmdline='com.google.android.dialer',
    name='com.google.android.dialer')
trace.add_process(14, 1, 'com.google.android.gm', 10014)
trace.add_package_list(
    ts=1, name='com.google.android.gm', uid=10014, version_code=1)
trace.add_thread(
    tid=14,
    tgid=14,
    cmdline='com.google.android.gm',
    name='com.google.android.gm')
trace.add_process(15, 1, 'com.google.android.gm', 10015)
trace.add_package_list(
    ts=1, name='com.google.android.gm', uid=10015, version_code=1)
trace.add_thread(
    tid=15,
    tgid=15,
    cmdline='com.google.android.gm',
    name='com.google.android.gm')
trace.add_process(16, 1, 'com.google.android.apps.messaging', 10016)
trace.add_package_list(
    ts=1, name='com.google.android.apps.messaging', uid=10016, version_code=1)
trace.add_thread(
    tid=16,
    tgid=16,
    cmdline='com.google.android.apps.messaging',
    name='com.google.android.apps.messaging')
trace.add_process(17, 1, 'com.google.android.apps.messaging', 10017)
trace.add_package_list(
    ts=1, name='com.google.android.apps.messaging', uid=10017, version_code=1)
trace.add_thread(
    tid=17,
    tgid=17,
    cmdline='com.google.android.apps.messaging',
    name='com.google.android.apps.messaging')
trace.add_process(18, 1, 'com.netflix.mediaclient', 10018)
trace.add_package_list(
    ts=1, name='com.netflix.mediaclient', uid=10018, version_code=1)
trace.add_thread(
    tid=18,
    tgid=18,
    cmdline='com.netflix.mediaclient',
    name='com.netflix.mediaclient')
trace.add_process(19, 1, 'com.netflix.mediaclient', 10019)
trace.add_package_list(
    ts=1, name='com.netflix.mediaclient', uid=10019, version_code=1)
trace.add_thread(
    tid=19,
    tgid=19,
    cmdline='com.netflix.mediaclient',
    name='com.netflix.mediaclient')
trace.add_process(20, 1, 'com.google.android.apps.photos', 10020)
trace.add_package_list(
    ts=1, name='com.google.android.apps.photos', uid=10020, version_code=1)
trace.add_thread(
    tid=20,
    tgid=20,
    cmdline='com.google.android.apps.photos',
    name='com.google.android.apps.photos')
trace.add_process(21, 1, 'com.google.android.apps.photos', 10021)
trace.add_package_list(
    ts=1, name='com.google.android.apps.photos', uid=10021, version_code=1)
trace.add_thread(
    tid=21,
    tgid=21,
    cmdline='com.google.android.apps.photos',
    name='com.google.android.apps.photos')
trace.add_process(22, 1, 'com.twitter.android', 10022)
trace.add_package_list(
    ts=1, name='com.twitter.android', uid=10022, version_code=1)
trace.add_thread(
    tid=22, tgid=22, cmdline='com.twitter.android', name='com.twitter.android')
trace.add_process(23, 1, 'com.twitter.android', 10023)
trace.add_package_list(
    ts=1, name='com.twitter.android', uid=10023, version_code=1)
trace.add_thread(
    tid=23, tgid=23, cmdline='com.twitter.android', name='com.twitter.android')

trace.add_ftrace_packet(cpu=0)

trace.add_atrace_begin(
    ts=1000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=1001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=1010, tid=2, pid=2, buf='launching: com.google.android.deskclock')
trace.add_atrace_begin(ts=1020, tid=10, pid=10, buf='activityStart')
trace.add_atrace_end(ts=1025, tid=10, pid=10)
trace.add_atrace_begin(ts=1025, tid=10, pid=10, buf='activityResume')
trace.add_atrace_end(ts=1028, tid=10, pid=10)
trace.add_atrace_async_begin(
    ts=1030, tid=10, pid=10, buf='animator:translationZ')
trace.add_atrace_async_end(ts=1040, tid=10, pid=10, buf='animator:translationZ')
trace.add_atrace_begin(ts=1050, tid=10, pid=10, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=1060, tid=10, pid=10)
trace.add_atrace_async_end(
    ts=1065, tid=2, pid=2, buf='launching: com.google.android.deskclock')
trace.add_atrace_begin(
    ts=1065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=1066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=2000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=2001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=2010, tid=2, pid=2, buf='launching: com.google.android.deskclock')
trace.add_atrace_begin(ts=2020, tid=11, pid=11, buf='activityStart')
trace.add_atrace_end(ts=2025, tid=11, pid=11)
trace.add_atrace_begin(ts=2025, tid=11, pid=11, buf='activityResume')
trace.add_atrace_end(ts=2028, tid=11, pid=11)
trace.add_atrace_async_begin(
    ts=2030, tid=11, pid=11, buf='animator:View(id/status_bar):translationZ')
trace.add_atrace_async_end(
    ts=2040, tid=11, pid=11, buf='animator:View(id/status_bar):translationZ')
trace.add_atrace_begin(ts=2050, tid=11, pid=11, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=2060, tid=11, pid=11)
trace.add_atrace_async_end(
    ts=2065, tid=2, pid=2, buf='launching: com.google.android.deskclock')
trace.add_atrace_begin(
    ts=2065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=2066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=3000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=3001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=3010, tid=2, pid=2, buf='launching: com.google.android.dialer')
trace.add_atrace_begin(ts=3020, tid=12, pid=12, buf='activityStart')
trace.add_atrace_end(ts=3025, tid=12, pid=12)
trace.add_atrace_begin(ts=3025, tid=12, pid=12, buf='activityResume')
trace.add_atrace_end(ts=3028, tid=12, pid=12)
trace.add_atrace_async_begin(ts=3030, tid=12, pid=12, buf='animator:scaleX')
trace.add_atrace_async_end(ts=3040, tid=12, pid=12, buf='animator:scaleX')
trace.add_atrace_begin(ts=3050, tid=12, pid=12, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=3060, tid=12, pid=12)
trace.add_atrace_async_end(
    ts=3065, tid=2, pid=2, buf='launching: com.google.android.dialer')
trace.add_atrace_begin(
    ts=3065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=3066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=4000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=4001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=4010, tid=2, pid=2, buf='launching: com.google.android.dialer')
trace.add_atrace_begin(ts=4020, tid=13, pid=13, buf='activityStart')
trace.add_atrace_end(ts=4025, tid=13, pid=13)
trace.add_atrace_begin(ts=4025, tid=13, pid=13, buf='activityResume')
trace.add_atrace_end(ts=4028, tid=13, pid=13)
trace.add_atrace_async_begin(
    ts=4030, tid=13, pid=13, buf='animator:View(id/icon):scaleX')
trace.add_atrace_async_end(
    ts=4040, tid=13, pid=13, buf='animator:View(id/icon):scaleX')
trace.add_atrace_begin(ts=4050, tid=13, pid=13, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=4060, tid=13, pid=13)
trace.add_atrace_async_end(
    ts=4065, tid=2, pid=2, buf='launching: com.google.android.dialer')
trace.add_atrace_begin(
    ts=4065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=4066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=5000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=5001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=5010, tid=2, pid=2, buf='launching: com.google.android.gm')
trace.add_atrace_begin(ts=5020, tid=14, pid=14, buf='activityStart')
trace.add_atrace_end(ts=5025, tid=14, pid=14)
trace.add_atrace_begin(ts=5025, tid=14, pid=14, buf='activityResume')
trace.add_atrace_end(ts=5028, tid=14, pid=14)
trace.add_atrace_async_begin(ts=5030, tid=14, pid=14, buf='animator:elevation')
trace.add_atrace_async_end(ts=5040, tid=14, pid=14, buf='animator:elevation')
trace.add_atrace_begin(ts=5050, tid=14, pid=14, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=5060, tid=14, pid=14)
trace.add_atrace_async_end(
    ts=5065, tid=2, pid=2, buf='launching: com.google.android.gm')
trace.add_atrace_begin(
    ts=5065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=5066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=6000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=6001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=6010, tid=2, pid=2, buf='launching: com.google.android.gm')
trace.add_atrace_begin(ts=6020, tid=15, pid=15, buf='activityStart')
trace.add_atrace_end(ts=6025, tid=15, pid=15)
trace.add_atrace_begin(ts=6025, tid=15, pid=15, buf='activityResume')
trace.add_atrace_end(ts=6028, tid=15, pid=15)
trace.add_atrace_async_begin(
    ts=6030, tid=15, pid=15, buf='animator:View(id/compose_button):elevation')
trace.add_atrace_async_end(
    ts=6040, tid=15, pid=15, buf='animator:View(id/compose_button):elevation')
trace.add_atrace_begin(ts=6050, tid=15, pid=15, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=6060, tid=15, pid=15)
trace.add_atrace_async_end(
    ts=6065, tid=2, pid=2, buf='launching: com.google.android.gm')
trace.add_atrace_begin(
    ts=6065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=6066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=7000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=7001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=7010, tid=2, pid=2, buf='launching: com.google.android.apps.messaging')
trace.add_atrace_begin(ts=7020, tid=16, pid=16, buf='activityStart')
trace.add_atrace_end(ts=7025, tid=16, pid=16)
trace.add_atrace_begin(ts=7025, tid=16, pid=16, buf='activityResume')
trace.add_atrace_end(ts=7028, tid=16, pid=16)
trace.add_atrace_async_begin(
    ts=7030, tid=16, pid=16, buf='animator:translationZ')
trace.add_atrace_async_end(ts=7040, tid=16, pid=16, buf='animator:translationZ')
trace.add_atrace_begin(ts=7035, tid=16, pid=16, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=7045, tid=16, pid=16)
trace.add_atrace_async_end(
    ts=7050, tid=2, pid=2, buf='launching: com.google.android.apps.messaging')
trace.add_atrace_begin(
    ts=7050,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=7051, tid=2, pid=2)

trace.add_atrace_begin(
    ts=8000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=8001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=8010, tid=2, pid=2, buf='launching: com.google.android.apps.messaging')
trace.add_atrace_begin(ts=8020, tid=17, pid=17, buf='activityStart')
trace.add_atrace_end(ts=8025, tid=17, pid=17)
trace.add_atrace_begin(ts=8025, tid=17, pid=17, buf='activityResume')
trace.add_atrace_end(ts=8028, tid=17, pid=17)
trace.add_atrace_async_begin(
    ts=8030, tid=17, pid=17, buf='animator:View(id/status_bar):translationZ')
trace.add_atrace_async_end(
    ts=8040, tid=17, pid=17, buf='animator:View(id/status_bar):translationZ')
trace.add_atrace_begin(ts=8035, tid=17, pid=17, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=8045, tid=17, pid=17)
trace.add_atrace_async_end(
    ts=8050, tid=2, pid=2, buf='launching: com.google.android.apps.messaging')
trace.add_atrace_begin(
    ts=8050,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=8051, tid=2, pid=2)

trace.add_atrace_begin(
    ts=9000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=9001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=9010, tid=2, pid=2, buf='launching: com.netflix.mediaclient')
trace.add_atrace_begin(ts=9020, tid=18, pid=18, buf='activityStart')
trace.add_atrace_end(ts=9025, tid=18, pid=18)
trace.add_atrace_begin(ts=9025, tid=18, pid=18, buf='activityResume')
trace.add_atrace_end(ts=9028, tid=18, pid=18)
trace.add_atrace_async_begin(ts=9030, tid=18, pid=18, buf='animator:alpha')
trace.add_atrace_async_end(ts=9040, tid=18, pid=18, buf='animator:alpha')
trace.add_atrace_begin(ts=9015, tid=18, pid=18, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=9025, tid=18, pid=18)
trace.add_atrace_async_end(
    ts=9045, tid=2, pid=2, buf='launching: com.netflix.mediaclient')
trace.add_atrace_begin(
    ts=9045,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=9046, tid=2, pid=2)

trace.add_atrace_begin(
    ts=10000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=10001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=10010, tid=2, pid=2, buf='launching: com.netflix.mediaclient')
trace.add_atrace_begin(ts=10020, tid=19, pid=19, buf='activityStart')
trace.add_atrace_end(ts=10025, tid=19, pid=19)
trace.add_atrace_begin(ts=10025, tid=19, pid=19, buf='activityResume')
trace.add_atrace_end(ts=10028, tid=19, pid=19)
trace.add_atrace_async_begin(
    ts=10030, tid=19, pid=19, buf='animator:View(id/splash):alpha')
trace.add_atrace_async_end(
    ts=10040, tid=19, pid=19, buf='animator:View(id/splash):alpha')
trace.add_atrace_begin(
    ts=10015, tid=19, pid=19, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=10025, tid=19, pid=19)
trace.add_atrace_async_end(
    ts=10045, tid=2, pid=2, buf='launching: com.netflix.mediaclient')
trace.add_atrace_begin(
    ts=10045,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=10046, tid=2, pid=2)

trace.add_atrace_begin(
    ts=11000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=11001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=11010, tid=2, pid=2, buf='launching: com.google.android.apps.photos')
trace.add_atrace_begin(ts=11020, tid=20, pid=20, buf='activityStart')
trace.add_atrace_end(ts=11025, tid=20, pid=20)
trace.add_atrace_begin(ts=11025, tid=20, pid=20, buf='activityResume')
trace.add_atrace_end(ts=11028, tid=20, pid=20)
trace.add_atrace_async_begin(
    ts=11030, tid=20, pid=20, buf='animator:translationZ')
trace.add_atrace_async_end(
    ts=11040, tid=20, pid=20, buf='animator:translationZ')
trace.add_atrace_begin(
    ts=11050, tid=20, pid=20, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=11060, tid=20, pid=20)
trace.add_atrace_async_end(
    ts=11065, tid=2, pid=2, buf='launching: com.google.android.apps.photos')
trace.add_atrace_begin(
    ts=11065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=11066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=12000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=12001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=12010, tid=2, pid=2, buf='launching: com.google.android.apps.photos')
trace.add_atrace_begin(ts=12020, tid=21, pid=21, buf='activityStart')
trace.add_atrace_end(ts=12025, tid=21, pid=21)
trace.add_atrace_begin(ts=12025, tid=21, pid=21, buf='activityResume')
trace.add_atrace_end(ts=12028, tid=21, pid=21)
trace.add_atrace_async_begin(
    ts=12030, tid=21, pid=21, buf='animator:View(id/toolbar):translationZ')
trace.add_atrace_async_end(
    ts=12040, tid=21, pid=21, buf='animator:View(id/toolbar):translationZ')
trace.add_atrace_begin(
    ts=12050, tid=21, pid=21, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=12060, tid=21, pid=21)
trace.add_atrace_async_end(
    ts=12065, tid=2, pid=2, buf='launching: com.google.android.apps.photos')
trace.add_atrace_begin(
    ts=12065,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=12066, tid=2, pid=2)

trace.add_atrace_begin(
    ts=13000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=13001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=13010, tid=2, pid=2, buf='launching: com.twitter.android')
trace.add_atrace_begin(ts=13020, tid=22, pid=22, buf='activityStart')
trace.add_atrace_end(ts=13025, tid=22, pid=22)
trace.add_atrace_begin(ts=13025, tid=22, pid=22, buf='activityResume')
trace.add_atrace_end(ts=13028, tid=22, pid=22)
trace.add_atrace_async_begin(ts=13030, tid=22, pid=22, buf='animator')
trace.add_atrace_async_end(ts=13040, tid=22, pid=22, buf='animator')
trace.add_atrace_begin(
    ts=13025, tid=22, pid=22, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=13035, tid=22, pid=22)
trace.add_atrace_async_end(
    ts=13045, tid=2, pid=2, buf='launching: com.twitter.android')
trace.add_atrace_begin(
    ts=13045,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=13046, tid=2, pid=2)

trace.add_atrace_begin(
    ts=14000,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyIntentStarted')
trace.add_atrace_end(ts=14001, tid=2, pid=2)
trace.add_atrace_async_begin(
    ts=14010, tid=2, pid=2, buf='launching: com.twitter.android')
trace.add_atrace_begin(ts=14020, tid=23, pid=23, buf='activityStart')
trace.add_atrace_end(ts=14025, tid=23, pid=23)
trace.add_atrace_begin(ts=14025, tid=23, pid=23, buf='activityResume')
trace.add_atrace_end(ts=14028, tid=23, pid=23)
trace.add_atrace_async_begin(
    ts=14030, tid=23, pid=23, buf='animator:View(id/logo)')
trace.add_atrace_async_end(
    ts=14040, tid=23, pid=23, buf='animator:View(id/logo)')
trace.add_atrace_begin(
    ts=14025, tid=23, pid=23, buf='Choreographer#doFrame 123')
trace.add_atrace_end(ts=14035, tid=23, pid=23)
trace.add_atrace_async_end(
    ts=14045, tid=2, pid=2, buf='launching: com.twitter.android')
trace.add_atrace_begin(
    ts=14045,
    tid=2,
    pid=2,
    buf='MetricsLogger:launchObserverNotifyActivityLaunchFinished')
trace.add_atrace_end(ts=14046, tid=2, pid=2)

sys.stdout.buffer.write(trace.trace.SerializeToString())
