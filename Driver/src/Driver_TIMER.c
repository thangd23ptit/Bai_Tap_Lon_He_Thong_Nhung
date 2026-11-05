#include "Driver_TIMER.h"
#include "Driver_GPIO.h"
#include "misc.h"

#define TIMER_CHANNEL_COUNT 4U

/* ------------------------------------------------------------------ */
/* Bảng ánh xạ phần cứng cho từng Timer                               */
/* ------------------------------------------------------------------ */
typedef struct
{
    TIM_TypeDef *TIMx;
    uint32_t RccPeriph;
    IRQn_Type Irq;
    GPIO_TypeDef *PwmPort[TIMER_CHANNEL_COUNT];
    uint16_t PwmPin[TIMER_CHANNEL_COUNT];
} Driver_Timer_Hw_t;

static const Driver_Timer_Hw_t s_hw[drvTimerCount] = {
    {TIM2, RCC_APB1Periph_TIM2, TIM2_IRQn, {GPIOA, GPIOA, GPIOA, GPIOA}, {GPIO_Pin_0, GPIO_Pin_1, GPIO_Pin_2, GPIO_Pin_3}},
    {TIM3, RCC_APB1Periph_TIM3, TIM3_IRQn, {GPIOA, GPIOA, GPIOB, GPIOB}, {GPIO_Pin_6, GPIO_Pin_7, GPIO_Pin_0, GPIO_Pin_1}},
    {TIM4, RCC_APB1Periph_TIM4, TIM4_IRQn, {GPIOB, GPIOB, GPIOB, GPIOB}, {GPIO_Pin_6, GPIO_Pin_7, GPIO_Pin_8, GPIO_Pin_9}},
};

/* ------------------------------------------------------------------ */
/* Trạng thái của từng Timer                                          */
/* ------------------------------------------------------------------ */
typedef struct
{
    volatile Driver_Timer_Callback_t Callback;
    void *volatile Arg;
    uint8_t PwmMask; // Bit n = 1: kênh n đã được cấu hình PWM
    uint8_t Initialized;
} Driver_Timer_State_t;

static Driver_Timer_State_t s_state[drvTimerCount];

/* ------------------------------------------------------------------ */
/* Hàm nội bộ                                                         */
/* ------------------------------------------------------------------ */

static uint8_t prv_is_ready(Driver_Timer_Port_t port)
{
    return (port < drvTimerCount && s_state[port].Initialized != 0U) ? 1U : 0U;
}

/* Xung nhịp đưa vào Timer trên APB1:
   nếu prescaler APB1 khác 1 thì xung nhịp Timer = 2 x PCLK1 */
static uint32_t prv_timer_clock(void)
{
    RCC_ClocksTypeDef clocks;

    RCC_GetClocksFreq(&clocks);
    return (clocks.HCLK_Frequency == clocks.PCLK1_Frequency)
               ? clocks.PCLK1_Frequency
               : (clocks.PCLK1_Frequency * 2U);
}

/* Tính PSC và ARR từ chu kỳ (us). Cả hai đều là thanh ghi 16 bit */
static Driver_Timer_Status_t prv_calc_period(uint32_t periodUs, uint16_t *psc, uint16_t *arr)
{
    uint64_t total;
    uint32_t div;
    uint32_t ticks;

    if (periodUs == 0U)
    {
        return drvTimerError;
    }

    total = ((uint64_t)prv_timer_clock() * periodUs) / 1000000ULL;

    if (total < 2ULL || total > (65536ULL * 65536ULL))
    {
        return drvTimerError;
    }

    div = (uint32_t)((total - 1ULL) / 65536ULL) + 1U; // = PSC + 1, nhỏ nhất để ARR vừa 16 bit
    ticks = (uint32_t)(total / div);                  // = ARR + 1

    *psc = (uint16_t)(div - 1U);
    *arr = (uint16_t)(ticks - 1U);
    return drvTimerOk;
}

