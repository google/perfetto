#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PackageLookup(TestSuite):

  def test_package_lookup(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 0
          packages_list {
            packages {
              name: "dynsystem"
              uid: 1000
            }
            packages {
              name: "com.google.android.gsf"
              uid: 10001
            }
            packages {
              name: "com.google.android.gms"
              uid: 10001
            }
            packages {
              name: "com.android.providers.youtube"
              uid: 10002
            }
            packages {
              name: "com.google.android.youtube"
              uid: 10002
            }
          }
        }
        """),
        query="""
        SELECT uid, package_lookup(uid) AS package_name
        FROM (
          SELECT 1000 AS uid UNION ALL    -- System package
          SELECT 10001 AS uid UNION ALL   -- GMS preferred
          SELECT 10002 AS uid UNION ALL   -- Provider de-preferred
          SELECT 1010002 AS uid UNION ALL -- Multi-user
          SELECT 10003 AS uid UNION ALL   -- Package not found
          SELECT null AS uid              -- Null uid
        );
        """,
        out=Csv("""
        "uid","package_name"
        1000,"AID_SYSTEM_USER"
        10001,"com.google.android.gms"
        10002,"com.google.android.youtube"
        1010002,"com.google.android.youtube"
        10003,"uid=10003"
        "[NULL]","[NULL]"
        """))
