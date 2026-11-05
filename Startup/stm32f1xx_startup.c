
/*
 * Startup file for STM32F103C8T6
 * Core       : ARM Cortex-M3
 * Compiler   : ARM GCC
 * Library    : STM32F10x SPL
 */

#include <stdint.h>
#include "stm32f10x.h"

/*==========================================================
 * Linker symbols
 *==========================================================*/

extern uint32_t _estack;

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;

extern uint32_t _sbss;
extern uint32_t _ebss;

/*==========================================================
 * External functions
 *==========================================================*/

extern int main(void);
extern void SystemInit(void);

/*==========================================================
 * Handler declarations
 *==========================================================*/

void Reset_Handler(void);
void Default_Handler(void);

/* Core exception handlers */

void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

/* Peripheral interrupt handlers */

void WWDG_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void PVD_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TAMPER_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RTC_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void FLASH_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RCC_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI4_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel4_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel5_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel6_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void DMA1_Channel7_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void ADC1_2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_HP_CAN1_TX_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void CAN1_RX1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void CAN1_SCE_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI9_5_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM1_BRK_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM1_UP_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM1_TRG_COM_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM1_CC_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void TIM4_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void I2C1_EV_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void I2C1_ER_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void I2C2_EV_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void I2C2_ER_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void SPI1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void SPI2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART2_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void EXTI15_10_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void RTCAlarm_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USBWakeUp_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

/*==========================================================
 * Vector table
 * STM32F103x8/xB - 43 peripheral IRQ entries
 *==========================================================*/

__attribute__((section(".isr_vector"), used))
const uintptr_t VectorTable[] =
    {
        (uintptr_t)&_estack, /*  0 - Initial Stack Pointer */

        (uintptr_t)Reset_Handler,      /*  1 - Reset */
        (uintptr_t)NMI_Handler,        /*  2 - NMI */
        (uintptr_t)HardFault_Handler,  /*  3 - HardFault */
        (uintptr_t)MemManage_Handler,  /*  4 - MemManage */
        (uintptr_t)BusFault_Handler,   /*  5 - BusFault */
        (uintptr_t)UsageFault_Handler, /*  6 - UsageFault */

        0, /*  7 - Reserved */
        0, /*  8 - Reserved */
        0, /*  9 - Reserved */
        0, /* 10 - Reserved */

        (uintptr_t)SVC_Handler,      /* 11 - SVCall */
        (uintptr_t)DebugMon_Handler, /* 12 - Debug Monitor */

        0, /* 13 - Reserved */

        (uintptr_t)PendSV_Handler,  /* 14 - PendSV */
        (uintptr_t)SysTick_Handler, /* 15 - SysTick */

        /* External interrupts */

        (uintptr_t)WWDG_IRQHandler,            /*  0 */
        (uintptr_t)PVD_IRQHandler,             /*  1 */
        (uintptr_t)TAMPER_IRQHandler,          /*  2 */
        (uintptr_t)RTC_IRQHandler,             /*  3 */
        (uintptr_t)FLASH_IRQHandler,           /*  4 */
        (uintptr_t)RCC_IRQHandler,             /*  5 */
        (uintptr_t)EXTI0_IRQHandler,           /*  6 */
        (uintptr_t)EXTI1_IRQHandler,           /*  7 */
        (uintptr_t)EXTI2_IRQHandler,           /*  8 */
        (uintptr_t)EXTI3_IRQHandler,           /*  9 */
        (uintptr_t)EXTI4_IRQHandler,           /* 10 */
        (uintptr_t)DMA1_Channel1_IRQHandler,   /* 11 */
        (uintptr_t)DMA1_Channel2_IRQHandler,   /* 12 */
        (uintptr_t)DMA1_Channel3_IRQHandler,   /* 13 */
        (uintptr_t)DMA1_Channel4_IRQHandler,   /* 14 */
        (uintptr_t)DMA1_Channel5_IRQHandler,   /* 15 */
        (uintptr_t)DMA1_Channel6_IRQHandler,   /* 16 */
        (uintptr_t)DMA1_Channel7_IRQHandler,   /* 17 */
        (uintptr_t)ADC1_2_IRQHandler,          /* 18 */
        (uintptr_t)USB_HP_CAN1_TX_IRQHandler,  /* 19 */
        (uintptr_t)USB_LP_CAN1_RX0_IRQHandler, /* 20 */
        (uintptr_t)CAN1_RX1_IRQHandler,        /* 21 */
        (uintptr_t)CAN1_SCE_IRQHandler,        /* 22 */
        (uintptr_t)EXTI9_5_IRQHandler,         /* 23 */
        (uintptr_t)TIM1_BRK_IRQHandler,        /* 24 */
        (uintptr_t)TIM1_UP_IRQHandler,         /* 25 */
        (uintptr_t)TIM1_TRG_COM_IRQHandler,    /* 26 */
        (uintptr_t)TIM1_CC_IRQHandler,         /* 27 */
        (uintptr_t)TIM2_IRQHandler,            /* 28 */
        (uintptr_t)TIM3_IRQHandler,            /* 29 */
        (uintptr_t)TIM4_IRQHandler,            /* 30 */
        (uintptr_t)I2C1_EV_IRQHandler,         /* 31 */
        (uintptr_t)I2C1_ER_IRQHandler,         /* 32 */
        (uintptr_t)I2C2_EV_IRQHandler,         /* 33 */
        (uintptr_t)I2C2_ER_IRQHandler,         /* 34 */
        (uintptr_t)SPI1_IRQHandler,            /* 35 */
        (uintptr_t)SPI2_IRQHandler,            /* 36 */
        (uintptr_t)USART1_IRQHandler,          /* 37 */
        (uintptr_t)USART2_IRQHandler,          /* 38 */
        (uintptr_t)USART3_IRQHandler,          /* 39 */
        (uintptr_t)EXTI15_10_IRQHandler,       /* 40 */
        (uintptr_t)RTCAlarm_IRQHandler,        /* 41 */
        (uintptr_t)USBWakeUp_IRQHandler        /* 42 */
};

/*==========================================================
 * Reset Handler
 *==========================================================*/

void Reset_Handler(void)
{
  uint32_t *src;
  uint32_t *dst;

  /* Copy initialized data from FLASH to RAM */

  src = &_sidata;
  dst = &_sdata;

  while (dst < &_edata)
  {
    *dst++ = *src++;
  }

  /* Clear BSS section */

  dst = &_sbss;

  while (dst < &_ebss)
  {
    *dst++ = 0;
  }

  /* Initialize system */

  SystemInit();

  /* Enter application */

  main();

  /* main should never return */

  while (1)
  {
  }
}

/*==========================================================
 * Default Handler
 *==========================================================*/

void Default_Handler(void)
{
  while (1)
  {
  }
}
