#!/bin/bash
G=/home/simonea/ultima7_exult/tmp/gap4
SC=$(cat $G/results/scenes.txt)
run(){ tag=$1; bin=$2; cfg=$3; svals=$4; n=$5; extra=$6
  echo "$tag load: $(cut -d' ' -f1-3 /proc/loadavg)" >> $G/results/loadlog.txt
  env $extra EXULT_PAINT_BENCH=1 EXULT_BENCH_N=$n EXULT_BENCH_TAG=$tag EXULT_BENCH_S=$svals EXULT_BENCH_OUT=$G/results/$tag.txt EXULT_BENCH_SCENES="$SC" SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy timeout 900 $bin --bg --nomenu -c $G/cfg/$cfg.cfg > $G/results/$tag.log 2>&1
  echo "$tag exit=$? load-after: $(cut -d' ' -f1-3 /proc/loadavg)" >> $G/results/loadlog.txt
}
run q320 $G/exult-src/exult g320 2,3,4,6 300 X=1
run q640 $G/exult-src/exult g640 2,3 200 X=1
run q1280 $G/exult-src/exult g1280 2 200 X=1
run q1720 $G/exult-src/exult g1720 2 200 X=1
run q1920 $G/exult-src/exult g1920 2 200 EXULT_BENCH_NOHIRES=1
run q320O0 $G/exult-src-O0/exult g320 6 100 X=1
