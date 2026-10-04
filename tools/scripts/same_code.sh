#!/bin/sh
# Сравнить код и данные двух ELF-сборок, без отладочной информации.
# firmware.bin содержит SHA-256 всего ELF (поле app_elf_sha256), а он меняется от
# одного только сдвига строк в исходнике (номера строк в .debug_*). Поэтому
# «сборка автора не изменилась» проверяется по загружаемым секциям, а не по .bin.
# Использование: same_code.sh a.elf b.elf
TC="$USERPROFILE/.platformio/packages/toolchain-riscv32-esp/bin"
OD="$TC/riscv32-esp-elf-objdump"
secs=$("$OD" -h "$1" | awk '$2 ~ /^\./ && $2 !~ /^\.(debug|comment|riscv)/ {print $2}')
rc=0
sizes() { "$OD" -h "$1" | awk '$2 ~ /^\./ && $2 !~ /^\.(debug|comment|riscv)/ {print $2,$3}'; }
ha=$(sizes "$1"); hb=$(sizes "$2")
[ "$ha" = "$hb" ] || { echo "DIFF размеры секций"; rc=1; }
for s in $secs; do
  a=$("$OD" -s -j "$s" "$1" 2>/dev/null | tail -n +4 | md5sum)
  b=$("$OD" -s -j "$s" "$2" 2>/dev/null | tail -n +4 | md5sum)
  if [ "$a" != "$b" ]; then echo "DIFF $s"; rc=1; fi
done
[ $rc = 0 ] && echo "код и данные совпадают ($(echo $secs | wc -w) секций)"
exit $rc
