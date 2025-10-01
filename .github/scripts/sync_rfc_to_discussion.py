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
"""
Sync RFC/ADR markdown files to GitHub Discussions.
Creates or updates discussions based on RFC files using GraphQL API.
"""

import os
import sys
import re
import requests


def extract_title_from_markdown(content):
    """Extract title from the first H1 heading in markdown content."""
    if not content:
        return None

    # Match first H1 heading (# Title)
    match = re.search(r'^#\s+(.+)$', content, re.MULTILINE)
    if match:
        return match.group(1).strip()
    return None


def extract_rfc_info(filename, content=None):
    """Extract RFC number and title from filename and/or content.

    Args:
        filename: The RFC filename
        content: Optional markdown content to extract H1 title from

    Returns:
        tuple: (rfc_number, title)
    """
    basename = os.path.basename(filename)
    match = re.match(r'(\d+)-(.+)\.md$', basename)
    if not match:
        return None, None

    rfc_number = match.group(1)

    # Prefer H1 title from content if available
    title = None
    if content:
        title = extract_title_from_markdown(content)

    # Fallback to filename-based title
    if not title:
        title_slug = match.group(2)
        title = title_slug.replace('-', ' ').title()

    return rfc_number, title


def read_file_content(filepath):
    """Read the content of the RFC file."""
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            return f.read()
    except Exception as e:
        print(f"Error reading file {filepath}: {e}")
        return None


def graphql_query(token, query, variables=None):
    """Execute a GraphQL query against GitHub API."""
    headers = {
        'Authorization': f'Bearer {token}',
        'Content-Type': 'application/json',
    }

    data = {'query': query}
    if variables:
        data['variables'] = variables

    response = requests.post('https://api.github.com/graphql',
                             headers=headers,
                             json=data)

    if response.status_code != 200:
        raise Exception(
            f"GraphQL query failed: {response.status_code} - {response.text}")

    result = response.json()
    if 'errors' in result:
        raise Exception(f"GraphQL errors: {result['errors']}")

    return result['data']


def get_repository_id(token, owner, repo):
    """Get the repository node ID."""
    query = """
    query($owner: String!, $repo: String!) {
      repository(owner: $owner, name: $repo) {
        id
        discussionCategories(first: 10) {
          nodes {
            id
            name
          }
        }
      }
    }
    """

    data = graphql_query(token, query, {'owner': owner, 'repo': repo})
    return data['repository']['id'], data['repository'][
        'discussionCategories']['nodes']


def find_existing_discussion(token, owner, repo, rfc_number, category_name):
    """Find existing discussion for the RFC number using GitHub Search API."""
    query = """
    query($q: String!) {
      search(type: DISCUSSION, query: $q, first: 10) {
        discussionCount
        nodes {
          ... on Discussion {
            id
            number
            title
            url
          }
        }
      }
    }
    """

    # Construct search query with category filter: "repo:owner/repo is:discussion in:title category:Ideas RFC-0001"
    search_query = f"repo:{owner}/{repo} is:discussion in:title category:\"{category_name}\" RFC-{rfc_number}"

    try:
        data = graphql_query(token, query, {'q': search_query})

        discussions = data['search']['nodes']
        # Find exact match with "RFC-NNNN:" prefix
        search_prefix = f"RFC-{rfc_number}:"
        for discussion in discussions:
            if discussion['title'].startswith(search_prefix):
                return discussion
    except Exception as e:
        print(f"Error searching discussions: {e}")

    return None


def fix_image_urls(content, owner, repo, branch):
    """Fix relative image URLs to use raw.githubusercontent.com."""

    # Pattern to match markdown images: ![alt](path)
    def replace_image_url(match):
        alt_text = match.group(1)
        image_path = match.group(2)

        # Only process relative URLs (not already absolute)
        if not image_path.startswith(('http://', 'https://', '//')):
            # Convert to raw.githubusercontent.com URL
            # e.g., media/0001/test.svg -> https://raw.githubusercontent.com/owner/repo/branch/media/0001/test.svg
            absolute_url = f"https://raw.githubusercontent.com/{owner}/{repo}/{branch}/{image_path}"
            return f"![{alt_text}]({absolute_url})"

        return match.group(0)  # Return original if already absolute

    # Replace all markdown image references
    fixed_content = re.sub(r'!\[([^\]]*)\]\(([^)]+)\)', replace_image_url,
                           content)
    return fixed_content


