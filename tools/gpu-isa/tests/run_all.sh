cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
F="-O0 -ffp-contract=off -Wall -I../core"
for t in sf_test frag_test vert_test uni_test blend_test pack_test rast_test isa_test; do
  if [ -f $t.c ]; then
    gcc $F -o /tmp/$t $t.c -lm || { echo "BUILD FAIL $t"; continue; }
    case $t in
      frag_test|blend_test) /tmp/$t ../data/scenes.bin > /tmp/$t.out 2>&1 ;;
      isa_test) /tmp/$t ../data/corpus4.bin ../data/scenes.bin ../data/corpus_comp.bin > /tmp/$t.out 2>&1 ;;
      *) /tmp/$t > /tmp/$t.out 2>&1 ;;
    esac
    echo "$t exit=$? $(tail -1 /tmp/$t.out)"
  fi
done
gcc $F -o /tmp/corpus_run corpus_run.c -lm && /tmp/corpus_run ../data/corpus4.bin | tail -4
gcc $F -o /tmp/corpus_frag corpus_frag.c -lm && /tmp/corpus_frag ../data/corpus4.bin | tail -1
gcc $F -o /tmp/comp_run comp_run.c -lm && /tmp/comp_run ../data/corpus_comp.bin | grep -E "compositor fragment|stops by"
echo "--- C++ syntax check of the kernel headers"
for h in s8gfx_frag.h s8gfx_sf.h s8gfx_tex.h; do g++ -fsyntax-only -Wall -x c++ -I../core ../core/$h && echo "g++ ok $h"; done
