
TARGET = main

GCC_dir:=G:/Embedded/Makefiile_Embedded/GNU_Make/GNU_Arm_ToolChain/10_2021_10
CC=$(GCC_dir)/bin/arm-none-eabi-gcc
OBJ_TOOL = $(GCC_dir)/arm-none-eabi/bin/objcopy.exe

STFLASH = G:\Embedded\Makefiile_Embedded\Project\makefile-master\makefile-master\Makefile_Core\STM32_ST-LINK_Utility\ST-LINK_Utility

GDB     = gdb-multiarch

SRC_DIRS = User/src \
		  Startup \
		  Lib/STM32F10x_StdPeriph_Driver/src\
		  Lib/CMSIS/CM3/DeviceSupport/ST/STM32F10x\
		  Lib/CMSIS/CM3/CoreSupport\
		  Middlerware/FreeRTOS/FreeRTOS-Kernel\
		  Middlerware/FreeRTOS/FreeRTOS-Kernel/portable/GCC/ARM_CM3\
		  Driver/src
		  

SRC_FILES +=$(foreach SRC_DIRS,$(SRC_DIRS),$(wildcard $(SRC_DIRS)/*.c))
HEAP_DIR = Middlerware\FreeRTOS\FreeRTOS-Kernel\portable\MemMang

SRC_FILES += $(HEAP_DIR)/heap_4.c

INC_DIRS = User/inc \
			  Startup \
			  Lib/STM32F10x_StdPeriph_Driver/inc\
			  Lib/CMSIS/CM3/DeviceSupport/ST/STM32F10x\
			  Lib/CMSIS/CM3/CoreSupport\
			  Middlerware/FreeRTOS/FreeRTOS-Kernel\
			  Middlerware/FreeRTOS/FreeRTOS-Kernel/portable/GCC/ARM_CM3\
			  Middlerware/FreeRTOS\FreeRTOS-Kernel/include\
			  Lib/stm32f10x_lib/STM32F10x_StdPeriph_Driver\
			  Driver/inc

INC_FILES +=$(foreach INC_DIRS,$(INC_DIRS),$(wildcard $(INC_DIRS)/*.h))
INC_DIR_OPT = $(foreach INC_DIRS,$(INC_DIRS),-I$(INC_DIRS))

PATH_OUTPUT = Output

LD_File = Linker/stm32f1xx.ld

MCU = cortex-m3

CHIP=cortex-m3

CCFLAGS = -c -mcpu=$(CHIP) -mthumb -std=gnu11 -Os -Wall \
          -ffunction-sections -fdata-sections \
          $(INC_DIR_OPT) -DSTM32F10X_MD -DUSE_STDPERIPH_DRIVER

vpath %.c $(SRC_DIRS) $(HEAP_DIR)

OBJ_FILE := $(notdir $(SRC_FILES))
OBJ_FILE := $(subst .c,.o,$(OBJ_FILE))
PATH_OBJ := $(foreach OBJ_FILE,$(OBJ_FILE),$(PATH_OUTPUT)/$(OBJ_FILE))
LDFLAGS = -mcpu=$(CHIP) -mthumb -nostdlib -T $(LD_File) \
          -Wl,-Map=$(PATH_OUTPUT)/$(TARGET).map -Wl,--gc-sections
LIBS    = -Wl,--start-group -lc_nano -lgcc -lnosys -Wl,--end-group

build: $(PATH_OUTPUT)/$(TARGET).hex

$(PATH_OUTPUT)/%.o: %.c
	@mkdir -p $(PATH_OUTPUT)
	$(CC) $(CCFLAGS) $< -o $@

$(PATH_OUTPUT)/$(TARGET).elf: $(PATH_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(PATH_OUTPUT)/$(TARGET).hex: $(PATH_OUTPUT)/$(TARGET).elf
	$(OBJ_TOOL) -O ihex "$^" "$@"

run:
	./STM32_ST-LINK_Utility/ST-LINK_Utility/ST-LINK_CLI.exe -ME
	./STM32_ST-LINK_Utility/ST-LINK_Utility/ST-LINK_CLI.exe -p "$(PATH_OUTPUT)/$(TARGET).hex" 0x08000000
	./STM32_ST-LINK_Utility/ST-LINK_Utility/ST-LINK_CLI.exe -rst
erase:
	$(STFLASH) erase

clean:
	rm -f Output/*
print-%:
	@echo $($(subst print-,,$@))