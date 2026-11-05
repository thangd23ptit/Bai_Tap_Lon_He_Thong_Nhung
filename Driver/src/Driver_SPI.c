#include "Driver_SPI.h"
#include "Driver_GPIO.h"
#include "Driver_DMA.h"

/* ------------------------------------------------------------------ */
/* Bảng ánh xạ phần cứng cho từng cổng                                */
/* ------------------------------------------------------------------ */
typedef struct
{
    SPI_TypeDef *SPIx;
    uint32_t RccPeriph;
    uint8_t OnApb2; // 1: SPI nằm trên APB2, 0: nằm trên APB1
    GPIO_TypeDef *Port;
    uint16_t SckPin;
    uint16_t MisoPin;
    uint16_t MosiPin;
    Driver_DMA_Channel_t RxDmaChannel;
    Driver_DMA_Channel_t TxDmaChannel;
} Driver_SPI_Hw_t;

static const Driver_SPI_Hw_t s_hw[drvSpiPortCount] = {
    {SPI1, RCC_APB2Periph_SPI1, 1U, GPIOA, GPIO_Pin_5, GPIO_Pin_6, GPIO_Pin_7, drvDmaChannel2, drvDmaChannel3},
    {SPI2, RCC_APB1Periph_SPI2, 0U, GPIOB, GPIO_Pin_13, GPIO_Pin_14, GPIO_Pin_15, drvDmaChannel4, drvDmaChannel5},
};

/* Hệ số chia xung nhịp và giá trị thanh ghi tương ứng, xếp theo thứ tự tăng dần */
typedef struct
{
    uint16_t Divider;
    uint16_t RegValue;
} Driver_SPI_Prescaler_t;

static const Driver_SPI_Prescaler_t s_prescaler[8] = {
    {2U, SPI_BaudRatePrescaler_2},
    {4U, SPI_BaudRatePrescaler_4},
    {8U, SPI_BaudRatePrescaler_8},
    {16U, SPI_BaudRatePrescaler_16},
    {32U, SPI_BaudRatePrescaler_32},
    {64U, SPI_BaudRatePrescaler_64},
    {128U, SPI_BaudRatePrescaler_128},
    {256U, SPI_BaudRatePrescaler_256},
};

static uint8_t s_initialized[drvSpiPortCount];

/* Trạng thái DMA của từng cổng */
typedef struct
{
    uint8_t UseDma; // 1: đã cấp 2 kênh DMA
    volatile uint8_t Busy;
    volatile Driver_SPI_Status_t LastStatus;
    volatile Driver_SPI_Callback_t Callback;
    void *volatile Arg;
} Driver_SPI_Dma_t;

static Driver_SPI_Dma_t s_dma[drvSpiPortCount];

/* Dữ liệu giả: nguồn gửi 0xFF khi không có TxData, nơi bỏ dữ liệu khi không có RxData */
static const uint8_t s_dummyTx = 0xFFU;
static volatile uint8_t s_dummyRx;

/* ------------------------------------------------------------------ */
/* Hàm nội bộ                                                         */
/* ------------------------------------------------------------------ */

static uint8_t prv_is_ready(Driver_SPI_Port_t port)
{
    return (port < drvSpiPortCount && s_initialized[port] != 0U) ? 1U : 0U;
}

static uint32_t prv_bus_clock(Driver_SPI_Port_t port)
{
    RCC_ClocksTypeDef clocks;

    RCC_GetClocksFreq(&clocks);
    return (s_hw[port].OnApb2 != 0U) ? clocks.PCLK2_Frequency : clocks.PCLK1_Frequency;
}

