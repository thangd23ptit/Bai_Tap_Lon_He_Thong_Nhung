#include "Driver_UART.h"
#include "Driver_GPIO.h"
#include "Driver_DMA.h"
#include "misc.h"

/* ------------------------------------------------------------------ */
/* Kiểm tra lúc biên dịch: kích thước bộ đệm phải là lũy thừa của 2   */
/* ------------------------------------------------------------------ */
typedef char Driver_UART_RxSizeCheck_t[((DRIVER_UART_RX_BUFFER_SIZE & (DRIVER_UART_RX_BUFFER_SIZE - 1U)) == 0U) ? 1 : -1];

#define RX_MASK ((uint16_t)(DRIVER_UART_RX_BUFFER_SIZE - 1U))

/* ------------------------------------------------------------------ */
/* Bảng ánh xạ phần cứng cho từng cổng                                */
/* ------------------------------------------------------------------ */
typedef struct
{
    USART_TypeDef *USARTx;
    uint32_t RccPeriph;
    uint8_t OnApb2; // 1: USART nằm trên APB2, 0: nằm trên APB1
    IRQn_Type Irq;
    GPIO_TypeDef *TxPort;
    uint16_t TxPin;
    GPIO_TypeDef *RxPort;
    uint16_t RxPin;
    Driver_DMA_Channel_t TxDmaChannel;
    Driver_DMA_Channel_t RxDmaChannel;
} Driver_UART_Hw_t;

static const Driver_UART_Hw_t s_hw[drvUartPortCount] = {
    {USART1, RCC_APB2Periph_USART1, 1U, USART1_IRQn, GPIOA, GPIO_Pin_9, GPIOA, GPIO_Pin_10, drvDmaChannel4, drvDmaChannel5},
    {USART2, RCC_APB1Periph_USART2, 0U, USART2_IRQn, GPIOA, GPIO_Pin_2, GPIOA, GPIO_Pin_3, drvDmaChannel7, drvDmaChannel6},
    {USART3, RCC_APB1Periph_USART3, 0U, USART3_IRQn, GPIOB, GPIO_Pin_10, GPIOB, GPIO_Pin_11, drvDmaChannel2, drvDmaChannel3},
};

/* ------------------------------------------------------------------ */
/* Trạng thái bộ đệm nhận                                             */
/* Chế độ ngắt: ISR là bên ghi duy nhất của Head                      */
/* Chế độ DMA : Head được tính từ bộ đếm của DMA                      */
/* Task là bên đọc duy nhất của Tail nên không cần khóa khi dùng      */
/* trong FreeRTOS (1 task đọc / 1 cổng)                               */
/* ------------------------------------------------------------------ */
typedef struct
{
    volatile uint8_t Buffer[DRIVER_UART_RX_BUFFER_SIZE];
    volatile uint16_t Head;
    volatile uint16_t Tail;
    volatile uint32_t Dropped;
    volatile uint8_t Initialized;
    uint8_t RxDma; // 1: đang nhận bằng DMA
    uint8_t TxDma; // 1: đã cấp kênh DMA để gửi
} Driver_UART_Rx_t;

static Driver_UART_Rx_t s_rx[drvUartPortCount];

typedef struct
{
    volatile uint8_t Busy; // 1: một lần gửi DMA đang chạy
    volatile Driver_UART_TxCallback_t Callback;
    void *volatile Arg;
} Driver_UART_Tx_t;

static Driver_UART_Tx_t s_tx[drvUartPortCount];

/* ------------------------------------------------------------------ */
/* Hàm nội bộ                                                         */
/* ------------------------------------------------------------------ */

static uint8_t prv_is_ready(Driver_UART_Port_t port)
{
    return (port < drvUartPortCount && s_rx[port].Initialized != 0U) ? 1U : 0U;
}

