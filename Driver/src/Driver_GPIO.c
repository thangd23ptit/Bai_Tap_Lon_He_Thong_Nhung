#include "Driver_GPIO.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"

/* Map GPIO Port to RCC Clock */
static uint32_t prvPortToRCC(GPIO_TypeDef *GPIOx)
{
#ifdef GPIOA
    if (GPIOx == GPIOA)
        return RCC_APB2Periph_GPIOA;
#endif

#ifdef GPIOB
    if (GPIOx == GPIOB)
        return RCC_APB2Periph_GPIOB;
#endif

#ifdef GPIOC
    if (GPIOx == GPIOC)
        return RCC_APB2Periph_GPIOC;
#endif

#ifdef GPIOD
    if (GPIOx == GPIOD)
        return RCC_APB2Periph_GPIOD;
#endif

#ifdef GPIOE
    if (GPIOx == GPIOE)
        return RCC_APB2Periph_GPIOE;
#endif

#ifdef GPIOF
    if (GPIOx == GPIOF)
        return RCC_APB2Periph_GPIOF;
#endif

#ifdef GPIOG
    if (GPIOx == GPIOG)
        return RCC_APB2Periph_GPIOG;
#endif

    return 0U;
}

/* Convert Driver Mode to SPL Mode */
static GPIOMode_TypeDef prvModeToGPIO(Driver_GPIO_Mode_t mode,
                                      Driver_GPIO_Pull_t pull)
{
    switch (mode)
    {
    case drvGpioModeInput:
        if (pull == drvGpioPullUp)
            return GPIO_Mode_IPU;

        if (pull == drvGpioPullDown)
            return GPIO_Mode_IPD;

        return GPIO_Mode_IN_FLOATING;

    case drvGpioModeOutputPP:
        return GPIO_Mode_Out_PP;

    case drvGpioModeOutputOD:
        return GPIO_Mode_Out_OD;

    case drvGpioModeAFPP:
        return GPIO_Mode_AF_PP;

    case drvGpioModeAFOD:
        return GPIO_Mode_AF_OD;

    case drvGpioModeAnalog:
        return GPIO_Mode_AIN;

    default:
        return GPIO_Mode_IN_FLOATING;
    }
}

/* Convert Driver Speed to SPL Speed */
static GPIOSpeed_TypeDef prvSpeedToGPIO(Driver_GPIO_Speed_t speed)
{
    switch (speed)
    {
    case drvGpioSpeedLow:
        return GPIO_Speed_2MHz;

    case drvGpioSpeedMedium:
        return GPIO_Speed_10MHz;

    case drvGpioSpeedHigh:
        return GPIO_Speed_50MHz;

    default:
        return GPIO_Speed_2MHz;
    }
}

/* Check Configuration */
static uint8_t prvCheckConfig(Driver_GPIO_Config_t *GPIO_Config)
{
    if (GPIO_Config == 0)
        return 0;

    if (GPIO_Config->GPIOx == 0)
        return 0;

    if (GPIO_Config->GPIO_Pin == 0)
        return 0;

    if (prvPortToRCC(GPIO_Config->GPIOx) == 0)
        return 0;

    if (GPIO_Config->GPIO_Mode > drvGpioModeAnalog)
        return 0;

    if (GPIO_Config->GPIO_Pull > drvGpioPullDown)
        return 0;

    if (GPIO_Config->GPIO_Speed > drvGpioSpeedHigh)
        return 0;

    return 1;
}

/* GPIO Initialization */
Driver_GPIO_Status_t Driver_GPIO_Init(Driver_GPIO_Config_t *GPIO_Config)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    uint32_t RCC_APB2Periph_GPIOx;

    if (prvCheckConfig(GPIO_Config) == 0)
        return drvGpioError;

    RCC_APB2Periph_GPIOx = prvPortToRCC(GPIO_Config->GPIOx);

    /* Enable GPIO Clock */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOx, ENABLE);

    /* Configure GPIO */
    GPIO_InitStruct.GPIO_Pin = GPIO_Config->GPIO_Pin;

    GPIO_InitStruct.GPIO_Mode =
        prvModeToGPIO(GPIO_Config->GPIO_Mode, GPIO_Config->GPIO_Pull);

    GPIO_InitStruct.GPIO_Speed =
        prvSpeedToGPIO(GPIO_Config->GPIO_Speed);

    GPIO_Init(GPIO_Config->GPIOx, &GPIO_InitStruct);

    return drvGpioOK;
}

