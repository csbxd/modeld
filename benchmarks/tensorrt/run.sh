#!/usr/bin/env bash
set -euo pipefail
root=${1:?Usage: run.sh RUN_DIRECTORY MODEL_ROOT}
source_root=${2:?Usage: run.sh RUN_DIRECTORY MODEL_ROOT}
mkdir -p "$root/results" "$root/engines" "$root/cuda/rtx" "$root/cuda/trt"
if compgen -G "$root/engines/*.plan" > /dev/null; then
    echo "Use a fresh run directory for new-plan build timing; existing plans were preserved." >&2
    exit 2
fi
nvidia-smi --query-gpu=timestamp,utilization.gpu,memory.used,power.draw,temperature.gpu,clocks.current.sm,clocks.current.memory,power.limit \
    --format=csv --loop-ms=500 > "$root/gpu.csv" &
telemetry_pid=$!
nvidia-smi pmon -s um -d 1 > "$root/process-gpu.log" &
process_monitor_pid=$!
trap 'kill "$telemetry_pid" "$process_monitor_pid" 2>/dev/null || true; wait "$telemetry_pid" "$process_monitor_pid" 2>/dev/null || true' EXIT
execute() {
    local backend=$1 width=$2 height=$3 label=$4 export_mode=$5
    local sdk_root=/opt/tensorrt
    [[ "$backend" == rtx ]] && sdk_root=/opt/tensorrt-rtx
    printf '%s starting %s\n' "$(date -u +%FT%TZ)" "$label"
    env NVIDIA_TF32_OVERRIDE=0 CUDA_CACHE_PATH="$root/cuda/$backend" \
        LD_LIBRARY_PATH="$sdk_root/lib:/opt/cuda/lib64" \
        "$source_root/build-bench-$backend/trt-bench" \
        /usr/share/modeld/models/rife-4.26-heavy.onnx "$root/engines" \
        "$width" "$height" "$root/samples/manifest.json" "$root/results" "$label" "$export_mode" \
        2> "$root/results/$label.log"
}
for dimensions in '1920 1080' '1280 720'; do
    read -r width height <<< "$dimensions"
    # Missing engine caches: record build and first-prepare times, export quality samples.
    execute rtx "$width" "$height" "rtx-${height}-cold" --export
    execute trt "$width" "$height" "trt-${height}-cold" --export
    # Cached-engine process restarts, ABBA ordering; each process has three timed trials.
    execute rtx "$width" "$height" "rtx-${height}-r1" --no-export
    execute trt "$width" "$height" "trt-${height}-r1" --no-export
    execute trt "$width" "$height" "trt-${height}-r2" --no-export
    execute rtx "$width" "$height" "rtx-${height}-r2" --no-export
done
OMP_NUM_THREADS=16 "$source_root/build-bench-rtx/compare-output" \
    "$root/samples/manifest.json" "$root/results" "$root/quality.json" "$root/quality-summary.json" \
    > "$root/quality-progress.log"
echo "All performance and accuracy comparisons completed"
