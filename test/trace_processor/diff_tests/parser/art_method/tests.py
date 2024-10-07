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
          SELECT ts, dur, name, extract_arg(arg_set_id, 'pathname') AS pathname
          FROM slice
          LIMIT 10
        """,
        out=Csv('''
          "ts","dur","name","pathname"
          430421819465000,-1,"com.android.internal.os.ZygoteInit.main: ([Ljava/lang/String;)V","ZygoteInit.java"
          430421819468000,-1,"com.android.internal.os.RuntimeInit$MethodAndArgsCaller.run: ()V","RuntimeInit.java"
          430421819469000,-1,"java.lang.reflect.Method.invoke: (Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;","Method.java"
          430421819472000,-1,"android.app.ActivityThread.main: ([Ljava/lang/String;)V","ActivityThread.java"
          430421819473000,-1,"android.os.Looper.loop: ()V","Looper.java"
          430421819473000,-1,"android.os.Looper.loopOnce: (Landroid/os/Looper;JI)Z","Looper.java"
          430421819475000,-1,"android.os.MessageQueue.next: ()Landroid/os/Message;","MessageQueue.java"
          430421819476000,-1,"android.os.MessageQueue.nativePollOnce: (JI)V","MessageQueue.java"
          430421819490000,-1,"java.lang.Thread.run: ()V","Thread.java"
          430421819508000,-1,"java.lang.Daemons$Daemon.run: ()V","Daemons.java"
        '''))