static Driver_UART_Status_t prv_wait_flag(USART_TypeDef *usart, uint16_t flag)
{
    uint32_t timeout = DRIVER_UART_TIMEOUT_LOOPS;

    while (USART_GetFlagStatus(usart, flag) == RESET)
    {
        if (timeout-- == 0U)
        {
            return drvUartTimeout;
        }
    }
    return drvUartOk;
}

static void prv_clock(const Driver_UART_Hw_t *hw, FunctionalState state)
{
    if (hw->OnApb2 != 0U)
    {
        RCC_APB2PeriphClockCmd(hw->RccPeriph, state);
    }
    else
    {
        RCC_APB1PeriphClockCmd(hw->RccPeriph, state);
    }
}

/* Vị trí ghi hiện tại của bộ đệm nhận */
static uint16_t prv_rx_head(Driver_UART_Port_t port)
{
    if (s_rx[port].RxDma != 0U)
    {
        /* DMA chạy vòng: số phần tử còn lại đếm ngược từ kích thước bộ đệm về 1 rồi nạp lại */
        return (uint16_t)((DRIVER_UART_RX_BUFFER_SIZE - Driver_DMA_GetRemaining(s_hw[port].RxDmaChannel)) & RX_MASK);
    }

    return s_rx[port].Head;
}

/* Giải phóng các kênh DMA mà cổng này đang giữ */
static void prv_release_dma(Driver_UART_Port_t port)
{
    const Driver_UART_Hw_t *hw = &s_hw[port];

    if (s_rx[port].RxDma != 0U)
    {
        (void)Driver_DMA_DeInit(hw->RxDmaChannel);
        s_rx[port].RxDma = 0U;
    }
    if (s_rx[port].TxDma != 0U)
    {
        (void)Driver_DMA_DeInit(hw->TxDmaChannel);
        s_rx[port].TxDma = 0U;
    }

    s_tx[port].Busy = 0U;
}

static void prv_rx_isr(Driver_UART_Port_t port)
{
    USART_TypeDef *usart = s_hw[port].USARTx;
    Driver_UART_Rx_t *rx = &s_rx[port];
    uint16_t sr = usart->SR;

    if (sr & (USART_FLAG_RXNE | USART_FLAG_ORE))
    {
        /* Đọc SR rồi đọc DR sẽ xóa cờ RXNE/ORE/FE/NE/PE */
        uint8_t data = (uint8_t)USART_ReceiveData(usart);

        if ((sr & USART_FLAG_RXNE) &&
            !(sr & (USART_FLAG_PE | USART_FLAG_FE | USART_FLAG_NE)))
        {
            uint16_t next = (uint16_t)((rx->Head + 1U) & RX_MASK);

            if (next == rx->Tail)
            {
                rx->Dropped++; // Bộ đệm đầy, bỏ byte mới
            }
            else
            {
                rx->Buffer[rx->Head] = data;
                rx->Head = next;
            }
        }
    }
}

