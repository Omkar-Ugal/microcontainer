#!/usr/bin/env bash
set -euo pipefail

binary=${1:?usage: test_cli.sh BINARY}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$binary" help >"$tmp/help"
grep -q 'Usage:' "$tmp/help"
grep -q 'inspect' "$tmp/help"

if "$binary" not-a-command >"$tmp/out" 2>"$tmp/err"; then
    echo "unknown command unexpectedly succeeded" >&2
    exit 1
fi
grep -q "unknown command" "$tmp/err"

if "$binary" run --memory 0 /tmp /bin/sh >"$tmp/out" 2>"$tmp/err"; then
    echo "invalid memory limit unexpectedly succeeded" >&2
    exit 1
fi
grep -q -- '--memory requires' "$tmp/err"

if "$binary" run --network bridge /tmp /bin/sh >"$tmp/out" 2>"$tmp/err"; then
    echo "unsupported network mode unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'unsupported network mode' "$tmp/err"

if "$binary" run /tmp sh >"$tmp/out" 2>"$tmp/err"; then
    echo "relative command unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'absolute path' "$tmp/err"

if "$binary" inspect extra >"$tmp/out" 2>"$tmp/err"; then
    echo "inspect with extra arguments unexpectedly succeeded" >&2
    exit 1
fi

echo "MicroContainer CLI tests passed"
