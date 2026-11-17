#!/usr/bin/env bash
set -euo pipefail
umask 077
if (( $# != 1 )); then
	printf 'Usage: create-fixtures.sh NEW_DIRECTORY\n' >&2
	exit 1
fi
fixture_dir=$1
mkdir -p -- "$fixture_dir"
for fixture_name in captions.ass source.mkv portrait.mp4 landscape.mp4; do
	if [[ -e "$fixture_dir/$fixture_name" ]]; then
		printf 'Fixture already exists: %s\n' "$fixture_dir/$fixture_name" >&2
		exit 1
	fi
done
cat > "$fixture_dir/captions.ass" <<'ASS'
[Script Info]
ScriptType: v4.00+
PlayResX: 320
PlayResY: 180
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,18,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:09.00,Default,,0,0,0,,Across segment boundaries
ASS
ffmpeg -v error -n -f lavfi -i testsrc2=size=320x180:rate=30:duration=12 \
	-f lavfi -i sine=frequency=997:sample_rate=48000:duration=12 \
	-i "$fixture_dir/captions.ass" -map 0:v -map 1:a -map 2:s \
	-c:v libx264 -preset ultrafast -crf 18 -g 60 -pix_fmt yuv420p -c:a flac -c:s ass \
	-t 12 "$fixture_dir/source.mkv"
while IFS=: read -r fixture_name fixture_size; do
	ffmpeg -v error -n -f lavfi -i "testsrc2=size=$fixture_size:rate=30:duration=2" \
		-c:v libx264 -preset ultrafast -crf 24 -g 60 -pix_fmt yuv420p "$fixture_dir/$fixture_name.mp4"
done <<'SIZES'
landscape:1920x1080
portrait:1080x1920
SIZES
