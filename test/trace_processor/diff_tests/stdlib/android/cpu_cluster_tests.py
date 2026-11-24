#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite

_UTILIZATION_TRACE = TextProto(r"""
packet {
  clock_snapshot {
    clocks { clock_id: 3 timestamp: 0 }
    clocks { clock_id: 6 timestamp: 0 }
  }
}
packet {
  cpu_info {
    cpus { processor: "little" capacity: 100 }
    cpus { processor: "little" capacity: 100 }
    cpus { processor: "big" capacity: 1000 }
  }
}
packet {
  ftrace_events {
    cpu: 0
    event { timestamp: 1000000 pid: 0 sched_switch { prev_comm: "swapper/0" prev_pid: 0 prev_prio: 120 prev_state: 0 next_comm: "t1" next_pid: 1 next_prio: 120 } }
    event { timestamp: 2000000 pid: 1 sched_switch { prev_comm: "t1" prev_pid: 1 prev_prio: 120 prev_state: 0 next_comm: "swapper/0" next_pid: 0 next_prio: 120 } }
  }
}
packet {
  ftrace_events {
    cpu: 2
    event { timestamp: 1500000 pid: 0 sched_switch { prev_comm: "swapper/2" prev_pid: 0 prev_prio: 120 prev_state: 0 next_comm: "t2" next_pid: 2 next_prio: 120 } }
    event { timestamp: 2000000 pid: 2 sched_switch { prev_comm: "t2" prev_pid: 2 prev_prio: 120 prev_state: 0 next_comm: "swapper/2" next_pid: 0 next_prio: 120 } }
  }
}
""")


class CpuClusters(TestSuite):

  def test_android_cpu_cluster_mapping_is_partitioned_by_machine(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          cpu_info {
            cpus { processor: "host little" capacity: 100 frequencies: 100000 }
            cpus { processor: "host big" capacity: 1000 frequencies: 200000 }
          }
        }
        packet {
          machine_id: 1001
          cpu_info {
            cpus { processor: "guest little" capacity: 200 frequencies: 100000 }
            cpus { processor: "guest big" capacity: 900 frequencies: 200000 }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_type;

        SELECT
          m.raw_id AS raw_machine_id,
          c.cpu,
          c.cluster_type
        FROM android_cpu_cluster_mapping c
        JOIN machine m ON m.id = c.machine_id
        ORDER BY c.machine_id, c.cpu;
        """,
        out=Csv("""
        "raw_machine_id","cpu","cluster_type"
        0,0,"little"
        0,1,"big"
        1001,0,"little"
        1001,1,"big"
        """))

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

  def test_android_cpu_cluster_utilization_in_interval(self):
    return DiffTestBlueprint(
        trace=_UTILIZATION_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.cpu.cluster_utilization;

        SELECT
          cluster_type,
          core_count,
          active_dur,
          utilization
        FROM android_cpu_cluster_utilization_in_interval(1000000, 2000000)
        ORDER BY cluster_type;
        """,
        out=Csv("""
        "cluster_type","core_count","active_dur","utilization"
        "big",1,500000,0.250000
        "little",2,1000000,0.250000
        """))
