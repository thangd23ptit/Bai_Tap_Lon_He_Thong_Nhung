
#include "stm32f10x.h"
#include "Driver_GPIO.h"

static void Delay(volatile uint32_t time)
{
    while (time--)
        ;
}

int main(void)
{
    /* GPIO Configuration */
    Driver_GPIO_Config_t LED_Config =
        {
            .GPIOx = GPIOC,
            .GPIO_Pin = GPIO_Pin_13,
            .GPIO_Mode = drvGpioModeOutputPP,
            .GPIO_Pull = drvGpioPullNone,
            .GPIO_Speed = drvGpioSpeedLow};

    /* Initialize GPIO */
    Driver_GPIO_Init(&LED_Config);

    /* LED Loop */
    while (1)
    {
        Driver_GPIO_TogglePin(GPIOC, GPIO_Pin_13);

        Delay(500000);
    }
}
