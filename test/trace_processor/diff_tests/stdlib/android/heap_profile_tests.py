#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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


class HeapProfile(TestSuite):

  def test_heap_profile_summary_tree(self):
    return DiffTestBlueprint(
        trace=DataPath('system-server-native-profile'),
        query="""
          INCLUDE PERFETTO MODULE android.memory.heap_profile.summary_tree;

          SELECT
            name,
            self_size,
            cumulative_size,
            self_alloc_size,
            cumulative_alloc_size
          FROM android_heap_profile_summary_tree
          ORDER BY cumulative_size DESC, name
          LIMIT 10;
        """,
        out=Csv("""
          "name","self_size","cumulative_size","self_alloc_size","cumulative_alloc_size"
          "__pthread_start(void*)",0,84848,0,1084996
          "__start_thread",0,84848,0,1084996
          "art::ArtMethod::Invoke(art::Thread*, unsigned int*, unsigned int, art::JValue*, char const*)",0,57144,0,736946
          "art::JValue art::InvokeVirtualOrInterfaceWithJValues<art::ArtMethod*>(art::ScopedObjectAccessAlreadyRunnable const&, _jobject*, art::ArtMethod*, jvalue const*)",0,57144,0,736946
          "art::Thread::CreateCallback(void*)",0,57144,0,736946
          "art_quick_invoke_stub",0,57144,0,736946
          "android.os.HandlerThread.run",0,53048,0,197068
          "com.android.server.UiThread.run",0,53048,0,197068
          "android::AndroidRuntime::javaThreadShell(void*)",0,27704,0,348050
          "(anonymous namespace)::nativeInitSensorEventQueue(_JNIEnv*, _jclass*, long, _jobject*, _jobject*, _jstring*, int)",0,26624,0,26624
        """))
