#include "Driver_DMA.h"
#include "misc.h"

/* Các cờ trạng thái của mỗi kênh trong DMA1->ISR / IFCR: 4 bit liên tiếp cho mỗi kênh */
#define PRV_GRP_GIF 0x1U
#define PRV_GRP_TCIF 0x2U
#define PRV_GRP_HTIF 0x4U
#define PRV_GRP_TEIF 0x8U
#define PRV_GRP_ALL 0xFU

#define PRV_EVENT_VALID_MASK ((uint8_t)(drvDmaEventHalfTransfer | drvDmaEventTransferComplete | drvDmaEventTransferError))

/* ------------------------------------------------------------------ */
/* Bảng ánh xạ phần cứng cho từng kênh                                */
/* ------------------------------------------------------------------ */
typedef struct
{
    DMA_Channel_TypeDef *Channel;
    IRQn_Type Irq;
} Driver_DMA_Hw_t;

static const Driver_DMA_Hw_t s_hw[drvDmaChannelCount] = {
    {DMA1_Channel1, DMA1_Channel1_IRQn},
    {DMA1_Channel2, DMA1_Channel2_IRQn},
    {DMA1_Channel3, DMA1_Channel3_IRQn},
    {DMA1_Channel4, DMA1_Channel4_IRQn},
    {DMA1_Channel5, DMA1_Channel5_IRQn},
    {DMA1_Channel6, DMA1_Channel6_IRQn},
    {DMA1_Channel7, DMA1_Channel7_IRQn},
};

typedef struct
{
    volatile Driver_DMA_Callback_t Callback;
    void *volatile Arg;
    volatile uint8_t EventMask;
    uint8_t PeripheralBytes; // Dùng để kiểm tra căn lề địa chỉ
    uint8_t MemoryBytes;
    uint8_t Initialized;
} Driver_DMA_State_t;

static Driver_DMA_State_t s_state[drvDmaChannelCount];
static uint8_t s_activeChannels; // Số kênh đang được khởi tạo, để quyết định lúc tắt clock DMA1

/* ------------------------------------------------------------------ */
/* Hàm nội bộ                                                         */
/* ------------------------------------------------------------------ */

static uint8_t prv_is_ready(Driver_DMA_Channel_t ch)
{
    return (ch < drvDmaChannelCount && s_state[ch].Initialized != 0U) ? 1U : 0U;
}

static uint8_t prv_width_bytes(Driver_DMA_Width_t width)
{
    switch (width)
    {
    case drvDmaWidth16b:
        return 2U;
    case drvDmaWidth32b:
        return 4U;
    default:
        return 1U;
    }
}

static uint32_t prv_periph_size(Driver_DMA_Width_t width)
{
    switch (width)
    {
    case drvDmaWidth16b:
        return DMA_PeripheralDataSize_HalfWord;
    case drvDmaWidth32b:
        return DMA_PeripheralDataSize_Word;
    default:
        return DMA_PeripheralDataSize_Byte;
    }
}

static uint32_t prv_memory_size(Driver_DMA_Width_t width)
{
    switch (width)
    {
    case drvDmaWidth16b:
        return DMA_MemoryDataSize_HalfWord;
    case drvDmaWidth32b:
        return DMA_MemoryDataSize_Word;
    default:
        return DMA_MemoryDataSize_Byte;
    }
}

static uint32_t prv_priority(Driver_DMA_Priority_t priority)
{
    switch (priority)
    {
    case drvDmaPriorityMedium:
        return DMA_Priority_Medium;
    case drvDmaPriorityHigh:
        return DMA_Priority_High;
    case drvDmaPriorityVeryHigh:
        return DMA_Priority_VeryHigh;
    default:
        return DMA_Priority_Low;
    }
}

static uint8_t prv_get_flags(Driver_DMA_Channel_t ch)
{
    return (uint8_t)((DMA1->ISR >> (4U * (uint32_t)ch)) & PRV_GRP_ALL);
}

static void prv_clear_flags(Driver_DMA_Channel_t ch, uint8_t flags)
{
    DMA1->IFCR = ((uint32_t)flags) << (4U * (uint32_t)ch);
}

