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

from python.generators.diff_tests.testing import Path, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Power(DiffTestModule):

  def test_power_rails_power_rails(self):
    return DiffTestBlueprint(
        trace=Path('../../data/power_rails.pb'),
        query="""
SELECT name, AVG(value), COUNT(*)
FROM counters
WHERE name GLOB "power.*"
GROUP BY name
LIMIT 20;
""",
        out=Csv("""
"name","AVG(value)","COUNT(*)"
"power.PPVAR_VPH_PWR_ABH_uws",7390700.360656,61
"power.PPVAR_VPH_PWR_OLED_uws",202362991.655738,61
"""))

  def test_power_rails_event_power_rails_custom_clock(self):
    return DiffTestBlueprint(
        trace=Path('power_rails_custom_clock.textproto'),
        query="""
SELECT ts, value
FROM counters
WHERE name GLOB "power.*"
LIMIT 20;
""",
        out=Csv("""
"ts","value"
104000000,333.000000
106000000,666.000000
106000000,999.000000
109000000,0.000000
"""))

  def test_power_rails_timestamp_sort(self):
    return DiffTestBlueprint(
        trace=Path('power_rails.textproto'),
        query="""
SELECT ts, value, t.name AS name
FROM counter c JOIN counter_track t ON t.id = c.track_id
ORDER BY ts
LIMIT 20;
""",
        out=Csv("""
"ts","value","name"
3000000,333.000000,"power.test_rail_uws"
3000000,0.000000,"power.test_rail_uws"
3000004,1000.000000,"Testing"
3000005,999.000000,"power.test_rail2_uws"
5000000,666.000000,"power.test_rail_uws"
"""))

  def test_power_rails_well_known_power_rails(self):
    return DiffTestBlueprint(
        trace=Path('power_rails_well_known.textproto'),
        query="""
SELECT name, AVG(value), COUNT(*)
FROM counters
WHERE name GLOB "power.*"
GROUP BY name
LIMIT 20;
""",
        out=Csv("""
"name","AVG(value)","COUNT(*)"
"power.rails.cpu.mid",333.000000,3
"power.rails.gpu",999.000000,1
"""))

  def test_dvfs_metric(self):
    return DiffTestBlueprint(
        trace=Path('dvfs_metric.textproto'),
        query=Metric('android_dvfs'),
        out=Path('dvfs_metric.out'))

  def test_wakesource_wakesource(self):
    return DiffTestBlueprint(
        trace=Path('wakesource.textproto'),
        query="""
SELECT ts, dur, slice.name
FROM slice
JOIN track ON slice.track_id = track.id
WHERE track.name GLOB 'Wakelock*'
ORDER BY ts;
""",
        out=Csv("""
"ts","dur","name"
34298714043271,7872467,"Wakelock(s2mpw02-power-keys)"
34298721846504,42732654,"Wakelock(event0)"
34298721915739,16,"Wakelock(s2mpw02-power-keys)"
34298764569658,14538,"Wakelock(eventpoll)"
"""))

  def test_suspend_resume(self):
    return DiffTestBlueprint(
        trace=Path('suspend_resume.textproto'),
        query="""
SELECT
  s.ts,
  s.dur,
  s.name AS action
FROM
  slice AS s
JOIN
  track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'Suspend/Resume Latency'
ORDER BY s.ts;
""",
        out=Csv("""
"ts","dur","action"
10000,10000,"suspend_enter(3)"
30000,10000,"CPU(0)"
50000,10000,"timekeeping_freeze(0)"
"""))

  def test_suspend_period(self):
    return DiffTestBlueprint(
        trace=Path('suspend_period.textproto'),
        query=Metric('android_batt'),
        out=TextProto(r"""
android_batt {
  battery_aggregates {
    sleep_ns: 20000
  }
  suspend_period {
    timestamp_ns: 30000
    duration_ns: 10000
  }
  suspend_period {
    timestamp_ns: 50000
    duration_ns: 10000
  }
}
"""))

  def test_energy_breakdown_table_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        query="""
SELECT consumer_id, name, consumer_type, ordinal
FROM energy_counter_track;
""",
        out=Csv("""
"consumer_id","name","consumer_type","ordinal"
0,"CPUCL0","CPU_CLUSTER",0
"""))

  def test_energy_breakdown_event_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        query="""
SELECT ts, value
FROM counter
JOIN energy_counter_track ON counter.track_id = energy_counter_track.id
ORDER BY ts;
""",
        out=Csv("""
"ts","value"
1030255882785,98567522.000000
"""))

  def test_energy_breakdown_uid_table_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
SELECT uid, name
FROM uid_counter_track;
""",
        out=Csv("""
"uid","name"
10234,"GPU"
10190,"GPU"
10235,"GPU"
"""))

  def test_energy_breakdown_uid_event_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
SELECT ts, value
FROM counter
JOIN uid_counter_track ON counter.track_id = uid_counter_track.id
ORDER BY ts;
""",
        out=Csv("""
"ts","value"
1026753926322,3004536.000000
1026753926322,0.000000
1026753926322,4002274.000000
"""))

  def test_energy_per_uid_table_test(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
SELECT consumer_id, uid
FROM energy_per_uid_counter_track;
""",
        out=Csv("""
"consumer_id","uid"
3,10234
3,10190
3,10235
"""))

  def test_cpu_counters_p_state_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_counters.pb'),
        query="""
SELECT RUN_METRIC("android/p_state.sql");

SELECT * FROM P_STATE_OVER_INTERVAL(2579596465618, 2579606465618);
""",
        out=Path('cpu_counters_p_state_test.out'))

  def test_cpu_powerups_test(self):
    return DiffTestBlueprint(
        trace=Path('../../data/cpu_powerups_1.pb'),
        query="""
SELECT IMPORT("chrome.cpu_powerups");
SELECT * FROM chrome_cpu_power_first_toplevel_slice_after_powerup;
""",
        out=Csv("""
"slice_id","previous_power_state"
424,2
703,2
708,2
"""))
