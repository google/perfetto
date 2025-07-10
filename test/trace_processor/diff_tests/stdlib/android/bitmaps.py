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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class AndroidBitmaps(TestSuite):

  def test_android_bitmap_counters_per_process(self):
    return DiffTestBlueprint(
        trace=DataPath('sysui_qsmedia_microbenchmark.pb'),
        query="""
        INCLUDE PERFETTO MODULE android.bitmaps;

        SELECT
          process_name,
          ts,
          dur,
          bitmap_memory,
          bitmap_count
        FROM android_bitmap_counters_per_process
        WHERE process_name = 'com.android.systemui';
        """,
        out=Csv("""
        "process_name","ts","dur","bitmap_memory","bitmap_count"
        "com.android.systemui",606922346935,984049,773876.000000,18.000000
        "com.android.systemui",606923330984,5249,1133876.000000,18.000000
        "com.android.systemui",606923336233,18882813,1133876.000000,19.000000
        "com.android.systemui",606942219046,1627,1162100.000000,19.000000
        "com.android.systemui",606942220673,15951,1162100.000000,20.000000
        "com.android.systemui",606942236624,1139,1198964.000000,20.000000
        "com.android.systemui",606942237763,232341,1198964.000000,21.000000
        "com.android.systemui",606942470104,1139,1162100.000000,21.000000
        "com.android.systemui",606942471243,1161174,1162100.000000,20.000000
        "com.android.systemui",606943632417,3906,1190324.000000,20.000000
        "com.android.systemui",606943636323,34261,1190324.000000,21.000000
        "com.android.systemui",606943670584,3336,1227188.000000,21.000000
        "com.android.systemui",606943673920,580770,1227188.000000,22.000000
        "com.android.systemui",606944254690,2971,1190324.000000,22.000000
        "com.android.systemui",606944257661,28646077,1190324.000000,21.000000
        "com.android.systemui",606972903738,1546,1198004.000000,21.000000
        "com.android.systemui",606972905284,414633,1198004.000000,22.000000
        "com.android.systemui",606973319917,1546,1205684.000000,22.000000
        "com.android.systemui",606973321463,335693,1205684.000000,23.000000
        "com.android.systemui",606973657156,1587,1213364.000000,23.000000
        "com.android.systemui",606973658743,28796021,1213364.000000,24.000000
        "com.android.systemui",607002454764,2115,1573364.000000,24.000000
        "com.android.systemui",607002456879,5690471,1573364.000000,25.000000
        "com.android.systemui",607008147350,7243,1622648.000000,25.000000
        "com.android.systemui",607008154593,70557861,1622648.000000,26.000000
        "com.android.systemui",607078712454,1750,1573364.000000,26.000000
        "com.android.systemui",607078714204,7490315,1573364.000000,25.000000
        "com.android.systemui",607086204519,1750,1601588.000000,25.000000
        "com.android.systemui",607086206269,24251,1601588.000000,26.000000
        "com.android.systemui",607086230520,1221,1638452.000000,26.000000
        "com.android.systemui",607086231741,232829,1638452.000000,27.000000
        "com.android.systemui",607086464570,1099,1601588.000000,27.000000
        "com.android.systemui",607086465669,6377807,1601588.000000,26.000000
        "com.android.systemui",607092843476,1668,1638708.000000,26.000000
        "com.android.systemui",607092845144,120036,1638708.000000,27.000000
        "com.android.systemui",607092965180,1262,1610484.000000,27.000000
        "com.android.systemui",607092966442,148265991,1610484.000000,26.000000
        "com.android.systemui",607241232433,2319,1614580.000000,26.000000
        "com.android.systemui",607241234752,694621,1614580.000000,27.000000
        "com.android.systemui",607241929373,1994,1709836.000000,27.000000
        "com.android.systemui",607241931367,196655,1709836.000000,28.000000
        "com.android.systemui",607242128022,936,1725212.000000,28.000000
        "com.android.systemui",607242128958,125814,1725212.000000,29.000000
        "com.android.systemui",607242254772,976,1729308.000000,29.000000
        "com.android.systemui",607242255748,77515,1729308.000000,30.000000
        "com.android.systemui",607242333263,977,1731156.000000,30.000000
        "com.android.systemui",607242334240,48706,1731156.000000,31.000000
        "com.android.systemui",607242382946,936,1735252.000000,31.000000
        "com.android.systemui",607242383882,17893269,1735252.000000,32.000000
        "com.android.systemui",607260277151,2401,1746488.000000,32.000000
        "com.android.systemui",607260279552,592367,1746488.000000,33.000000
        "com.android.systemui",607260871919,1831,1757724.000000,33.000000
        "com.android.systemui",607260873750,185547,1757724.000000,34.000000
        "com.android.systemui",607261059297,1749,1768960.000000,34.000000
        "com.android.systemui",607261061046,229859,1768960.000000,35.000000
        "com.android.systemui",607261290905,1709,1780196.000000,35.000000
        "com.android.systemui",607261292614,88376017,1780196.000000,36.000000
        "com.android.systemui",607349668631,2075,1791432.000000,36.000000
        "com.android.systemui",607349670706,354736,1791432.000000,37.000000
        "com.android.systemui",607350025442,814,1802668.000000,37.000000
        "com.android.systemui",607350026256,87769,1802668.000000,38.000000
        "com.android.systemui",607350114025,732,1813904.000000,38.000000
        "com.android.systemui",607350114757,53274984,1813904.000000,39.000000
        "com.android.systemui",607403389741,1058,1820628.000000,39.000000
        "com.android.systemui",607403390799,190145,1820628.000000,40.000000
        "com.android.systemui",607403580944,651,1831864.000000,40.000000
        "com.android.systemui",607403581595,1689715821,1831864.000000,41.000000
        "com.android.systemui",609093297416,3011,1834673.000000,41.000000
        "com.android.systemui",609093300427,614787,1834673.000000,42.000000
        "com.android.systemui",609093915214,2604,1837482.000000,42.000000
        "com.android.systemui",609093917818,2434489,1837482.000000,43.000000
        "com.android.systemui",609096352307,1750,1848718.000000,43.000000
        "com.android.systemui",609096354057,4565470,1848718.000000,44.000000
        "com.android.systemui",609100919527,4394,1859954.000000,44.000000
        "com.android.systemui",609100923921,630412,1859954.000000,45.000000
        "com.android.systemui",609101554333,2157,1871190.000000,45.000000
        "com.android.systemui",609101556490,89599,1871190.000000,46.000000
        "com.android.systemui",609101646089,1343,1882426.000000,46.000000
        "com.android.systemui",609101647432,133545,1882426.000000,47.000000
        "com.android.systemui",609101780977,1750,1893662.000000,47.000000
        "com.android.systemui",609101782727,235229,1893662.000000,48.000000
        "com.android.systemui",609102017956,1628,1904898.000000,48.000000
        "com.android.systemui",609102019584,74951,1904898.000000,49.000000
        "com.android.systemui",609102094535,1424,1916134.000000,49.000000
        "com.android.systemui",609102095959,106446,1916134.000000,50.000000
        "com.android.systemui",609102202405,1464,1927370.000000,50.000000
        "com.android.systemui",609102203869,547838217,1927370.000000,51.000000
        "com.android.systemui",609650042086,1180,1930898.000000,51.000000
        "com.android.systemui",609650043266,144572,1930898.000000,52.000000
        "com.android.systemui",609650187838,854,1934426.000000,52.000000
        "com.android.systemui",609650188692,44678,1934426.000000,53.000000
        "com.android.systemui",609650233370,854,1937954.000000,53.000000
        "com.android.systemui",609650234224,18577963,1937954.000000,54.000000
        "com.android.systemui",609668812187,1790,1948770.000000,54.000000
        "com.android.systemui",609668813977,54366048,1948770.000000,55.000000
        "com.android.systemui",609723180025,1709,1960006.000000,55.000000
        "com.android.systemui",609723181734,164144,1960006.000000,56.000000
        "com.android.systemui",609723345878,1628,1971242.000000,56.000000
        "com.android.systemui",609723347506,69092,1971242.000000,57.000000
        "com.android.systemui",609723416598,732,1982478.000000,57.000000
        "com.android.systemui",609723417330,82520,1982478.000000,58.000000
        "com.android.systemui",609723499850,773,1993714.000000,58.000000
        "com.android.systemui",609723500623,36539,1993714.000000,59.000000
        "com.android.systemui",609723537162,773,2004950.000000,59.000000
        "com.android.systemui",609723537935,87688,2004950.000000,60.000000
        "com.android.systemui",609723625623,773,2016186.000000,60.000000
        "com.android.systemui",609723626396,68481,2016186.000000,61.000000
        "com.android.systemui",609723694877,773,2027422.000000,61.000000
        "com.android.systemui",609723695650,6692203617,2027422.000000,62.000000
        "com.android.systemui",616415899267,1831,2038658.000000,62.000000
        "com.android.systemui",616415901098,2967911826,2038658.000000,63.000000
        """))
