#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

import argparse
from collections import Counter
import pandas as pd


def main():
  parser = argparse.ArgumentParser(
      description="Summarize clustered Android heap dump results.")
  parser.add_argument(
      "--process_name", required=True, help="Target process name.")
  parser.add_argument(
      "--input_file",
      required=True,
      help=("Path to clustered CSV containing"
            " cluster_id,trace_uuid,path,class_name,self_size."),
  )
  parser.add_argument(
      "--output_file",
      required=True,
      help="Path to save summary report text file.",
  )
  args = parser.parse_args()

  print(f"Reading clustered data from {args.input_file}...")
  df = pd.read_csv(args.input_file)

  if df.empty or "cluster_id" not in df.columns:
    print("Error: Invalid or empty clustered CSV.")
    return

  total_traces = len(df["trace_uuid"].unique())
  total_clusters = df["cluster_id"].nunique()

  summary = []
  summary.append(
      f"====================================================================")
  summary.append(f"HEAP DUMP CLUSTER SUMMARY REPORT")
  summary.append(f"Process: {args.process_name}")
  summary.append(f"Total Traces Analyzed: {total_traces}")
  summary.append(f"Total Clusters Discovered: {total_clusters}")
  summary.append(
      f"====================================================================\n")

  # Group by cluster_id and analyze
  cluster_groups = df.groupby("cluster_id")

  for cid, group in cluster_groups:
    cluster_size = len(group)
    percentage = (cluster_size / total_traces) * 100
    avg_size = group["self_size"].mean()

    # Find most common path
    common_paths = Counter(group["path"])
    most_common_path, path_count = common_paths.most_common(1)[0]

    # Find most common leaf class
    common_classes = Counter(group["class_name"])
    most_common_class, class_count = common_classes.most_common(1)[0]

    summary.append(
        "--------------------------------------------------------------------")
    summary.append(
        f"CLUSTER {cid} (Contains {cluster_size} traces, {percentage:.1f}% of"
        " total)")
    summary.append(
        "--------------------------------------------------------------------")
    summary.append(f"Primary Retaining Class: {most_common_class}")
    summary.append(
        f"Average Dominated Memory Size: {avg_size / (1024*1024):.2f} MiB"
        f" ({avg_size:.0f} bytes)")
    summary.append("Representative Dominator Path:")
    summary.append(f"  {most_common_path}\n")
    summary.append("Cluster Path Breakdown (Top Variations):")
    for p, p_cnt in common_paths.most_common(3):
      summary.append(f"  - [{p_cnt} traces] {p}")
    summary.append("\n")

  summary_str = "\n".join(summary)

  print(f"Saving summary report to {args.output_file}...")
  with open(args.output_file, "w") as f:
    f.write(summary_str)

  print("Summarization complete.")


if __name__ == "__main__":
  main()
