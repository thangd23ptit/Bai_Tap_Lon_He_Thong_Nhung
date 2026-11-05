#include "Driver_ADC.h"
#include "Driver_GPIO.h"
#include "Driver_DMA.h"

#define ADC_GPIO_CHANNEL_COUNT 16U
#define ADC_MAX_CHANNEL 17U
#define ADC_VALID_MASK 0x3FFFFUL // Bit 0..17

/* Trên STM32F103 chỉ ADC1 có DMA, cố định ở kênh 1 */
#define ADC_DMA_CHANNEL drvDmaChannel1

/* ------------------------------------------------------------------ */
/* Bảng ánh xạ                                                        */
/* ------------------------------------------------------------------ */
typedef struct
{
    ADC_TypeDef *ADCx;
    uint32_t RccPeriph;
} Driver_ADC_Hw_t;

static const Driver_ADC_Hw_t s_hw[drvAdcCount] = {
    {ADC1, RCC_APB2Periph_ADC1},
    {ADC2, RCC_APB2Periph_ADC2},
};

/* Chân GPIO của kênh 0..15 */
static GPIO_TypeDef *const s_channelPort[ADC_GPIO_CHANNEL_COUNT] = {
    GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA, GPIOA,
    GPIOB, GPIOB,
    GPIOC, GPIOC, GPIOC, GPIOC, GPIOC, GPIOC};

static const uint16_t s_channelPin[ADC_GPIO_CHANNEL_COUNT] = {
    GPIO_Pin_0, GPIO_Pin_1, GPIO_Pin_2, GPIO_Pin_3,
    GPIO_Pin_4, GPIO_Pin_5, GPIO_Pin_6, GPIO_Pin_7,
    GPIO_Pin_0, GPIO_Pin_1,
    GPIO_Pin_0, GPIO_Pin_1, GPIO_Pin_2, GPIO_Pin_3, GPIO_Pin_4, GPIO_Pin_5};

static const uint8_t s_sampleTime[8] = {
    ADC_SampleTime_1Cycles5, ADC_SampleTime_7Cycles5, ADC_SampleTime_13Cycles5,
    ADC_SampleTime_28Cycles5, ADC_SampleTime_41Cycles5, ADC_SampleTime_55Cycles5,
    ADC_SampleTime_71Cycles5, ADC_SampleTime_239Cycles5};

typedef struct
{
    uint32_t ChannelMask;
    uint8_t SampleTime; // Giá trị ADC_SampleTime_xCycles5 của SPL
    uint8_t Initialized;
    volatile uint8_t DmaRunning; // 1: đang quét bằng DMA
    uint8_t DmaOwned;            // 1: driver đang giữ kênh DMA 1
    uint8_t DmaCircular;
    uint8_t ScanActive; // 1: ADC đang ở cấu hình quét liên tục, cần khôi phục trước khi đọc đơn lẻ
    volatile Driver_ADC_DmaCallback_t DmaCallback;
    void *volatile DmaArg;
} Driver_ADC_State_t;

static Driver_ADC_State_t s_state[drvAdcCount];

/* ------------------------------------------------------------------ */
/* Hàm nội bộ                                                         */
/* ------------------------------------------------------------------ */

/* Xung nhịp ADC (chung cho ADC1/ADC2) phải <= 14 MHz: chọn hệ số chia nhỏ nhất thỏa điều kiện */
static uint8_t prv_adc_clock(void)
{
    static const uint32_t divider[4] = {2U, 4U, 6U, 8U};
    static const uint32_t rccDiv[4] = {RCC_PCLK2_Div2, RCC_PCLK2_Div4, RCC_PCLK2_Div6, RCC_PCLK2_Div8};
    RCC_ClocksTypeDef clocks;
    uint8_t i;

    RCC_GetClocksFreq(&clocks);

    for (i = 0U; i < 4U; i++)
    {
        if ((clocks.PCLK2_Frequency / divider[i]) <= 14000000UL)
        {
            RCC_ADCCLKConfig(rccDiv[i]);
            return 1U;
        }
    }
    return 0U;
}

