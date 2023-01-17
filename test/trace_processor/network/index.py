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


class DiffTestModule_Network(DiffTestModule):

  def test_netif_receive_skb(self):
    return DiffTestBlueprint(
        trace=Path('netif_receive_skb.textproto'),
        query=Path('netif_receive_skb_test.sql'),
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
        query=Path('net_dev_xmit_test.sql'),
        out=Csv("""
"ts","dev","cpu","len"
10000,"rmnet0",0,1000
10000,"rmnet0",1,1000
10010,"rmnet0",0,1000
12000,"wlan0",4,1300
"""))

  def test_netperf_metric(self):
    return DiffTestBlueprint(
        trace=Path('netperf_metric.textproto'),
        query=Metric('android_netperf'),
        out=Path('netperf_metric.out'))

  def test_inet_sock_set_state(self):
    return DiffTestBlueprint(
        trace=Path('inet_sock_set_state.textproto'),
        query=Path('inet_sock_set_state_test.sql'),
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
        trace=Path('tcp_retransmit_skb.textproto'),
        query=Path('tcp_retransmit_skb_test.sql'),
        out=Csv("""
"ts","name","dur"
110000000,"sport=56789,dport=5001",0
720000000,"sport=56790,dport=5002",0
"""))

  def test_napi_gro_receive(self):
    return DiffTestBlueprint(
        trace=Path('napi_gro_receive.textproto'),
        query=Path('napi_gro_receive_test.sql'),
        out=Csv("""
"ts","name","dur","cat","name","ret","len"
10000,"rmnet0",20,"napi_gro","Napi Gro Cpu 2",2,1000
20000,"rmnet0",20,"napi_gro","Napi Gro Cpu 2",1,1000
30000,"wlan",20,"napi_gro","Napi Gro Cpu 4",3,500
"""))

  def test_kfree_skb(self):
    return DiffTestBlueprint(
        trace=Path('kfree_skb.textproto'),
        query=Path('kfree_skb_test.sql'),
        out=Csv("""
"ts","value","prot"
10000,1.000000,"IP"
10020,2.000000,"IPV6"
20020,3.000000,"IP"
"""))
