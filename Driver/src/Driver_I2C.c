#include "Driver_I2C.h"
#include "Driver_GPIO.h"
#include "Driver_DMA.h"
#include "misc.h"

/* ------------------------------------------------------------------ */
/* Bảng ánh xạ phần cứng cho từng cổng                                */
/* ------------------------------------------------------------------ */
typedef struct
{
    I2C_TypeDef *I2Cx;
    uint32_t RccPeriph;
    GPIO_TypeDef *Port;
    uint16_t SclPin;
    uint16_t SdaPin;
    IRQn_Type EvIrq;
    IRQn_Type ErIrq;
    Driver_DMA_Channel_t TxDmaChannel;
    Driver_DMA_Channel_t RxDmaChannel;
} Driver_I2C_Hw_t;

static const Driver_I2C_Hw_t s_hw[drvI2cPortCount] = {
    {I2C1, RCC_APB1Periph_I2C1, GPIOB, GPIO_Pin_6, GPIO_Pin_7, I2C1_EV_IRQn, I2C1_ER_IRQn, drvDmaChannel6, drvDmaChannel7},
    {I2C2, RCC_APB1Periph_I2C2, GPIOB, GPIO_Pin_10, GPIO_Pin_11, I2C2_EV_IRQn, I2C2_ER_IRQn, drvDmaChannel4, drvDmaChannel5},
};

typedef struct
{
    Driver_I2C_Config_t Config; // Giữ lại để cấu hình lại khi khôi phục bus
    uint8_t Initialized;
    uint8_t UseDma;                // 1: đã cấp 2 kênh DMA và bật ngắt EV/ER
    volatile uint8_t NeedsRecover; // 1: lỗi bus trong ngắt, cần khôi phục trước giao dịch kế tiếp
} Driver_I2C_State_t;

static Driver_I2C_State_t s_state[drvI2cPortCount];

/* Trạng thái giao dịch DMA */
typedef struct
{
    volatile uint8_t Busy;
    volatile uint8_t IsRead;
    volatile Driver_I2C_Status_t LastStatus;
    volatile Driver_I2C_Callback_t Callback;
    void *volatile Arg;
} Driver_I2C_Dma_t;

static Driver_I2C_Dma_t s_dma[drvI2cPortCount];

/* ------------------------------------------------------------------ */
/* Hàm nội bộ                                                         */
/* ------------------------------------------------------------------ */

/* Tắt ngắt toàn cục và trả về trạng thái PRIMASK cũ (không phụ thuộc core_cm3.c) */
static inline uint32_t prv_irq_save(void)
{
    uint32_t primask;

    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    return primask;
}

static inline void prv_irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

static uint8_t prv_is_ready(Driver_I2C_Port_t port)
{
    return (port < drvI2cPortCount && s_state[port].Initialized != 0U) ? 1U : 0U;
}

/* Kiểm tra dùng chung cho các hàm polling: tham số hợp lệ và không có giao dịch DMA đang chạy */
static Driver_I2C_Status_t prv_check(Driver_I2C_Port_t port, uint8_t address7)
{
    if (prv_is_ready(port) == 0U || address7 > 0x7FU)
    {
        return drvI2cError;
    }
    if (s_dma[port].Busy != 0U)
    {
        return drvI2cBusy;
    }
    return drvI2cOk;
}

/* Trễ xấp xỉ theo micro giây (dùng khi khôi phục bus, không cần chính xác) */
static void prv_delay_us(uint32_t us)
{
    RCC_ClocksTypeDef clocks;
    volatile uint32_t n;

    RCC_GetClocksFreq(&clocks);
    n = ((clocks.HCLK_Frequency / 1000000UL) * us) / 4UL + 1UL;
    while (n > 0UL)
    {
        n--;
    }
}

static uint8_t prv_pins_config(const Driver_I2C_Hw_t *hw, Driver_GPIO_Mode_t mode)
{
    Driver_GPIO_Config_t pin;

    pin.GPIOx = hw->Port;
    pin.GPIO_Pin = (uint16_t)(hw->SclPin | hw->SdaPin);
    pin.GPIO_Mode = mode;
    pin.GPIO_Pull = drvGpioPullNone;
    pin.GPIO_Speed = drvGpioSpeedHigh;

    return (Driver_GPIO_Init(&pin) == drvGpioOk) ? 1U : 0U;
}

