#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

from urllib import request

from .protos import ProtoFactory


class TraceProcessorHttp:

  def __init__(self, url):
    self.protos = ProtoFactory()
    self.url = 'http://' + url

  def execute_query(self, query):
    args = self.protos.RawQueryArgs()
    args.sql_query = query
    byte_data = args.SerializeToString()
    req = request.Request(self.url + '/query', data=byte_data)
    with request.urlopen(req) as f:
      result = self.protos.QueryResult()
      result.ParseFromString(f.read())
      return result

  def compute_metric(self, metrics):
    args = self.protos.ComputeMetricArgs()
    args.metric_names.extend(metrics)
    byte_data = args.SerializeToString()
    req = request.Request(self.url + '/compute_metric', data=byte_data)
    with request.urlopen(req) as f:
      result = self.protos.ComputeMetricResult()
      result.ParseFromString(f.read())
      return result

  def parse(self, chunk):
    req = request.Request(self.url + '/parse', data=chunk)
    with request.urlopen(req) as f:
      return f.read()

  def notify_eof(self):
    req = request.Request(self.url + '/notify_eof')
    with request.urlopen(req) as f:
      return f.read()

  def status(self):
    req = request.Request(self.url + '/status')
    with request.urlopen(req) as f:
      result = self.protos.StatusResult()
      result.ParseFromString(f.read())
      return result
