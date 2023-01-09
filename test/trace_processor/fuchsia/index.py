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

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Fuchsia(DiffTestModule):

  def test_fuchsia_smoke(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_trace.fxt'),
        query=Path('../common/smoke_test.sql'),
        out=Path('fuchsia_smoke.out'))

  def test_fuchsia_smoke_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_trace.fxt'),
        query=Path('../common/smoke_slices_test.sql'),
        out=Path('fuchsia_smoke_slices.out'))

  def test_fuchsia_smoke_instants(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_trace.fxt'),
        query=Path('smoke_instants_test.sql'),
        out=Path('fuchsia_smoke_instants.out'))

  def test_fuchsia_smoke_counters(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_trace.fxt'),
        query=Path('smoke_counters_test.sql'),
        out=Path('fuchsia_smoke_counters.out'))

  def test_fuchsia_smoke_flow(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_trace.fxt'),
        query=Path('smoke_flow_test.sql'),
        out=Path('fuchsia_smoke_flow.out'))

  def test_fuchsia_smoke_type(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_trace.fxt'),
        query=Path('smoke_type_test.sql'),
        out=Path('fuchsia_smoke_type.out'))

  def test_fuchsia_workstation_smoke_slices(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_workstation.fxt'),
        query=Path('../common/smoke_slices_test.sql'),
        out=Path('fuchsia_workstation_smoke_slices.out'))

  def test_fuchsia_workstation_smoke_args(self):
    return DiffTestBlueprint(
        trace=Path('../../data/fuchsia_workstation.fxt'),
        query=Path('smoke_args_test.sql'),
        out=Path('fuchsia_workstation_smoke_args.out'))
