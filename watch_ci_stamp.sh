#!/bin/bash
SHA=1cf96cf; R=The412Banner/win-fg
for i in $(seq 1 20); do
  sleep 45
  LINE=$(gh run list -R $R -L 6 --json databaseId,headSha,status,conclusion \
    --jq ".[] | select(.headSha|startswith(\"$SHA\")) | \"\(.databaseId) \(.status) \(.conclusion // \"-\")\"" 2>/dev/null | head -1)
  echo "[$((i*45))s] $LINE"
  ST=$(echo "$LINE" | awk '{print $2}'); ID=$(echo "$LINE" | awk '{print $1}')
  if [ "$ST" = "completed" ]; then
    CC=$(echo "$LINE" | awk '{print $3}')
    echo "=== CI for $SHA => $CC ==="
    if [ "$CC" != "success" ]; then
      echo "--- failing log (stamp/build region) ---"
      gh run view "$ID" -R $R --log-failed 2>/dev/null | grep -iE "win-fg-build|version_stamp|error|stamp missing|CMake" | head -25
    else
      echo "stamp build GREEN — git-describe embed compiles + stamp-verify passed"
    fi
    exit 0
  fi
done
echo "=== watch window ended, last: $LINE ==="
