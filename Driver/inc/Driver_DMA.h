#ifndef DRIVER_DMA_H
#define DRIVER_DMA_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include "stm32f10x.h"
#include "stm32f10x_dma.h"
#include "stm32f10x_rcc.h"

    /*
    Số vòng lặp tối đa khi chờ truyền xong bằng Driver_DMA_WaitComplete (timeout),
    có thể ghi đè bằng -D trong Makefile
    */
#ifndef DRIVER_DMA_TIMEOUT_LOOPS
#define DRIVER_DMA_TIMEOUT_LOOPS 5000000UL
#endif

    /*
    Trạng thái trả về của các hàm DMA
    */

    typedef enum
    {
        drvDmaOk = 0,       // Thành công
        drvDmaError,        // Tham số sai, sai căn lề địa chỉ hoặc kênh chưa được khởi tạo
        drvDmaTimeout,      // Hết thời gian chờ
        drvDmaTransferError // Phần cứng báo lỗi truyền (địa chỉ không hợp lệ)
    } Driver_DMA_Status_t;

    /*
    Các kênh của DMA1 (STM32F103 medium-density). Mỗi kênh phục vụ một số peripheral cố định:
        Kênh 1: ADC1, TIM2_CH3, TIM4_CH1
        Kênh 2: SPI1_RX, USART3_TX, TIM1_CH1, TIM2_UP, TIM3_CH3
        Kênh 3: SPI1_TX, USART3_RX, TIM1_CH2, TIM3_CH4, TIM3_UP
        Kênh 4: SPI2_RX, USART1_TX, I2C2_TX, TIM1_CH4/TRIG/COM, TIM4_CH2
        Kênh 5: SPI2_TX, USART1_RX, I2C2_RX, TIM1_UP, TIM2_CH1, TIM4_CH3
        Kênh 6: USART2_RX, I2C1_TX, TIM1_CH3, TIM3_CH1/TRIG
        Kênh 7: USART2_TX, I2C1_RX, TIM2_CH2/CH4, TIM4_UP
    Một kênh chỉ dùng được cho MỘT peripheral tại một thời điểm.
    Memory-to-memory có thể dùng bất kỳ kênh nào chưa bận.
    DMA2 (chỉ có trên dòng high-density) chưa được hỗ trợ.
    */

    typedef enum
    {
        drvDmaChannel1 = 0,
        drvDmaChannel2,
        drvDmaChannel3,
        drvDmaChannel4,
        drvDmaChannel5,
        drvDmaChannel6,
        drvDmaChannel7,
        drvDmaChannelCount // Chỉ dùng để đếm, không truyền vào hàm
    } Driver_DMA_Channel_t;

    /*
    Hướng truyền
    */

    typedef enum
    {
        drvDmaPeriphToMemory = 0, // Peripheral -> bộ nhớ (ví dụ nhận UART, đọc ADC)
        drvDmaMemoryToPeriph,     // Bộ nhớ -> peripheral (ví dụ gửi UART, SPI)
        drvDmaMemoryToMemory      // Bộ nhớ -> bộ nhớ: "PeripheralAddress" là nguồn, "MemoryAddress" là đích
    } Driver_DMA_Direction_t;

    /*
    Chế độ truyền
        Normal  : truyền đủ Count phần tử thì dừng
        Circular: tự nạp lại và truyền lặp vòng quanh bộ đệm (không dùng được với memory-to-memory)
    */

    typedef enum
    {
        drvDmaModeNormal = 0,
        drvDmaModeCircular
    } Driver_DMA_Mode_t;

    /*
    Độ rộng một phần tử dữ liệu
    */

    typedef enum
    {
        drvDmaWidth8b = 0,
        drvDmaWidth16b,
        drvDmaWidth32b
    } Driver_DMA_Width_t;

    /*
    Tự tăng địa chỉ sau mỗi phần tử
    (peripheral thường để Disable, bộ đệm trong RAM thường để Enable)
    */

    typedef enum
    {
        drvDmaIncDisable = 0,
        drvDmaIncEnable
    } Driver_DMA_Increment_t;

    /*
    Mức ưu tiên giữa các kênh DMA
    */

    typedef enum
    {
        drvDmaPriorityLow = 0,
        drvDmaPriorityMedium,
        drvDmaPriorityHigh,
        drvDmaPriorityVeryHigh
    } Driver_DMA_Priority_t;

    /*
    Sự kiện của DMA, có thể OR với nhau để tạo EventMask khi gọi Driver_DMA_SetCallback
    */

    typedef enum
    {
        drvDmaEventHalfTransfer = 0x01U,     // Đã truyền xong một nửa bộ đệm
        drvDmaEventTransferComplete = 0x02U, // Đã truyền xong toàn bộ (hoặc hết một vòng ở chế độ Circular)
        drvDmaEventTransferError = 0x04U     // Lỗi truyền
    } Driver_DMA_Event_t;

    /*
    Hàm callback của DMA.
    LƯU Ý: callback chạy trong ngữ cảnh NGẮT. Nếu bên trong gọi API FreeRTOS
    thì phải dùng bản FromISR và DMA_IrqPriority phải >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4
    (thường là 5). Callback phải ngắn gọn, không được chặn hay delay.
    */

    typedef void (*Driver_DMA_Callback_t)(Driver_DMA_Channel_t Channel, Driver_DMA_Event_t Event, void *Arg);

    /*Cấu trúc khởi tạo cho một kênh DMA*/

    typedef struct
    {
        Driver_DMA_Channel_t DMA_Channel;
        Driver_DMA_Direction_t DMA_Direction;
        Driver_DMA_Mode_t DMA_Mode;
        Driver_DMA_Width_t DMA_PeripheralWidth;
        Driver_DMA_Increment_t DMA_PeripheralIncrement;
        Driver_DMA_Width_t DMA_MemoryWidth;
        Driver_DMA_Increment_t DMA_MemoryIncrement;
        Driver_DMA_Priority_t DMA_Priority;
        /*
        Mức ưu tiên ngắt (preemption priority).
        Với NVIC_PriorityGroup_4 giá trị 0..15
        */
        uint8_t DMA_IrqPriority;
    } Driver_DMA_Config_t;

    /*
    Các API của DMA.
    Lưu ý: bản thân driver chỉ điều khiển bộ DMA. Với truyền từ/đến peripheral, bạn còn phải
    bật yêu cầu DMA ở phía peripheral (ví dụ USART_DMACmd, SPI_I2S_DMACmd, ADC_DMACmd, I2C_DMACmd, TIM_DMACmd).
    */
    // Bật clock DMA1, cấu hình kênh (chưa chạy, cần gọi Driver_DMA_Start)
    Driver_DMA_Status_t Driver_DMA_Init(const Driver_DMA_Config_t *DMA_Config);
    // Tắt kênh, trả về trạng thái mặc định (tắt clock DMA1 khi không còn kênh nào dùng)
    Driver_DMA_Status_t Driver_DMA_DeInit(Driver_DMA_Channel_t DMA_Channel);
    /*
    Bắt đầu truyền.
        PeripheralAddress: địa chỉ thanh ghi của peripheral (ví dụ (uint32_t)&USART1->DR),
                           hoặc địa chỉ nguồn nếu là memory-to-memory
        MemoryAddress    : địa chỉ bộ đệm trong RAM, hoặc địa chỉ đích nếu là memory-to-memory
        Count            : số lần truyền, tính theo độ rộng PeripheralWidth (không phải số byte)
    Địa chỉ phải căn lề theo độ rộng dữ liệu tương ứng. Bộ đệm phải còn hợp lệ cho tới khi truyền xong.
    */
    Driver_DMA_Status_t Driver_DMA_Start(Driver_DMA_Channel_t DMA_Channel, uint32_t PeripheralAddress, uint32_t MemoryAddress, uint16_t Count);
    // Dừng truyền
    Driver_DMA_Status_t Driver_DMA_Stop(Driver_DMA_Channel_t DMA_Channel);
    // Đăng ký callback cho các sự kiện trong EventMask (OR của Driver_DMA_Event_t). Callback = 0 để tắt ngắt
    Driver_DMA_Status_t Driver_DMA_SetCallback(Driver_DMA_Channel_t DMA_Channel, Driver_DMA_Callback_t Callback, void *Arg, uint8_t EventMask);
    // Chờ truyền xong bằng cách đọc cờ (không dùng ngắt). Không dùng cho chế độ Circular
    Driver_DMA_Status_t Driver_DMA_WaitComplete(Driver_DMA_Channel_t DMA_Channel);
    // Kênh có đang truyền không (1: đang truyền, 0: rảnh hoặc đã xong)
    uint8_t Driver_DMA_IsBusy(Driver_DMA_Channel_t DMA_Channel);
    // Số phần tử còn lại chưa truyền. Ở chế độ Circular: vị trí ghi hiện tại = Count - số này
    uint16_t Driver_DMA_GetRemaining(Driver_DMA_Channel_t DMA_Channel);
    // Kênh đã được khởi tạo (đang bị một driver khác chiếm) chưa? 1: rồi, 0: còn trống
    uint8_t Driver_DMA_IsInUse(Driver_DMA_Channel_t DMA_Channel);
    // Đổi chế độ tự tăng địa chỉ bộ nhớ khi kênh đang rảnh (dùng cho dữ liệu giả: gửi 0xFF, bỏ dữ liệu nhận)
    Driver_DMA_Status_t Driver_DMA_SetMemoryIncrement(Driver_DMA_Channel_t DMA_Channel, Driver_DMA_Increment_t Increment);
#ifdef __cplusplus
}
#endif
#endif