static void prv_irq(Driver_DMA_Channel_t ch)
{
    uint8_t flags = prv_get_flags(ch);
    uint8_t mask = s_state[ch].EventMask;
    Driver_DMA_Callback_t cb = s_state[ch].Callback;
    void *arg = s_state[ch].Arg;

    /* Chỉ xử lý sự kiện đã đăng ký, các cờ còn lại để Driver_DMA_WaitComplete dùng */
    if ((flags & PRV_GRP_TEIF) != 0U && (mask & drvDmaEventTransferError) != 0U)
    {
        prv_clear_flags(ch, PRV_GRP_TEIF);
        if (cb != 0)
        {
            cb(ch, drvDmaEventTransferError, arg);
        }
    }

    if ((flags & PRV_GRP_HTIF) != 0U && (mask & drvDmaEventHalfTransfer) != 0U)
    {
        prv_clear_flags(ch, PRV_GRP_HTIF);
        if (cb != 0)
        {
            cb(ch, drvDmaEventHalfTransfer, arg);
        }
    }

    if ((flags & PRV_GRP_TCIF) != 0U && (mask & drvDmaEventTransferComplete) != 0U)
    {
        prv_clear_flags(ch, PRV_GRP_TCIF);
        if (cb != 0)
        {
            cb(ch, drvDmaEventTransferComplete, arg);
        }
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

Driver_DMA_Status_t Driver_DMA_Init(const Driver_DMA_Config_t *DMA_Config)
{
    const Driver_DMA_Hw_t *hw;
    Driver_DMA_State_t *state;
    DMA_InitTypeDef init;
    NVIC_InitTypeDef nvicInit;

    if (DMA_Config == 0 ||
        DMA_Config->DMA_Channel >= drvDmaChannelCount ||
        DMA_Config->DMA_Direction > drvDmaMemoryToMemory ||
        DMA_Config->DMA_Mode > drvDmaModeCircular ||
        DMA_Config->DMA_PeripheralWidth > drvDmaWidth32b ||
        DMA_Config->DMA_MemoryWidth > drvDmaWidth32b ||
        DMA_Config->DMA_Priority > drvDmaPriorityVeryHigh)
    {
        return drvDmaError;
    }

    /* Memory-to-memory không hỗ trợ chế độ vòng */
    if (DMA_Config->DMA_Direction == drvDmaMemoryToMemory &&
        DMA_Config->DMA_Mode == drvDmaModeCircular)
    {
        return drvDmaError;
    }

    hw = &s_hw[DMA_Config->DMA_Channel];
    state = &s_state[DMA_Config->DMA_Channel];

    /* 1. Bật clock DMA1 */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    if (state->Initialized == 0U)
    {
        s_activeChannels++;
    }
    state->Initialized = 0U;

    /* 2. Đưa kênh về trạng thái ban đầu rồi cấu hình */
    DMA_DeInit(hw->Channel);

    init.DMA_PeripheralBaseAddr = 0U; // Đặt khi gọi Driver_DMA_Start
    init.DMA_MemoryBaseAddr = 0U;
    init.DMA_DIR = (DMA_Config->DMA_Direction == drvDmaMemoryToPeriph) ? DMA_DIR_PeripheralDST : DMA_DIR_PeripheralSRC;
    init.DMA_BufferSize = 0U;
    init.DMA_PeripheralInc = (DMA_Config->DMA_PeripheralIncrement == drvDmaIncEnable) ? DMA_PeripheralInc_Enable : DMA_PeripheralInc_Disable;
    init.DMA_MemoryInc = (DMA_Config->DMA_MemoryIncrement == drvDmaIncEnable) ? DMA_MemoryInc_Enable : DMA_MemoryInc_Disable;
    init.DMA_PeripheralDataSize = prv_periph_size(DMA_Config->DMA_PeripheralWidth);
    init.DMA_MemoryDataSize = prv_memory_size(DMA_Config->DMA_MemoryWidth);
    init.DMA_Mode = (DMA_Config->DMA_Mode == drvDmaModeCircular) ? DMA_Mode_Circular : DMA_Mode_Normal;
    init.DMA_Priority = prv_priority(DMA_Config->DMA_Priority);
    init.DMA_M2M = (DMA_Config->DMA_Direction == drvDmaMemoryToMemory) ? DMA_M2M_Enable : DMA_M2M_Disable;
    DMA_Init(hw->Channel, &init);

    /* 3. NVIC (ngắt DMA chỉ được bật khi đăng ký callback) */
    nvicInit.NVIC_IRQChannel = hw->Irq;
    nvicInit.NVIC_IRQChannelPreemptionPriority = DMA_Config->DMA_IrqPriority;
    nvicInit.NVIC_IRQChannelSubPriority = 0U;
    nvicInit.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvicInit);

    state->Callback = 0;
    state->Arg = 0;
    state->EventMask = 0U;
    state->PeripheralBytes = prv_width_bytes(DMA_Config->DMA_PeripheralWidth);
    state->MemoryBytes = prv_width_bytes(DMA_Config->DMA_MemoryWidth);
    state->Initialized = 1U;

    return drvDmaOk;
}

Driver_DMA_Status_t Driver_DMA_DeInit(Driver_DMA_Channel_t DMA_Channel)
{
    const Driver_DMA_Hw_t *hw;
    NVIC_InitTypeDef nvicInit;

    if (prv_is_ready(DMA_Channel) == 0U)
    {
        return drvDmaError;
    }

    hw = &s_hw[DMA_Channel];

    DMA_Cmd(hw->Channel, DISABLE);
    DMA_ITConfig(hw->Channel, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE, DISABLE);

    nvicInit.NVIC_IRQChannel = hw->Irq;
    nvicInit.NVIC_IRQChannelPreemptionPriority = 0U;
    nvicInit.NVIC_IRQChannelSubPriority = 0U;
    nvicInit.NVIC_IRQChannelCmd = DISABLE;
    NVIC_Init(&nvicInit);

    DMA_DeInit(hw->Channel);

    s_state[DMA_Channel].Callback = 0;
    s_state[DMA_Channel].Arg = 0;
    s_state[DMA_Channel].EventMask = 0U;
    s_state[DMA_Channel].Initialized = 0U;

    if (s_activeChannels > 0U)
    {
        s_activeChannels--;
    }
    if (s_activeChannels == 0U)
    {
        RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, DISABLE);
    }

    return drvDmaOk;
}

