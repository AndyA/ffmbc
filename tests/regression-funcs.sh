#!/bin/sh
#
# common regression functions for ffmpeg
#
#

test="${1#regtest-}"
test_ref=$2
raw_src_dir=$3
target_exec=$4
target_path=$5

datadir="./tests/data"
target_datadir="${target_path}/${datadir}"

this="$test.$test_ref"
logdir="$datadir/regression/$test_ref"
logfile="$logdir/$test"
outfile="$datadir/$test_ref/"
errfile="$datadir/$this.err"

# various files
ffmpeg="$target_exec ${target_path}/ffmbc"
tiny_psnr="tests/tiny_psnr"
benchfile="$datadir/$this.bench"
bench="$datadir/$this.bench.tmp"
bench2="$datadir/$this.bench2.tmp"
raw_src="${target_path}/$raw_src_dir/%02d.pgm"
raw_dst="$datadir/$this.out.yuv"
raw_ref="$datadir/$test_ref.ref.yuv"
pcm_src="$target_datadir/asynth1.sw"
pcm_dst="$datadir/$this.out.wav"
pcm_ref="$datadir/$test_ref.ref.wav"
crcfile="$datadir/$this.crc"
target_crcfile="$target_datadir/$this.crc"

cleanfiles="$raw_dst $pcm_dst $crcfile $bench $bench2"
trap 'rm -f -- $cleanfiles' EXIT

mkdir -p "$datadir"
mkdir -p "$outfile"
mkdir -p "$logdir"

(exec >&3) 2>/dev/null || exec 3>&2

[ "${V-0}" -gt 0 ] && echov=echov || echov=:
[ "${V-0}" -gt 1 ] || exec 2>$errfile

echov(){
    echo "$@" >&3
}

. $(dirname $0)/md5.sh

BITEXACT="-flags +bitexact -sws_flags +accurate_rnd+bitexact"
ENC_OPTS="$BITEXACT -idct simple -dct fastint"
DEC_OPTS="-v 0 -y -flags +bitexact -idct simple"

run_ffmpeg()
{
    $echov $ffmpeg $DEC_OPTS $*
    $ffmpeg $DEC_OPTS $*
}

do_ffmpeg()
{
    f="$1"
    shift
    set -- $* ${target_path}/$f
    $echov $ffmpeg $DEC_OPTS $*
    $ffmpeg $DEC_OPTS -benchmark $* > $bench
    do_md5sum $f >> $logfile
    if [ $f = $raw_dst ] ; then
        $tiny_psnr $f $raw_ref >> $logfile
    elif [ $f = $pcm_dst ] ; then
        $tiny_psnr $f $pcm_ref 2 >> $logfile
    else
        wc -c $f >> $logfile
    fi
    expr "$(cat $bench)" : '.*utime=\(.*s\)' > $bench2
    echo $(cat $bench2) $f >> $benchfile
}

do_ffmpeg_nomd5()
{
    f="$1"
    shift
    set -- $* ${target_path}/$f
    $echov $ffmpeg $DEC_OPTS $*
    $ffmpeg $DEC_OPTS -benchmark $* > $bench
    if [ $f = $raw_dst ] ; then
        $tiny_psnr $f $raw_ref >> $logfile
    elif [ $f = $pcm_dst ] ; then
        $tiny_psnr $f $pcm_ref 2 >> $logfile
    else
        wc -c $f >> $logfile
    fi
    expr "$(cat $bench)" : '.*utime=\(.*s\)' > $bench2
    echo $(cat $bench2) $f >> $benchfile
}

do_ffmpeg_crc()
{
    f="$1"
    shift
    $echov $ffmpeg $DEC_OPTS $* -f crc "$target_crcfile"
    $ffmpeg $DEC_OPTS $* -f crc "$target_crcfile"
    echo "$f $(cat $crcfile)" >> $logfile
}

do_ffmpeg_nocheck()
{
    f="$1"
    shift
    $echov $ffmpeg $DEC_OPTS $*
    $ffmpeg $DEC_OPTS -benchmark $* > $bench
    expr "$(cat $bench)" : '.*utime=\(.*s\)' > $bench2
    echo $(cat $bench2) $f >> $benchfile
}

do_video_decoding()
{
    do_ffmpeg $raw_dst $1 -i $target_path/$file $ENC_OPTS -f rawvideo $2
}

do_video_encoding()
{
    file=${outfile}$1
    do_ffmpeg $file -f image2 -vcodec pgmyuv $4 -i $raw_src $ENC_OPTS $2 $3
}

do_audio_encoding()
{
    file=${outfile}$1
    do_ffmpeg $file -ac 2 -f s16le -i $pcm_src $ENC_OPTS -ab 128k $3
}

do_audio_decoding()
{
    do_ffmpeg $pcm_dst -i $target_path/$file -sample_fmt s16 -f wav
}
