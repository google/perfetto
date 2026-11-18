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

# linux.cgroup memcg values of:
# - the cgroup of process 42 (uid 10001), which ends at 2500,
# - the cgroups of uids 10001, 1010001 (10001 for user 10), 10002 (a shared
#   uid with two packages) and 1000 (a system uid with a package),
# - the top-level apps and system cgroups.
# The polls at 2000, 4000 and 5000 only have the values that changed
# (MemcgConfig.cache_ttl_ms); the one at 3000 has all of them. A limit set to
# "max" is UINT64_MAX.
MEMCG_TRACE = TextProto(r"""
packet {
  timestamp: 500
  process_tree {
    processes { pid: 42 ppid: 1 cmdline: "com.example.app" uid: 10001 }
  }
}
packet {
  timestamp: 500
  packages_list {
    packages { name: "com.example.app" uid: 10001 }
    packages { name: "com.example.shared.b" uid: 10002 }
    packages { name: "com.example.shared.a" uid: 10002 }
    packages { name: "android" uid: 1000 }
  }
}
packet {
  timestamp: 1000
  memcg_stats {
    cgroup {
      android_group: GROUP_APPS
      uid: 10001
      pid: 42
      current: 100
      high: 18446744073709551615
      events_local { high: 0 }
      stat { anon: 20 file: 60 shmem: 10 }
    }
    cgroup {
      android_group: GROUP_APPS
      uid: 10001
      current: 300
      high: 1000
      stat { file: 200 shmem: 50 }
    }
    cgroup { android_group: GROUP_APPS uid: 1010001 current: 5 }
    cgroup { android_group: GROUP_APPS uid: 10002 current: 7 }
    cgroup { android_group: GROUP_SYSTEM uid: 1000 current: 50 }
    cgroup { android_group: GROUP_APPS current: 2000 }
    cgroup { android_group: GROUP_SYSTEM current: 1000 }
  }
}
packet {
  timestamp: 2000
  memcg_stats {
    cgroup {
      android_group: GROUP_APPS
      uid: 10001
      pid: 42
      current: 150
      high: 120
      events_local { high: 3 }
      stat { shmem: 20 }
    }
    cgroup { android_group: GROUP_APPS uid: 10001 current: 1200 }
  }
}
packet {
  ftrace_events {
    cpu: 0
    event {
      timestamp: 2500
      pid: 0
      sched_process_free { comm: "com.example.app" pid: 42 prio: 120 }
    }
  }
}
packet {
  timestamp: 3000
  memcg_stats {
    cgroup {
      android_group: GROUP_APPS
      uid: 10001
      current: 1200
      high: 18446744073709551615
      stat { file: 200 shmem: 50 }
    }
    cgroup { android_group: GROUP_APPS uid: 1010001 current: 5 }
    cgroup { android_group: GROUP_APPS uid: 10002 current: 7 }
    cgroup { android_group: GROUP_SYSTEM uid: 1000 current: 50 }
    cgroup { android_group: GROUP_APPS current: 2000 }
    cgroup { android_group: GROUP_SYSTEM current: 1000 }
  }
}
packet {
  timestamp: 4000
  memcg_stats {
    cgroup {
      android_group: GROUP_APPS
      uid: 10001
      stat { file: 40 }
    }
  }
}
packet {
  timestamp: 5000
  memcg_stats {
    cgroup { android_group: GROUP_APPS current: 2100 }
  }
}
""")

# Process 77 under three different (group, uid) pairs: pid 77 is reused, and,
# as the trace doesn't say when processes end, all three have the same upid.
PID_REUSE_TRACE = TextProto(r"""
packet {
  timestamp: 1000
  memcg_stats {
    cgroup {
      android_group: GROUP_APPS
      uid: 10003
      pid: 77
      current: 50
      high: 100
      stat { file: 30 shmem: 10 }
    }
  }
}
packet {
  timestamp: 2000
  memcg_stats {
    cgroup {
      android_group: GROUP_APPS
      uid: 10004
      pid: 77
      current: 300
      high: 1000
      stat { file: 200 shmem: 20 }
    }
  }
}
packet {
  timestamp: 3000
  memcg_stats {
    cgroup {
      android_group: GROUP_SYSTEM
      uid: 10004
      pid: 77
      current: 7
    }
  }
}
packet {
  timestamp: 4000
  memcg_stats {
    cgroup { android_group: GROUP_SYSTEM current: 1000 }
  }
}
""")


