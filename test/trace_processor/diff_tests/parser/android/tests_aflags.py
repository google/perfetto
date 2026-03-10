#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class AndroidAflags(TestSuite):

  def test_android_aflags(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1000
          android_aflags {
            flags {
              package: "com.android.settings"
              name: "my_flag"
              flag_namespace: "settings_ns"
              container: "system"
              value: "enabled"
              staged_value: "disabled"
              permission: FLAG_PERMISSION_READ_WRITE
              value_picked_from: VALUE_PICKED_FROM_LOCAL
              storage_backend: FLAG_STORAGE_BACKEND_ACONFIGD
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.aflags;
        SELECT ts, package, name, flag_namespace, container, value, staged_value, permission, value_picked_from, storage_backend
        FROM android_aflags;
        """,
        out=Csv("""
        "ts","package","name","flag_namespace","container","value","staged_value","permission","value_picked_from","storage_backend"
        1000,"com.android.settings","my_flag","settings_ns","system","enabled","disabled","read-write","local","aconfigd"
        """))
