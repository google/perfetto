#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ArtMethodParser(TestSuite):

  def test_art_method_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('art-method-tracing.trace'),
        query="""
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT ts, dur, name, thread_name, extract_arg(arg_set_id, 'pathname') AS pathname
          FROM thread_slice
          ORDER BY dur desc
          LIMIT 10
        """,
        out=Csv('''
          "ts","dur","name","thread_name","pathname"
          430421819633000,182000,"androidx.benchmark.MethodTracing.start: (Ljava/lang/String;)Landroidx/benchmark/Profiler$ResultFile;","Instr: androidx.test.runner.AndroidJUnitRunner","Profiler.kt"
          430421819633000,175000,"androidx.benchmark.ProfilerKt.startRuntimeMethodTracing: (Ljava/lang/String;Z)Landroidx/benchmark/Profiler$ResultFile;","Instr: androidx.test.runner.AndroidJUnitRunner","Profiler.kt"
          430421819634000,67000,"android.os.Debug.startMethodTracing: (Ljava/lang/String;II)V","Instr: androidx.test.runner.AndroidJUnitRunner","Debug.java"
          430421819635000,62000,"dalvik.system.VMDebug.startMethodTracing: (Ljava/lang/String;IIZI)V","Instr: androidx.test.runner.AndroidJUnitRunner","VMDebug.java"
          430421819636000,57000,"dalvik.system.VMDebug.startMethodTracingFilename: (Ljava/lang/String;IIZI)V","Instr: androidx.test.runner.AndroidJUnitRunner","VMDebug.java"
          430421819788000,19000,"androidx.benchmark.Profiler$ResultFile.<init>: (Ljava/lang/String;Ljava/lang/String;)V","Instr: androidx.test.runner.AndroidJUnitRunner","Profiler.kt"
          430421819795000,2000,"kotlin.jvm.internal.Intrinsics.checkNotNullParameter: (Ljava/lang/Object;Ljava/lang/String;)V","Instr: androidx.test.runner.AndroidJUnitRunner","Intrinsics.java"
          430421819817000,2000,"androidx.benchmark.vmtrace.ArtTraceTest.myTracedMethod: ()V","Instr: androidx.test.runner.AndroidJUnitRunner","ArtTraceTest.kt"
          430421819801000,1000,"kotlin.jvm.internal.Intrinsics.checkNotNullParameter: (Ljava/lang/Object;Ljava/lang/String;)V","Instr: androidx.test.runner.AndroidJUnitRunner","Intrinsics.java"
          430421819804000,1000,"java.lang.Object.<init>: ()V","Instr: androidx.test.runner.AndroidJUnitRunner","Object.java"
        '''))

  def test_art_method_streaming_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('art-method-tracing-streaming.trace'),
        query="""
          INCLUDE PERFETTO MODULE slices.with_context;

          SELECT ts, dur, name, thread_name, extract_arg(arg_set_id, 'pathname') AS pathname
          FROM thread_slice
          ORDER BY dur desc
          LIMIT 10
        """,
        out=Csv('''
          "ts","dur","name","thread_name","pathname"
          793682939000,26513017000,"java.util.concurrent.ThreadPoolExecutor.getTask: ()Ljava/lang/Runnable;","AsyncTask #1","ThreadPoolExecutor.java"
          793682939000,26513017000,"java.util.concurrent.LinkedBlockingQueue.take: ()Ljava/lang/Object;","AsyncTask #1","LinkedBlockingQueue.java"
          793682939000,26513017000,"java.util.concurrent.locks.AbstractQueuedSynchronizer$ConditionObject.await: ()V","AsyncTask #1","AbstractQueuedSynchronizer.java"
          793682939000,26513017000,"java.util.concurrent.locks.LockSupport.park: (Ljava/lang/Object;)V","AsyncTask #1","LockSupport.java"
          793682939000,26513017000,"sun.misc.Unsafe.park: (ZJ)V","AsyncTask #1","Unsafe.java"
          793682939000,26513017000,"java.lang.Thread.parkFor$: (J)V","AsyncTask #1","Thread.java"
          793682939000,26513017000,"java.lang.Object.wait: (JI)V","AsyncTask #1","Object.java"
          810910588000,13004716000,"java.lang.Object.wait: ()V","ReferenceQueueDaemon","Object.java"
          808685761000,12599705000,"java.lang.Object.wait: (JI)V","OkHttp ConnectionPool","Object.java"
          800148789000,10759203000,"java.lang.Object.wait: ()V","ReferenceQueueDaemon","Object.java"
        '''))