/* Chọn hệ số chia nhỏ nhất sao cho tốc độ thực tế <= baudRate */
static Driver_SPI_Status_t prv_calc_prescaler(Driver_SPI_Port_t port, uint32_t baudRate, uint16_t *regValue)
{
    uint32_t clk;
    uint8_t i;

    if (baudRate == 0U)
    {
        return drvSpiError;
    }

    clk = prv_bus_clock(port);

    for (i = 0U; i < 8U; i++)
    {
        if ((clk / s_prescaler[i].Divider) <= baudRate)
        {
            *regValue = s_prescaler[i].RegValue;
            return drvSpiOk;
        }
    }

    return drvSpiError; // Yêu cầu chậm hơn mức chậm nhất (PCLK / 256)
}

static Driver_SPI_Status_t prv_wait_flag(SPI_TypeDef *spi, uint16_t flag, FlagStatus until)
{
    uint32_t timeout = DRIVER_SPI_TIMEOUT_LOOPS;

    while (SPI_I2S_GetFlagStatus(spi, flag) != until)
    {
        if (timeout-- == 0UL)
        {
            return drvSpiTimeout;
        }
    }
    return drvSpiOk;
}

static Driver_SPI_Status_t prv_exchange(SPI_TypeDef *spi, uint8_t tx, uint8_t *rx)
{
    Driver_SPI_Status_t st;

    st = prv_wait_flag(spi, SPI_I2S_FLAG_TXE, SET);
    if (st != drvSpiOk)
    {
        return st;
    }
    SPI_I2S_SendData(spi, tx);

    st = prv_wait_flag(spi, SPI_I2S_FLAG_RXNE, SET);
    if (st != drvSpiOk)
    {
        return st;
    }

    if (rx != 0)
    {
        *rx = (uint8_t)SPI_I2S_ReceiveData(spi);
    }
    else
    {
        (void)SPI_I2S_ReceiveData(spi);
    }
    return drvSpiOk;
}

/* Bỏ dữ liệu nhận cũ còn sót trong DR (và xóa cờ OVR nếu có) */
static void prv_flush_rx(SPI_TypeDef *spi)
{
    if (SPI_I2S_GetFlagStatus(spi, SPI_I2S_FLAG_RXNE) != RESET)
    {
        (void)SPI_I2S_ReceiveData(spi);
    }
    (void)spi->SR;
}

/* Giải phóng 2 kênh DMA mà cổng này đang giữ */
static void prv_release_dma(Driver_SPI_Port_t port)
{
    if (s_dma[port].UseDma != 0U)
    {
        (void)Driver_DMA_DeInit(s_hw[port].RxDmaChannel);
        (void)Driver_DMA_DeInit(s_hw[port].TxDmaChannel);
        s_dma[port].UseDma = 0U;
    }
    s_dma[port].Busy = 0U;
}

