set architecture aarch64
set pagination off
target remote :1234
hbreak *0xfffffff007aa8e78
continue
echo \n== first panic(): fmt, args ==\n
x/s $x0
x/s $x2
echo \n== lr + frame chain (fp, lr) ==\n
info registers x1 x2 x3 x30
set $a = $x29
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
set $a = *(unsigned long*)$a
x/2gx $a
