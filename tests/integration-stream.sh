#!/usr/bin/env bash
# Requires a configured real modeld server. No fake inference or Python.
set -euo pipefail
umask 077
modelctl_bin=${MODELD_MODELCTL:-modelctl}
source_file=${1:?Usage: integration-stream.sh VIDEO [REPORT.json]}
report_file=${2:-modeld-integration.json}
segment_count=${MODELD_BENCH_SEGMENTS:-8}
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/modeld-stream-test.XXXXXX")
source_pid=
session_id=
cleanup() {
	if [[ -n ${session_id} ]]; then "${modelctl_bin}" delete "${session_id}" >/dev/null 2>&1 || true; fi
	if [[ -n ${source_pid} ]]; then kill "${source_pid}" 2>/dev/null || true; wait "${source_pid}" 2>/dev/null || true; fi
	rm -rf -- "${work_dir}"
}
trap cleanup EXIT
"${modelctl_bin}" models > "${work_dir}/models.json"
jq -e '.data[] | select(.id=="rife-4.26-heavy-fp32" and .precision=="fp32" and .tf32==false)' "${work_dir}/models.json" >/dev/null
"${modelctl_bin}" source "${source_file}" > "${work_dir}/source.json" &
source_pid=$!
for ((i=0;i<50;i++)); do
	if jq -e '.source_url' "${work_dir}/source.json" >/dev/null 2>&1; then break; fi
	sleep 0.1
done
request=$(jq -c '{source_url:.source_url,model:"rife-4.26-heavy-fp32",task:"frame_interpolation",target_fps:60}' "${work_dir}/source.json")
"${modelctl_bin}" session "${request}" > "${work_dir}/session.json"
session_id=$(jq -r '.id' "${work_dir}/session.json")
playlist_url=$("${modelctl_bin}" media-url "$(jq -r '.playlist_url' "${work_dir}/session.json")")
base_url=${playlist_url%index.m3u8}
curl -q --noproxy '*' -fsS --max-time 30 "${playlist_url}" > "${work_dir}/index.m3u8"
grep -q '#EXT-X-ENDLIST' "${work_dir}/index.m3u8"
start_time=$(date +%s.%N)
for ((i=0;i<segment_count;i++)); do
	curl -q --noproxy '*' -fsS --max-time 120 \
		-D "${work_dir}/headers-${i}" -w '%{time_total}' \
		"${base_url}segments/${i}.ts" -o "${work_dir}/segment-${i}.ts" > "${work_dir}/transfer-${i}"
	frames=$(awk 'tolower($1)=="x-modeld-frames:" {gsub("\r","",$2);print $2}' "${work_dir}/headers-${i}")
	render=$(awk 'tolower($1)=="x-modeld-render-seconds:" {gsub("\r","",$2);print $2}' "${work_dir}/headers-${i}")
	jq -n --argjson index "${i}" --argjson frames "${frames}" --argjson render "${render}" \
		--argjson transfer "$(cat "${work_dir}/transfer-${i}")" \
		'{index:$index,frames:$frames,render_seconds:$render,http_seconds:$transfer}' >> "${work_dir}/segments.jsonl"
	printf 'segment %d: %s frames, server %ss, HTTP %ss\n' "${i}" "${frames}" "${render}" "$(cat "${work_dir}/transfer-${i}")" >&2
done
end_time=$(date +%s.%N)
wall=$(awk -v start="${start_time}" -v stop="${end_time}" 'BEGIN {printf "%.6f",stop-start}')
for ((i=0;i<segment_count;i++)); do cat "${work_dir}/segment-${i}.ts"; done > "${work_dir}/combined.ts"
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=width,height,r_frame_rate,nb_read_frames,start_time -of json "${work_dir}/combined.ts" > "${work_dir}/probe.json" 2> "${work_dir}/probe.log"
jq -s '.' "${work_dir}/segments.jsonl" > "${work_dir}/segments.json"
jq -n --arg source "$(basename "${source_file}")" --argjson wall "${wall}" \
	--slurpfile info "${work_dir}/session.json" --slurpfile segments "${work_dir}/segments.json" --slurpfile probe "${work_dir}/probe.json" \
	'{source:$source,resolution:[$info[0].width,$info[0].height],source_fps:$info[0].source_fps,target_fps:60,model:$info[0].model,encoder:$info[0].encoder,encoder_qp:$info[0].encoder_qp,segments:$segments[0],wall_seconds:$wall,frames:([$segments[0][].frames]|add),end_to_end_fps:(([$segments[0][].frames]|add)/$wall),decoded_frames:($probe[0].streams[0].nb_read_frames|tonumber),decoded_rate:$probe[0].streams[0].r_frame_rate}' > "${report_file}"
jq -e '.frames==.decoded_frames and .decoded_rate=="60/1"' "${report_file}" >/dev/null
cat "${report_file}"
