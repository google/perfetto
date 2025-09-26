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
"""Script to track and display GitHub discussion changes over time.

This script fetches the edit history of a GitHub discussion and displays
the deltas/changes in a readable format.

Usage:
  python3 tools/github/diff_discussions.py <discussion_number> [options]

Examples:
  python3 tools/github/diff_discussions.py 3104
  python3 tools/github/diff_discussions.py 3104 --show-diffs
  python3 tools/github/diff_discussions.py 3104 --export-json deltas.json
"""

import argparse
import json
import subprocess
import sys
import difflib
from datetime import datetime
from pathlib import Path
from typing import List, Dict, Any, Optional


def run_gh_command(query: str) -> dict:
  """Run a GitHub CLI GraphQL command and return the parsed JSON response."""
  try:
    result = subprocess.run(['gh', 'api', 'graphql', '-f', f'query={query}'],
                            capture_output=True,
                            text=True,
                            check=True)
    return json.loads(result.stdout)
  except subprocess.CalledProcessError as e:
    print(f"Error running gh command: {e}", file=sys.stderr)
    print(f"stdout: {e.stdout}", file=sys.stderr)
    print(f"stderr: {e.stderr}", file=sys.stderr)
    sys.exit(1)
  except json.JSONDecodeError as e:
    print(f"Error parsing JSON response: {e}", file=sys.stderr)
    sys.exit(1)


def check_prerequisites():
  """Check that required tools are available."""
  try:
    subprocess.run(['gh', '--version'], capture_output=True, check=True)
  except (subprocess.CalledProcessError, FileNotFoundError):
    print(
        "Error: GitHub CLI (gh) is not installed or not in PATH",
        file=sys.stderr)
    print("Install it from: https://cli.github.com/", file=sys.stderr)
    sys.exit(1)

  try:
    subprocess.run(['gh', 'auth', 'status'], capture_output=True, check=True)
  except subprocess.CalledProcessError:
    print("Error: GitHub CLI is not authenticated", file=sys.stderr)
    print("Run: gh auth login", file=sys.stderr)
    sys.exit(1)


def get_current_discussion_content(discussion_number: int) -> str:
  """Get the current content of the discussion.

    Note: We actually don't need this function anymore since the 'diff' field
    in each edit contains the full content state after that edit.
    """
  query = f"""
    query {{
      repository(owner: "google", name: "perfetto") {{
        discussion(number: {discussion_number}) {{
          bodyText
        }}
      }}
    }}
    """

  response = run_gh_command(query)
  discussion_data = response.get('data', {}).get('repository',
                                                 {}).get('discussion')

  if not discussion_data:
    return ""

  return discussion_data.get('bodyText', '')


def create_unified_diff(old_content: str, new_content: str, old_label: str,
                        new_label: str) -> str:
  """Create a unified diff between two content strings."""
  # Check if this is just a formatting change (line endings, whitespace, etc.)
  old_normalized = old_content.replace('\r\n', '\n').replace('\r', '\n')
  new_normalized = new_content.replace('\r\n', '\n').replace('\r', '\n')

  if old_normalized == new_normalized:
    # This is just a formatting change
    return f"--- {old_label}\n+++ {new_label}\n@@ Formatting-only changes @@\n(Line ending or whitespace changes only - content is identical)\n"

  old_lines = old_content.splitlines(keepends=True)
  new_lines = new_content.splitlines(keepends=True)

  diff = difflib.unified_diff(
      old_lines, new_lines, fromfile=old_label, tofile=new_label, lineterm='')

  return ''.join(diff)