/* Cấu hình peripheral I2C theo cấu hình đã lưu (clock và chân đã được bật sẵn) */
static void prv_hw_init(Driver_I2C_Port_t port)
{
    I2C_InitTypeDef init;
    I2C_TypeDef *i2c = s_hw[port].I2Cx;

    I2C_DeInit(i2c);

    init.I2C_ClockSpeed = s_state[port].Config.I2C_ClockSpeed;
    init.I2C_Mode = I2C_Mode_I2C;
    init.I2C_DutyCycle = I2C_DutyCycle_2;
    init.I2C_OwnAddress1 = 0x00U;
    init.I2C_Ack = I2C_Ack_Enable;
    init.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(i2c, &init);

    I2C_Cmd(i2c, ENABLE);
}

/*
Khôi phục bus khi bị kẹt (cờ BUSY không tự xóa, hoặc slave giữ SDA ở mức thấp):
phát tối đa 9 xung SCL bằng GPIO cho tới khi SDA lên cao, tạo điều kiện STOP,
rồi cấu hình lại peripheral. Đây là lỗi phổ biến của I2C trên STM32F1.
*/
static void prv_recover(Driver_I2C_Port_t port)
{
    const Driver_I2C_Hw_t *hw = &s_hw[port];
    uint8_t i;

    I2C_Cmd(hw->I2Cx, DISABLE);

    (void)prv_pins_config(hw, drvGpioModeOutputOD);
    Driver_GPIO_SetPin(hw->Port, (uint16_t)(hw->SclPin | hw->SdaPin));
    prv_delay_us(5U);

    for (i = 0U; i < 9U; i++)
    {
        if (Driver_GPIO_ReadPin(hw->Port, hw->SdaPin) == drvGpioLevelHigh)
        {
            break;
        }
        Driver_GPIO_ResetPin(hw->Port, hw->SclPin);
        prv_delay_us(5U);
        Driver_GPIO_SetPin(hw->Port, hw->SclPin);
        prv_delay_us(5U);
    }

    /* Điều kiện STOP: SDA từ thấp lên cao khi SCL đang cao */
    Driver_GPIO_ResetPin(hw->Port, hw->SdaPin);
    prv_delay_us(5U);
    Driver_GPIO_SetPin(hw->Port, hw->SclPin);
    prv_delay_us(5U);
    Driver_GPIO_SetPin(hw->Port, hw->SdaPin);
    prv_delay_us(5U);

    (void)prv_pins_config(hw, drvGpioModeAFOD);
    prv_hw_init(port);
}

/* Chờ một cờ được set. Trả về drvI2cNack nếu slave NACK trong lúc chờ */
static Driver_I2C_Status_t prv_wait_flag(I2C_TypeDef *i2c, uint32_t flag)
{
    uint32_t timeout = DRIVER_I2C_TIMEOUT_LOOPS;

    while (I2C_GetFlagStatus(i2c, flag) == RESET)
    {
        if (I2C_GetFlagStatus(i2c, I2C_FLAG_AF) != RESET)
        {
            return drvI2cNack;
        }
        if (timeout-- == 0UL)
        {
            return drvI2cTimeout;
        }
    }
    return drvI2cOk;
}

static Driver_I2C_Status_t prv_wait_not_busy(Driver_I2C_Port_t port)
{
    I2C_TypeDef *i2c = s_hw[port].I2Cx;
    uint32_t timeout = DRIVER_I2C_TIMEOUT_LOOPS;

    /* Giao dịch DMA trước đó gặp lỗi bus (BERR/ARLO) thì khôi phục trước khi bắt đầu */
    if (s_state[port].NeedsRecover != 0U)
    {
        s_state[port].NeedsRecover = 0U;
        prv_recover(port);
    }

    while (I2C_GetFlagStatus(i2c, I2C_FLAG_BUSY) != RESET)
    {
        if (timeout-- == 0UL)
        {
            prv_recover(port);
            return (I2C_GetFlagStatus(i2c, I2C_FLAG_BUSY) != RESET) ? drvI2cTimeout : drvI2cOk;
        }
    }
    return drvI2cOk;
}

/* START + gửi địa chỉ, chờ slave ACK (cờ ADDR). Chưa xóa cờ ADDR */
static Driver_I2C_Status_t prv_address(I2C_TypeDef *i2c, uint8_t address7, uint8_t direction)
{
    Driver_I2C_Status_t st;

    I2C_GenerateSTART(i2c, ENABLE);
    st = prv_wait_flag(i2c, I2C_FLAG_SB);
    if (st != drvI2cOk)
    {
        return st;
    }

    I2C_Send7bitAddress(i2c, (uint8_t)(address7 << 1), direction);
    return prv_wait_flag(i2c, I2C_FLAG_ADDR);
}

/* Cờ ADDR được xóa bằng cách đọc SR1 rồi SR2 */
static void prv_clear_addr(I2C_TypeDef *i2c)
{
    (void)i2c->SR1;
    (void)i2c->SR2;
}

