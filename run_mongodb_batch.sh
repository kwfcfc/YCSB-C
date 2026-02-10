#!/usr/bin/env bash

OUT=${1:-"results.txt"}
THREAD=${2:-"1"}
CORE=${3:-"12"}
COUNT=${4:-"200"}

> "$OUT"

## Benchmark config
echo "-----------------------------------"
echo "Benchmark："
echo "  - Runs: $COUNT"
echo "  - Thread: $THREAD"
echo "  - Output: $OUT"
echo "-----------------------------------"

{
for i in $(seq 1 $COUNT); do
    taskset -c "$CORE" ./ycsbc \
        -db mongodb \
        -threads "$THREAD" \
	-quiet -latency \
	-P workloads/workloadc.spec 2>&1
done
} > "$OUT"

echo "Done. Results saved to $OUT"