def get_discussion_edit_history(discussion_number: int) -> Dict[str, Any]:
  """Get the complete edit history for a discussion."""
  print(f"Fetching edit history for discussion #{discussion_number}...")

  # First, get basic discussion info
  basic_query = f"""
    query {{
      repository(owner: "google", name: "perfetto") {{
        discussion(number: {discussion_number}) {{
          id
          title
          createdAt
          updatedAt
          author {{
            login
          }}
          userContentEdits(first: 100) {{
            edges {{
              node {{
                id
                createdAt
                editedAt
                editor {{
                  login
                }}
                diff
              }}
            }}
            pageInfo {{
              hasNextPage
              endCursor
            }}
            totalCount
          }}
        }}
      }}
    }}
    """

  response = run_gh_command(basic_query)
  discussion_data = response.get('data', {}).get('repository',
                                                 {}).get('discussion')

  if not discussion_data:
    print(f"Error: Discussion #{discussion_number} not found", file=sys.stderr)
    sys.exit(1)

  # Get all edits if there are more than 100
  all_edits = []
  edges = discussion_data['userContentEdits']['edges']
  all_edits.extend([edge['node'] for edge in edges])

  page_info = discussion_data['userContentEdits']['pageInfo']

  # Fetch additional pages if needed
  while page_info['hasNextPage']:
    cursor = page_info['endCursor']
    paginated_query = f"""
        query {{
          repository(owner: "google", name: "perfetto") {{
            discussion(number: {discussion_number}) {{
              userContentEdits(first: 100, after: "{cursor}") {{
                edges {{
                  node {{
                    id
                    createdAt
                    editedAt
                    editor {{
                      login
                    }}
                    diff
                  }}
                }}
                pageInfo {{
                  hasNextPage
                  endCursor
                }}
              }}
            }}
          }}
        }}
        """

    paginated_response = run_gh_command(paginated_query)
    paginated_data = paginated_response.get('data',
                                            {}).get('repository',
                                                    {}).get('discussion')

    if paginated_data:
      edges = paginated_data['userContentEdits']['edges']
      all_edits.extend([edge['node'] for edge in edges])
      page_info = paginated_data['userContentEdits']['pageInfo']
    else:
      break

  return {
      'id':
          discussion_data['id'],
      'title':
          discussion_data['title'],
      'createdAt':
          discussion_data['createdAt'],
      'updatedAt':
          discussion_data['updatedAt'],
      'author':
          discussion_data['author']['login']
          if discussion_data['author'] else 'Unknown',
      'totalEdits':
          discussion_data['userContentEdits']['totalCount'],
      'edits':
          all_edits
  }


def format_timestamp(iso_timestamp: str) -> str:
  """Format ISO timestamp to readable format."""
  dt = datetime.fromisoformat(iso_timestamp.replace('Z', '+00:00'))
  return dt.strftime('%Y-%m-%d %H:%M:%S UTC')


def display_discussion_summary(data: Dict[str, Any]):
  """Display a summary of the discussion and its edit history."""
  print(f"\n📋 Discussion: {data['title']}")
  print(f"🆔 ID: {data['id']}")
  print(f"👤 Author: {data['author']}")
  print(f"📅 Created: {format_timestamp(data['createdAt'])}")
  print(f"📝 Last Updated: {format_timestamp(data['updatedAt'])}")
  print(f"✏️  Total Edits: {data['totalEdits']}")


