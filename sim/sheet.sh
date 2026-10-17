#!/bin/sh
# usage: sheet.sh <out.png> <every-nth-frame> <cols> <rows> -- sim args...
out=$1; n=$2; c=$3; r=$4; shift 5
"$(dirname "$0")/sim" "$@" | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 240x240 -r 50 -i - \
  -vf "select='not(mod(n\,$n))',scale=160:160,drawtext=text='%{n}':x=3:y=3:fontsize=10:fontcolor=gray,tile=${c}x${r}:padding=2:color=0x333333" -frames:v 1 "$out"
