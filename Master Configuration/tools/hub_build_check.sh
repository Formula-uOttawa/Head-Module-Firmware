#!/bin/zsh
# Out-of-tree compile + link check of the hub firmware (SensorHubV1) with CubeIDE's bundled toolchain.
# usage: tools/hub_build_check.sh [output-dir]
R=$(cd "$(dirname "$0")/../../../SensorHubV1" && pwd)
O=${1:-${TMPDIR:-/tmp}/hubfw_build}
TC=$(ls -d /Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14*/tools/bin | tail -1)
GCC=$TC/arm-none-eabi-gcc
rm -rf "$O"; mkdir -p "$O"
ARCH=(-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
INC=(-I$R/Core/Inc -I$R/Drivers/STM32G4xx_HAL_Driver/Inc -I$R/Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I$R/Drivers/CMSIS/Device/ST/STM32G4xx/Include -I$R/Drivers/CMSIS/Include)
CF=($ARCH -std=gnu11 -g3 -O0 -ffunction-sections -fdata-sections -Wall ${=WEXTRA} --specs=nano.specs -mthumb -DUSE_HAL_DRIVER -DSTM32G474xx -DDEBUG $INC)
fail=0
for f in $R/Core/Src/*.c $R/Drivers/STM32G4xx_HAL_Driver/Src/*.c(N); do
  case $f in *_template.c) continue;; esac
  $GCC $CF -c $f -o $O/$(basename $f .c).o 2> $O/err.txt || { echo "== COMPILE FAIL $f"; head -20 $O/err.txt; fail=1; }
  [ -s $O/err.txt ] && { echo "== warnings $f"; head -10 $O/err.txt; }
done
$GCC $ARCH -x assembler-with-cpp -c $R/Core/Startup/*.s -o $O/startup.o || fail=1
[ $fail = 0 ] && $GCC $ARCH -T$R/STM32G474RETX_FLASH.ld --specs=nosys.specs -Wl,-Map=$O/out.map -Wl,--gc-sections ${=LDEXTRA} -static --specs=nano.specs -mthumb -Wl,--start-group -lc -lm -Wl,--end-group $O/*.o -o $O/out.elf 2>&1 | head -${LINKLINES:-20}
[ -f $O/out.elf ] && $TC/arm-none-eabi-size $O/out.elf
echo "hub: fail=$fail"; exit $fail
