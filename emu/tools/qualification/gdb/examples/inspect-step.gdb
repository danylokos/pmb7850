# Run via the generated session.gdb, which connects and enables logs.
source common.gdb
source seed.gdb
info registers pc r0 r1 guest_icount
x/3i $pc

# Ordinary RAM inspection and writes, with a bounded host-side command loop.
set $i = 0
set $sum = 0
while $i < 4
  set {unsigned short}(0x3000 + 2 * $i) = 0x100 + $i
  set $value = *(unsigned short *)(0x3000 + 2 * $i)
  c166-assert-eq $value (0x100+$i)
  if $i < 2
    set $sum = $sum + $value
  else
    set $sum = $sum - $value
  end
  set $i = $i + 1
end
c166-assert-eq $sum -4
x/4hx 0x3000

# Generated from the existing manual-referenced execution fixture.
source step-checks.gdb
printf "C166_SCRIPT_CHECKS=%u\n", $checks
