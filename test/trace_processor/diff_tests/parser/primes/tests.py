#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PrimesTraceParser(TestSuite):

  def test_primes_trace_slice_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('startup.primestrace'),
        query="""
          SELECT id, ts, dur, track_id, name, slice_id
          FROM slice
          ORDER BY dur DESC
          LIMIT 10
        """,
        out=Csv('''
          "id","ts","dur","track_id","name","slice_id"
          0,1762575324667907750,209225666,0,"startup",0
          1,1762575324682560666,150582375,1,"-[GDAAppDelegate application:willFinishLaunchingWithOptions:]",1
          584,1762575324749003375,71142250,4,"InjectGDAUserMediator",584
          4,1762575324686395291,54320417,3,"injectGDKPrimesLatencyManager()",4
          5,1762575324686596375,53317791,4,"InjectGDAApplicationServices",5
          946,1762575324776541291,44636125,271,"-[CCTLogWriter writeLog:pseudonymousID:logDirectory:clock:logTransformers:logLossMetricsService:completionQueue:completion:]",946
          611,1762575324750041125,37464458,166,"injectFileDropService(gaiaAccountID:)",611
          614,1762575324750121541,22349542,167,"InjectGDAUserServices",614
          971,1762575324787560125,18815041,166,"injectGenAIAccountMetadataListener(gaiaAccountID:)",971
          1275,1762575324821346583,17858917,271,"-[CCTLogWriter writeLog:pseudonymousID:logDirectory:clock:logTransformers:logLossMetricsService:completionQueue:completion:]",1275
        '''))

  def test_primes_trace_track_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('startup.primestrace'),
        query="""
        SELECT id, name, track_group_id
        FROM track
        WHERE name IS NOT null
        LIMIT 10;
      """,
        out=Csv('''
        "id","name","track_group_id"
        0,"com.apple.main-thread",0
        12,"com.google.ExperimentStateQueue.clienttracing.ios#com.google.Drive.dev",1
        16,"com.google.drive.phenotype",2
        19,"PHTFlatFilePhenotype",3
        22,"CCTClearcutAutoCounters",4
        23,"PHTFlatFilePhenotypeCompletion",5
        44,"com.google.ExperimentStateQueue.ssoauth_ios#com.google.Drive.dev",6
        53,"com.google.ssoauth.SSOServiceIvarQueue",7
        61,"com.google.ssoauth.deviceupgrade",8
        71,"com.google.ExperimentStateQueue.com.google.ios.apps.drive.device#com.google.Drive.dev",9
      '''))