Driver_DMA_Status_t Driver_DMA_Start(Driver_DMA_Channel_t DMA_Channel, uint32_t PeripheralAddress, uint32_t MemoryAddress, uint16_t Count)
{
    DMA_Channel_TypeDef *channel;

    if (prv_is_ready(DMA_Channel) == 0U ||
        Count == 0U ||
        PeripheralAddress == 0U ||
        MemoryAddress == 0U)
    {
        return drvDmaError;
    }

    /* Địa chỉ phải căn lề theo độ rộng dữ liệu */
    if ((PeripheralAddress % s_state[DMA_Channel].PeripheralBytes) != 0U ||
        (MemoryAddress % s_state[DMA_Channel].MemoryBytes) != 0U)
    {
        return drvDmaError;
    }

    channel = s_hw[DMA_Channel].Channel;

    /* CPAR, CMAR, CNDTR chỉ ghi được khi kênh đang tắt */
    DMA_Cmd(channel, DISABLE);

    channel->CPAR = PeripheralAddress;
    channel->CMAR = MemoryAddress;
    DMA_SetCurrDataCounter(channel, Count);

    /* Xóa cờ cũ (kể cả TEIF, nếu không kênh không thể bật lại sau lỗi) */
    prv_clear_flags(DMA_Channel, PRV_GRP_ALL);

    DMA_Cmd(channel, ENABLE);
    return drvDmaOk;
}

Driver_DMA_Status_t Driver_DMA_Stop(Driver_DMA_Channel_t DMA_Channel)
{
    if (prv_is_ready(DMA_Channel) == 0U)
    {
        return drvDmaError;
    }

    DMA_Cmd(s_hw[DMA_Channel].Channel, DISABLE);
    return drvDmaOk;
}

Driver_DMA_Status_t Driver_DMA_SetCallback(Driver_DMA_Channel_t DMA_Channel, Driver_DMA_Callback_t Callback, void *Arg, uint8_t EventMask)
{
    DMA_Channel_TypeDef *channel;

    if (prv_is_ready(DMA_Channel) == 0U || (EventMask & (uint8_t)~PRV_EVENT_VALID_MASK) != 0U)
    {
        return drvDmaError;
    }

    channel = s_hw[DMA_Channel].Channel;

    /* Tắt ngắt trước khi đổi callback để ISR không đọc cặp (Callback, Arg) dở dang */
    DMA_ITConfig(channel, DMA_IT_TC | DMA_IT_HT | DMA_IT_TE, DISABLE);

    s_state[DMA_Channel].Callback = Callback;
    s_state[DMA_Channel].Arg = Arg;
    s_state[DMA_Channel].EventMask = (Callback != 0) ? EventMask : 0U;

    if (Callback != 0)
    {
        if ((EventMask & drvDmaEventHalfTransfer) != 0U)
        {
            DMA_ITConfig(channel, DMA_IT_HT, ENABLE);
        }
        if ((EventMask & drvDmaEventTransferComplete) != 0U)
        {
            DMA_ITConfig(channel, DMA_IT_TC, ENABLE);
        }
        if ((EventMask & drvDmaEventTransferError) != 0U)
        {
            DMA_ITConfig(channel, DMA_IT_TE, ENABLE);
        }
    }

    return drvDmaOk;
}

