################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/hcan_timing.c \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/head_cfg.c \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/head_decode.c \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/head_default_blob.c \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/hframe.c \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/hproto.c \
/Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c 

OBJS += \
./Common/Src/hcan_timing.o \
./Common/Src/head_cfg.o \
./Common/Src/head_decode.o \
./Common/Src/head_default_blob.o \
./Common/Src/hframe.o \
./Common/Src/hproto.o \
./Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.o 

C_DEPS += \
./Common/Src/hcan_timing.d \
./Common/Src/head_cfg.d \
./Common/Src/head_decode.d \
./Common/Src/head_default_blob.d \
./Common/Src/hframe.d \
./Common/Src/hproto.d \
./Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.d 


# Each subdirectory must supply rules for building sources it contributes
Common/Src/hcan_timing.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/hcan_timing.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
Common/Src/head_cfg.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/head_cfg.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
Common/Src/head_decode.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/head_decode.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
Common/Src/head_default_blob.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/head_default_blob.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
Common/Src/hframe.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/hframe.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
Common/Src/hproto.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/hproto.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.o: /Users/joshuadeering/STM32CubeIDE/workspace_1.19.0/HeadModuleV1/Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c Common/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H745xx -DUSE_PWR_LDO_SUPPLY -c -I../Core/Inc -I../../Common/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Common-2f-Src

clean-Common-2f-Src:
	-$(RM) ./Common/Src/hcan_timing.cyclo ./Common/Src/hcan_timing.d ./Common/Src/hcan_timing.o ./Common/Src/hcan_timing.su ./Common/Src/head_cfg.cyclo ./Common/Src/head_cfg.d ./Common/Src/head_cfg.o ./Common/Src/head_cfg.su ./Common/Src/head_decode.cyclo ./Common/Src/head_decode.d ./Common/Src/head_decode.o ./Common/Src/head_decode.su ./Common/Src/head_default_blob.cyclo ./Common/Src/head_default_blob.d ./Common/Src/head_default_blob.o ./Common/Src/head_default_blob.su ./Common/Src/hframe.cyclo ./Common/Src/hframe.d ./Common/Src/hframe.o ./Common/Src/hframe.su ./Common/Src/hproto.cyclo ./Common/Src/hproto.d ./Common/Src/hproto.o ./Common/Src/hproto.su ./Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.cyclo ./Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.d ./Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.o ./Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.su

.PHONY: clean-Common-2f-Src

