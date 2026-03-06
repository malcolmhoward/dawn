#!/usr/bin/env python3
"""
Search Quality Analyzer

Compares two sets of SearXNG result files and prints per-query metrics.

Usage:
   python3 analyze_results.py <before_dir> <after_dir>
   python3 analyze_results.py /tmp/search_before /tmp/search_after
"""

import json
import os
import sys
from pathlib import Path
from urllib.parse import urlparse


def analyze_results(filepath):
   """Extract metrics from a single result file."""
   try:
      with open(filepath) as f:
         data = json.load(f)
   except (json.JSONDecodeError, FileNotFoundError):
      return None

   results = data.get("results", [])
   count = len(results)

   if count == 0:
      return {
         "count": 0,
         "avg_score": 0.0,
         "date_coverage": 0.0,
         "host_diversity": 0.0,
         "keyword_hits": 0.0,
      }

   # Average SearXNG score
   scores = [r.get("score", 0.0) for r in results]
   avg_score = sum(scores) / len(scores) if scores else 0.0

   # Date coverage (% of results with publishedDate)
   dated = sum(1 for r in results if r.get("publishedDate"))
   date_coverage = dated / count * 100

   # Host diversity (unique hosts / total)
   hosts = set()
   for r in results:
      url = r.get("url", "")
      try:
         hosts.add(urlparse(url).netloc)
      except Exception:
         pass
   host_diversity = len(hosts) / count * 100 if count else 0

   return {
      "count": count,
      "avg_score": avg_score,
      "date_coverage": date_coverage,
      "host_diversity": host_diversity,
   }


def check_keywords(filepath, keywords):
   """Check what percentage of expected keywords appear in titles+snippets."""
   try:
      with open(filepath) as f:
         data = json.load(f)
   except (json.JSONDecodeError, FileNotFoundError):
      return 0.0

   results = data.get("results", [])
   if not results or not keywords:
      return 0.0

   # Combine all titles and snippets into one searchable string
   text = " ".join(
      (r.get("title", "") + " " + r.get("content", "")).lower() for r in results
   )

   hits = sum(1 for kw in keywords if kw.lower() in text)
   return hits / len(keywords) * 100


def main():
   if len(sys.argv) < 3:
      print(f"Usage: {sys.argv[0]} <before_dir> <after_dir>")
      sys.exit(1)

   before_dir = Path(sys.argv[1])
   after_dir = Path(sys.argv[2])

   if not before_dir.exists():
      print(f"Error: {before_dir} does not exist")
      sys.exit(1)
   if not after_dir.exists():
      print(f"Error: {after_dir} does not exist")
      sys.exit(1)

   # Find all query files
   before_files = {f.stem: f for f in before_dir.glob("*.json")}
   after_files = {f.stem: f for f in after_dir.glob("*.json")}
   all_queries = sorted(set(before_files.keys()) | set(after_files.keys()))

   if not all_queries:
      print("No result files found.")
      sys.exit(1)

   # Print comparison table
   header = f"{'Query':<45} {'Count':>10} {'AvgScore':>10} {'Dated%':>8} {'Hosts%':>8}"
   sep = "-" * len(header)

   print(f"\n{'BEFORE':>50}{'':>5}{'AFTER':>40}")
   print(f"{'Query':<30} {'Cnt':>4} {'Score':>6} {'Date%':>6} {'Host%':>6}  |  {'Cnt':>4} {'Score':>6} {'Date%':>6} {'Host%':>6}  | {'Delta':>6}")
   print("-" * 110)

   total_before = 0
   total_after = 0

   for query_name in all_queries:
      b_metrics = analyze_results(before_files[query_name]) if query_name in before_files else None
      a_metrics = analyze_results(after_files[query_name]) if query_name in after_files else None

      display_name = query_name[:28].replace("_", " ")

      b_str = (
         f"{b_metrics['count']:>4} {b_metrics['avg_score']:>6.2f} {b_metrics['date_coverage']:>5.1f}% {b_metrics['host_diversity']:>5.1f}%"
         if b_metrics
         else f"{'N/A':>4} {'N/A':>6} {'N/A':>6} {'N/A':>6}"
      )
      a_str = (
         f"{a_metrics['count']:>4} {a_metrics['avg_score']:>6.2f} {a_metrics['date_coverage']:>5.1f}% {a_metrics['host_diversity']:>5.1f}%"
         if a_metrics
         else f"{'N/A':>4} {'N/A':>6} {'N/A':>6} {'N/A':>6}"
      )

      delta = ""
      if b_metrics and a_metrics:
         d = a_metrics["count"] - b_metrics["count"]
         delta = f"{'+' if d > 0 else ''}{d}"
         total_before += b_metrics["count"]
         total_after += a_metrics["count"]

      print(f"{display_name:<30} {b_str}  |  {a_str}  | {delta:>6}")

   print("-" * 110)
   print(f"{'TOTALS':<30} {total_before:>4}{'':>21}  |  {total_after:>4}{'':>21}  | {'+' if total_after > total_before else ''}{total_after - total_before:>5}")
   print()


if __name__ == "__main__":
   main()