static Driver_I2C_Status_t prv_tx_byte(I2C_TypeDef *i2c, uint8_t byte)
{
    Driver_I2C_Status_t st = prv_wait_flag(i2c, I2C_FLAG_TXE);

    if (st == drvI2cOk)
    {
        I2C_SendData(i2c, byte);
    }
    return st;
}

/* Kết thúc giao dịch: nếu lỗi thì STOP, xóa cờ AF, timeout thì khôi phục bus */
static Driver_I2C_Status_t prv_finish(Driver_I2C_Port_t port, Driver_I2C_Status_t st)
{
    I2C_TypeDef *i2c = s_hw[port].I2Cx;

    if (st != drvI2cOk)
    {
        I2C_GenerateSTOP(i2c, ENABLE);
        I2C_ClearFlag(i2c, I2C_FLAG_AF);

        if (st == drvI2cTimeout)
        {
            prv_recover(port);
        }
    }

    I2C_AcknowledgeConfig(i2c, ENABLE);
    I2C_NACKPositionConfig(i2c, I2C_NACKPosition_Current);
    return st;
}

static uint8_t prv_pack_reg(uint16_t regAddr, Driver_I2C_RegAddrSize_t size, uint8_t reg[2])
{
    if (size == drvI2cRegAddr16b)
    {
        reg[0] = (uint8_t)(regAddr >> 8);
        reg[1] = (uint8_t)(regAddr & 0xFFU);
        return 2U;
    }

    reg[0] = (uint8_t)(regAddr & 0xFFU);
    return 1U;
}

/* Giao dịch ghi (regLen = 0 nghĩa là không có địa chỉ thanh ghi) */
static Driver_I2C_Status_t prv_write(Driver_I2C_Port_t port, uint8_t address7,
                                     const uint8_t *reg, uint8_t regLen,
                                     const uint8_t *data, uint16_t len)
{
    I2C_TypeDef *i2c = s_hw[port].I2Cx;
    Driver_I2C_Status_t st;
    uint16_t i;

    st = prv_wait_not_busy(port);
    if (st != drvI2cOk)
    {
        return st;
    }

    I2C_ClearFlag(i2c, I2C_FLAG_AF);
    I2C_AcknowledgeConfig(i2c, ENABLE);

    st = prv_address(i2c, address7, I2C_Direction_Transmitter);
    if (st == drvI2cOk)
    {
        prv_clear_addr(i2c);

        for (i = 0U; i < regLen && st == drvI2cOk; i++)
        {
            st = prv_tx_byte(i2c, reg[i]);
        }
        for (i = 0U; i < len && st == drvI2cOk; i++)
        {
            st = prv_tx_byte(i2c, data[i]);
        }

        /* Chờ byte cuối truyền xong hoàn toàn */
        if (st == drvI2cOk && (regLen + len) > 0U)
        {
            st = prv_wait_flag(i2c, I2C_FLAG_BTF);
        }
        if (st == drvI2cOk)
        {
            I2C_GenerateSTOP(i2c, ENABLE);
        }
    }

    return prv_finish(port, st);
}

/*
Nhận len byte theo đúng trình tự của tài liệu RM0008 (khác nhau cho N = 1, 2, >= 3).
Gọi khi cờ ADDR đã set nhưng chưa xóa.
*/
static Driver_I2C_Status_t prv_receive(I2C_TypeDef *i2c, uint8_t *data, uint16_t len)
{
    Driver_I2C_Status_t st;
    uint32_t primask;

    if (len == 1U)
    {
        I2C_AcknowledgeConfig(i2c, DISABLE);

        primask = prv_irq_save(); // Xóa ADDR và đặt STOP phải sát nhau
        prv_clear_addr(i2c);
        I2C_GenerateSTOP(i2c, ENABLE);
        prv_irq_restore(primask);

        st = prv_wait_flag(i2c, I2C_FLAG_RXNE);
        if (st == drvI2cOk)
        {
            data[0] = I2C_ReceiveData(i2c);
        }
        return st;
    }

    if (len == 2U)
    {
        I2C_NACKPositionConfig(i2c, I2C_NACKPosition_Next);

        primask = prv_irq_save();
        prv_clear_addr(i2c);
        I2C_AcknowledgeConfig(i2c, DISABLE);
        prv_irq_restore(primask);

        st = prv_wait_flag(i2c, I2C_FLAG_BTF);
        if (st != drvI2cOk)
        {
            return st;
        }

        I2C_GenerateSTOP(i2c, ENABLE);
        data[0] = I2C_ReceiveData(i2c);

        st = prv_wait_flag(i2c, I2C_FLAG_RXNE);
        if (st == drvI2cOk)
        {
            data[1] = I2C_ReceiveData(i2c);
        }
        return st;
    }

    /* len >= 3 */
    prv_clear_addr(i2c);

    while (len > 3U)
    {
        st = prv_wait_flag(i2c, I2C_FLAG_RXNE);
        if (st != drvI2cOk)
        {
            return st;
        }
        *data++ = I2C_ReceiveData(i2c);
        len--;
    }

    /* 3 byte cuối: byte N-2 trong DR, byte N-1 trong thanh ghi dịch */
    st = prv_wait_flag(i2c, I2C_FLAG_BTF);
    if (st != drvI2cOk)
    {
        return st;
    }
    I2C_AcknowledgeConfig(i2c, DISABLE);
    *data++ = I2C_ReceiveData(i2c); // N-2

    st = prv_wait_flag(i2c, I2C_FLAG_BTF);
    if (st != drvI2cOk)
    {
        return st;
    }
    I2C_GenerateSTOP(i2c, ENABLE);
    *data++ = I2C_ReceiveData(i2c); // N-1

    st = prv_wait_flag(i2c, I2C_FLAG_RXNE);
    if (st == drvI2cOk)
    {
        *data = I2C_ReceiveData(i2c); // N
    }
    return st;
}