def create_discussion_body(content, filepath, branch, repo_name):
    """Create the discussion body with RFC content and file link."""
    repo_url = os.environ.get('GITHUB_SERVER_URL', 'https://github.com')
    file_link = f"{repo_url}/{repo_name}/blob/{branch}/{filepath}"

    # Parse owner and repo from repo_name
    owner, repo = repo_name.split('/')

    # Fix image URLs to use raw.githubusercontent.com
    fixed_content = fix_image_urls(content, owner, repo, branch)

    body = f"""📄 **RFC Doc:** [{filepath}]({file_link})

---

{fixed_content}

---

💬 **Discussion Guidelines:**
- This discussion is automatically synced with the RFC document
- Please provide constructive feedback and suggestions
"""
    return body


def create_discussion(token, repo_id, category_id, title, body):
    """Create a new discussion using GraphQL."""
    mutation = """
    mutation($repositoryId: ID!, $categoryId: ID!, $title: String!, $body: String!) {
      createDiscussion(input: {
        repositoryId: $repositoryId
        categoryId: $categoryId
        title: $title
        body: $body
      }) {
        discussion {
          id
          url
          number
        }
      }
    }
    """

    data = graphql_query(
        token, mutation, {
            'repositoryId': repo_id,
            'categoryId': category_id,
            'title': title,
            'body': body
        })

    return data['createDiscussion']['discussion']


def update_discussion(token, discussion_id, body, title=None):
    """Update an existing discussion body and optionally title using GraphQL."""
    if title:
        mutation = """
        mutation($discussionId: ID!, $body: String!, $title: String!) {
          updateDiscussion(input: {
            discussionId: $discussionId
            body: $body
            title: $title
          }) {
            discussion {
              id
              url
            }
          }
        }
        """
        data = graphql_query(token, mutation, {
            'discussionId': discussion_id,
            'body': body,
            'title': title
        })
    else:
        mutation = """
        mutation($discussionId: ID!, $body: String!) {
          updateDiscussion(input: {
            discussionId: $discussionId
            body: $body
          }) {
            discussion {
              id
              url
            }
          }
        }
        """
        data = graphql_query(token, mutation, {
            'discussionId': discussion_id,
            'body': body
        })

    return data['updateDiscussion']['discussion']


def add_discussion_comment(token, discussion_id, body):
    """Add a comment to an existing discussion using GraphQL."""
    mutation = """
    mutation($discussionId: ID!, $body: String!) {
      addDiscussionComment(input: {
        discussionId: $discussionId
        body: $body
      }) {
        comment {
          id
          url
        }
      }
    }
    """

    data = graphql_query(token, mutation, {
        'discussionId': discussion_id,
        'body': body
    })

    return data['addDiscussionComment']['comment']


def get_commit_sha_for_file(filepath, branch):
    """Get the current commit SHA for a file (for diff links)."""
    try:
        # Use git to get the latest commit hash for this file
        import subprocess
        result = subprocess.run(
            ['git', 'log', '-1', '--format=%H', branch, '--', filepath],
            capture_output=True,
            text=True,
            check=True)
        return result.stdout.strip()
    except Exception:
        return None


