# Reproducible extraction of the ACMM 33^3 colour-world LUT.
#
# The kit does NOT bundle this table.  It is a vendor runtime table: while
# converting the page, the vendor filter builds a 33x33x33 byte lookup table
# and applies it through the 3-tap element PCM_lfFilter8Eval3x1.  Dump it from
# the running filter and point the kit at it with KONICA_LUT=/path/to/lut33.bin.
#
#   PPD=/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd
#   export CONTENT_TYPE=application/vnd.cups-raster FINAL_CONTENT_TYPE=application/vnd.cups-raster
#   export CUPS_SERVERROOT=/etc/cups CUPS_DATADIR=/usr/share/cups
#   export LD_LIBRARY_PATH=/usr/local/lib/konica/lib
#
#   F=/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
#   gdb -q -batch -x tools/dump-lut.gdb --args "$F" 1 u t 1 "$OPTS" \
#       < /root/konica-corpus-full/corpus/037-size-16K.ras
#
# Result: /tmp/lut33.bin (35937 bytes)
#   sha256 84a424f5cccdbe7070b3f926fe9834b515db69391440ccac1dc5b046622c6d6f
#
# The table is an sGrayInv.icm (kTRC)-derived gray transform and is identical
# for OCM_TonerSave on/off; it does not depend on the page content.  Its
# *generator* (PCM_fColorWorldCreateFromConversion -> PCM_lfCompositBasicTakeUp)
# has not been reproduced, so the kit loads a dump instead of recomputing it.
#
# Break at the element entry.  The eval prologue overwrites r8, but the
# element object is still reachable as rsi->0x18 (the descriptor's owner),
# and element+0x78 is the 33^3 table base.
set pagination off
set confirm off
break *0x4512d0
run
set $e = *(unsigned long*)($rsi + 0x18)
set $lut = *(unsigned long*)($e + 0x78)
printf "ELEM = %p, LUT base = %p\n", $e, $lut
dump binary memory /tmp/lut33.bin $lut $lut + 35937
printf "wrote /tmp/lut33.bin\n"
quit