/* Giao dịch đọc (regLen = 0 nghĩa là đọc trực tiếp, không gửi địa chỉ thanh ghi) */
static Driver_I2C_Status_t prv_read(Driver_I2C_Port_t port, uint8_t address7,
                                    const uint8_t *reg, uint8_t regLen,
                                    uint8_t *data, uint16_t len)
{
    I2C_TypeDef *i2c = s_hw[port].I2Cx;
    Driver_I2C_Status_t st = drvI2cOk;
    uint8_t i;

    st = prv_wait_not_busy(port);
    if (st != drvI2cOk)
    {
        return st;
    }

    I2C_ClearFlag(i2c, I2C_FLAG_AF);
    I2C_AcknowledgeConfig(i2c, ENABLE);
    I2C_NACKPositionConfig(i2c, I2C_NACKPosition_Current);

    if (regLen > 0U)
    {
        st = prv_address(i2c, address7, I2C_Direction_Transmitter);
        if (st == drvI2cOk)
        {
            prv_clear_addr(i2c);
            for (i = 0U; i < regLen && st == drvI2cOk; i++)
            {
                st = prv_tx_byte(i2c, reg[i]);
            }
            if (st == drvI2cOk)
            {
                st = prv_wait_flag(i2c, I2C_FLAG_BTF);
            }
        }
    }

    if (st == drvI2cOk)
    {
        st = prv_address(i2c, address7, I2C_Direction_Receiver); // (repeated) START
    }
    if (st == drvI2cOk)
    {
        st = prv_receive(i2c, data, len);
    }

    return prv_finish(port, st);
}

/* ------------------------------------------------------------------ */
/* Phần DMA                                                           */
/* ------------------------------------------------------------------ */

/* Kết thúc giao dịch DMA (gọi trong ngắt hoặc khi hủy): dọn dẹp, ghi kết quả, gọi callback */
static void prv_dma_finish(Driver_I2C_Port_t port, Driver_I2C_Status_t st)
{
    const Driver_I2C_Hw_t *hw = &s_hw[port];
    I2C_TypeDef *i2c = hw->I2Cx;
    Driver_I2C_Callback_t cb;

    I2C_ITConfig(i2c, I2C_IT_EVT | I2C_IT_ERR, DISABLE);
    I2C_DMALastTransferCmd(i2c, DISABLE);
    I2C_DMACmd(i2c, DISABLE);

    (void)Driver_DMA_Stop(hw->TxDmaChannel);
    (void)Driver_DMA_Stop(hw->RxDmaChannel);

    I2C_AcknowledgeConfig(i2c, ENABLE);

    s_dma[port].LastStatus = st;
    s_dma[port].Busy = 0U;

    cb = s_dma[port].Callback;
    if (cb != 0)
    {
        cb(port, st, s_dma[port].Arg);
    }
}

