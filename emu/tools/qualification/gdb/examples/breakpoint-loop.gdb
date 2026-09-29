# Synthetic ADD r0,#1 / JMPR loop, sourced from the execution fixture.
source common.gdb
source seed.gdb
set breakpoint condition-evaluation host
set $hits = 0
break *$base if $r0 >= 2
set $loop_bp = $bpnum
commands
  silent
  c166-assert-eq $_hit_bpnum $loop_bp
  c166-assert-eq $pc $base
  c166-assert-eq $guest_icount (2*$r0)
  c166-assert-eq $r0 ($hits+2)
  set $hits = $hits + 1
  printf "C166_SCRIPT_HIT=%u r0=%u count=%llu\n", $hits, $r0, $guest_icount
  if $hits < 3
    continue
  end
end
continue
c166-assert-eq $hits 3
c166-assert-eq $r0 4
c166-assert-eq $guest_icount 8
c166-assert-eq $pc $base
delete $loop_bp
stepi
c166-assert-eq $r0 5
c166-assert-eq $pc ($base+2)
c166-assert-eq $guest_icount 9
printf "C166_SCRIPT_CHECKS=%u\n", $checks
