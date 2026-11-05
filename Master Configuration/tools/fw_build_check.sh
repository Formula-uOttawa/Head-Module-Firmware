#!/bin/zsh
# Out-of-tree compile + link check of one core with the project's own flags/linker script.
# Build output under */Debug/ is tracked in git, so this builds elsewhere and leaves the repo untouched.
# usage: tools/fw_build_check.sh CM7|CM4 [output-dir]
CORE=${1:?usage: fw_build_check.sh CM7|CM4 [outdir]}
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${2:-${TMPDIR:-/tmp}/headfw_build/$CORE}
TC=$(ls -d /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14*/tools/bin | tail -1)
GCC=$TC/arm-none-eabi-gcc
rm -rf "$O"; mkdir -p "$O"
if [ $CORE = CM7 ]; then ARCH=(-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard); else ARCH=(-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard); fi
INC=(-I$R/$CORE/Core/Inc -I$R/Common/Inc -I$R/Drivers/STM32H7xx_HAL_Driver/Inc -I$R/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I$R/Drivers/CMSIS/Device/ST/STM32H7xx/Include -I$R/Drivers/CMSIS/Include)
CF=($ARCH -std=gnu11 -g3 -O0 -ffunction-sections -fdata-sections -Wall ${=WEXTRA} --specs=nano.specs -mthumb -DCORE_$CORE -DUSE_HAL_DRIVER -DSTM32H745xx -DDEBUG $INC)
HAL=($(grep -o 'PARENT-1-PROJECT_LOC/Drivers/STM32H7xx_HAL_Driver/Src/[a-z0-9_]*\.c' $R/$CORE/.project | sed "s#PARENT-1-PROJECT_LOC#$R#"))
fail=0
for f in $R/$CORE/Core/Src/*.c $R/Common/Src/*.c $HAL; do
  $GCC $CF -c $f -o $O/$(basename $f .c).o 2> $O/err.txt || { echo "== COMPILE FAIL $f"; head -20 $O/err.txt; fail=1; }
  [ -s $O/err.txt ] && case $f in */Drivers/*) ;; *) echo "== warnings $f"; head -${WLINES:-8} $O/err.txt;; esac
done
$GCC $ARCH -x assembler-with-cpp -c $R/$CORE/Core/Startup/*.s -o $O/startup.o || fail=1
[ $fail = 0 ] && $GCC $ARCH -T$R/$CORE/STM32H745ZITX_FLASH.ld --specs=nosys.specs -Wl,-Map=$O/out.map -Wl,--gc-sections ${=LDEXTRA} -static --specs=nano.specs -mthumb -Wl,--start-group -lc -lm -Wl,--end-group $O/*.o -o $O/out.elf 2>&1 | head -${LINKLINES:-20}
[ -f $O/out.elf ] && $TC/arm-none-eabi-size $O/out.elf
echo "$CORE: fail=$fail"; exit $fail