static void prv_set_compare(TIM_TypeDef *tim, Driver_Timer_Channel_t ch, uint16_t value)
{
    switch (ch)
    {
    case drvTimerChannel1:
        TIM_SetCompare1(tim, value);
        break;
    case drvTimerChannel2:
        TIM_SetCompare2(tim, value);
        break;
    case drvTimerChannel3:
        TIM_SetCompare3(tim, value);
        break;
    default:
        TIM_SetCompare4(tim, value);
        break;
    }
}

static uint16_t prv_duty_to_compare(TIM_TypeDef *tim, uint16_t dutyPermille)
{
    uint32_t cmp = (((uint32_t)tim->ARR + 1U) * dutyPermille) / 1000U;

    /* ARR = 0xFFFF và duty 100% cho CCR = 65536 không vừa 16 bit, giới hạn lại
       (xuất hiện 1 tick mức thấp trong mỗi chu kỳ, bỏ qua được) */
    if (cmp > 0xFFFFU)
    {
        cmp = 0xFFFFU;
    }
    return (uint16_t)cmp;
}

static void prv_irq(Driver_Timer_Port_t port)
{
    TIM_TypeDef *tim = s_hw[port].TIMx;

    if (TIM_GetITStatus(tim, TIM_IT_Update) != RESET)
    {
        Driver_Timer_Callback_t cb = s_state[port].Callback;

        TIM_ClearITPendingBit(tim, TIM_IT_Update);

        if (cb != 0)
        {
            cb(s_state[port].Arg);
        }
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

Driver_Timer_Status_t Driver_Timer_Init(const Driver_Timer_Config_t *Timer_Config)
{
    const Driver_Timer_Hw_t *hw;
    TIM_TimeBaseInitTypeDef base;
    NVIC_InitTypeDef nvicInit;
    uint16_t psc;
    uint16_t arr;

    if (Timer_Config == 0 || Timer_Config->Timer_Port >= drvTimerCount)
    {
        return drvTimerError;
    }

    if (prv_calc_period(Timer_Config->Timer_PeriodUs, &psc, &arr) != drvTimerOk)
    {
        return drvTimerError;
    }

    hw = &s_hw[Timer_Config->Timer_Port];

    /* 1. Bật clock và đưa Timer về trạng thái ban đầu */
    s_state[Timer_Config->Timer_Port].Initialized = 0U;
    RCC_APB1PeriphClockCmd(hw->RccPeriph, ENABLE);
    TIM_DeInit(hw->TIMx);

    s_state[Timer_Config->Timer_Port].Callback = 0;
    s_state[Timer_Config->Timer_Port].Arg = 0;
    s_state[Timer_Config->Timer_Port].PwmMask = 0U;

    /* 2. Cấu hình time base, đếm lên */
    base.TIM_Prescaler = psc;
    base.TIM_Period = arr;
    base.TIM_ClockDivision = TIM_CKD_DIV1;
    base.TIM_CounterMode = TIM_CounterMode_Up;
    base.TIM_RepetitionCounter = 0U;
    TIM_TimeBaseInit(hw->TIMx, &base);

    /* ARR preload: giá trị mới chỉ có hiệu lực ở lần tràn kế tiếp, tránh méo PWM */
    TIM_ARRPreloadConfig(hw->TIMx, ENABLE);

    /* TIM_TimeBaseInit tạo sẵn một update event, xóa cờ để không bị ngắt giả */
    TIM_ClearITPendingBit(hw->TIMx, TIM_IT_Update);

    /* 3. NVIC (ngắt tràn chỉ được bật khi đăng ký callback) */
    nvicInit.NVIC_IRQChannel = hw->Irq;
    nvicInit.NVIC_IRQChannelPreemptionPriority = Timer_Config->Timer_IrqPriority;
    nvicInit.NVIC_IRQChannelSubPriority = 0U;
    nvicInit.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvicInit);

    s_state[Timer_Config->Timer_Port].Initialized = 1U;
    return drvTimerOk;
}

Driver_Timer_Status_t Driver_Timer_DeInit(Driver_Timer_Port_t Timer_Port)
{
    const Driver_Timer_Hw_t *hw;
    NVIC_InitTypeDef nvicInit;
    uint8_t ch;

    if (Timer_Port >= drvTimerCount)
    {
        return drvTimerError;
    }

    hw = &s_hw[Timer_Port];
    s_state[Timer_Port].Initialized = 0U;

    TIM_Cmd(hw->TIMx, DISABLE);
    TIM_ITConfig(hw->TIMx, TIM_IT_Update, DISABLE);

    nvicInit.NVIC_IRQChannel = hw->Irq;
    nvicInit.NVIC_IRQChannelPreemptionPriority = 0U;
    nvicInit.NVIC_IRQChannelSubPriority = 0U;
    nvicInit.NVIC_IRQChannelCmd = DISABLE;
    NVIC_Init(&nvicInit);

    TIM_DeInit(hw->TIMx);
    RCC_APB1PeriphClockCmd(hw->RccPeriph, DISABLE);

    for (ch = 0U; ch < TIMER_CHANNEL_COUNT; ch++)
    {
        if (s_state[Timer_Port].PwmMask & (uint8_t)(1U << ch))
        {
            (void)Driver_GPIO_DeInit(hw->PwmPort[ch], hw->PwmPin[ch]);
        }
    }

    s_state[Timer_Port].PwmMask = 0U;
    s_state[Timer_Port].Callback = 0;
    s_state[Timer_Port].Arg = 0;

    return drvTimerOk;
}

Driver_Timer_Status_t Driver_Timer_Start(Driver_Timer_Port_t Timer_Port)
{
    if (prv_is_ready(Timer_Port) == 0U)
    {
        return drvTimerError;
    }

    TIM_Cmd(s_hw[Timer_Port].TIMx, ENABLE);
    return drvTimerOk;
}

Driver_Timer_Status_t Driver_Timer_Stop(Driver_Timer_Port_t Timer_Port)
{
    if (prv_is_ready(Timer_Port) == 0U)
    {
        return drvTimerError;
    }

    TIM_Cmd(s_hw[Timer_Port].TIMx, DISABLE);
    return drvTimerOk;
}

Driver_Timer_Status_t Driver_Timer_SetPeriodUs(Driver_Timer_Port_t Timer_Port, uint32_t PeriodUs)
{
    TIM_TypeDef *tim;
    uint16_t psc;
    uint16_t arr;

    if (prv_is_ready(Timer_Port) == 0U)
    {
        return drvTimerError;
    }

    if (prv_calc_period(PeriodUs, &psc, &arr) != drvTimerOk)
    {
        return drvTimerError;
    }

    tim = s_hw[Timer_Port].TIMx;

    TIM_SetAutoreload(tim, arr);
    /* Nạp PSC ngay lập tức: việc này tạo update event, nạp luôn ARR vào thanh ghi bóng
       và đặt bộ đếm về 0 */
    TIM_PrescalerConfig(tim, psc, TIM_PSCReloadMode_Immediate);
    TIM_ClearITPendingBit(tim, TIM_IT_Update); // Không gọi callback vì update event nhân tạo

    return drvTimerOk;
}

Driver_Timer_Status_t Driver_Timer_SetCallback(Driver_Timer_Port_t Timer_Port, Driver_Timer_Callback_t Callback, void *Arg)
{
    TIM_TypeDef *tim;

    if (prv_is_ready(Timer_Port) == 0U)
    {
        return drvTimerError;
    }

    tim = s_hw[Timer_Port].TIMx;

    /* Tắt ngắt trước khi đổi callback để ISR không đọc cặp (Callback, Arg) dở dang */
    TIM_ITConfig(tim, TIM_IT_Update, DISABLE);

    s_state[Timer_Port].Callback = Callback;
    s_state[Timer_Port].Arg = Arg;

    if (Callback != 0)
    {
        TIM_ClearITPendingBit(tim, TIM_IT_Update);
        TIM_ITConfig(tim, TIM_IT_Update, ENABLE);
    }

    return drvTimerOk;
}

uint16_t Driver_Timer_GetCounter(Driver_Timer_Port_t Timer_Port)
{
    if (prv_is_ready(Timer_Port) == 0U)
    {
        return 0U;
    }

    return TIM_GetCounter(s_hw[Timer_Port].TIMx);
}

uint32_t Driver_Timer_GetTickFrequency(Driver_Timer_Port_t Timer_Port)
{
    if (prv_is_ready(Timer_Port) == 0U)
    {
        return 0U;
    }

    return prv_timer_clock() / ((uint32_t)s_hw[Timer_Port].TIMx->PSC + 1U);
}

Driver_Timer_Status_t Driver_Timer_PwmInit(const Driver_Timer_PwmConfig_t *PwmConfig)
{
    const Driver_Timer_Hw_t *hw;
    TIM_OCInitTypeDef oc;
    Driver_GPIO_Config_t pin;

    if (PwmConfig == 0 ||
        prv_is_ready(PwmConfig->Timer_Port) == 0U ||
        PwmConfig->Timer_Channel > drvTimerChannel4 ||
        PwmConfig->Timer_DutyPermille > 1000U)
    {
        return drvTimerError;
    }

    hw = &s_hw[PwmConfig->Timer_Port];

    /* 1. Chân GPIO: alternate function push-pull */
    pin.GPIOx = hw->PwmPort[PwmConfig->Timer_Channel];
    pin.GPIO_Pin = hw->PwmPin[PwmConfig->Timer_Channel];
    pin.GPIO_Mode = drvGpioModeAFPP;
    pin.GPIO_Pull = drvGpioPullNone;
    pin.GPIO_Speed = drvGpioSpeedHigh;

    if (Driver_GPIO_Init(&pin) != drvGpioOk)
    {
        return drvTimerError;
    }

    /* 2. Kênh output compare ở chế độ PWM mode 1 */
    TIM_OCStructInit(&oc);
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_OCPolarity = (PwmConfig->Timer_PwmPolarity == drvTimerPwmActiveLow)
                            ? TIM_OCPolarity_Low
                            : TIM_OCPolarity_High;
    oc.TIM_Pulse = prv_duty_to_compare(hw->TIMx, PwmConfig->Timer_DutyPermille);

    switch (PwmConfig->Timer_Channel)
    {
    case drvTimerChannel1:
        TIM_OC1Init(hw->TIMx, &oc);
        TIM_OC1PreloadConfig(hw->TIMx, TIM_OCPreload_Enable);
        break;
    case drvTimerChannel2:
        TIM_OC2Init(hw->TIMx, &oc);
        TIM_OC2PreloadConfig(hw->TIMx, TIM_OCPreload_Enable);
        break;
    case drvTimerChannel3:
        TIM_OC3Init(hw->TIMx, &oc);
        TIM_OC3PreloadConfig(hw->TIMx, TIM_OCPreload_Enable);
        break;
    default:
        TIM_OC4Init(hw->TIMx, &oc);
        TIM_OC4PreloadConfig(hw->TIMx, TIM_OCPreload_Enable);
        break;
    }

    s_state[PwmConfig->Timer_Port].PwmMask |= (uint8_t)(1U << PwmConfig->Timer_Channel);
    return drvTimerOk;
}

Driver_Timer_Status_t Driver_Timer_PwmSetDuty(Driver_Timer_Port_t Timer_Port, Driver_Timer_Channel_t Timer_Channel, uint16_t DutyPermille)
{
    if (prv_is_ready(Timer_Port) == 0U ||
        Timer_Channel > drvTimerChannel4 ||
        DutyPermille > 1000U ||
        (s_state[Timer_Port].PwmMask & (uint8_t)(1U << Timer_Channel)) == 0U)
    {
        return drvTimerError;
    }

    prv_set_compare(s_hw[Timer_Port].TIMx, Timer_Channel,
                    prv_duty_to_compare(s_hw[Timer_Port].TIMx, DutyPermille));
    return drvTimerOk;
}

/* ------------------------------------------------------------------ */
/* Trình xử lý ngắt (không định nghĩa lại trong stm32f10x_it.c)        */
/* ------------------------------------------------------------------ */

void TIM2_IRQHandler(void)
{
    prv_irq(drvTimer2);
}

void TIM3_IRQHandler(void)
{
    prv_irq(drvTimer3);
}

void TIM4_IRQHandler(void)
{
    prv_irq(drvTimer4);
}