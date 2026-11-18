set architecture aarch64
set pagination off
set confirm off
target remote :1234
python
import gdb, time
gdb.execute("continue &")
time.sleep(25)
gdb.execute("interrupt")
time.sleep(1)
end
echo \n== after 25 s ==\n
info registers pc sp x0 x1 x2 x3 x29 x30 cpsr
x/6i $pc
set $a = $x29
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