static uint8_t prv_is_ready(Driver_ADC_Port_t port, uint8_t channel)
{
    return (port < drvAdcCount &&
            channel <= ADC_MAX_CHANNEL &&
            s_state[port].Initialized != 0U &&
            (s_state[port].ChannelMask & DRIVER_ADC_CHANNEL_BIT(channel)) != 0UL)
               ? 1U
               : 0U;
}

/* Cấu hình cơ bản: độc lập, 1 kênh, chuyển đổi đơn, kích hoạt bằng phần mềm */
static void prv_config_single(ADC_TypeDef *adc)
{
    ADC_InitTypeDef init;

    init.ADC_Mode = ADC_Mode_Independent;
    init.ADC_ScanConvMode = DISABLE;
    init.ADC_ContinuousConvMode = DISABLE;
    init.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    init.ADC_DataAlign = ADC_DataAlign_Right;
    init.ADC_NbrOfChannel = 1U;
    ADC_Init(adc, &init);
}

static Driver_ADC_Status_t prv_calibrate(ADC_TypeDef *adc)
{
    uint32_t timeout;
    volatile uint32_t settle;

    /* Cần >= 2 chu kỳ xung ADC sau khi bật ADON trước khi hiệu chuẩn */
    for (settle = 0U; settle < 1000U; settle++)
    {
    }

    ADC_ResetCalibration(adc);
    timeout = DRIVER_ADC_TIMEOUT_LOOPS;
    while (ADC_GetResetCalibrationStatus(adc) == SET)
    {
        if (timeout-- == 0UL)
        {
            return drvAdcTimeout;
        }
    }

    ADC_StartCalibration(adc);
    timeout = DRIVER_ADC_TIMEOUT_LOOPS;
    while (ADC_GetCalibrationStatus(adc) == SET)
    {
        if (timeout-- == 0UL)
        {
            return drvAdcTimeout;
        }
    }

    return drvAdcOk;
}

/* Đưa ADC từ chế độ quét liên tục về chế độ đọc đơn lẻ */
static void prv_restore_single(Driver_ADC_Port_t port)
{
    ADC_TypeDef *adc = s_hw[port].ADCx;
    uint32_t timeout = DRIVER_ADC_TIMEOUT_LOOPS;

    if (s_state[port].ScanActive == 0U)
    {
        return;
    }

    ADC_DMACmd(adc, DISABLE);
    adc->CR2 &= ~ADC_CR2_CONT; // Dừng sau khi chuỗi đang quét kết thúc

    /* Đợi chuỗi quét dở dang kết thúc (cờ EOC), không có chuỗi nào dở thì cờ EOC đã set sẵn */
    while (ADC_GetFlagStatus(adc, ADC_FLAG_EOC) == RESET)
    {
        if (timeout-- == 0UL)
        {
            break;
        }
    }

    (void)ADC_GetConversionValue(adc); // Đọc DR để xóa dữ liệu cũ
    ADC_ClearFlag(adc, ADC_FLAG_EOC | ADC_FLAG_STRT);

    prv_config_single(adc);
    s_state[port].ScanActive = 0U;
}

