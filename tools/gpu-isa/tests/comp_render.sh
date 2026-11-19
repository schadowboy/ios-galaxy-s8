# tests/comp_render.sh: host frame renderer. Run in WSL:  wsl -u root -d Ubuntu -- bash tests/comp_render.sh [name cap session [extra flags]]
# default: renders capture17 s8 (merge capture17 s10), capture18 s11 and s14 -> logs/_frame_<name>.png (+ logs/_render_<name>.txt)
# usage: bash tests/comp_render.sh <name> <capture dir name> <session> [merge snapshot dirs under /tmp/s8s ...] -- [comp_render flags]
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
gcc -O2 -ffp-contract=off -Wall -Wno-misleading-indentation -Wno-unused-function -Icore -o /tmp/comp_render tests/comp_render.c || exit 1
snap() { [ -d /tmp/s8s/$1/s$2 ] || python3 -I tests/replay_dump.py data/$1 $2 /tmp/s8s/$1/s$2; }
render() {   # name cap session merge-cap:session flags
  n=$1; c=$2; s=$3; m=$4; shift 4
  snap $c $s; mm=""
  if [ -n "$m" ]; then snap ${m%%:*} ${m##*:}; mm=/tmp/s8s/${m%%:*}/s${m##*:}; fi
  /tmp/comp_render /tmp/s8s/$c/s$s $mm -o /tmp/frame_$n "$@" > logs/_render_$n.txt
  python3 -I tests/ppm2png.py /tmp/frame_$n.ppm logs/_frame_$n.png
  for f in /tmp/frame_${n}_t*.ppm; do [ -f "$f" ] && python3 -I tests/ppm2png.py $f logs/_frame_$(basename $f .ppm | sed 's/^frame_//').png; done
  echo "== $n: $(grep -E '^SUMMARY' logs/_render_$n.txt)"; grep -E '^fragment unknown|^display target' logs/_render_$n.txt
}
if [ -n "$1" ]; then
  n=$1; c=$2; s=$3; shift 3
  m=""; [ "$1" != "--" ] && [ -n "$1" ] && m=$1 && shift
  [ "$1" = "--" ] && shift
  render $n $c $s "$m" "$@"
else
  render c17_s8 capture17 8 capture17:10 -all
  render c18_s11 capture18 11 capture17:10 -all
  render c18_s14 capture18 14 capture17:10 -all
fi