/* GPIO DeInitialization
   Đưa các chân được chọn về Input Floating */
Driver_GPIO_Status_t Driver_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    uint32_t RCC_APB2Periph_GPIOx;

    if (GPIOx == 0 || GPIO_Pin == 0)
        return drvGpioError;

    RCC_APB2Periph_GPIOx = prvPortToRCC(GPIOx);

    if (RCC_APB2Periph_GPIOx == 0)
        return drvGpioError;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOx, ENABLE);

    GPIO_InitStruct.GPIO_Pin = GPIO_Pin;
    GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitStruct.GPIO_Speed = GPIO_Speed_2MHz;

    GPIO_Init(GPIOx, &GPIO_InitStruct);

    return drvGpioOK;
}

/* GPIO Remap */
void Driver_GPIO_Remap(uint32_t GPIO_Remap, FunctionalState NewState)
{
    if (NewState != ENABLE && NewState != DISABLE)
        return;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);

    GPIO_PinRemapConfig(GPIO_Remap, NewState);
}

/* Write GPIO Pin */
void Driver_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin,
                          Driver_GPIO_Level_t PinLevel)
{
    if (GPIOx == 0 || GPIO_Pin == 0)
        return;

    if (PinLevel == drvGpioLevelHigh)
        GPIO_SetBits(GPIOx, GPIO_Pin);
    else if (PinLevel == drvGpioLevelLow)
        GPIO_ResetBits(GPIOx, GPIO_Pin);
}

/* Set GPIO Pin */
void Driver_GPIO_SetPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    if (GPIOx == 0 || GPIO_Pin == 0)
        return;

    GPIO_SetBits(GPIOx, GPIO_Pin);
}

/* Reset GPIO Pin */
void Driver_GPIO_ResetPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    if (GPIOx == 0 || GPIO_Pin == 0)
        return;

    GPIO_ResetBits(GPIOx, GPIO_Pin);
}

/* Toggle GPIO Pin */
void Driver_GPIO_TogglePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    uint16_t ODR_Value;

    if (GPIOx == 0 || GPIO_Pin == 0)
        return;

    ODR_Value = (uint16_t)GPIOx->ODR;

    GPIOx->BSRR =
        ((uint32_t)(ODR_Value & GPIO_Pin) << 16U) |
        (uint32_t)(~ODR_Value & GPIO_Pin);
}

/* Read GPIO Input */
Driver_GPIO_Level_t Driver_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    if (GPIOx == 0 || GPIO_Pin == 0)
        return drvGpioLevelLow;

    return (GPIOx->IDR & GPIO_Pin) ? drvGpioLevelHigh : drvGpioLevelLow;
}

/* Read GPIO Output */
Driver_GPIO_Level_t Driver_GPIO_ReadOutputPin(GPIO_TypeDef *GPIOx,
                                              uint16_t GPIO_Pin)
{
    if (GPIOx == 0 || GPIO_Pin == 0)
        return drvGpioLevelLow;

    return (GPIOx->ODR & GPIO_Pin) ? drvGpioLevelHigh : drvGpioLevelLow;
}

/* Write GPIO Port */
void Driver_GPIO_WritePort(GPIO_TypeDef *GPIOx, uint16_t PortValue)
{
    if (GPIOx == 0)
        return;

    GPIOx->ODR = PortValue;
}

/* Read GPIO Port */
uint16_t Driver_GPIO_ReadPort(GPIO_TypeDef *GPIOx)
{
    if (GPIOx == 0)
        return 0U;

    return (uint16_t)GPIOx->IDR;
}