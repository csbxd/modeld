#!/usr/bin/env bash
set -euo pipefail
destination=${1:?Usage: prepare.sh DESTINATION}
mkdir -p "$destination"
destination=$(realpath "$destination")
printf '{"conversion":"BT.709 limited YUV to full RGB24; no resizing","samples":[' > "$destination/manifest.json"
separator=
for clip in ducks_take_off_1080p50 crowd_run_1080p50 park_joy_1080p50 FourPeople_1280x720_60 720p50_parkrun_ter; do
    source_url="https://media.xiph.org/video/derf/y4m/${clip}.y4m"
    curl -fsS --connect-timeout 10 --max-time 60 --retry 2 --max-filesize 4096 \
        --range 0-1023 "$source_url" -o "$destination/${clip}.head"
    IFS= read -r y4m_header < "$destination/${clip}.head"
    read -r width height < <(printf '%s\n' "$y4m_header" | perl -ne '/\bW(\d+)\s+H(\d+)/ or die "Bad Y4M dimensions"; print "$1 $2\n"')
    # These public Xiph samples have untagged six-byte FRAME headers and 8-bit 4:2:0.
    if [[ "$y4m_header" == *C* && "$y4m_header" != *C420* ]] || [[ "$y4m_header" == *p10* || "$y4m_header" == *p12* ]]; then
        echo "Unsupported Y4M chroma: $y4m_header" >&2; exit 1
    fi
    frame_bytes=$((width * height * 3 / 2))
    stride=$((frame_bytes + 6))
    header_bytes=$((${#y4m_header} + 1))
    for first_frame in 30 120; do
        sample_id="${clip}-${first_frame}"
        first_byte=$((header_bytes + first_frame * stride))
        byte_count=$((3 * stride))
        last_byte=$((first_byte + byte_count - 1))
        curl -fsS --connect-timeout 10 --max-time 120 --retry 2 --max-filesize "$byte_count" \
            --range "${first_byte}-${last_byte}" "$source_url" -o "$destination/${sample_id}.frames"
        perl -e 'my($file,$stride)=@ARGV; open my $f,"<:raw",$file or die $!; local $/; my $b=<$f>; length($b)==3*$stride or die "Range length mismatch"; for(0..2){substr($b,$_*$stride,6) eq "FRAME\n" or die "Nonconstant frame headers"}' \
            "$destination/${sample_id}.frames" "$stride"
        { printf '%s\n' "$y4m_header"; cat "$destination/${sample_id}.frames"; } > "$destination/${sample_id}.y4m"
        ffmpeg -nostdin -hide_banner -loglevel error -i "$destination/${sample_id}.y4m" \
            -vf 'scale=in_color_matrix=bt709:in_range=tv:out_range=full' \
            -frames:v 3 -pix_fmt rgb24 -f rawvideo -y "$destination/${sample_id}.rgb"
        printf '%s' "$separator" >> "$destination/manifest.json"
        perl -MJSON::PP -e 'my($id,$url,$first,$w,$h)=@ARGV; print encode_json({id=>$id,source_url=>$url,first_frame=>0+$first,width=>0+$w,height=>0+$h,rgb_file=>"$id.rgb",reference_frame=>1})' \
            "$sample_id" "$source_url" "$first_frame" "$width" "$height" >> "$destination/manifest.json"
        separator=,
        printf '%s: native %sx%s, frames %s..%s\n' "$sample_id" "$width" "$height" "$first_frame" "$((first_frame+2))"
    done
done
printf ']}\n' >> "$destination/manifest.json"
(cd "$destination" && sha256sum ./*.rgb > rgb-sha256.txt)
