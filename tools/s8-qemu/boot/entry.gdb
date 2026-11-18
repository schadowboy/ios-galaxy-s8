set architecture aarch64
set pagination off
target remote :1234
hbreak *0xb143b488
continue
echo \n== at kernel entry (physical, MMU off) ==\n
info registers x0 x1 x2 x3 x16 pc sp cpsr
x/s $x0+108
x/8gx $x0
x/6i $pc
hbreak *0xfffffff007437000
continue
echo \n== first exception vector hit ==\n
info registers x0 x1 x2 x3 x4 x5 x6 x7 x28 x29 x30 pc sp cpsr
info registers elr_el1 esr_el1 far_el1 sctlr_el1 ttbr1_el1 vbar_el1
x/8i $x30-16
