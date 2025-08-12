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

from python.generators.diff_tests.testing import Csv, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class GenericFtrace(TestSuite):

  # Trace collected with |denser_generic_event_encoding|, containing two
  # generic events.
  def test_(self):
    return DiffTestBlueprint(
        trace=DataPath('ftrace_dense_generic.pftrace'),
        query="""
        select ts, cpu, name, args.flat_key, args.display_value
        from raw join args using (arg_set_id)
        order by ts, cpu asc
        """,
        out=Csv("""
        "ts","cpu","name","flat_key","display_value"
        409974957436916,0,"sched_wake_idle_without_ipi","cpu","7"
        409974957464773,7,"sched_wake_idle_without_ipi","cpu","10"
        409974957820350,10,"sched_wake_idle_without_ipi","cpu","7"
        409974957826539,7,"sched_wake_idle_without_ipi","cpu","2"
        409974959642859,0,"sched_wake_idle_without_ipi","cpu","11"
        409974959785566,0,"sched_wake_idle_without_ipi","cpu","4"
        409974959794911,0,"hrtimer_init","hrtimer","1"
        409974959794911,0,"hrtimer_init","clockid","1"
        409974959794911,0,"hrtimer_init","mode","0"
        409974959797612,0,"hrtimer_init","hrtimer","1"
        409974959797612,0,"hrtimer_init","clockid","1"
        409974959797612,0,"hrtimer_init","mode","0"
        409974959805693,0,"hrtimer_init","hrtimer","1"
        409974959805693,0,"hrtimer_init","clockid","1"
        409974959805693,0,"hrtimer_init","mode","0"
        409974959817766,0,"hrtimer_init","hrtimer","1"
        409974959817766,0,"hrtimer_init","clockid","1"
        409974959817766,0,"hrtimer_init","mode","0"
        409974959821375,0,"hrtimer_init","hrtimer","1"
        409974959821375,0,"hrtimer_init","clockid","1"
        409974959821375,0,"hrtimer_init","mode","0"
        409974959824124,0,"hrtimer_init","hrtimer","1"
        409974959824124,0,"hrtimer_init","clockid","1"
        409974959824124,0,"hrtimer_init","mode","0"
        409974959853750,4,"hrtimer_init","hrtimer","2"
        409974959853750,4,"hrtimer_init","clockid","1"
        409974959853750,4,"hrtimer_init","mode","0"
        409974959861467,4,"hrtimer_init","hrtimer","2"
        409974959861467,4,"hrtimer_init","clockid","1"
        409974959861467,4,"hrtimer_init","mode","0"
        """))
