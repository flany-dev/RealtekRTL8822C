#!/bin/sh
set -eu

list_file="$(mktemp -t realtekrtl8822c-docs.XXXXXX)"
public_list="$(mktemp -t realtekrtl8822c-public-docs.XXXXXX)"
trap 'rm -f "$list_file" "$public_list"' EXIT HUP INT TERM

find . \
    \( -path './.git' -o -path './build' -o -path './RealtekRTL8822C.kext' \) -prune \
    -o -type f \( -name '*.md' -o -name '*.txt' -o -name '*.example' -o -name 'Makefile' \) \
    -print > "$list_file"

if xargs grep -En '/Users/|/Volumes/' < "$list_file"; then
    echo "documentation privacy check failed: machine-local absolute path" >&2
    exit 1
fi

if xargs grep -En '([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}|192\.168\.[0-9]{1,3}\.[0-9]{1,3}' < "$list_file"; then
    echo "documentation privacy check failed: unredacted network identifier" >&2
    exit 1
fi

grep -Fxq 'AGENTS.md' .gitignore || {
    echo "documentation privacy check failed: AGENTS.md is not ignored" >&2
    exit 1
}
grep -Fxq 'docs/internal/' .gitignore || {
    echo "documentation privacy check failed: docs/internal is not ignored" >&2
    exit 1
}

tracked_private="$(git ls-files | grep -E \
    '(^|/)(AGENTS\.md|\.DS_Store)$|^docs/internal/|^build/' || true)"
if test -n "$tracked_private"; then
    echo "documentation privacy check failed: private path is tracked" >&2
    echo "$tracked_private" >&2
    exit 1
fi

find . \
    \( -path './.git' -o -path './build' -o -path './docs/internal' \) -prune \
    -o -path './AGENTS.md' -prune \
    -o -type f -name '*.md' -print > "$public_list"

if xargs grep -En 'handoff-v[0-9]+|Research Handoff|Next runtime test:' < "$public_list"; then
    echo "documentation privacy check failed: research handoff leaked into public docs" >&2
    exit 1
fi

echo "Documentation privacy check passed"