def display_edit_timeline(edits: List[Dict[str, Any]],
                          discussion_number: int,
                          show_diffs: bool = False,
                          chronological: bool = False):
  """Display the edit timeline."""
  if not edits:
    print("\n📝 No edits found.")
    return

  order_desc = "chronological (oldest first)" if chronological else "reverse chronological (newest first)"
  print(f"\n📜 Edit Timeline ({len(edits)} edits, {order_desc}):")
  print("=" * 80)

  # Get current content for comparison
  current_content = get_current_discussion_content(
      discussion_number) if show_diffs else ""

  # Sort edits by creation time
  sorted_edits = sorted(
      edits, key=lambda x: x['createdAt'], reverse=not chronological)

  for i, edit in enumerate(sorted_edits, 1):
    editor = edit['editor']['login'] if edit['editor'] else 'Unknown'
    created_time = format_timestamp(edit['createdAt'])
    edited_time = format_timestamp(
        edit['editedAt']) if edit['editedAt'] != edit['createdAt'] else None

    print(f"\n{i}. Edit #{edit['id'][-8:]}")  # Show last 8 chars of ID
    print(f"   👤 Editor: {editor}")
    print(f"   📅 Created: {created_time}")
    if edited_time:
      print(f"   ✏️  Edited: {edited_time}")

    if show_diffs:
      current_content = edit['diff'] if edit['diff'] else ""

      if chronological:
        # Chronological order: compare with previous in display order
        if i == 1:
          old_content = ""
          old_label = "original (empty)"
        else:
          prev_edit = sorted_edits[i - 2]  # i-2 because i is 1-indexed
          old_content = prev_edit['diff'] if prev_edit['diff'] else ""
          prev_editor = prev_edit['editor']['login'] if prev_edit[
              'editor'] else 'Unknown'
          old_label = f"before edit (by {prev_editor})"
      else:
        # Reverse chronological: compare with next in display order (which is chronologically previous)
        if i == len(sorted_edits):
          old_content = ""
          old_label = "original (empty)"
        else:
          next_edit = sorted_edits[
              i]  # i is 1-indexed, so sorted_edits[i] is the next one
          old_content = next_edit['diff'] if next_edit['diff'] else ""
          next_editor = next_edit['editor']['login'] if next_edit[
              'editor'] else 'Unknown'
          old_label = f"before edit (by {next_editor})"

      new_content = current_content
      new_label = f"after edit by {editor}"

      if old_content != new_content:
        diff_output = create_unified_diff(old_content, new_content, old_label,
                                          new_label)
        if diff_output.strip():
          print(f"   📋 Changes:")
          # Show diff with proper syntax highlighting
          diff_lines = diff_output.split('\n')
          displayed_lines = 0
          max_lines = 20  # Limit diff display

          for line in diff_lines:
            if displayed_lines >= max_lines:
              remaining = len(diff_lines) - displayed_lines
              print(f"      ... ({remaining} more lines)")
              break

            if line.startswith('---') or line.startswith('+++'):
              print(f"      \033[1m{line}\033[0m")  # Bold
            elif line.startswith('@@'):
              print(f"      \033[36m{line}\033[0m")  # Cyan
            elif line.startswith('+'):
              print(f"      \033[32m{line}\033[0m")  # Green
            elif line.startswith('-'):
              print(f"      \033[31m{line}\033[0m")  # Red
            else:
              print(f"      {line}")

            displayed_lines += 1
        else:
          print("   📋 No meaningful changes detected")
      else:
        print("   📋 No changes detected")

    print("-" * 40)


def export_to_json(data: Dict[str, Any], output_file: Path):
  """Export the edit history to a JSON file."""
  try:
    with open(output_file, 'w', encoding='utf-8') as f:
      json.dump(data, f, indent=2, ensure_ascii=False)
    print(f"\n💾 Edit history exported to: {output_file}")
  except Exception as e:
    print(f"Error writing to file: {e}", file=sys.stderr)


def main():
  """Main entry point."""
  parser = argparse.ArgumentParser(
      description="Track and display GitHub discussion changes over time",
      formatter_class=argparse.RawDescriptionHelpFormatter,
      epilog="""
Examples:
  %(prog)s 3104
  %(prog)s 3104 --show-diffs
  %(prog)s 3104 --export-json deltas.json

Prerequisites:
  - GitHub CLI (gh) must be installed and authenticated
        """)

  parser.add_argument(
      'discussion_number', type=int, help='The discussion number (e.g., 3104)')

  parser.add_argument(
      '--show-diffs',
      action='store_true',
      help='Show diff previews for each edit')

  parser.add_argument(
      '--export-json',
      type=Path,
      metavar='FILE',
      help='Export edit history to JSON file')

  parser.add_argument(
      '--latest-only',
      action='store_true',
      help='Show only the most recent edit')

  parser.add_argument(
      '--chronological',
      action='store_true',
      help='Show edits in chronological order (oldest first) instead of reverse chronological (newest first)'
  )

  args = parser.parse_args()

  check_prerequisites()

  # Get edit history
  data = get_discussion_edit_history(args.discussion_number)

  # Display summary
  display_discussion_summary(data)

  # Filter edits if requested
  edits_to_show = data['edits']
  if args.latest_only and edits_to_show:
    # Get the most recent edit (first in the list since they're sorted by timestamp desc from API)
    edits_to_show = [max(edits_to_show, key=lambda x: x['createdAt'])]

  # Display timeline
  display_edit_timeline(edits_to_show, args.discussion_number, args.show_diffs,
                        args.chronological)

  # Export if requested
  if args.export_json:
    export_to_json(data, args.export_json)


if __name__ == '__main__':
  main()
