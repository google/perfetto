#!/usr/bin/env vpython
# Copyright 2020 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import os
import sys
import xml.etree.ElementTree as ET


def ExtractValues(xml_path, correction):
  root = ET.parse(xml_path).getroot()

  speeds = []
  power = []
  clusters = []
  for array in root.iter('array'):
    if array.get('name') == 'cpu.clusters.cores':
      clusters = [int(value.text) for value in array.iter('value')]
    if array.get('name').startswith('cpu.core_speeds.'):
      speeds.append([int(value.text) for value in array.iter('value')])
    if array.get('name').startswith('cpu.core_power.'):
      power.append([float(value.text) for value in array.iter('value')])

  values = []
  cpu = 0
  for cluster, n_cpus in enumerate(clusters):
    for _ in range(n_cpus):
      for freq, drain in zip(speeds[cluster], power[cluster]):
        if correction:
          drain /= n_cpus
        values.append((cpu, cluster, freq, drain))
      cpu += 1

  return values


def ExportProfiles(device_xmls, sql_path):
  sql_values = []
  for device, xml_path, correction in device_xmls:
    sql_values += [
        '("%s", %s, %s, %s, %s)' % ((device,) + v)
        for v in ExtractValues(xml_path, correction == 'yes')
    ]

  with open(sql_path, 'w') as sql_file:
    sql_file.write('INSERT OR REPLACE INTO power_profile VALUES\n')
    sql_file.write(',\n'.join(sql_values))
    sql_file.write(';\n')


def main(args):
  parser = argparse.ArgumentParser(
      description='Export XML power profile as a SQL INSERT query.',
      epilog='Example usage:\n'
      'python export_power_profiles.py '
      '--device-xml sailfish sailfish/power_profile.xml no '
      '--device-xml sargo sargo/power_profile.xml yes '
      '--output power_profile_data.sql')
  parser.add_argument(
      '--device-xml',
      nargs=3,
      metavar=('DEVICE', 'XML_FILE', 'CORRECTION'),
      action='append',
      help='First argument: device name; second argument: path to the XML '
      'file with the device power profile; third argument(yes|no): '
      'whether correction is necessary. Can be used multiple times.')
  parser.add_argument(
      '--output', metavar='SQL_FILE', help='Path to the output file.')

  args = parser.parse_args(args)

  sql_path = 'result.sql'
  ExportProfiles(args.device_xml, args.output)


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
