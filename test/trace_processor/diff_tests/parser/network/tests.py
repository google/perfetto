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
from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class NetworkParser(TestSuite):
  # Network performance
  def test_netif_receive_skb(self):
    return DiffTestBlueprint(
        trace=Path('netif_receive_skb.textproto'),
        query="""
        SELECT
          ts,
          REPLACE(name, " Received KB", "") AS dev,
          EXTRACT_ARG(arg_set_id, 'cpu') AS cpu,
          EXTRACT_ARG(arg_set_id, 'len') AS len
        FROM
          counter AS c
        LEFT JOIN
          counter_track AS t
          ON c.track_id = t.id
        WHERE
          name GLOB "* Received KB"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dev","cpu","len"
        10000,"rmnet0",0,1000
        10000,"rmnet0",1,1000
        10010,"rmnet0",0,1000
        10011,"rmnet0",1,1000
        12000,"wlan",4,1300
        """))

  def test_net_dev_xmit(self):
    return DiffTestBlueprint(
        trace=Path('net_dev_xmit.textproto'),
        query="""
        SELECT
          ts,
          REPLACE(name, " Transmitted KB", "") AS dev,
          EXTRACT_ARG(arg_set_id, 'cpu') AS cpu,
          EXTRACT_ARG(arg_set_id, 'len') AS len
        FROM
          counter AS c
        LEFT JOIN
          counter_track AS t
          ON c.track_id = t.id
        WHERE
          name GLOB "* Transmitted KB"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dev","cpu","len"
        10000,"rmnet0",0,1000
        10000,"rmnet0",1,1000
        10010,"rmnet0",0,1000
        12000,"wlan0",4,1300
        """))

  def test_inet_sock_set_state(self):
    return DiffTestBlueprint(
        trace=Path('inet_sock_set_state.textproto'),
        query="""
        SELECT
          ts,
          s.name,
          dur,
          t.name
        FROM
          slice AS s
        LEFT JOIN track AS t
          ON s.track_id = t.id
        WHERE
          t.name GLOB "TCP stream#*"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name","dur","name"
        10000000,"TCP_SYN_SENT(pid=123)",100000000,"TCP stream#1"
        110000000,"TCP_ESTABLISHED(sport=56789,dport=5001)",500000000,"TCP stream#1"
        610000000,"TCP_CLOSE_WAIT",-1,"TCP stream#1"
        710000000,"TCP_SYN_SENT(pid=567)",10000000,"TCP stream#2"
        720000000,"TCP_ESTABLISHED(sport=56790,dport=5002)",300000000,"TCP stream#2"
        1020000000,"TCP_CLOSE_WAIT",-1,"TCP stream#2"
        """))

  def test_tcp_retransmit_skb(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 110000000
              pid: 234
              tcp_retransmit_skb {
                daddr: 19216801
                saddr: 127001
                dport: 5001
                sport: 56789
                state: 1
                skaddr: 77889900
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 1
            event {
              timestamp: 720000000
              pid: 234
              tcp_retransmit_skb {
                daddr: 0
                saddr: 0
                dport: 5002
                sport: 56790
                state: 2
                skaddr: 33445566
              }
            }
          }
        }
        """),
        query="""
        SELECT
          ts,
          s.name,
          dur
        FROM
          slice AS s
        LEFT JOIN track AS t
          ON s.track_id = t.id
        WHERE
          t.name = "TCP Retransmit Skb"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name","dur"
        110000000,"sport=56789,dport=5001",0
        720000000,"sport=56790,dport=5002",0
        """))

  def test_napi_gro_receive(self):
    return DiffTestBlueprint(
        trace=Path('napi_gro_receive.textproto'),
        query="""
        SELECT
          ts,
          s.name,
          dur,
          cat,
          t.name,
          EXTRACT_ARG(arg_set_id, 'ret') AS ret,
          EXTRACT_ARG(arg_set_id, 'len') AS len
        FROM
          slice AS s
        LEFT JOIN
          track AS t
          ON s.track_id = t.id
        WHERE
          t.name GLOB "Napi Gro Cpu *"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","name","dur","cat","name","ret","len"
        10000,"rmnet0",20,"napi_gro","Napi Gro Cpu 2",2,1000
        20000,"rmnet0",20,"napi_gro","Napi Gro Cpu 2",1,1000
        30000,"wlan",20,"napi_gro","Napi Gro Cpu 4",3,500
        """))

  def test_kfree_skb(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 10000
              pid: 200
              kfree_skb {
                protocol: 2048
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 10020
              pid: 300
              kfree_skb {
                protocol: 34525
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 20000
              pid: 200
              kfree_skb {
                protocol: 1536
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 20020
              pid: 300
              kfree_skb {
                protocol: 2048
              }
            }
          }
        }
        """),
        query="""
        SELECT
          ts,
          value,
          EXTRACT_ARG(arg_set_id, 'protocol') AS prot
        FROM
          counter AS c
        LEFT JOIN
          counter_track AS t
          ON c.track_id = t.id
        WHERE
          name GLOB "Kfree Skb IP Prot"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value","prot"
        10000,1.000000,"IP"
        10020,2.000000,"IPV6"
        20020,3.000000,"IP"
        """))
