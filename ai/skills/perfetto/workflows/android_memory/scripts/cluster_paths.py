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
import re
import pandas as pd
from sklearn.cluster import KMeans
from sklearn.feature_extraction.text import TfidfVectorizer
from sklearn.metrics import silhouette_score


def preprocess_path(path):
  if not isinstance(path, str):
    return ""
  # Remove instance counts like [104]
  path = re.sub(r"\[\d+\]", "", path)
  # Remove root tags like [ROOT] or [ROOT_JAVA_FRAME]
  path = re.sub(r"\[ROOT.*?\]", "", path)
  # Split by -> and join back with spaces for TfidfVectorizer tokenize
  classes = [c.strip() for c in path.split("->") if c.strip()]
  return " ".join(classes)


def main():
  parser = argparse.ArgumentParser(
      description=(
          "Cluster Android heap dump attribution paths using TF-IDF and"
          " K-Means."))
  parser.add_argument(
      "--process_name", required=True, help="Target process name.")
  parser.add_argument(
      "--input_file",
      required=True,
      help=("Path to input CSV containing"
            " trace_uuid,process_name,path,class_name,self_size."),
  )
  parser.add_argument(
      "--output_file", required=True, help="Path to save clustered output CSV.")
  parser.add_argument(
      "--max_clusters",
      type=int,
      default=20,
      help="Maximum number of clusters to evaluate.",
  )
  args = parser.parse_args()

  print(f"Reading input data from {args.input_file}...")
  df = pd.read_csv(args.input_file)

  if df.empty:
    print("Error: Input CSV is empty.")
    return

  print("Preprocessing attribution paths...")
  df["processed_path"] = df["path"].apply(preprocess_path)

  print("Vectorizing paths using TF-IDF...")
  vectorizer = TfidfVectorizer()
  X = vectorizer.fit_transform(df["processed_path"])

  n_samples = X.shape[0]
  max_k = min(args.max_clusters, n_samples - 1)

  if max_k < 2:
    print("Not enough samples for clustering. Assigning all to cluster 0.")
    df["cluster_id"] = 0
    df.drop(columns=["processed_path"]).to_csv(args.output_file, index=False)
    return

  print("Evaluating K-Means clusters to find optimal K via Silhouette Score...")
  best_k = 2
  best_score = -1.0

  for k in range(2, max_k + 1):
    kmeans = KMeans(n_clusters=k, random_state=42, n_init="auto")
    labels = kmeans.fit_predict(X)

    if len(set(labels)) > 1:
      score = silhouette_score(X, labels)
      print(f"  K={k:2d}, Silhouette Score={score:.4f}")
      if score > best_score:
        best_score = score
        best_k = k

  print(f"\nOptimal K selected: {best_k} (Silhouette Score: {best_score:.4f})")
  kmeans = KMeans(n_clusters=best_k, random_state=42, n_init="auto")
  df["cluster_id"] = kmeans.fit_predict(X)

  print(f"Saving clustered results to {args.output_file}...")
  df.drop(columns=["processed_path"]).to_csv(args.output_file, index=False)
  print("Clustering complete.")


if __name__ == "__main__":
  main()