/* Callback của DMA (chạy trong ngắt DMA) */
static void prv_dma_cb(Driver_DMA_Channel_t channel, Driver_DMA_Event_t event, void *arg)
{
    Driver_ADC_Port_t port = (Driver_ADC_Port_t)(uintptr_t)arg;
    ADC_TypeDef *adc = s_hw[port].ADCx;
    Driver_ADC_DmaCallback_t cb = s_state[port].DmaCallback;
    Driver_ADC_DmaEvent_t ev;

    if (event == drvDmaEventHalfTransfer)
    {
        ev = drvAdcDmaEventHalf;
    }
    else
    {
        if (event == drvDmaEventTransferComplete)
        {
            ev = drvAdcDmaEventComplete;
        }
        else
        {
            ev = drvAdcDmaEventError;
        }

        /* Chế độ Once xong, hoặc có lỗi: dừng quét. Cấu hình đơn lẻ sẽ được khôi phục
           ở lần Read kế tiếp (hoặc StopDma) để không phải chờ trong ngắt */
        if (s_state[port].DmaCircular == 0U || ev == drvAdcDmaEventError)
        {
            ADC_DMACmd(adc, DISABLE);
            adc->CR2 &= ~ADC_CR2_CONT;
            (void)Driver_DMA_Stop(channel);
            s_state[port].DmaRunning = 0U;
        }
    }

    if (cb != 0)
    {
        cb(port, ev, s_state[port].DmaArg);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

Driver_ADC_Status_t Driver_ADC_Init(const Driver_ADC_Config_t *ADC_Config)
{
    const Driver_ADC_Hw_t *hw;
    Driver_GPIO_Config_t pin;
    Driver_ADC_Status_t st;
    uint8_t ch;

    if (ADC_Config == 0 ||
        ADC_Config->ADC_Port >= drvAdcCount ||
        ADC_Config->ADC_ChannelMask == 0UL ||
        (ADC_Config->ADC_ChannelMask & ~ADC_VALID_MASK) != 0UL ||
        ADC_Config->ADC_SampleTime > drvAdcSampleTime239p5)
    {
        return drvAdcError;
    }

    /* Cảm biến nhiệt độ và Vrefint chỉ có trên ADC1 */
    if (ADC_Config->ADC_Port != drvAdc1 &&
        (ADC_Config->ADC_ChannelMask &
         (DRIVER_ADC_CHANNEL_BIT(DRIVER_ADC_CHANNEL_TEMP) | DRIVER_ADC_CHANNEL_BIT(DRIVER_ADC_CHANNEL_VREF))) != 0UL)
    {
        return drvAdcError;
    }

    hw = &s_hw[ADC_Config->ADC_Port];

    /* Init lại khi đang quét DMA: dừng và trả kênh DMA trước */
    if (s_state[ADC_Config->ADC_Port].Initialized != 0U)
    {
        (void)Driver_ADC_StopDma(ADC_Config->ADC_Port);
    }
    s_state[ADC_Config->ADC_Port].Initialized = 0U;
    s_state[ADC_Config->ADC_Port].ScanActive = 0U;

    /* 1. Clock ADC */
    RCC_APB2PeriphClockCmd(hw->RccPeriph, ENABLE);
    if (prv_adc_clock() == 0U)
    {
        return drvAdcError;
    }

    /* 2. Các chân của kênh được dùng chuyển sang analog */
    for (ch = 0U; ch < ADC_GPIO_CHANNEL_COUNT; ch++)
    {
        if ((ADC_Config->ADC_ChannelMask & DRIVER_ADC_CHANNEL_BIT(ch)) != 0UL)
        {
            pin.GPIOx = s_channelPort[ch];
            pin.GPIO_Pin = s_channelPin[ch];
            pin.GPIO_Mode = drvGpioModeAnalog;
            pin.GPIO_Pull = drvGpioPullNone;
            pin.GPIO_Speed = drvGpioSpeedLow;

            if (Driver_GPIO_Init(&pin) != drvGpioOk)
            {
                return drvAdcError;
            }
        }
    }

    /* 3. Cấu hình ADC */
    ADC_DeInit(hw->ADCx);
    prv_config_single(hw->ADCx);

    if ((ADC_Config->ADC_ChannelMask &
         (DRIVER_ADC_CHANNEL_BIT(DRIVER_ADC_CHANNEL_TEMP) | DRIVER_ADC_CHANNEL_BIT(DRIVER_ADC_CHANNEL_VREF))) != 0UL)
    {
        ADC_TempSensorVrefintCmd(ENABLE);
    }

    /* 4. Bật ADC và hiệu chuẩn */
    ADC_Cmd(hw->ADCx, ENABLE);

    st = prv_calibrate(hw->ADCx);
    if (st != drvAdcOk)
    {
        return st;
    }

    s_state[ADC_Config->ADC_Port].ChannelMask = ADC_Config->ADC_ChannelMask;
    s_state[ADC_Config->ADC_Port].SampleTime = s_sampleTime[ADC_Config->ADC_SampleTime];
    s_state[ADC_Config->ADC_Port].Initialized = 1U;

    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_DeInit(Driver_ADC_Port_t ADC_Port)
{
    const Driver_ADC_Hw_t *hw;
    uint8_t ch;

    if (ADC_Port >= drvAdcCount)
    {
        return drvAdcError;
    }

    hw = &s_hw[ADC_Port];

    if (s_state[ADC_Port].Initialized != 0U)
    {
        (void)Driver_ADC_StopDma(ADC_Port);
    }

    ADC_Cmd(hw->ADCx, DISABLE);

    if (ADC_Port == drvAdc1)
    {
        ADC_TempSensorVrefintCmd(DISABLE);
    }

    ADC_DeInit(hw->ADCx);
    RCC_APB2PeriphClockCmd(hw->RccPeriph, DISABLE);

    for (ch = 0U; ch < ADC_GPIO_CHANNEL_COUNT; ch++)
    {
        if ((s_state[ADC_Port].ChannelMask & DRIVER_ADC_CHANNEL_BIT(ch)) != 0UL)
        {
            (void)Driver_GPIO_DeInit(s_channelPort[ch], s_channelPin[ch]);
        }
    }

    s_state[ADC_Port].ChannelMask = 0UL;
    s_state[ADC_Port].Initialized = 0U;

    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_Read(Driver_ADC_Port_t ADC_Port, uint8_t Channel, uint16_t *Value)
{
    ADC_TypeDef *adc;
    uint32_t timeout = DRIVER_ADC_TIMEOUT_LOOPS;

    if (Value == 0 || prv_is_ready(ADC_Port, Channel) == 0U)
    {
        return drvAdcError;
    }

    if (s_state[ADC_Port].DmaRunning != 0U)
    {
        return drvAdcBusy;
    }

    adc = s_hw[ADC_Port].ADCx;

    /* Vừa quét DMA xong (chế độ Once): đưa ADC về cấu hình đọc đơn lẻ */
    prv_restore_single(ADC_Port);

    ADC_RegularChannelConfig(adc, Channel, 1U, s_state[ADC_Port].SampleTime);
    ADC_SoftwareStartConvCmd(adc, ENABLE);

    while (ADC_GetFlagStatus(adc, ADC_FLAG_EOC) == RESET)
    {
        if (timeout-- == 0UL)
        {
            return drvAdcTimeout;
        }
    }

    *Value = ADC_GetConversionValue(adc); // Đọc DR cũng xóa cờ EOC
    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_ReadAverage(Driver_ADC_Port_t ADC_Port, uint8_t Channel, uint16_t Samples, uint16_t *Value)
{
    uint32_t sum = 0UL;
    uint16_t raw;
    uint16_t i;
    Driver_ADC_Status_t st;

    if (Value == 0 || Samples == 0U)
    {
        return drvAdcError;
    }

    for (i = 0U; i < Samples; i++)
    {
        st = Driver_ADC_Read(ADC_Port, Channel, &raw);
        if (st != drvAdcOk)
        {
            return st;
        }
        sum += raw;
    }

    *Value = (uint16_t)((sum + (Samples / 2U)) / Samples); // Làm tròn
    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_ReadMillivolt(Driver_ADC_Port_t ADC_Port, uint8_t Channel, uint32_t VrefMillivolt, uint32_t *Millivolt)
{
    uint16_t raw;
    Driver_ADC_Status_t st;

    if (Millivolt == 0 || VrefMillivolt == 0UL)
    {
        return drvAdcError;
    }

    st = Driver_ADC_Read(ADC_Port, Channel, &raw);
    if (st != drvAdcOk)
    {
        return st;
    }

    *Millivolt = (((uint32_t)raw * VrefMillivolt) + 2047UL) / 4095UL;
    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_StartDma(const Driver_ADC_DmaConfig_t *ADC_DmaConfig)
{
    ADC_TypeDef *adc;
    ADC_InitTypeDef init;
    Driver_DMA_Config_t dmaCfg;
    Driver_ADC_Port_t port;
    uint8_t i;

    if (ADC_DmaConfig == 0 ||
        ADC_DmaConfig->ADC_Port != drvAdc1 || // Chỉ ADC1 có DMA
        ADC_DmaConfig->ADC_Channels == 0 ||
        ADC_DmaConfig->ADC_ChannelCount == 0U ||
        ADC_DmaConfig->ADC_ChannelCount > DRIVER_ADC_MAX_SCAN_CHANNELS ||
        ADC_DmaConfig->ADC_Buffer == 0 ||
        ADC_DmaConfig->ADC_BufferLength == 0U ||
        (ADC_DmaConfig->ADC_BufferLength % ADC_DmaConfig->ADC_ChannelCount) != 0U ||
        ADC_DmaConfig->ADC_DmaMode > drvAdcDmaCircular)
    {
        return drvAdcError;
    }

    port = ADC_DmaConfig->ADC_Port;

    if (s_state[port].Initialized == 0U)
    {
        return drvAdcError;
    }
    if (s_state[port].DmaRunning != 0U)
    {
        return drvAdcBusy;
    }

    /* Mọi kênh phải đã được đăng ký (và cấu hình chân) lúc Init */
    for (i = 0U; i < ADC_DmaConfig->ADC_ChannelCount; i++)
    {
        if (ADC_DmaConfig->ADC_Channels[i] > ADC_MAX_CHANNEL ||
            (s_state[port].ChannelMask & DRIVER_ADC_CHANNEL_BIT(ADC_DmaConfig->ADC_Channels[i])) == 0UL)
        {
            return drvAdcError;
        }
    }

    adc = s_hw[port].ADCx;

    /* Lần quét Once trước đã xong: đưa ADC về trạng thái đọc đơn lẻ và trả kênh DMA mà driver đang giữ */
    prv_restore_single(port);
    if (s_state[port].DmaOwned != 0U)
    {
        (void)Driver_DMA_DeInit(ADC_DMA_CHANNEL);
        s_state[port].DmaOwned = 0U;
    }

    if (Driver_DMA_IsInUse(ADC_DMA_CHANNEL) != 0U)
    {
        return drvAdcError; // Kênh DMA 1 đang bị driver khác chiếm
    }

    /* 1. Cấu hình DMA: 16 bit từ thanh ghi DR của ADC vào bộ đệm */
    dmaCfg.DMA_Channel = ADC_DMA_CHANNEL;
    dmaCfg.DMA_Direction = drvDmaPeriphToMemory;
    dmaCfg.DMA_Mode = (ADC_DmaConfig->ADC_DmaMode == drvAdcDmaCircular) ? drvDmaModeCircular : drvDmaModeNormal;
    dmaCfg.DMA_PeripheralWidth = drvDmaWidth16b;
    dmaCfg.DMA_PeripheralIncrement = drvDmaIncDisable;
    dmaCfg.DMA_MemoryWidth = drvDmaWidth16b;
    dmaCfg.DMA_MemoryIncrement = drvDmaIncEnable;
    dmaCfg.DMA_Priority = drvDmaPriorityHigh;
    dmaCfg.DMA_IrqPriority = ADC_DmaConfig->ADC_DmaIrqPriority;

    if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
    {
        return drvAdcError;
    }
    s_state[port].DmaOwned = 1U;

    if (Driver_DMA_SetCallback(ADC_DMA_CHANNEL, prv_dma_cb, (void *)(uintptr_t)port,
                               (uint8_t)(drvDmaEventHalfTransfer | drvDmaEventTransferComplete | drvDmaEventTransferError)) != drvDmaOk)
    {
        (void)Driver_DMA_DeInit(ADC_DMA_CHANNEL);
        s_state[port].DmaOwned = 0U;
        return drvAdcError;
    }

    /* 2. Cấu hình ADC: quét liên tục các kênh theo thứ tự đã cho */
    init.ADC_Mode = ADC_Mode_Independent;
    init.ADC_ScanConvMode = (ADC_DmaConfig->ADC_ChannelCount > 1U) ? ENABLE : DISABLE;
    init.ADC_ContinuousConvMode = ENABLE;
    init.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    init.ADC_DataAlign = ADC_DataAlign_Right;
    init.ADC_NbrOfChannel = ADC_DmaConfig->ADC_ChannelCount;
    ADC_Init(adc, &init);
    s_state[port].ScanActive = 1U;

    for (i = 0U; i < ADC_DmaConfig->ADC_ChannelCount; i++)
    {
        ADC_RegularChannelConfig(adc, ADC_DmaConfig->ADC_Channels[i], (uint8_t)(i + 1U), s_state[port].SampleTime);
    }

    /* 3. Chạy: khởi động DMA rồi bật yêu cầu DMA của ADC và bắt đầu chuyển đổi */
    s_state[port].DmaCircular = (ADC_DmaConfig->ADC_DmaMode == drvAdcDmaCircular) ? 1U : 0U;
    s_state[port].DmaRunning = 1U;

    if (Driver_DMA_Start(ADC_DMA_CHANNEL, (uint32_t)&adc->DR, (uint32_t)ADC_DmaConfig->ADC_Buffer,
                         ADC_DmaConfig->ADC_BufferLength) != drvDmaOk)
    {
        s_state[port].DmaRunning = 0U;
        prv_restore_single(port);
        (void)Driver_DMA_DeInit(ADC_DMA_CHANNEL);
        s_state[port].DmaOwned = 0U;
        return drvAdcError;
    }

    ADC_DMACmd(adc, ENABLE);
    ADC_SoftwareStartConvCmd(adc, ENABLE);

    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_StopDma(Driver_ADC_Port_t ADC_Port)
{
    if (ADC_Port >= drvAdcCount || s_state[ADC_Port].Initialized == 0U)
    {
        return drvAdcError;
    }

    if (s_state[ADC_Port].DmaOwned != 0U)
    {
        (void)Driver_DMA_Stop(ADC_DMA_CHANNEL);
    }
    s_state[ADC_Port].DmaRunning = 0U;

    prv_restore_single(ADC_Port);

    if (s_state[ADC_Port].DmaOwned != 0U)
    {
        (void)Driver_DMA_DeInit(ADC_DMA_CHANNEL);
        s_state[ADC_Port].DmaOwned = 0U;
    }

    return drvAdcOk;
}

Driver_ADC_Status_t Driver_ADC_SetDmaCallback(Driver_ADC_Port_t ADC_Port, Driver_ADC_DmaCallback_t Callback, void *Arg)
{
    if (ADC_Port >= drvAdcCount)
    {
        return drvAdcError;
    }

    s_state[ADC_Port].DmaCallback = 0; // Gỡ callback cũ trước để ISR không dùng cặp (Callback, Arg) dở dang
    s_state[ADC_Port].DmaArg = Arg;
    s_state[ADC_Port].DmaCallback = Callback;

    return drvAdcOk;
}

uint8_t Driver_ADC_IsDmaRunning(Driver_ADC_Port_t ADC_Port)
{
    return (ADC_Port < drvAdcCount) ? s_state[ADC_Port].DmaRunning : 0U;
}