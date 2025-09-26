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
"""Script to update GitHub discussions using the GraphQL API.

This script updates a GitHub discussion in the google/perfetto repository
using the GitHub CLI and GraphQL API.

Usage:
  python3 tools/github/update_github_discussion.py <discussion_number> <content_file>

Examples:
  python3 tools/github/update_github_discussion.py 3104 /tmp/discussion_body.md
  python3 tools/github/update_github_discussion.py 3104 design_doc.md
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path


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
  # Check if gh is available
  try:
    subprocess.run(['gh', '--version'], capture_output=True, check=True)
  except (subprocess.CalledProcessError, FileNotFoundError):
    print(
        "Error: GitHub CLI (gh) is not installed or not in PATH",
        file=sys.stderr)
    print("Install it from: https://cli.github.com/", file=sys.stderr)
    sys.exit(1)

  # Check if gh is authenticated
  try:
    subprocess.run(['gh', 'auth', 'status'], capture_output=True, check=True)
  except subprocess.CalledProcessError:
    print("Error: GitHub CLI is not authenticated", file=sys.stderr)
    print("Run: gh auth login", file=sys.stderr)
    sys.exit(1)


def get_discussion_id(discussion_number: int) -> tuple[str, str]:
  """Get the discussion ID and title for a given discussion number.

    Returns:
        Tuple of (discussion_id, title)
    """
  print(f"Getting discussion ID for discussion #{discussion_number}...")

  query = f"""
    query {{
      repository(owner: "google", name: "perfetto") {{
        discussion(number: {discussion_number}) {{
          id
          title
        }}
      }}
    }}
    """

  response = run_gh_command(query)

  discussion_data = response.get('data', {}).get('repository',
                                                 {}).get('discussion')
  if not discussion_data:
    print(f"Error: Discussion #{discussion_number} not found", file=sys.stderr)
    sys.exit(1)

  discussion_id = discussion_data['id']
  title = discussion_data['title']

  print(f"Found discussion: {title}")
  return discussion_id, title


def update_discussion(discussion_id: str, content_file: Path):
  """Update the discussion with content from the specified file."""
  if not content_file.exists():
    print(f"Error: Content file '{content_file}' not found", file=sys.stderr)
    sys.exit(1)

  print(f"Reading content from {content_file}...")
  try:
    content = content_file.read_text(encoding='utf-8')
  except UnicodeDecodeError as e:
    print(f"Error reading file: {e}", file=sys.stderr)
    sys.exit(1)

  # Escape the content for GraphQL
  escaped_content = json.dumps(content)

  print("Updating discussion...")

  query = f"""
    mutation {{
      updateDiscussion(input: {{
        discussionId: "{discussion_id}"
        body: {escaped_content}
      }}) {{
        discussion {{
          id
          title
          url
        }}
      }}
    }}
    """

  response = run_gh_command(query)

  update_data = response.get('data', {}).get('updateDiscussion',
                                             {}).get('discussion')
  if not update_data:
    print("Error: Failed to update discussion", file=sys.stderr)
    print(f"Response: {response}", file=sys.stderr)
    sys.exit(1)

  title = update_data['title']
  url = update_data['url']

  print(f"Successfully updated discussion: {title}")
  print(f"URL: {url}")


def main():
  """Main entry point."""
  parser = argparse.ArgumentParser(
      description="Update a GitHub discussion in the google/perfetto repository",
      formatter_class=argparse.RawDescriptionHelpFormatter,
      epilog="""
Examples:
  %(prog)s 3104 /tmp/discussion_body.md
  %(prog)s 3104 design_doc.md

Prerequisites:
  - GitHub CLI (gh) must be installed and authenticated
        """)

  parser.add_argument(
      'discussion_number', type=int, help='The discussion number (e.g., 3104)')

  parser.add_argument(
      'content_file',
      type=Path,
      help='Path to file containing the new discussion body')

  args = parser.parse_args()

  check_prerequisites()

  discussion_id, title = get_discussion_id(args.discussion_number)
  update_discussion(discussion_id, args.content_file)


if __name__ == '__main__':
  main()
