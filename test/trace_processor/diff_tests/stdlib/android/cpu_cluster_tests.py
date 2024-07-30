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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class CpuClusters(TestSuite):

  def test_android_cpu_cluster_type_one_core(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            cpu_info {
              cpus {
                processor: "unknown"
                capacity: 1024
                frequencies: 100000
                frequencies: 200000
              }
            }
          }
          """),
        query="""
          INCLUDE PERFETTO MODULE android.cpu.cluster_type;

          SELECT
            ucpu,
            cpu,
            cluster_type
          FROM
            android_cpu_cluster_mapping;
          """,
        out=Csv("""
          "ucpu","cpu","cluster_type"
          0,0,"[NULL]"
          """))

  def test_android_cpu_cluster_type_two_core(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            cpu_info {
              cpus {
                processor: "unknown"
                capacity: 158
                frequencies: 100000
                frequencies: 200000
              }
              cpus {
                processor: "unknown"
                capacity: 1024
                frequencies: 500000
                frequencies: 574000
              }
            }
          }
          """),
        query="""
          INCLUDE PERFETTO MODULE android.cpu.cluster_type;

          SELECT
            ucpu,
            cpu,
            cluster_type
          FROM
            android_cpu_cluster_mapping;
          """,
        out=Csv("""
          "ucpu","cpu","cluster_type"
          0,0,"little"
          1,1,"big"
          """))

  def test_android_cpu_cluster_type_three_core(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus {
              processor: "unknown"
              capacity: 158
              frequencies: 100000
              frequencies: 200000
            }
            cpus {
              processor: "unknown"
              capacity: 550
              frequencies: 300000
              frequencies: 400000
            }
            cpus {
              processor: "unknown"
              capacity: 1024
              frequencies: 500000
              frequencies: 574000
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          ucpu,
          cpu,
          cluster_type
        FROM
          android_cpu_cluster_mapping;
        """,
        out=Csv("""
        "ucpu","cpu","cluster_type"
        0,0,"little"
        1,1,"medium"
        2,2,"big"
        """))

  def test_android_cpu_cluster_type_four_core(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus {
              processor: "unknown"
              capacity: 158
              frequencies: 100000
              frequencies: 200000
            }
            cpus {
              processor: "unknown"
              capacity: 550
              frequencies: 300000
              frequencies: 400000
            }
            cpus {
              processor: "unknown"
              capacity: 700
              frequencies: 400000
              frequencies: 500000
            }
            cpus {
              processor: "unknown"
              capacity: 1024
              frequencies: 500000
              frequencies: 574000
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          ucpu,
          cpu,
          cluster_type
        FROM
          android_cpu_cluster_mapping;
        """,
        out=Csv("""
        "ucpu","cpu","cluster_type"
        0,0,"little"
        1,1,"medium"
        2,2,"medium"
        3,3,"big"
        """))

  def test_android_cpu_cluster_type_five_core(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus {
              processor: "unknown"
              capacity: 158
              frequencies: 100000
              frequencies: 200000
            }
            cpus {
              processor: "unknown"
              capacity: 550
              frequencies: 300000
              frequencies: 400000
            }
            cpus {
              processor: "unknown"
              capacity: 700
              frequencies: 400000
              frequencies: 500000
            }
            cpus {
              processor: "unknown"
              capacity: 800
              frequencies: 500000
              frequencies: 520000
            }
            cpus {
              processor: "unknown"
              capacity: 1024
              frequencies: 500000
              frequencies: 574000
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          ucpu,
          cpu,
          cluster_type
        FROM
          android_cpu_cluster_mapping;
        """,
        out=Csv("""
        "ucpu","cpu","cluster_type"
        0,0,"[NULL]"
        1,1,"[NULL]"
        2,2,"[NULL]"
        3,3,"[NULL]"
        4,4,"[NULL]"
        """))

  def test_android_cpu_cluster_type_capacity_not_present(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus {
              processor: "unknown"
              frequencies: 100000
              frequencies: 200000
            }
            cpus {
              processor: "unknown"
              frequencies: 300000
              frequencies: 400000
            }
            cpus {
              processor: "unknown"
              frequencies: 500000
              frequencies: 574000
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          ucpu,
          cpu,
          cluster_type
        FROM
          android_cpu_cluster_mapping;
        """,
        out=Csv("""
        "ucpu","cpu","cluster_type"
        0,0,"little"
        1,1,"medium"
        2,2,"big"
        """))

  def test_android_cpu_cluster_type_insufficient_data_to_calculate(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus {
              processor: "unknown"
              frequencies: 10000
            }
            cpus {
              processor: "unknown"
              frequencies: 10000
            }
            cpus {
              processor: "unknown"
              frequencies: 10000
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          ucpu,
          cpu,
          cluster_type
        FROM
          android_cpu_cluster_mapping;
        """,
        out=Csv("""
        "ucpu","cpu","cluster_type"
        0,0,"[NULL]"
        1,1,"[NULL]"
        2,2,"[NULL]"
        """))

  def test_android_cpu_cluster_type_no_frequencies(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus {
              processor: "unknown"
            }
            cpus {
              processor: "unknown"
            }
            cpus {
              processor: "unknown"
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          ucpu,
          cpu,
          cluster_type
        FROM
          android_cpu_cluster_mapping;
        """,
        out=Csv("""
        "ucpu","cpu","cluster_type"
        0,0,"[NULL]"
        1,1,"[NULL]"
        2,2,"[NULL]"
        """))