/* Callback của DMA (chạy trong ngắt DMA) */
static void prv_dma_cb(Driver_DMA_Channel_t channel, Driver_DMA_Event_t event, void *arg)
{
    Driver_I2C_Port_t port = (Driver_I2C_Port_t)(uintptr_t)arg;
    I2C_TypeDef *i2c = s_hw[port].I2Cx;

    (void)channel;

    if (s_dma[port].Busy == 0U)
    {
        return;
    }

    if (event == drvDmaEventTransferError)
    {
        I2C_GenerateSTOP(i2c, ENABLE);
        prv_dma_finish(port, drvI2cError);
        return;
    }

    if (event == drvDmaEventTransferComplete)
    {
        if (s_dma[port].IsRead != 0U)
        {
            /* DMA đã lấy đủ dữ liệu, byte cuối đã được NACK nhờ bit LAST: chỉ còn phát STOP */
            I2C_GenerateSTOP(i2c, ENABLE);
            prv_dma_finish(port, drvI2cOk);
        }
        else
        {
            /* DMA đã đưa byte cuối vào DR. Phải chờ byte đó ra hết (cờ BTF) mới phát STOP được,
               việc này giao cho ngắt sự kiện I2C để không phải chờ trong ngắt DMA */
            I2C_ITConfig(i2c, I2C_IT_EVT, ENABLE);
        }
    }
}

static void prv_ev_irq(Driver_I2C_Port_t port)
{
    I2C_TypeDef *i2c = s_hw[port].I2Cx;

    if (s_dma[port].Busy == 0U)
    {
        I2C_ITConfig(i2c, I2C_IT_EVT, DISABLE);
        return;
    }

    /* Chỉ giao dịch ghi bật ngắt EV: BTF set nghĩa là byte cuối đã truyền xong */
    if (s_dma[port].IsRead == 0U && I2C_GetFlagStatus(i2c, I2C_FLAG_BTF) != RESET)
    {
        I2C_GenerateSTOP(i2c, ENABLE);
        prv_dma_finish(port, drvI2cOk);
    }
}

static void prv_er_irq(Driver_I2C_Port_t port)
{
    I2C_TypeDef *i2c = s_hw[port].I2Cx;
    uint16_t sr1 = i2c->SR1;
    uint16_t errors = (uint16_t)(sr1 & (I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR));
    Driver_I2C_Status_t st = ((errors & I2C_SR1_AF) != 0U) ? drvI2cNack : drvI2cError;

    /* Xóa mọi cờ lỗi để ngắt không lặp lại */
    I2C_ClearFlag(i2c, I2C_FLAG_AF | I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_OVR |
                           I2C_FLAG_PECERR | I2C_FLAG_TIMEOUT | I2C_FLAG_SMBALERT);

    if ((errors & (I2C_SR1_BERR | I2C_SR1_ARLO)) != 0U)
    {
        s_state[port].NeedsRecover = 1U; // Khôi phục bus ở giao dịch kế tiếp (không trễ trong ngắt)
    }

    if (s_dma[port].Busy != 0U)
    {
        if ((errors & I2C_SR1_AF) != 0U)
        {
            I2C_GenerateSTOP(i2c, ENABLE);
        }
        prv_dma_finish(port, st);
    }
    else
    {
        I2C_ITConfig(i2c, I2C_IT_ERR, DISABLE);
    }
}

/* Giải phóng 2 kênh DMA và tắt ngắt I2C của cổng này */
static void prv_release_dma(Driver_I2C_Port_t port)
{
    const Driver_I2C_Hw_t *hw = &s_hw[port];
    NVIC_InitTypeDef nvicInit;

    if (s_state[port].UseDma != 0U)
    {
        I2C_ITConfig(hw->I2Cx, I2C_IT_EVT | I2C_IT_ERR, DISABLE);

        nvicInit.NVIC_IRQChannelPreemptionPriority = 0U;
        nvicInit.NVIC_IRQChannelSubPriority = 0U;
        nvicInit.NVIC_IRQChannelCmd = DISABLE;

        nvicInit.NVIC_IRQChannel = hw->EvIrq;
        NVIC_Init(&nvicInit);
        nvicInit.NVIC_IRQChannel = hw->ErIrq;
        NVIC_Init(&nvicInit);

        (void)Driver_DMA_DeInit(hw->TxDmaChannel);
        (void)Driver_DMA_DeInit(hw->RxDmaChannel);
        s_state[port].UseDma = 0U;
    }
    s_dma[port].Busy = 0U;
}