class AndroidMemcg(TestSuite):

  # One row per track, with the level and kind from the track type, the
  # dimensions, and a package for app uids (also for other users) only.
  def test_android_memcg_track(self):
    return DiffTestBlueprint(
        trace=MEMCG_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.memory.memcg;
        SELECT
          t.level,
          t.kind,
          t.memcg_group,
          t.uid,
          p.pid,
          t.key,
          t.name,
          t.unit,
          t.package_name
        FROM android_memcg_track AS t
        LEFT JOIN process AS p USING (upid)
        ORDER BY t.name;
        """,
        out=Csv("""
        "level","kind","memcg_group","uid","pid","key","name","unit","package_name"
        "group","size","apps","[NULL]","[NULL]","current","memcg.apps.current","bytes","[NULL]"
        "uid","size","apps",10001,"[NULL]","current","memcg.apps.uid_10001.current","bytes","com.example.app"
        "uid","limit","apps",10001,"[NULL]","high","memcg.apps.uid_10001.high","bytes","com.example.app"
        "uid","size","apps",10001,"[NULL]","stat.file","memcg.apps.uid_10001.stat.file","bytes","com.example.app"
        "uid","size","apps",10001,"[NULL]","stat.shmem","memcg.apps.uid_10001.stat.shmem","bytes","com.example.app"
        "uid","size","apps",10002,"[NULL]","current","memcg.apps.uid_10002.current","bytes","com.example.shared.a"
        "uid","size","apps",1010001,"[NULL]","current","memcg.apps.uid_1010001.current","bytes","com.example.app"
        "process","size","apps",10001,42,"current","memcg.current","bytes","com.example.app"
        "process","event","apps",10001,42,"events.local.high","memcg.events.local.high","[NULL]","com.example.app"
        "process","limit","apps",10001,42,"high","memcg.high","bytes","com.example.app"
        "process","size","apps",10001,42,"stat.anon","memcg.stat.anon","bytes","com.example.app"
        "process","size","apps",10001,42,"stat.file","memcg.stat.file","bytes","com.example.app"
        "process","size","apps",10001,42,"stat.shmem","memcg.stat.shmem","bytes","com.example.app"
        "group","size","system","[NULL]","[NULL]","current","memcg.system.current","bytes","[NULL]"
        "uid","size","system",1000,"[NULL]","current","memcg.system.uid_1000.current","bytes","[NULL]"
        """))

  # Intervals: values hold until the next one (the polls only have changes),
  # the intervals of process 42 end with it at 2500, a limit set to "max" is
  # NULL and has no delta, and event counts have their increments.
  def test_android_memcg_intervals(self):
    return DiffTestBlueprint(
        trace=MEMCG_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.memory.memcg;
        SELECT
          i.level,
          i.kind,
          i.memcg_group,
          i.uid,
          p.pid,
          i.key,
          i.ts,
          i.dur,
          i.value,
          i.delta_value
        FROM android_memcg_intervals AS i
        LEFT JOIN process AS p USING (upid)
        WHERE i.level != 'group' AND i.uid % 100000 = 10001
        ORDER BY i.level, i.uid, i.key, i.ts;
        """,
        out=Csv("""
        "level","kind","memcg_group","uid","pid","key","ts","dur","value","delta_value"
        "process","size","apps",10001,42,"current",1000,1000,100.000000,"[NULL]"
        "process","size","apps",10001,42,"current",2000,500,150.000000,50.000000
        "process","event","apps",10001,42,"events.local.high",1000,1000,0.000000,"[NULL]"
        "process","event","apps",10001,42,"events.local.high",2000,500,3.000000,3.000000
        "process","limit","apps",10001,42,"high",1000,1000,"[NULL]","[NULL]"
        "process","limit","apps",10001,42,"high",2000,500,120.000000,"[NULL]"
        "process","size","apps",10001,42,"stat.anon",1000,1500,20.000000,"[NULL]"
        "process","size","apps",10001,42,"stat.file",1000,1500,60.000000,"[NULL]"
        "process","size","apps",10001,42,"stat.shmem",1000,1000,10.000000,"[NULL]"
        "process","size","apps",10001,42,"stat.shmem",2000,500,20.000000,10.000000
        "uid","size","apps",10001,"[NULL]","current",1000,1000,300.000000,"[NULL]"
        "uid","size","apps",10001,"[NULL]","current",2000,1000,1200.000000,900.000000
        "uid","size","apps",10001,"[NULL]","current",3000,2000,1200.000000,0.000000
        "uid","limit","apps",10001,"[NULL]","high",1000,2000,1000.000000,"[NULL]"
        "uid","limit","apps",10001,"[NULL]","high",3000,2000,"[NULL]","[NULL]"
        "uid","size","apps",10001,"[NULL]","stat.file",1000,2000,200.000000,"[NULL]"
        "uid","size","apps",10001,"[NULL]","stat.file",3000,1000,200.000000,0.000000
        "uid","size","apps",10001,"[NULL]","stat.file",4000,1000,40.000000,-160.000000
        "uid","size","apps",10001,"[NULL]","stat.shmem",1000,2000,50.000000,"[NULL]"
        "uid","size","apps",10001,"[NULL]","stat.shmem",3000,2000,50.000000,0.000000
        "uid","size","apps",1010001,"[NULL]","current",1000,2000,5.000000,"[NULL]"
        "uid","size","apps",1010001,"[NULL]","current",3000,2000,5.000000,0.000000
        """))

  # A cgroup_key per cgroup, and so per (group, uid) for a process, even when
  # the processes have the same upid.
  def test_android_memcg_track_cgroup_key(self):
    return DiffTestBlueprint(
        trace=PID_REUSE_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.memory.memcg;
        SELECT
          t.level,
          t.memcg_group,
          t.uid,
          p.pid,
          count(*) AS tracks,
          count(DISTINCT t.cgroup_key) AS cgroup_keys,
          (
            SELECT count(DISTINCT cgroup_key)
            FROM android_memcg_track
          ) AS all_cgroup_keys,
          (
            SELECT count(DISTINCT upid)
            FROM android_memcg_track
            WHERE level = 'process'
          ) AS all_upids
        FROM android_memcg_track AS t
        LEFT JOIN process AS p USING (upid)
        GROUP BY t.level, t.memcg_group, t.uid, p.pid
        ORDER BY t.level, t.memcg_group, t.uid;
        """,
        out=Csv("""
        "level","memcg_group","uid","pid","tracks","cgroup_keys","all_cgroup_keys","all_upids"
        "group","system","[NULL]","[NULL]",1,1,4,1
        "process","apps",10003,77,4,1,4,1
        "process","apps",10004,77,4,1,4,1
        "process","system",10004,77,1,1,4,1
        """))

  # Derived series. file_excl_shmem changes whenever file or shmem does (the
  # polls only have the changes, so they don't change together) and doesn't go
  # below 0. headroom is NULL while high is "max" and negative while current
  # is above high.
  def test_android_memcg_derived_intervals(self):
    return DiffTestBlueprint(
        trace=MEMCG_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.memory.memcg;
        SELECT
          d.level,
          d.memcg_group,
          d.uid,
          p.pid,
          d.key,
          d.ts,
          d.dur,
          d.value
        FROM android_memcg_derived_intervals AS d
        LEFT JOIN process AS p USING (upid)
        ORDER BY d.level, d.uid, d.key, d.ts;
        """,
        out=Csv("""
        "level","memcg_group","uid","pid","key","ts","dur","value"
        "process","apps",10001,42,"file_excl_shmem",1000,1000,50.000000
        "process","apps",10001,42,"file_excl_shmem",2000,500,40.000000
        "process","apps",10001,42,"headroom",1000,1000,"[NULL]"
        "process","apps",10001,42,"headroom",2000,500,-30.000000
        "uid","apps",10001,"[NULL]","file_excl_shmem",1000,2000,150.000000
        "uid","apps",10001,"[NULL]","file_excl_shmem",3000,1000,150.000000
        "uid","apps",10001,"[NULL]","file_excl_shmem",4000,1000,0.000000
        "uid","apps",10001,"[NULL]","headroom",1000,1000,700.000000
        "uid","apps",10001,"[NULL]","headroom",2000,1000,-200.000000
        "uid","apps",10001,"[NULL]","headroom",3000,2000,"[NULL]"
        """))

  # The cgroups that only have one of the inputs of a series have no
  # intervals for it, rather than intervals with a NULL value.
  def test_android_memcg_derived_intervals_missing_input(self):
    return DiffTestBlueprint(
        trace=MEMCG_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.memory.memcg;
        SELECT
          t.name,
          (
            SELECT count(*)
            FROM android_memcg_derived_intervals AS d
            WHERE d.cgroup_key = t.cgroup_key AND d.key = 'headroom'
          ) AS headroom_intervals
        FROM android_memcg_track AS t
        WHERE t.key = 'current'
        ORDER BY t.name;
        """,
        out=Csv("""
        "name","headroom_intervals"
        "memcg.apps.current",0
        "memcg.apps.uid_10001.current",3
        "memcg.apps.uid_10002.current",0
        "memcg.apps.uid_1010001.current",0
        "memcg.current",2
        "memcg.system.current",0
        "memcg.system.uid_1000.current",0
        """))

  # The inputs of a series are paired per cgroup, not per upid: the processes
  # with the same upid but another uid or group have their own series.
  def test_android_memcg_derived_intervals_pid_reuse(self):
    return DiffTestBlueprint(
        trace=PID_REUSE_TRACE,
        query="""
        INCLUDE PERFETTO MODULE android.memory.memcg;
        SELECT
          d.level,
          d.memcg_group,
          d.uid,
          p.pid,
          d.key,
          d.ts,
          d.dur,
          d.value
        FROM android_memcg_derived_intervals AS d
        LEFT JOIN process AS p USING (upid)
        ORDER BY d.memcg_group, d.uid, d.key, d.ts;
        """,
        out=Csv("""
        "level","memcg_group","uid","pid","key","ts","dur","value"
        "process","apps",10003,77,"file_excl_shmem",1000,3000,20.000000
        "process","apps",10003,77,"headroom",1000,3000,50.000000
        "process","apps",10004,77,"file_excl_shmem",2000,2000,180.000000
        "process","apps",10004,77,"headroom",2000,2000,700.000000
        """))
