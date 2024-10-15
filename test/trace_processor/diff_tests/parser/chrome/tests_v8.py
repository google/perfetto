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
          include perfetto module callstacks.stack_profile;

          select name, source_file, self_count
          from _callstacks_for_cpu_profile_stack_samples!(
            cpu_profile_stack_sample
          )
          where self_count > 0
          order by self_count desc
          limit 20
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
        "","https://www.gstatic.com/_/mss/boq-one-google/_/js/k=boq-one-google.OneGoogleWidgetUi.en.Dv_TT86KXl4.es5.O/ck=boq-one-google.OneGoogleWidgetUi.xexmpZqkioA.L.B1.O/am=QKBgwGw/d=1/exm=_b,_tp/excm=_b,_tp,calloutview/ed=1/wt=2/ujg=1/rs=AM-SdHu61g-i-YBZiLcGm3tURf4VJO5hyA/ee=EVNhjf:pw70Gc;EmZ2Bf:zr1jrb;Erl4fe:FloWmf;JsbNhc:Xd8iUd;LBgRLc:SdcwHb;Me32dd:MEeYgc;NPKaK:SdcwHb;NSEoX:lazG7b;Oj465e:KG2eXe;Pjplud:EEDORb;QGR0gd:Mlhmy;SNUn3:ZwDk9d;a56pNe:JEfCwb;cEt90b:ws9Tlc;dIoSBb:SpsfSb;eBAeSb:zbML3c;iFQyKf:QIhFr;io8t5d:yDVVkb;kMFpHd:OTA3Ae;nAFL3:s39S4;oGtAuc:sOXFj;pXdRYb:MdUzUe;qddgKe:xQtZb;sP4Vbe:VwDzFe;uY49fb:COQbmf;ul9GGd:VDovNc;wR5FRb:O1Gjze;xqZiqf:wmnU7d;yxTchf:KUM7Z;zxnPse:GkRiKb/m=ws9Tlc,n73qwf,GkRiKb,e5qFLc,IZT63,UUJqVe,O1Gjze,byfTOb,lsjVmc,xUdipf,OTA3Ae,COQbmf,fKUV3e,aurFic,U0aPgd,ZwDk9d,V3dDOb,mI3LFb,yYB61,O6y8ed,PrPYRd,MpJwZc,LEikZe,NwH0H,OmgaI,lazG7b,XVMNvd,L1AAkb,KUM7Z,Mlhmy,s39S4,lwddkf,gychg,w9hDv,EEDORb,RMhBfe,SdcwHb,aW3pY,pw70Gc,EFQ78c,Ulmmrd,ZfAoz,mdR7q,wmnU7d,xQtZb,JNoxi,kWgXee,MI6k7c,kjKdXe,BVgquf,QIhFr,ov",13
        "updateAttrs","https://ui.perfetto.dev/v46.0-0a53e685b/frontend_bundle.js",12
        "","https://www.gstatic.com/_/mss/boq-one-google/_/js/k=boq-one-google.OneGoogleWidgetUi.en.Dv_TT86KXl4.es5.O/am=QKBgwGw/d=1/excm=_b,_tp,calloutview/ed=1/dg=0/wt=2/ujg=1/rs=AM-SdHsuxqEW2z6uUf-9MJvUVpOyFk0ecQ/m=_b,_tp",11
        "a._isVisible","https://ogs.google.com/widget/callout?prid=19037050&pgid=19037049&puid=6a851fbb7ce797ac&eom=1&cce=1&dc=1&origin=https%3A%2F%2Fwww.google.com&cn=callout&pid=1&spid=538&hl=en&dm=",11
        "","chrome-untrusted://read-anything-side-panel.top-chrome/read_anything.js",11
        "","https://www.google.com/xjs/_/js/k=xjs.hd.en.nSJdbfIGUiE.O/ck=xjs.hd.F00K1IyvS9A.L.B1.O/am=IFEAAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAAIAAAEAAAAAAAASAEakAAABZ5sAMBiAAAABAAIAAQIAQDEAQAAAwQ4AAAEAQAUABQREAEKEgTgUTYAhIAwAQQoQAgUQAICQBCFCAAAAAMAACEIDDAMQKgAYBQgAAAAAEBABAAAYAA3BhAgAMAPAAAYAKICAAAhoAMQAAABgAJAgIACAtghAwgAAAQAAACgDwCCB8AghQcAAAAAAAAAAAAAAAKQIJgLCSgIQAAAAAAAAAAAAAAAAACkpIkLCw/d=0/dg=0/br=1/ujg=1/rs=ACT90oFYV6TnvY5P3NcVPbMRvVPRlxmm8A/m=sb_wiz,aa,abd,sytt,syts,sytn,syfx,sytr,sytd,sy101,syz7,syti,syz6,syto,sytq,sytm,syu7,sytb,syu8,syu9,syu0,syu4,sytj,syty,syu1,syu2,sytv,sytw,syte,sytf,sys4,syru,syrs,syrr,syth,syz5,syug,syuh,syuf,async,syvk,ifl,pHXghd,sf,sy1c2,sy1c5,sy4e0,sonic,TxCJfd,sy4e4,qzxzOb,IsdWVc,sy4e6,sy1gs,sy1d4,sy1d0,syrq,syro,syrp,syrn,syrm,sy4cl,sy4co,sy2ib,sy18p,sy18r,sy13l,sy13m,syrj,syrh,syfb,sybv,syby,sybt,sybx,sybw,sycp,spch,sys7,sys6,rtH1bd,sy1ea,sy19r,sy18g,syg9,sy1e9,sy13t,sy1e8,sy18h,sygb,sy1eb,SMquOb,sy8f,sygh,sygf,sygg,sygi,syge,sygp,sygn,sygl,sygd,sycm,sych,syck,syak,syac,syb6,syaj,syai,sya",10
        "maybeUpdateMoreOptions","chrome-untrusted://read-anything-side-panel.top-chrome/read_anything.js",10
        '''))