/*
Bắt đầu một giao dịch DMA. Phần địa chỉ làm bằng polling, sau đó khởi động DMA cho phần dữ liệu.
Ghi:  START - địa chỉ(W) - [thanh ghi] - (DMA: dữ liệu) - STOP (phát trong ngắt EV khi BTF)
Đọc:  START - địa chỉ(W) - [thanh ghi] - repeated START - địa chỉ(R) - (DMA: dữ liệu) - STOP (phát khi DMA xong)
*/
static Driver_I2C_Status_t prv_dma_begin(Driver_I2C_Port_t port, uint8_t address7,
                                         const uint8_t *reg, uint8_t regLen,
                                         const uint8_t *data, uint16_t len, uint8_t isRead)
{
    const Driver_I2C_Hw_t *hw = &s_hw[port];
    I2C_TypeDef *i2c = hw->I2Cx;
    Driver_I2C_Status_t st;
    Driver_DMA_Status_t dmaSt;
    uint8_t i;

    st = prv_wait_not_busy(port);
    if (st != drvI2cOk)
    {
        return st;
    }

    I2C_ClearFlag(i2c, I2C_FLAG_AF);
    I2C_AcknowledgeConfig(i2c, ENABLE);
    I2C_NACKPositionConfig(i2c, I2C_NACKPosition_Current);

    /* Giai đoạn địa chỉ (polling) */
    if (isRead == 0U || regLen > 0U)
    {
        st = prv_address(i2c, address7, I2C_Direction_Transmitter);
        if (st == drvI2cOk)
        {
            prv_clear_addr(i2c);
            for (i = 0U; i < regLen && st == drvI2cOk; i++)
            {
                st = prv_tx_byte(i2c, reg[i]);
            }
            if (isRead != 0U && st == drvI2cOk)
            {
                st = prv_wait_flag(i2c, I2C_FLAG_BTF);
            }
        }
    }

    if (isRead != 0U && st == drvI2cOk)
    {
        st = prv_address(i2c, address7, I2C_Direction_Receiver); // (repeated) START
    }

    if (st != drvI2cOk)
    {
        return prv_finish(port, st);
    }

    /* Giai đoạn dữ liệu (DMA) */
    s_dma[port].IsRead = isRead;
    s_dma[port].LastStatus = drvI2cOk;
    s_dma[port].Busy = 1U;

    I2C_ITConfig(i2c, I2C_IT_ERR, ENABLE);

    if (isRead != 0U)
    {
        /* Bit LAST: tự NACK ở byte cuối. Phải bật DMA và LAST trước khi xóa ADDR */
        I2C_DMALastTransferCmd(i2c, ENABLE);
        dmaSt = Driver_DMA_Start(hw->RxDmaChannel, (uint32_t)&i2c->DR, (uint32_t)data, len);
        if (dmaSt == drvDmaOk)
        {
            I2C_DMACmd(i2c, ENABLE);
            prv_clear_addr(i2c);
        }
    }
    else
    {
        dmaSt = Driver_DMA_Start(hw->TxDmaChannel, (uint32_t)&i2c->DR, (uint32_t)data, len);
        if (dmaSt == drvDmaOk)
        {
            I2C_DMACmd(i2c, ENABLE);
        }
    }

    if (dmaSt != drvDmaOk)
    {
        I2C_ITConfig(i2c, I2C_IT_ERR, DISABLE);
        I2C_DMALastTransferCmd(i2c, DISABLE);
        s_dma[port].Busy = 0U;
        return prv_finish(port, drvI2cError);
    }

    return drvI2cOk;
}

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