/* Callback của DMA (chạy trong ngắt): kết thúc lần truyền khi RX xong hoặc có lỗi */
static void prv_dma_cb(Driver_DMA_Channel_t channel, Driver_DMA_Event_t event, void *arg)
{
    Driver_SPI_Port_t port = (Driver_SPI_Port_t)(uintptr_t)arg;
    Driver_SPI_Callback_t cb = s_dma[port].Callback;
    Driver_SPI_Status_t status;

    (void)channel;

    if (s_dma[port].Busy == 0U)
    {
        return;
    }

    status = (event == drvDmaEventTransferComplete) ? drvSpiOk : drvSpiError;

    (void)Driver_DMA_Stop(s_hw[port].TxDmaChannel);
    (void)Driver_DMA_Stop(s_hw[port].RxDmaChannel);

    s_dma[port].LastStatus = status;
    s_dma[port].Busy = 0U;

    if (cb != 0)
    {
        cb(port, status, s_dma[port].Arg);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

Driver_SPI_Status_t Driver_SPI_Init(const Driver_SPI_Config_t *SPI_Config)
{
    const Driver_SPI_Hw_t *hw;
    SPI_InitTypeDef init;
    Driver_GPIO_Config_t outPins;
    Driver_GPIO_Config_t misoPin;
    Driver_DMA_Config_t dmaCfg;
    uint16_t prescaler;
    uint8_t useDma;

    if (SPI_Config == 0 ||
        SPI_Config->SPI_Port >= drvSpiPortCount ||
        SPI_Config->SPI_DmaMode > drvSpiDmaEnable)
    {
        return drvSpiError;
    }

    if (prv_calc_prescaler(SPI_Config->SPI_Port, SPI_Config->SPI_BaudRate, &prescaler) != drvSpiOk)
    {
        return drvSpiError;
    }

    hw = &s_hw[SPI_Config->SPI_Port];
    useDma = (SPI_Config->SPI_DmaMode == drvSpiDmaEnable) ? 1U : 0U;

    s_initialized[SPI_Config->SPI_Port] = 0U;
    prv_release_dma(SPI_Config->SPI_Port); // Init lại: trả kênh DMA cũ trước

    /* Kênh DMA đang bị driver khác chiếm thì không dùng được */
    if (useDma != 0U &&
        (Driver_DMA_IsInUse(hw->RxDmaChannel) != 0U || Driver_DMA_IsInUse(hw->TxDmaChannel) != 0U))
    {
        return drvSpiError;
    }

    /* 1. Bật clock SPI */
    if (hw->OnApb2 != 0U)
    {
        RCC_APB2PeriphClockCmd(hw->RccPeriph, ENABLE);
    }
    else
    {
        RCC_APB1PeriphClockCmd(hw->RccPeriph, ENABLE);
    }

    /* 2. Chân: SCK, MOSI = AF push-pull; MISO = input pull-up */
    outPins.GPIOx = hw->Port;
    outPins.GPIO_Pin = (uint16_t)(hw->SckPin | hw->MosiPin);
    outPins.GPIO_Mode = drvGpioModeAFPP;
    outPins.GPIO_Pull = drvGpioPullNone;
    outPins.GPIO_Speed = drvGpioSpeedHigh;

    misoPin.GPIOx = hw->Port;
    misoPin.GPIO_Pin = hw->MisoPin;
    misoPin.GPIO_Mode = drvGpioModeInput;
    misoPin.GPIO_Pull = drvGpioPullUp;
    misoPin.GPIO_Speed = drvGpioSpeedLow;

    if (Driver_GPIO_Init(&outPins) != drvGpioOk || Driver_GPIO_Init(&misoPin) != drvGpioOk)
    {
        return drvSpiError;
    }

    /* 3. Cấu hình SPI master, NSS phần mềm */
    SPI_I2S_DeInit(hw->SPIx);

    init.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    init.SPI_Mode = SPI_Mode_Master;
    init.SPI_DataSize = SPI_DataSize_8b;
    init.SPI_NSS = SPI_NSS_Soft;
    init.SPI_BaudRatePrescaler = prescaler;
    init.SPI_FirstBit = (SPI_Config->SPI_BitOrder == drvSpiLsbFirst) ? SPI_FirstBit_LSB : SPI_FirstBit_MSB;
    init.SPI_CRCPolynomial = 7U;

    switch (SPI_Config->SPI_Mode)
    {
    case drvSpiMode1:
        init.SPI_CPOL = SPI_CPOL_Low;
        init.SPI_CPHA = SPI_CPHA_2Edge;
        break;
    case drvSpiMode2:
        init.SPI_CPOL = SPI_CPOL_High;
        init.SPI_CPHA = SPI_CPHA_1Edge;
        break;
    case drvSpiMode3:
        init.SPI_CPOL = SPI_CPOL_High;
        init.SPI_CPHA = SPI_CPHA_2Edge;
        break;
    default:
        init.SPI_CPOL = SPI_CPOL_Low;
        init.SPI_CPHA = SPI_CPHA_1Edge;
        break;
    }

    SPI_Init(hw->SPIx, &init);

    /* 4. DMA: RX ưu tiên cao hơn TX để không bị tràn dữ liệu nhận */
    if (useDma != 0U)
    {
        dmaCfg.DMA_Channel = hw->RxDmaChannel;
        dmaCfg.DMA_Direction = drvDmaPeriphToMemory;
        dmaCfg.DMA_Mode = drvDmaModeNormal;
        dmaCfg.DMA_PeripheralWidth = drvDmaWidth8b;
        dmaCfg.DMA_PeripheralIncrement = drvDmaIncDisable;
        dmaCfg.DMA_MemoryWidth = drvDmaWidth8b;
        dmaCfg.DMA_MemoryIncrement = drvDmaIncEnable;
        dmaCfg.DMA_Priority = drvDmaPriorityHigh;
        dmaCfg.DMA_IrqPriority = SPI_Config->SPI_DmaIrqPriority;

        if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
        {
            return drvSpiError;
        }

        dmaCfg.DMA_Channel = hw->TxDmaChannel;
        dmaCfg.DMA_Direction = drvDmaMemoryToPeriph;
        dmaCfg.DMA_Priority = drvDmaPriorityMedium;

        if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
        {
            (void)Driver_DMA_DeInit(hw->RxDmaChannel);
            return drvSpiError;
        }

        s_dma[SPI_Config->SPI_Port].UseDma = 1U;
        s_dma[SPI_Config->SPI_Port].LastStatus = drvSpiOk;

        if (Driver_DMA_SetCallback(hw->RxDmaChannel, prv_dma_cb,
                                   (void *)(uintptr_t)SPI_Config->SPI_Port,
                                   (uint8_t)(drvDmaEventTransferComplete | drvDmaEventTransferError)) != drvDmaOk ||
            Driver_DMA_SetCallback(hw->TxDmaChannel, prv_dma_cb,
                                   (void *)(uintptr_t)SPI_Config->SPI_Port,
                                   (uint8_t)drvDmaEventTransferError) != drvDmaOk)
        {
            prv_release_dma(SPI_Config->SPI_Port);
            return drvSpiError;
        }

        /* Yêu cầu DMA của SPI luôn bật, kênh DMA chỉ chạy khi gọi TransferDma */
        SPI_I2S_DMACmd(hw->SPIx, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, ENABLE);
    }

    SPI_Cmd(hw->SPIx, ENABLE);

    s_initialized[SPI_Config->SPI_Port] = 1U;
    return drvSpiOk;
}

Driver_SPI_Status_t Driver_SPI_DeInit(Driver_SPI_Port_t SPI_Port)
{
    const Driver_SPI_Hw_t *hw;

    if (SPI_Port >= drvSpiPortCount)
    {
        return drvSpiError;
    }

    hw = &s_hw[SPI_Port];
    s_initialized[SPI_Port] = 0U;

    prv_release_dma(SPI_Port);

    (void)prv_wait_flag(hw->SPIx, SPI_I2S_FLAG_BSY, RESET); // Đợi truyền xong rồi mới tắt
    SPI_Cmd(hw->SPIx, DISABLE);
    SPI_I2S_DeInit(hw->SPIx);

    if (hw->OnApb2 != 0U)
    {
        RCC_APB2PeriphClockCmd(hw->RccPeriph, DISABLE);
    }
    else
    {
        RCC_APB1PeriphClockCmd(hw->RccPeriph, DISABLE);
    }

    (void)Driver_GPIO_DeInit(hw->Port, (uint16_t)(hw->SckPin | hw->MisoPin | hw->MosiPin));

    return drvSpiOk;
}

Driver_SPI_Status_t Driver_SPI_SetBaudRate(Driver_SPI_Port_t SPI_Port, uint32_t BaudRate)
{
    SPI_TypeDef *spi;
    uint16_t prescaler;
    Driver_SPI_Status_t st;

    if (prv_is_ready(SPI_Port) == 0U)
    {
        return drvSpiError;
    }

    if (s_dma[SPI_Port].Busy != 0U)
    {
        return drvSpiBusy;
    }

    if (prv_calc_prescaler(SPI_Port, BaudRate, &prescaler) != drvSpiOk)
    {
        return drvSpiError;
    }

    spi = s_hw[SPI_Port].SPIx;

    st = prv_wait_flag(spi, SPI_I2S_FLAG_BSY, RESET); // Không đổi tốc độ khi đang truyền
    if (st != drvSpiOk)
    {
        return st;
    }

    SPI_Cmd(spi, DISABLE);
    spi->CR1 = (uint16_t)((spi->CR1 & (uint16_t)~SPI_CR1_BR) | prescaler);
    SPI_Cmd(spi, ENABLE);

    return drvSpiOk;
}

uint32_t Driver_SPI_GetBaudRate(Driver_SPI_Port_t SPI_Port)
{
    uint8_t index;

    if (prv_is_ready(SPI_Port) == 0U)
    {
        return 0U;
    }

    index = (uint8_t)((s_hw[SPI_Port].SPIx->CR1 & SPI_CR1_BR) >> 3);
    return prv_bus_clock(SPI_Port) / s_prescaler[index].Divider;
}

Driver_SPI_Status_t Driver_SPI_TransferByte(Driver_SPI_Port_t SPI_Port, uint8_t TxData, uint8_t *RxData)
{
    SPI_TypeDef *spi;
    Driver_SPI_Status_t st;

    if (prv_is_ready(SPI_Port) == 0U)
    {
        return drvSpiError;
    }

    if (s_dma[SPI_Port].Busy != 0U)
    {
        return drvSpiBusy;
    }

    spi = s_hw[SPI_Port].SPIx;
    prv_flush_rx(spi);

    st = prv_exchange(spi, TxData, RxData);
    if (st != drvSpiOk)
    {
        return st;
    }

    /* Chờ hết bận để người dùng nhả CS an toàn ngay sau khi hàm trả về */
    return prv_wait_flag(spi, SPI_I2S_FLAG_BSY, RESET);
}

Driver_SPI_Status_t Driver_SPI_Transfer(Driver_SPI_Port_t SPI_Port, const uint8_t *TxData, uint8_t *RxData, uint16_t Length)
{
    SPI_TypeDef *spi;
    Driver_SPI_Status_t st;
    uint16_t i;

    if (prv_is_ready(SPI_Port) == 0U)
    {
        return drvSpiError;
    }

    if (s_dma[SPI_Port].Busy != 0U)
    {
        return drvSpiBusy;
    }

    spi = s_hw[SPI_Port].SPIx;
    prv_flush_rx(spi);

    for (i = 0U; i < Length; i++)
    {
        st = prv_exchange(spi,
                          (TxData != 0) ? TxData[i] : 0xFFU,
                          (RxData != 0) ? &RxData[i] : 0);
        if (st != drvSpiOk)
        {
            return st;
        }
    }

    return prv_wait_flag(spi, SPI_I2S_FLAG_BSY, RESET);
}

Driver_SPI_Status_t Driver_SPI_Write(Driver_SPI_Port_t SPI_Port, const uint8_t *TxData, uint16_t Length)
{
    if (TxData == 0)
    {
        return drvSpiError;
    }

    return Driver_SPI_Transfer(SPI_Port, TxData, 0, Length);
}

Driver_SPI_Status_t Driver_SPI_Read(Driver_SPI_Port_t SPI_Port, uint8_t *RxData, uint16_t Length)
{
    if (RxData == 0)
    {
        return drvSpiError;
    }

    return Driver_SPI_Transfer(SPI_Port, 0, RxData, Length);
}

Driver_SPI_Status_t Driver_SPI_TransferDma(Driver_SPI_Port_t SPI_Port, const uint8_t *TxData, uint8_t *RxData, uint16_t Length)
{
    const Driver_SPI_Hw_t *hw;
    SPI_TypeDef *spi;
    uint32_t txAddr;
    uint32_t rxAddr;

    if (prv_is_ready(SPI_Port) == 0U || s_dma[SPI_Port].UseDma == 0U || Length == 0U)
    {
        return drvSpiError;
    }

    if (s_dma[SPI_Port].Busy != 0U)
    {
        return drvSpiBusy;
    }

    hw = &s_hw[SPI_Port];
    spi = hw->SPIx;

    prv_flush_rx(spi);

    /* Không có dữ liệu thật: tắt tự tăng địa chỉ và dùng biến giả */
    if (Driver_DMA_SetMemoryIncrement(hw->RxDmaChannel, (RxData != 0) ? drvDmaIncEnable : drvDmaIncDisable) != drvDmaOk ||
        Driver_DMA_SetMemoryIncrement(hw->TxDmaChannel, (TxData != 0) ? drvDmaIncEnable : drvDmaIncDisable) != drvDmaOk)
    {
        return drvSpiError;
    }

    rxAddr = (RxData != 0) ? (uint32_t)RxData : (uint32_t)&s_dummyRx;
    txAddr = (TxData != 0) ? (uint32_t)TxData : (uint32_t)&s_dummyTx;

    s_dma[SPI_Port].LastStatus = drvSpiOk;
    s_dma[SPI_Port].Busy = 1U;

    /* Bật kênh RX trước, TX sau cùng vì TX là bên khởi động việc truyền */
    if (Driver_DMA_Start(hw->RxDmaChannel, (uint32_t)&spi->DR, rxAddr, Length) != drvDmaOk)
    {
        s_dma[SPI_Port].Busy = 0U;
        return drvSpiError;
    }

    if (Driver_DMA_Start(hw->TxDmaChannel, (uint32_t)&spi->DR, txAddr, Length) != drvDmaOk)
    {
        (void)Driver_DMA_Stop(hw->RxDmaChannel);
        s_dma[SPI_Port].Busy = 0U;
        return drvSpiError;
    }

    return drvSpiOk;
}

Driver_SPI_Status_t Driver_SPI_SetCallback(Driver_SPI_Port_t SPI_Port, Driver_SPI_Callback_t Callback, void *Arg)
{
    if (SPI_Port >= drvSpiPortCount)
    {
        return drvSpiError;
    }

    s_dma[SPI_Port].Callback = 0; // Gỡ callback cũ trước để ISR không dùng cặp (Callback, Arg) dở dang
    s_dma[SPI_Port].Arg = Arg;
    s_dma[SPI_Port].Callback = Callback;

    return drvSpiOk;
}

uint8_t Driver_SPI_IsBusy(Driver_SPI_Port_t SPI_Port)
{
    return (SPI_Port < drvSpiPortCount) ? s_dma[SPI_Port].Busy : 0U;
}

Driver_SPI_Status_t Driver_SPI_WaitComplete(Driver_SPI_Port_t SPI_Port)
{
    uint32_t timeout = DRIVER_SPI_DMA_TIMEOUT_LOOPS;
    Driver_SPI_Status_t st;

    if (prv_is_ready(SPI_Port) == 0U)
    {
        return drvSpiError;
    }

    while (s_dma[SPI_Port].Busy != 0U)
    {
        if (timeout-- == 0UL)
        {
            /* Hủy lần truyền đang treo */
            (void)Driver_DMA_Stop(s_hw[SPI_Port].TxDmaChannel);
            (void)Driver_DMA_Stop(s_hw[SPI_Port].RxDmaChannel);
            s_dma[SPI_Port].Busy = 0U;
            s_dma[SPI_Port].LastStatus = drvSpiTimeout;
            return drvSpiTimeout;
        }
    }

    st = s_dma[SPI_Port].LastStatus;

    /* DMA xong nhưng SPI có thể còn đang dịch bit cuối, chờ BSY xóa */
    if (prv_wait_flag(s_hw[SPI_Port].SPIx, SPI_I2S_FLAG_BSY, RESET) != drvSpiOk)
    {
        return drvSpiTimeout;
    }

    return st;
}