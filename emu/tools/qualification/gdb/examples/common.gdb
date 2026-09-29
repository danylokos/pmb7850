# Native GDB commands only. Arguments are expressions; group compound arguments.
set $checks = 0
define c166-assert-eq
  if ($arg0) != ($arg1)
    printf "C166_SCRIPT_FAILURE actual=%llu expected=%llu\n", ($arg0), ($arg1)
    quit 1
  end
  set $checks = $checks + 1
end
document c166-assert-eq
Assert two integer expressions are equal; fail the batch process otherwise.
end