def sync_rfc_to_discussion(github_token, repository, branch, rfc_file):
    """Sync a single RFC file to GitHub Discussions."""
    print(f"Processing RFC file: {rfc_file}")

    # Read RFC content first (needed for title extraction)
    content = read_file_content(rfc_file)
    if content is None:
        print(f"Skipping {rfc_file}: Could not read file")
        return False

    # Extract RFC info (now passing content for H1 title extraction)
    rfc_number, title = extract_rfc_info(rfc_file, content)
    if not rfc_number or not title:
        print(f"Skipping {rfc_file}: Invalid RFC filename format")
        return False

    # Parse repository owner and name
    owner, repo = repository.split('/')

    try:
        # Get repository ID and categories
        repo_id, categories = get_repository_id(github_token, owner, repo)

        if not categories:
            print(
                "⚠️  No discussion categories found. Please enable GitHub Discussions and create at least one category."
            )
            return False

        # Get required CATEGORY environment variable
        desired_category = os.environ.get('CATEGORY', '')

        if not desired_category:
            print("❌ Error: CATEGORY environment variable is required")
            print("Available categories:")
            for cat in categories:
                print(f"   - {cat['name']}")
            return False

        # Find category by name
        category_id = None
        for cat in categories:
            if cat['name'].lower() == desired_category.lower():
                category_id = cat['id']
                print(f"Using discussion category: {cat['name']}")
                break

        if not category_id:
            print(f"❌ Error: Category '{desired_category}' not found")
            print("Available categories:")
            for cat in categories:
                print(f"   - {cat['name']}")
            return False

        # Create discussion title (format: "RFC-NNNN: Title")
        discussion_title = f"RFC-{rfc_number}: {title}"

        # Create discussion body
        discussion_body = create_discussion_body(content, rfc_file, branch,
                                                 repository)

        # Check if discussion already exists (within the specified category)
        existing = find_existing_discussion(github_token, owner, repo,
                                            rfc_number, desired_category)

        if existing:
            print(f"Found existing discussion: {discussion_title}")
            print(f"Discussion URL: {existing['url']}")

            # Check if title needs updating
            title_needs_update = existing['title'] != discussion_title

            # Update the discussion body and title if needed
            if title_needs_update:
                print(
                    f"Updating discussion title from '{existing['title']}' to '{discussion_title}'"
                )
                update_discussion(github_token, existing['id'],
                                  discussion_body, discussion_title)
            else:
                update_discussion(github_token, existing['id'],
                                  discussion_body)

            print(f"✅ Updated discussion for RFC-{rfc_number}")

            # Post a comment about the update with diff link
            current_sha = get_commit_sha_for_file(rfc_file, branch)
            repo_url = os.environ.get('GITHUB_SERVER_URL',
                                      'https://github.com')

            if current_sha:
                # Create diff link to compare with previous version
                diff_url = f"{repo_url}/{repository}/commits/{branch}/{rfc_file}"
                comment_body = f"📝 **RFC Document Updated**\n\nView changes: [Commit History]({diff_url})"
            else:
                comment_body = f"📝 **RFC Document Updated**"

            # Add comment to discussion
            try:
                add_discussion_comment(github_token, existing['id'],
                                       comment_body)
                print(f"Posted update comment to discussion")
            except Exception as e:
                print(f"Warning: Could not post update comment: {e}")
        else:
            print(f"Creating new discussion: {discussion_title}")

            # Create new discussion
            discussion = create_discussion(github_token, repo_id, category_id,
                                           discussion_title, discussion_body)

            print(f"✅ Created discussion for RFC-{rfc_number}")
            print(f"Discussion URL: {discussion['url']}")

        return True

    except Exception as e:
        print(f"Error syncing RFC to discussion: {e}")
        import traceback
        traceback.print_exc()
        return False


def main():
    """Main entry point."""
    if len(sys.argv) < 2:
        print("Usage: sync_rfc_to_discussion.py <rfc_file1> [rfc_file2] ...")
        sys.exit(1)

    # Get environment variables
    github_token = os.environ.get('GITHUB_TOKEN')
    repository = os.environ.get('REPOSITORY')
    branch = os.environ.get('BRANCH', 'main')

    if not github_token:
        print("Error: GITHUB_TOKEN environment variable not set")
        sys.exit(1)

    if not repository:
        print("Error: REPOSITORY environment variable not set")
        sys.exit(1)

    # Process each RFC file
    success_count = 0
    fail_count = 0

    for rfc_file in sys.argv[1:]:
        if sync_rfc_to_discussion(github_token, repository, branch, rfc_file):
            success_count += 1
        else:
            fail_count += 1

    print(f"\n📊 Summary: {success_count} succeeded, {fail_count} failed")

    # Exit with error if any failed
    if fail_count > 0:
        sys.exit(1)


if __name__ == '__main__':
    main()
