#ifndef DRIVER_GPIO_H
#define DRIVER_GPIO_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "stm32f10x.h"

    /* GPIO Status */
    typedef enum
    {
        drvGpioOK = 0,
        drvGpioError
    } Driver_GPIO_Status_t;

    /* GPIO Mode */
    typedef enum
    {
        drvGpioModeInput = 0,
        drvGpioModeOutputPP,
        drvGpioModeOutputOD,
        drvGpioModeAFPP,
        drvGpioModeAFOD,
        drvGpioModeAnalog
    } Driver_GPIO_Mode_t;

    /* GPIO Pull */
    typedef enum
    {
        drvGpioPullNone = 0,
        drvGpioPullUp,
        drvGpioPullDown
    } Driver_GPIO_Pull_t;

    /* GPIO Speed */
    typedef enum
    {
        drvGpioSpeedLow = 0,
        drvGpioSpeedMedium,
        drvGpioSpeedHigh
    } Driver_GPIO_Speed_t;

    /* GPIO Logic Level */
    typedef enum
    {
        drvGpioLevelLow = 0,
        drvGpioLevelHigh
    } Driver_GPIO_Level_t;

    /* GPIO Configuration */
    typedef struct
    {
        GPIO_TypeDef *GPIOx;
        uint16_t GPIO_Pin;
        Driver_GPIO_Mode_t GPIO_Mode;
        Driver_GPIO_Pull_t GPIO_Pull;
        Driver_GPIO_Speed_t GPIO_Speed;
    } Driver_GPIO_Config_t;

    /* GPIO APIs */
    Driver_GPIO_Status_t Driver_GPIO_Init(Driver_GPIO_Config_t *GPIO_Config);

    Driver_GPIO_Status_t Driver_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

    void Driver_GPIO_Remap(uint32_t GPIO_Remap, FunctionalState NewState);

    void Driver_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin,
                              Driver_GPIO_Level_t PinLevel);

    void Driver_GPIO_SetPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

    void Driver_GPIO_ResetPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

    void Driver_GPIO_TogglePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

    Driver_GPIO_Level_t Driver_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

    Driver_GPIO_Level_t Driver_GPIO_ReadOutputPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

    void Driver_GPIO_WritePort(GPIO_TypeDef *GPIOx, uint16_t PortValue);

    uint16_t Driver_GPIO_ReadPort(GPIO_TypeDef *GPIOx);

#ifdef __cplusplus
}
#endif

#endif