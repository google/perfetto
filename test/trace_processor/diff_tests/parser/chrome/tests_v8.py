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

from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DataPath, Metric, Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.trace_processor_table.public import Alias
from src.trace_processor.tables.v8_tables import V8_ISOLATE, V8_JS_SCRIPT, V8_JS_FUNCTION, V8_WASM_SCRIPT


def _no_duplicates_query(table):
  group_by_columns = [
      c.name for c in table.columns if not isinstance(c.type, Alias)
  ]
  return f"""
  SELECT DISTINCT COUNT(*) AS count
  FROM {table.sql_name}
  GROUP BY {', '.join( group_by_columns)}"""


class ChromeV8Parser(TestSuite):

  def test_no_duplicates_in_v8_js_function(self):
    return DiffTestBlueprint(
        trace=DataPath('parser/v8.code.trace.pb.gz'),
        query=_no_duplicates_query(V8_JS_FUNCTION),
        out=Csv(""""count"\n1\n"""),
    )

  def test_no_duplicates_in_v8_js_script(self):
    return DiffTestBlueprint(
        trace=DataPath('parser/v8.code.trace.pb.gz'),
        query=_no_duplicates_query(V8_JS_SCRIPT),
        out=Csv(""""count"\n1\n"""),
    )

  def test_no_duplicates_in_v8_isolate(self):
    return DiffTestBlueprint(
        trace=DataPath('parser/v8.code.trace.pb.gz'),
        query=_no_duplicates_query(V8_ISOLATE),
        out=Csv(""""count"\n1\n"""),
    )

  def test_no_duplicates_in_v8_wasm_script(self):
    return DiffTestBlueprint(
        trace=DataPath('parser/v8.code.trace.pb.gz'),
        query=_no_duplicates_query(V8_WASM_SCRIPT),
        out=Csv(""""count"\n1\n"""),
    )

  def test_no_code_overlaps(self):
    return DiffTestBlueprint(
        trace=DataPath('parser/v8.code.trace.pb.gz'),
        query="""
INCLUDE PERFETTO MODULE stack_trace.jit;
WITH
  view AS (
    SELECT
      jit_code_id,
      start_address AS start,
      start_address + size AS end,
      create_ts,
      estimated_delete_ts,
      t.upid AS upid
    FROM
      _JIT_CODE AS c, thread AS t
    USING (utid)
    -- Prevent the cross join below from blowing up
    WHERE jit_code_id < 10000
  )
SELECT COUNT(*) AS num_overlaps
FROM
  view AS v1, view AS v2
WHERE
  -- Prevent comparison with self
  v1.jit_code_id <> v2.jit_code_id
  -- Code ranges in same process
  AND v1.upid = v2.upid
  -- Address overlap
  AND v1.start < v2.end
  AND v2.start < v1.end
  -- Time overlap
  AND (v2.estimated_delete_ts IS NULL OR v1.create_ts < v2.estimated_delete_ts)
  AND (v1.estimated_delete_ts IS NULL OR v2.create_ts < v1.estimated_delete_ts)
""",
        out=Csv(""""num_overlaps"
0
"""),
    )

  def test_v8_cpu_samples(self):
    return DiffTestBlueprint(
        trace=DataPath('v8-samples.pftrace'),
        query='''
          include perfetto module stacks.cpu_profiling;

          select name, source_file, self_count
          from cpu_profiling_summary_tree
          where self_count >= 15
          order by self_count desc, source_file
        ''',
        out=Csv('''
        "name","source_file","self_count"
        "(program)","[NULL]",17083
        "(program)","[NULL]",15399
        "(program)","[NULL]",9853
        "(program)","[NULL]",9391
        "(program)","[NULL]",7299
        "(program)","[NULL]",5245
        "(program)","[NULL]",2443
        "(garbage collector)","[NULL]",107
        "_.mg","chrome-untrusted://new-tab-page/one-google-bar?paramsencoded=",38
        "(garbage collector)","[NULL]",34
        "","https://www.google.com/xjs/_/js/k=xjs.hd.en.nSJdbfIGUiE.O/am=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAACAEKAAAABR4AAAAgAAAAAAAAAAQIAQDEAQAAAgA4AAAEAQAEABQQAAAKEATgUTYAgAAwAQAIAAAQAAACQAAACAAAAAMAACAIAAAAAKAAAAAAAAAAAAAAAAAAYAABBAAAAAAAAAAAAIACAAAAoAMAAAAAgAAAgIAAANghAwgAAAQAAACgDwCCB8AghQcAAAAAAAAAAAAAAAKQIJgLCSgIQAAAAAAAAAAAAAAAAACkpIkLCw/d=1/ed=1/dg=3/br=1/rs=ACT90oH8sSQRHJq5R0DO9ABVW-vZJa5Baw/ee=ALeJib:B8gLwd;AfeaP:TkrAjf;BMxAGc:E5bFse;BgS6mb:fidj5d;BjwMce:cXX2Wb;CxXAWb:YyRLvc;DULqB:RKfG5c;Dkk6ge:wJqrrd;DpcR3d:zL72xf;EABSZ:MXZt9d;ESrPQc:mNTJvc;EVNhjf:pw70Gc;EmZ2Bf:zr1jrb;EnlcNd:WeHg4;Erl4fe:FloWmf,FloWmf;F9mqte:UoRcbe;Fmv9Nc:O1Tzwc;G0KhTb:LIaoZ;G6wU6e:hezEbd;GleZL:J1A7Od;HMDDWe:G8QUdb;HoYVKb:PkDN7e;HqeXPd:cmbnH;IBADCc:RYquRb;IZrNqe:P8ha2c;IoGlCf:b5lhvb;IsdWVc:qzxzOb;JXS8fb:Qj0suc;JbMT3:M25sS;JsbNhc:Xd8iUd;KOxcK:OZqGte;KQzWid:ZMKkN;KcokUb:KiuZBf;KpRAue:Tia57b;LBgRLc:SdcwHb,XVMNvd;LEikZe:byfTOb,lsjVmc;LXA8b:q7OdKd;LsNahb:ucGLNb;Me32dd:MEeYgc;NPKaK:SdcwHb;NSEoX:lazG7b;Np8Qkd:Dpx6qc;Nyt6ic:jn2sGd;OgagBe:",33
        "_.m.Ddb","https://www.google.com/xjs/_/js/k=xjs.hd.en.nSJdbfIGUiE.O/am=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAACAEKAAAABR4AAAAgAAAAAAAAAAQIAQDEAQAAAgA4AAAEAQAEABQQAAAKEATgUTYAgAAwAQAIAAAQAAACQAAACAAAAAMAACAIAAAAAKAAAAAAAAAAAAAAAAAAYAABBAAAAAAAAAAAAIACAAAAoAMAAAAAgAAAgIAAANghAwgAAAQAAACgDwCCB8AghQcAAAAAAAAAAAAAAAKQIJgLCSgIQAAAAAAAAAAAAAAAAACkpIkLCw/d=1/ed=1/dg=3/br=1/rs=ACT90oH8sSQRHJq5R0DO9ABVW-vZJa5Baw/ee=ALeJib:B8gLwd;AfeaP:TkrAjf;BMxAGc:E5bFse;BgS6mb:fidj5d;BjwMce:cXX2Wb;CxXAWb:YyRLvc;DULqB:RKfG5c;Dkk6ge:wJqrrd;DpcR3d:zL72xf;EABSZ:MXZt9d;ESrPQc:mNTJvc;EVNhjf:pw70Gc;EmZ2Bf:zr1jrb;EnlcNd:WeHg4;Erl4fe:FloWmf,FloWmf;F9mqte:UoRcbe;Fmv9Nc:O1Tzwc;G0KhTb:LIaoZ;G6wU6e:hezEbd;GleZL:J1A7Od;HMDDWe:G8QUdb;HoYVKb:PkDN7e;HqeXPd:cmbnH;IBADCc:RYquRb;IZrNqe:P8ha2c;IoGlCf:b5lhvb;IsdWVc:qzxzOb;JXS8fb:Qj0suc;JbMT3:M25sS;JsbNhc:Xd8iUd;KOxcK:OZqGte;KQzWid:ZMKkN;KcokUb:KiuZBf;KpRAue:Tia57b;LBgRLc:SdcwHb,XVMNvd;LEikZe:byfTOb,lsjVmc;LXA8b:q7OdKd;LsNahb:ucGLNb;Me32dd:MEeYgc;NPKaK:SdcwHb;NSEoX:lazG7b;Np8Qkd:Dpx6qc;Nyt6ic:jn2sGd;OgagBe:",18
        "da","https://www.google.com/",15
        '''))

  def test_v8_cpu_samples_json(self):
    return DiffTestBlueprint(
        trace=DataPath('v8-samples.json'),
        query='''
          include perfetto module stacks.cpu_profiling;

          select name, source_file, self_count
          from cpu_profiling_summary_tree
          where self_count >= 15
          order by self_count desc, name
        ''',
        out=Csv('''
        "name","source_file","self_count"
        "(program)","[NULL]",17083
        "(program)","[NULL]",15399
        "(program)","[NULL]",9853
        "(program)","[NULL]",9391
        "(program)","[NULL]",7299
        "(program)","[NULL]",5245
        "(program)","[NULL]",2443
        "(garbage collector)","[NULL]",107
        "_.mg","chrome-untrusted://new-tab-page/one-google-bar?paramsencoded=",38
        "(garbage collector)","[NULL]",34
        "","https://www.google.com/xjs/_/js/k=xjs.hd.en.nSJdbfIGUiE.O/am=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAACAEKAAAABR4AAAAgAAAAAAAAAAQIAQDEAQAAAgA4AAAEAQAEABQQAAAKEATgUTYAgAAwAQAIAAAQAAACQAAACAAAAAMAACAIAAAAAKAAAAAAAAAAAAAAAAAAYAABBAAAAAAAAAAAAIACAAAAoAMAAAAAgAAAgIAAANghAwgAAAQAAACgDwCCB8AghQcAAAAAAAAAAAAAAAKQIJgLCSgIQAAAAAAAAAAAAAAAAACkpIkLCw/d=1/ed=1/dg=3/br=1/rs=ACT90oH8sSQRHJq5R0DO9ABVW-vZJa5Baw/ee=ALeJib:B8gLwd;AfeaP:TkrAjf;BMxAGc:E5bFse;BgS6mb:fidj5d;BjwMce:cXX2Wb;CxXAWb:YyRLvc;DULqB:RKfG5c;Dkk6ge:wJqrrd;DpcR3d:zL72xf;EABSZ:MXZt9d;ESrPQc:mNTJvc;EVNhjf:pw70Gc;EmZ2Bf:zr1jrb;EnlcNd:WeHg4;Erl4fe:FloWmf,FloWmf;F9mqte:UoRcbe;Fmv9Nc:O1Tzwc;G0KhTb:LIaoZ;G6wU6e:hezEbd;GleZL:J1A7Od;HMDDWe:G8QUdb;HoYVKb:PkDN7e;HqeXPd:cmbnH;IBADCc:RYquRb;IZrNqe:P8ha2c;IoGlCf:b5lhvb;IsdWVc:qzxzOb;JXS8fb:Qj0suc;JbMT3:M25sS;JsbNhc:Xd8iUd;KOxcK:OZqGte;KQzWid:ZMKkN;KcokUb:KiuZBf;KpRAue:Tia57b;LBgRLc:SdcwHb,XVMNvd;LEikZe:byfTOb,lsjVmc;LXA8b:q7OdKd;LsNahb:ucGLNb;Me32dd:MEeYgc;NPKaK:SdcwHb;NSEoX:lazG7b;Np8Qkd:Dpx6qc;Nyt6ic:jn2sGd;OgagBe:",33
        "_.m.Ddb","https://www.google.com/xjs/_/js/k=xjs.hd.en.nSJdbfIGUiE.O/am=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAACAEKAAAABR4AAAAgAAAAAAAAAAQIAQDEAQAAAgA4AAAEAQAEABQQAAAKEATgUTYAgAAwAQAIAAAQAAACQAAACAAAAAMAACAIAAAAAKAAAAAAAAAAAAAAAAAAYAABBAAAAAAAAAAAAIACAAAAoAMAAAAAgAAAgIAAANghAwgAAAQAAACgDwCCB8AghQcAAAAAAAAAAAAAAAKQIJgLCSgIQAAAAAAAAAAAAAAAAACkpIkLCw/d=1/ed=1/dg=3/br=1/rs=ACT90oH8sSQRHJq5R0DO9ABVW-vZJa5Baw/ee=ALeJib:B8gLwd;AfeaP:TkrAjf;BMxAGc:E5bFse;BgS6mb:fidj5d;BjwMce:cXX2Wb;CxXAWb:YyRLvc;DULqB:RKfG5c;Dkk6ge:wJqrrd;DpcR3d:zL72xf;EABSZ:MXZt9d;ESrPQc:mNTJvc;EVNhjf:pw70Gc;EmZ2Bf:zr1jrb;EnlcNd:WeHg4;Erl4fe:FloWmf,FloWmf;F9mqte:UoRcbe;Fmv9Nc:O1Tzwc;G0KhTb:LIaoZ;G6wU6e:hezEbd;GleZL:J1A7Od;HMDDWe:G8QUdb;HoYVKb:PkDN7e;HqeXPd:cmbnH;IBADCc:RYquRb;IZrNqe:P8ha2c;IoGlCf:b5lhvb;IsdWVc:qzxzOb;JXS8fb:Qj0suc;JbMT3:M25sS;JsbNhc:Xd8iUd;KOxcK:OZqGte;KQzWid:ZMKkN;KcokUb:KiuZBf;KpRAue:Tia57b;LBgRLc:SdcwHb,XVMNvd;LEikZe:byfTOb,lsjVmc;LXA8b:q7OdKd;LsNahb:ucGLNb;Me32dd:MEeYgc;NPKaK:SdcwHb;NSEoX:lazG7b;Np8Qkd:Dpx6qc;Nyt6ic:jn2sGd;OgagBe:",18
        "da","https://www.google.com/",15
        '''))
