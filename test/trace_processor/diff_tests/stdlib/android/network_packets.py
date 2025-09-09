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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class AndroidNetworkPackets(TestSuite):

  def test_network_uptime_spans(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.network_packets;

        WITH TestData(id, ts, dur, iface, packet_count, packet_length) AS (
            VALUES
                (0, 5, 0, "wlan", 1, 70),
                (1, 5, 0, "wlan", 1, 80),
                (2, 8, 0, "wlan", 1, 90),

                (3, 20, 5, "rmnet", 4, 320),
                (4, 22, 8, "rmnet", 4, 320),
                (5, 21, 0, "rmnet", 1, 80),
                (6, 26, 0, "rmnet", 1, 80),

                (7, 24, 0, "wlan", 1, 80)
        )
        SELECT *
        FROM android_network_uptime_spans!(
            TestData, (iface), 10
        )
        ORDER BY ts, iface;
        """,
        out=Csv("""
        "iface","ts","dur","packet_count","packet_length"
        "wlan",5,13,3,240
        "rmnet",20,20,10,800
        "wlan",24,10,1,80
         """))

  def test_network_uptime_cost(self):
    return DiffTestBlueprint(
        trace=TextProto(""),
        query="""
        INCLUDE PERFETTO MODULE android.network_packets;

        WITH TestData(id, ts, dur, iface, packet_count, packet_length) AS (
            VALUES
                (0, 5, 0, "wlan", 1, 70),
                (1, 5, 0, "wlan", 1, 80),
                (2, 8, 0, "wlan", 1, 90),

                (3, 20, 5, "rmnet", 4, 320),
                (4, 22, 8, "rmnet", 4, 320),
                (5, 21, 0, "rmnet", 1, 80),
                (6, 26, 0, "rmnet", 1, 80),

                (7, 24, 0, "wlan", 1, 80)
        )
        SELECT *
        FROM android_network_uptime_cost!(
            TestData, (iface), 10
        )
        ORDER BY id;
        """,
        out=Csv("""
        "id","uptime_cost"
        0,10
        1,0
        2,3
        3,14
        4,4
        5,1
        6,1
        7,10
         """))
