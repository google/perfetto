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

from python.generators.diff_tests.testing import Csv, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Gzip(TestSuite):

  def test_gzip_multi_stream(self):
    return DiffTestBlueprint(
        trace=DataPath('sfgate-gzip-multi-stream.json.gz'),
        query='''select ts, dur, name from slice limit 10''',
        out=Csv('''
        "ts","dur","name"
        2213649212614000,239000,"ThreadTimers::sharedTimerFiredInternal"
        2213649212678000,142000,"LayoutView::hitTest"
        2213649214331000,34000,"ThreadTimers::sharedTimerFiredInternal"
        2213649215569000,16727000,"ThreadTimers::sharedTimerFiredInternal"
        2213649216760000,50000,"Node::updateDistribution"
        2213649217290000,1373000,"StyleElement::processStyleSheet"
        2213649218908000,4862000,"Document::updateRenderTree"
        2213649218917000,50000,"Node::updateDistribution"
        2213649218970000,4796000,"Document::updateStyle"
        2213649218995000,54000,"RuleSet::addRulesFromSheet"
        '''))