/* Callback của DMA khi gửi xong (chạy trong ngắt DMA) */
static void prv_tx_dma_cb(Driver_DMA_Channel_t channel, Driver_DMA_Event_t event, void *arg)
{
    Driver_UART_Port_t port = (Driver_UART_Port_t)(uintptr_t)arg;
    Driver_UART_TxCallback_t cb = s_tx[port].Callback;
    Driver_UART_Status_t status = (event == drvDmaEventTransferComplete) ? drvUartOk : drvUartError;

    (void)Driver_DMA_Stop(channel);
    s_tx[port].Busy = 0U;

    if (cb != 0)
    {
        cb(port, status, s_tx[port].Arg);
    }
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

Driver_UART_Status_t Driver_UART_Init(const Driver_UART_Config_t *UART_Config)
{
    const Driver_UART_Hw_t *hw;
    USART_InitTypeDef usartInit;
    NVIC_InitTypeDef nvicInit;
    Driver_GPIO_Config_t txCfg;
    Driver_GPIO_Config_t rxCfg;
    Driver_DMA_Config_t dmaCfg;
    uint8_t rxDma;
    uint8_t txDma;

    if (UART_Config == 0 ||
        UART_Config->UART_Port >= drvUartPortCount ||
        UART_Config->UART_BaudRate == 0U ||
        UART_Config->UART_RxMode > drvUartRxDma ||
        UART_Config->UART_TxMode > drvUartTxDma)
    {
        return drvUartError;
    }

    hw = &s_hw[UART_Config->UART_Port];
    rxDma = (UART_Config->UART_RxMode == drvUartRxDma) ? 1U : 0U;
    txDma = (UART_Config->UART_TxMode == drvUartTxDma) ? 1U : 0U;

    /* 1. Tắt UART nếu đang chạy (trường hợp init lại), giải phóng DMA cũ và xóa bộ đệm */
    s_rx[UART_Config->UART_Port].Initialized = 0U;
    USART_Cmd(hw->USARTx, DISABLE);
    prv_release_dma(UART_Config->UART_Port);
    s_rx[UART_Config->UART_Port].Head = 0U;
    s_rx[UART_Config->UART_Port].Tail = 0U;
    s_rx[UART_Config->UART_Port].Dropped = 0U;

    /* Kênh DMA đang bị driver khác chiếm thì không dùng được */
    if ((rxDma != 0U && Driver_DMA_IsInUse(hw->RxDmaChannel) != 0U) ||
        (txDma != 0U && Driver_DMA_IsInUse(hw->TxDmaChannel) != 0U))
    {
        return drvUartError;
    }

    /* 2. Bật clock USART */
    prv_clock(hw, ENABLE);

    /* 3. Cấu hình chân: TX = AF push-pull, RX = input pull-up */
    txCfg.GPIOx = hw->TxPort;
    txCfg.GPIO_Pin = hw->TxPin;
    txCfg.GPIO_Mode = drvGpioModeAFPP;
    txCfg.GPIO_Pull = drvGpioPullNone;
    txCfg.GPIO_Speed = drvGpioSpeedHigh;

    rxCfg.GPIOx = hw->RxPort;
    rxCfg.GPIO_Pin = hw->RxPin;
    rxCfg.GPIO_Mode = drvGpioModeInput;
    rxCfg.GPIO_Pull = drvGpioPullUp;
    rxCfg.GPIO_Speed = drvGpioSpeedLow;

    if (Driver_GPIO_Init(&txCfg) != drvGpioOk ||
        Driver_GPIO_Init(&rxCfg) != drvGpioOk)
    {
        return drvUartError;
    }

    /* 4. Cấu hình USART */
    usartInit.USART_BaudRate = UART_Config->UART_BaudRate;
    usartInit.USART_WordLength =
        (UART_Config->UART_WordLength == drvUartWord9b) ? USART_WordLength_9b : USART_WordLength_8b;
    usartInit.USART_StopBits =
        (UART_Config->UART_StopBits == drvUartStop2) ? USART_StopBits_2 : USART_StopBits_1;
    switch (UART_Config->UART_Parity)
    {
    case drvUartParityEven:
        usartInit.USART_Parity = USART_Parity_Even;
        break;
    case drvUartParityOdd:
        usartInit.USART_Parity = USART_Parity_Odd;
        break;
    default:
        usartInit.USART_Parity = USART_Parity_No;
        break;
    }
    usartInit.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usartInit.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(hw->USARTx, &usartInit);

    /* 5. Nhận */
    if (rxDma != 0U)
    {
        /* DMA chạy vòng ghi thẳng vào bộ đệm, không cần ngắt RXNE */
        dmaCfg.DMA_Channel = hw->RxDmaChannel;
        dmaCfg.DMA_Direction = drvDmaPeriphToMemory;
        dmaCfg.DMA_Mode = drvDmaModeCircular;
        dmaCfg.DMA_PeripheralWidth = drvDmaWidth8b;
        dmaCfg.DMA_PeripheralIncrement = drvDmaIncDisable;
        dmaCfg.DMA_MemoryWidth = drvDmaWidth8b;
        dmaCfg.DMA_MemoryIncrement = drvDmaIncEnable;
        dmaCfg.DMA_Priority = drvDmaPriorityHigh;
        dmaCfg.DMA_IrqPriority = UART_Config->UART_IrqPriority;

        if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
        {
            return drvUartError;
        }
        s_rx[UART_Config->UART_Port].RxDma = 1U;

        if (Driver_DMA_Start(hw->RxDmaChannel, (uint32_t)&hw->USARTx->DR,
                             (uint32_t)s_rx[UART_Config->UART_Port].Buffer,
                             (uint16_t)DRIVER_UART_RX_BUFFER_SIZE) != drvDmaOk)
        {
            prv_release_dma(UART_Config->UART_Port);
            return drvUartError;
        }
        USART_DMACmd(hw->USARTx, USART_DMAReq_Rx, ENABLE);
    }
    else
    {
        nvicInit.NVIC_IRQChannel = hw->Irq;
        nvicInit.NVIC_IRQChannelPreemptionPriority = UART_Config->UART_IrqPriority;
        nvicInit.NVIC_IRQChannelSubPriority = 0U;
        nvicInit.NVIC_IRQChannelCmd = ENABLE;
        NVIC_Init(&nvicInit);

        USART_ITConfig(hw->USARTx, USART_IT_RXNE, ENABLE);
    }

    /* 6. Gửi bằng DMA (yêu cầu DMA của USART luôn bật, kênh DMA chỉ chạy khi gọi SendDma) */
    if (txDma != 0U)
    {
        dmaCfg.DMA_Channel = hw->TxDmaChannel;
        dmaCfg.DMA_Direction = drvDmaMemoryToPeriph;
        dmaCfg.DMA_Mode = drvDmaModeNormal;
        dmaCfg.DMA_PeripheralWidth = drvDmaWidth8b;
        dmaCfg.DMA_PeripheralIncrement = drvDmaIncDisable;
        dmaCfg.DMA_MemoryWidth = drvDmaWidth8b;
        dmaCfg.DMA_MemoryIncrement = drvDmaIncEnable;
        dmaCfg.DMA_Priority = drvDmaPriorityMedium;
        dmaCfg.DMA_IrqPriority = UART_Config->UART_IrqPriority;

        if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
        {
            prv_release_dma(UART_Config->UART_Port);
            return drvUartError;
        }
        s_rx[UART_Config->UART_Port].TxDma = 1U;

        if (Driver_DMA_SetCallback(hw->TxDmaChannel, prv_tx_dma_cb,
                                   (void *)(uintptr_t)UART_Config->UART_Port,
                                   (uint8_t)(drvDmaEventTransferComplete | drvDmaEventTransferError)) != drvDmaOk)
        {
            prv_release_dma(UART_Config->UART_Port);
            return drvUartError;
        }
        USART_DMACmd(hw->USARTx, USART_DMAReq_Tx, ENABLE);
    }

    /* 7. Bật USART */
    USART_Cmd(hw->USARTx, ENABLE);
    s_rx[UART_Config->UART_Port].Initialized = 1U;

    return drvUartOk;
}

Driver_UART_Status_t Driver_UART_DeInit(Driver_UART_Port_t UART_Port)
{
    const Driver_UART_Hw_t *hw;
    NVIC_InitTypeDef nvicInit;

    if (UART_Port >= drvUartPortCount)
    {
        return drvUartError;
    }

    hw = &s_hw[UART_Port];
    s_rx[UART_Port].Initialized = 0U;

    if (s_rx[UART_Port].RxDma == 0U)
    {
        USART_ITConfig(hw->USARTx, USART_IT_RXNE, DISABLE);

        nvicInit.NVIC_IRQChannel = hw->Irq;
        nvicInit.NVIC_IRQChannelPreemptionPriority = 0U;
        nvicInit.NVIC_IRQChannelSubPriority = 0U;
        nvicInit.NVIC_IRQChannelCmd = DISABLE;
        NVIC_Init(&nvicInit);
    }

    USART_Cmd(hw->USARTx, DISABLE);
    prv_release_dma(UART_Port);
    USART_DeInit(hw->USARTx);
    prv_clock(hw, DISABLE);

    (void)Driver_GPIO_DeInit(hw->TxPort, hw->TxPin);
    (void)Driver_GPIO_DeInit(hw->RxPort, hw->RxPin);

    return drvUartOk;
}

Driver_UART_Status_t Driver_UART_SendByte(Driver_UART_Port_t UART_Port, uint8_t Data)
{
    USART_TypeDef *usart;
    Driver_UART_Status_t status;

    if (prv_is_ready(UART_Port) == 0U)
    {
        return drvUartError;
    }

    if (s_tx[UART_Port].Busy != 0U)
    {
        return drvUartBusy;
    }

    usart = s_hw[UART_Port].USARTx;

    status = prv_wait_flag(usart, USART_FLAG_TXE);
    if (status != drvUartOk)
    {
        return status;
    }

    USART_SendData(usart, Data);
    return drvUartOk;
}

Driver_UART_Status_t Driver_UART_Send(Driver_UART_Port_t UART_Port, const uint8_t *Data, uint16_t Length)
{
    uint16_t i;
    Driver_UART_Status_t status;

    if (Data == 0 || prv_is_ready(UART_Port) == 0U)
    {
        return drvUartError;
    }

    if (s_tx[UART_Port].Busy != 0U)
    {
        return drvUartBusy;
    }

    for (i = 0U; i < Length; i++)
    {
        status = Driver_UART_SendByte(UART_Port, Data[i]);
        if (status != drvUartOk)
        {
            return status;
        }
    }

    /* Chờ byte cuối đã ra khỏi thanh ghi dịch */
    return prv_wait_flag(s_hw[UART_Port].USARTx, USART_FLAG_TC);
}

Driver_UART_Status_t Driver_UART_SendString(Driver_UART_Port_t UART_Port, const char *String)
{
    Driver_UART_Status_t status;

    if (String == 0 || prv_is_ready(UART_Port) == 0U)
    {
        return drvUartError;
    }

    if (s_tx[UART_Port].Busy != 0U)
    {
        return drvUartBusy;
    }

    while (*String != '\0')
    {
        status = Driver_UART_SendByte(UART_Port, (uint8_t)*String);
        if (status != drvUartOk)
        {
            return status;
        }
        String++;
    }

    return prv_wait_flag(s_hw[UART_Port].USARTx, USART_FLAG_TC);
}

Driver_UART_Status_t Driver_UART_SendDma(Driver_UART_Port_t UART_Port, const uint8_t *Data, uint16_t Length)
{
    USART_TypeDef *usart;

    if (Data == 0 || Length == 0U || prv_is_ready(UART_Port) == 0U || s_rx[UART_Port].TxDma == 0U)
    {
        return drvUartError;
    }

    if (s_tx[UART_Port].Busy != 0U)
    {
        return drvUartBusy;
    }

    usart = s_hw[UART_Port].USARTx;

    s_tx[UART_Port].Busy = 1U;

    /* Xóa cờ TC trước khi bắt đầu để sau đó dùng được cờ này báo "đã gửi xong hoàn toàn" */
    USART_ClearFlag(usart, USART_FLAG_TC);

    if (Driver_DMA_Start(s_hw[UART_Port].TxDmaChannel, (uint32_t)&usart->DR, (uint32_t)Data, Length) != drvDmaOk)
    {
        s_tx[UART_Port].Busy = 0U;
        return drvUartError;
    }

    return drvUartOk;
}

Driver_UART_Status_t Driver_UART_SetTxCallback(Driver_UART_Port_t UART_Port, Driver_UART_TxCallback_t Callback, void *Arg)
{
    if (UART_Port >= drvUartPortCount)
    {
        return drvUartError;
    }

    s_tx[UART_Port].Callback = 0; // Gỡ callback cũ trước để ISR không dùng cặp (Callback, Arg) dở dang
    s_tx[UART_Port].Arg = Arg;
    s_tx[UART_Port].Callback = Callback;

    return drvUartOk;
}

uint8_t Driver_UART_IsTxBusy(Driver_UART_Port_t UART_Port)
{
    if (prv_is_ready(UART_Port) == 0U)
    {
        return 0U;
    }

    if (s_tx[UART_Port].Busy != 0U)
    {
        return 1U;
    }

    return (USART_GetFlagStatus(s_hw[UART_Port].USARTx, USART_FLAG_TC) == RESET) ? 1U : 0U;
}

Driver_UART_Status_t Driver_UART_WaitTxComplete(Driver_UART_Port_t UART_Port)
{
    uint32_t timeout = DRIVER_UART_DMA_WAIT_LOOPS;

    if (prv_is_ready(UART_Port) == 0U)
    {
        return drvUartError;
    }

    while (Driver_UART_IsTxBusy(UART_Port) != 0U)
    {
        if (timeout-- == 0UL)
        {
            return drvUartTimeout;
        }
    }
    return drvUartOk;
}

uint16_t Driver_UART_Available(Driver_UART_Port_t UART_Port)
{
    if (prv_is_ready(UART_Port) == 0U)
    {
        return 0U;
    }

    return (uint16_t)((prv_rx_head(UART_Port) - s_rx[UART_Port].Tail) & RX_MASK);
}

Driver_UART_Status_t Driver_UART_ReadByte(Driver_UART_Port_t UART_Port, uint8_t *Data)
{
    Driver_UART_Rx_t *rx;

    if (Data == 0 || prv_is_ready(UART_Port) == 0U)
    {
        return drvUartError;
    }

    rx = &s_rx[UART_Port];

    if (prv_rx_head(UART_Port) == rx->Tail)
    {
        return drvUartEmpty;
    }

    *Data = rx->Buffer[rx->Tail];
    rx->Tail = (uint16_t)((rx->Tail + 1U) & RX_MASK);

    return drvUartOk;
}

uint16_t Driver_UART_Read(Driver_UART_Port_t UART_Port, uint8_t *Buffer, uint16_t MaxLength)
{
    uint16_t count = 0U;

    if (Buffer == 0)
    {
        return 0U;
    }

    while (count < MaxLength &&
           Driver_UART_ReadByte(UART_Port, &Buffer[count]) == drvUartOk)
    {
        count++;
    }

    return count;
}

void Driver_UART_FlushRx(Driver_UART_Port_t UART_Port)
{
    if (prv_is_ready(UART_Port) != 0U)
    {
        s_rx[UART_Port].Tail = prv_rx_head(UART_Port);
    }
}

uint32_t Driver_UART_GetRxDropped(Driver_UART_Port_t UART_Port)
{
    return (UART_Port < drvUartPortCount) ? s_rx[UART_Port].Dropped : 0U;
}

/* ------------------------------------------------------------------ */
/* Trình xử lý ngắt (không định nghĩa lại trong stm32f10x_it.c)        */
/* Chỉ phát sinh ngắt khi nhận ở chế độ drvUartRxInterrupt             */
/* ------------------------------------------------------------------ */

void USART1_IRQHandler(void)
{
    prv_rx_isr(drvUartPort1);
}

void USART2_IRQHandler(void)
{
    prv_rx_isr(drvUartPort2);
}

void USART3_IRQHandler(void)
{
    prv_rx_isr(drvUartPort3);
}