Driver_DMA_Status_t Driver_DMA_WaitComplete(Driver_DMA_Channel_t DMA_Channel)
{
    uint32_t timeout = DRIVER_DMA_TIMEOUT_LOOPS;
    uint8_t flags;

    if (prv_is_ready(DMA_Channel) == 0U)
    {
        return drvDmaError;
    }

    for (;;)
    {
        flags = prv_get_flags(DMA_Channel);

        if ((flags & PRV_GRP_TEIF) != 0U)
        {
            prv_clear_flags(DMA_Channel, PRV_GRP_ALL);
            return drvDmaTransferError;
        }
        if ((flags & PRV_GRP_TCIF) != 0U)
        {
            prv_clear_flags(DMA_Channel, (uint8_t)(PRV_GRP_TCIF | PRV_GRP_HTIF | PRV_GRP_GIF));
            return drvDmaOk;
        }
        if (timeout-- == 0UL)
        {
            return drvDmaTimeout;
        }
    }
}

uint8_t Driver_DMA_IsBusy(Driver_DMA_Channel_t DMA_Channel)
{
    DMA_Channel_TypeDef *channel;

    if (prv_is_ready(DMA_Channel) == 0U)
    {
        return 0U;
    }

    channel = s_hw[DMA_Channel].Channel;
    return (((channel->CCR & DMA_CCR1_EN) != 0U) && (channel->CNDTR != 0U)) ? 1U : 0U;
}

uint16_t Driver_DMA_GetRemaining(Driver_DMA_Channel_t DMA_Channel)
{
    if (prv_is_ready(DMA_Channel) == 0U)
    {
        return 0U;
    }

    return DMA_GetCurrDataCounter(s_hw[DMA_Channel].Channel);
}

uint8_t Driver_DMA_IsInUse(Driver_DMA_Channel_t DMA_Channel)
{
    if (DMA_Channel >= drvDmaChannelCount)
    {
        return 1U; // Kênh không hợp lệ coi như không dùng được
    }

    return s_state[DMA_Channel].Initialized;
}

Driver_DMA_Status_t Driver_DMA_SetMemoryIncrement(Driver_DMA_Channel_t DMA_Channel, Driver_DMA_Increment_t Increment)
{
    DMA_Channel_TypeDef *channel;

    if (prv_is_ready(DMA_Channel) == 0U || Increment > drvDmaIncEnable)
    {
        return drvDmaError;
    }

    if (Driver_DMA_IsBusy(DMA_Channel) != 0U)
    {
        return drvDmaError; // Không đổi cấu hình khi đang truyền
    }

    channel = s_hw[DMA_Channel].Channel;

    DMA_Cmd(channel, DISABLE); // Thanh ghi CCR chỉ sửa được khi kênh tắt

    if (Increment == drvDmaIncEnable)
    {
        channel->CCR |= DMA_CCR1_MINC;
    }
    else
    {
        channel->CCR &= (uint32_t)~DMA_CCR1_MINC;
    }

    return drvDmaOk;
}

/* ------------------------------------------------------------------ */
/* Trình xử lý ngắt (không định nghĩa lại trong stm32f10x_it.c)        */
/* ------------------------------------------------------------------ */

void DMA1_Channel1_IRQHandler(void)
{
    prv_irq(drvDmaChannel1);
}

void DMA1_Channel2_IRQHandler(void)
{
    prv_irq(drvDmaChannel2);
}

void DMA1_Channel3_IRQHandler(void)
{
    prv_irq(drvDmaChannel3);
}

void DMA1_Channel4_IRQHandler(void)
{
    prv_irq(drvDmaChannel4);
}

void DMA1_Channel5_IRQHandler(void)
{
    prv_irq(drvDmaChannel5);
}

void DMA1_Channel6_IRQHandler(void)
{
    prv_irq(drvDmaChannel6);
}

void DMA1_Channel7_IRQHandler(void)
{
    prv_irq(drvDmaChannel7);
}