Driver_I2C_Status_t Driver_I2C_Init(const Driver_I2C_Config_t *I2C_Config)
{
    const Driver_I2C_Hw_t *hw;
    NVIC_InitTypeDef nvicInit;
    Driver_DMA_Config_t dmaCfg;
    Driver_I2C_Port_t port;
    uint8_t useDma;

    if (I2C_Config == 0 ||
        I2C_Config->I2C_Port >= drvI2cPortCount ||
        I2C_Config->I2C_ClockSpeed == 0U ||
        I2C_Config->I2C_ClockSpeed > 400000U ||
        I2C_Config->I2C_DmaMode > drvI2cDmaEnable)
    {
        return drvI2cError;
    }

    port = I2C_Config->I2C_Port;
    hw = &s_hw[port];
    useDma = (I2C_Config->I2C_DmaMode == drvI2cDmaEnable) ? 1U : 0U;

    s_state[port].Initialized = 0U;
    prv_release_dma(port); // Init lại: trả kênh DMA cũ trước
    s_state[port].NeedsRecover = 0U;

    /* Kênh DMA đang bị driver khác chiếm thì không dùng được */
    if (useDma != 0U &&
        (Driver_DMA_IsInUse(hw->TxDmaChannel) != 0U || Driver_DMA_IsInUse(hw->RxDmaChannel) != 0U))
    {
        return drvI2cError;
    }

    RCC_APB1PeriphClockCmd(hw->RccPeriph, ENABLE);

    if (prv_pins_config(hw, drvGpioModeAFOD) == 0U)
    {
        return drvI2cError;
    }

    s_state[port].Config = *I2C_Config;
    prv_hw_init(port);

    if (useDma != 0U)
    {
        /* Kênh TX */
        dmaCfg.DMA_Channel = hw->TxDmaChannel;
        dmaCfg.DMA_Direction = drvDmaMemoryToPeriph;
        dmaCfg.DMA_Mode = drvDmaModeNormal;
        dmaCfg.DMA_PeripheralWidth = drvDmaWidth8b;
        dmaCfg.DMA_PeripheralIncrement = drvDmaIncDisable;
        dmaCfg.DMA_MemoryWidth = drvDmaWidth8b;
        dmaCfg.DMA_MemoryIncrement = drvDmaIncEnable;
        dmaCfg.DMA_Priority = drvDmaPriorityMedium;
        dmaCfg.DMA_IrqPriority = I2C_Config->I2C_IrqPriority;

        if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
        {
            return drvI2cError;
        }

        /* Kênh RX */
        dmaCfg.DMA_Channel = hw->RxDmaChannel;
        dmaCfg.DMA_Direction = drvDmaPeriphToMemory;

        if (Driver_DMA_Init(&dmaCfg) != drvDmaOk)
        {
            (void)Driver_DMA_DeInit(hw->TxDmaChannel);
            return drvI2cError;
        }

        s_state[port].UseDma = 1U;

        if (Driver_DMA_SetCallback(hw->TxDmaChannel, prv_dma_cb, (void *)(uintptr_t)port,
                                   (uint8_t)(drvDmaEventTransferComplete | drvDmaEventTransferError)) != drvDmaOk ||
            Driver_DMA_SetCallback(hw->RxDmaChannel, prv_dma_cb, (void *)(uintptr_t)port,
                                   (uint8_t)(drvDmaEventTransferComplete | drvDmaEventTransferError)) != drvDmaOk)
        {
            prv_release_dma(port);
            return drvI2cError;
        }

        /* Ngắt EV/ER của I2C (từng nguồn ngắt chỉ được bật trong lúc có giao dịch DMA) */
        nvicInit.NVIC_IRQChannelPreemptionPriority = I2C_Config->I2C_IrqPriority;
        nvicInit.NVIC_IRQChannelSubPriority = 0U;
        nvicInit.NVIC_IRQChannelCmd = ENABLE;

        nvicInit.NVIC_IRQChannel = hw->EvIrq;
        NVIC_Init(&nvicInit);
        nvicInit.NVIC_IRQChannel = hw->ErIrq;
        NVIC_Init(&nvicInit);
    }

    s_state[port].Initialized = 1U;

    return drvI2cOk;
}

Driver_I2C_Status_t Driver_I2C_DeInit(Driver_I2C_Port_t I2C_Port)
{
    const Driver_I2C_Hw_t *hw;

    if (I2C_Port >= drvI2cPortCount)
    {
        return drvI2cError;
    }

    hw = &s_hw[I2C_Port];
    s_state[I2C_Port].Initialized = 0U;

    prv_release_dma(I2C_Port);

    I2C_Cmd(hw->I2Cx, DISABLE);
    I2C_DeInit(hw->I2Cx);
    RCC_APB1PeriphClockCmd(hw->RccPeriph, DISABLE);

    (void)Driver_GPIO_DeInit(hw->Port, (uint16_t)(hw->SclPin | hw->SdaPin));

    return drvI2cOk;
}

Driver_I2C_Status_t Driver_I2C_Write(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, const uint8_t *Data, uint16_t Length)
{
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (Length > 0U && Data == 0)
    {
        return drvI2cError;
    }

    return prv_write(I2C_Port, DeviceAddress, 0, 0U, Data, Length);
}

Driver_I2C_Status_t Driver_I2C_Read(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint8_t *Data, uint16_t Length)
{
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (Data == 0 || Length == 0U)
    {
        return drvI2cError;
    }

    return prv_read(I2C_Port, DeviceAddress, 0, 0U, Data, Length);
}

Driver_I2C_Status_t Driver_I2C_MemWrite(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, const uint8_t *Data, uint16_t Length)
{
    uint8_t reg[2];
    uint8_t regLen;
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (Length > 0U && Data == 0)
    {
        return drvI2cError;
    }

    regLen = prv_pack_reg(RegAddress, RegAddressSize, reg);
    return prv_write(I2C_Port, DeviceAddress, reg, regLen, Data, Length);
}

Driver_I2C_Status_t Driver_I2C_MemRead(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, uint8_t *Data, uint16_t Length)
{
    uint8_t reg[2];
    uint8_t regLen;
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (Data == 0 || Length == 0U)
    {
        return drvI2cError;
    }

    regLen = prv_pack_reg(RegAddress, RegAddressSize, reg);
    return prv_read(I2C_Port, DeviceAddress, reg, regLen, Data, Length);
}

Driver_I2C_Status_t Driver_I2C_IsDeviceReady(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress)
{
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }

    return prv_write(I2C_Port, DeviceAddress, 0, 0U, 0, 0U);
}

Driver_I2C_Status_t Driver_I2C_WriteDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, const uint8_t *Data, uint16_t Length)
{
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (s_state[I2C_Port].UseDma == 0U || Data == 0 || Length == 0U)
    {
        return drvI2cError;
    }

    return prv_dma_begin(I2C_Port, DeviceAddress, 0, 0U, Data, Length, 0U);
}

Driver_I2C_Status_t Driver_I2C_ReadDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint8_t *Data, uint16_t Length)
{
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (s_state[I2C_Port].UseDma == 0U || Data == 0 || Length < 2U)
    {
        return drvI2cError;
    }

    return prv_dma_begin(I2C_Port, DeviceAddress, 0, 0U, Data, Length, 1U);
}

Driver_I2C_Status_t Driver_I2C_MemWriteDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, const uint8_t *Data, uint16_t Length)
{
    uint8_t reg[2];
    uint8_t regLen;
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (s_state[I2C_Port].UseDma == 0U || Data == 0 || Length == 0U)
    {
        return drvI2cError;
    }

    regLen = prv_pack_reg(RegAddress, RegAddressSize, reg);
    return prv_dma_begin(I2C_Port, DeviceAddress, reg, regLen, Data, Length, 0U);
}

Driver_I2C_Status_t Driver_I2C_MemReadDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, uint8_t *Data, uint16_t Length)
{
    uint8_t reg[2];
    uint8_t regLen;
    Driver_I2C_Status_t st = prv_check(I2C_Port, DeviceAddress);

    if (st != drvI2cOk)
    {
        return st;
    }
    if (s_state[I2C_Port].UseDma == 0U || Data == 0 || Length < 2U)
    {
        return drvI2cError;
    }

    regLen = prv_pack_reg(RegAddress, RegAddressSize, reg);
    return prv_dma_begin(I2C_Port, DeviceAddress, reg, regLen, Data, Length, 1U);
}

Driver_I2C_Status_t Driver_I2C_SetCallback(Driver_I2C_Port_t I2C_Port, Driver_I2C_Callback_t Callback, void *Arg)
{
    if (I2C_Port >= drvI2cPortCount)
    {
        return drvI2cError;
    }

    s_dma[I2C_Port].Callback = 0; // Gỡ callback cũ trước để ISR không dùng cặp (Callback, Arg) dở dang
    s_dma[I2C_Port].Arg = Arg;
    s_dma[I2C_Port].Callback = Callback;

    return drvI2cOk;
}

uint8_t Driver_I2C_IsBusy(Driver_I2C_Port_t I2C_Port)
{
    return (I2C_Port < drvI2cPortCount) ? s_dma[I2C_Port].Busy : 0U;
}

Driver_I2C_Status_t Driver_I2C_WaitComplete(Driver_I2C_Port_t I2C_Port)
{
    uint32_t timeout = DRIVER_I2C_DMA_TIMEOUT_LOOPS;
    uint32_t primask;

    if (prv_is_ready(I2C_Port) == 0U)
    {
        return drvI2cError;
    }

    while (s_dma[I2C_Port].Busy != 0U)
    {
        if (timeout-- == 0UL)
        {
            /* Hủy giao dịch treo: phát STOP, dọn dẹp, bus sẽ được khôi phục ở giao dịch kế tiếp */
            primask = prv_irq_save();
            if (s_dma[I2C_Port].Busy != 0U)
            {
                I2C_GenerateSTOP(s_hw[I2C_Port].I2Cx, ENABLE);
                s_state[I2C_Port].NeedsRecover = 1U;
                prv_dma_finish(I2C_Port, drvI2cTimeout);
            }
            prv_irq_restore(primask);
            return drvI2cTimeout;
        }
    }

    return s_dma[I2C_Port].LastStatus;
}

/* ------------------------------------------------------------------ */
/* Trình xử lý ngắt (không định nghĩa lại trong stm32f10x_it.c)        */
/* Chỉ phát sinh ngắt khi có giao dịch DMA                              */
/* ------------------------------------------------------------------ */

void I2C1_EV_IRQHandler(void)
{
    prv_ev_irq(drvI2cPort1);
}

void I2C1_ER_IRQHandler(void)
{
    prv_er_irq(drvI2cPort1);
}

void I2C2_EV_IRQHandler(void)
{
    prv_ev_irq(drvI2cPort2);
}

void I2C2_ER_IRQHandler(void)
{
    prv_er_irq(drvI2cPort2);
}