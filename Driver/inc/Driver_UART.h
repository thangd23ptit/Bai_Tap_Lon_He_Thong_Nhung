#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include "stm32f10x.h"
#include "stm32f10x_usart.h"
#include "stm32f10x_rcc.h"

    /*
    Cấu hình mặc định, có thể ghi đè bằng -D trong Makefile
    */

    // Kích thước bộ đệm nhận (ring buffer) cho mỗi cổng UART. PHẢI là lũy thừa của 2
#ifndef DRIVER_UART_RX_BUFFER_SIZE
#define DRIVER_UART_RX_BUFFER_SIZE 128U
#endif

    // Số vòng lặp tối đa khi chờ cờ TXE/TC (timeout khi gửi dữ liệu bằng polling)
#ifndef DRIVER_UART_TIMEOUT_LOOPS
#define DRIVER_UART_TIMEOUT_LOOPS 100000UL
#endif

    // Số vòng lặp tối đa khi chờ gửi xong bằng Driver_UART_WaitTxComplete (gửi DMA có thể kéo dài lâu)
#ifndef DRIVER_UART_DMA_WAIT_LOOPS
#define DRIVER_UART_DMA_WAIT_LOOPS 20000000UL
#endif

    /*
    Trạng thái trả về của các hàm UART
    */

    typedef enum
    {
        drvUartOk = 0,  // Thành công
        drvUartError,   // Tham số sai hoặc UART chưa được khởi tạo
        drvUartTimeout, // Hết thời gian chờ
        drvUartEmpty,   // Bộ đệm nhận rỗng
        drvUartBusy     // Đang có một lần gửi DMA chưa xong
    } Driver_UART_Status_t;

    /*
    Cổng UART và chân tương ứng (STM32F103, không remap)
        Port1: TX = PA9,  RX = PA10  (USART1)  DMA: TX = kênh 4, RX = kênh 5
        Port2: TX = PA2,  RX = PA3   (USART2)  DMA: TX = kênh 7, RX = kênh 6
        Port3: TX = PB10, RX = PB11  (USART3)  DMA: TX = kênh 2, RX = kênh 3
    */

    typedef enum
    {
        drvUartPort1 = 0,
        drvUartPort2,
        drvUartPort3,
        drvUartPortCount // Chỉ dùng để đếm, không truyền vào hàm
    } Driver_UART_Port_t;

    /*
    Số bit dữ liệu
    Lưu ý: khi dùng Parity thì bit parity nằm trong khung truyền,
    muốn 8 bit dữ liệu + parity thì chọn drvUartWord9b
    */

    typedef enum
    {
        drvUartWord8b = 0,
        drvUartWord9b
    } Driver_UART_WordLength_t;

    /*
    Số bit Stop
    */

    typedef enum
    {
        drvUartStop1 = 0,
        drvUartStop2
    } Driver_UART_StopBits_t;

    /*
    Chế độ Parity
    */

    typedef enum
    {
        drvUartParityNone = 0,
        drvUartParityEven,
        drvUartParityOdd
    } Driver_UART_Parity_t;

    /*
    Cách nhận dữ liệu
        Interrupt: ngắt RXNE, mỗi byte một ngắt, có đếm byte bị mất khi bộ đệm đầy
        Dma      : DMA chạy vòng (circular) ghi thẳng vào bộ đệm, không tốn ngắt nào cho mỗi byte.
                   Không phát hiện được tràn bộ đệm: nếu không đọc kịp trong một vòng thì dữ liệu cũ bị ghi đè,
                   nên chọn DRIVER_UART_RX_BUFFER_SIZE đủ lớn. Driver_UART_GetRxDropped luôn trả về 0
    */

    typedef enum
    {
        drvUartRxInterrupt = 0,
        drvUartRxDma
    } Driver_UART_RxMode_t;

    /*
    Cách gửi dữ liệu
        Polling: Driver_UART_Send... chờ từng byte (chiếm CPU)
        Dma    : dùng thêm Driver_UART_SendDma để gửi không chờ. Các hàm polling vẫn dùng được khi DMA rảnh
    */

    typedef enum
    {
        drvUartTxPolling = 0,
        drvUartTxDma
    } Driver_UART_TxMode_t;

    /*
    Callback khi một lần gửi DMA kết thúc, Status là drvUartOk hoặc drvUartError.
    Khi callback được gọi, DMA đã lấy hết dữ liệu nên bộ đệm có thể dùng lại. Byte cuối có thể
    vẫn đang được dịch ra chân, dùng Driver_UART_IsTxBusy nếu cần chắc chắn đã gửi xong hoàn toàn.
    LƯU Ý: callback chạy trong ngữ cảnh NGẮT, nếu gọi API FreeRTOS phải dùng bản FromISR và
    UART_IrqPriority phải >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4 (thường là 5).
    */

    typedef void (*Driver_UART_TxCallback_t)(Driver_UART_Port_t UART_Port, Driver_UART_Status_t Status, void *Arg);

    /*Cấu trúc khởi tạo cho UART*/

    typedef struct
    {
        Driver_UART_Port_t UART_Port;
        uint32_t UART_BaudRate;
        Driver_UART_WordLength_t UART_WordLength;
        Driver_UART_StopBits_t UART_StopBits;
        Driver_UART_Parity_t UART_Parity;
        /*
        Mức ưu tiên ngắt (preemption priority) của ngắt nhận / ngắt DMA.
        Với NVIC_PriorityGroup_4 giá trị 0..15. ISR của driver không gọi API FreeRTOS
        nên mức nào cũng chạy được, nhưng nên chọn >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4
        (thường là 5) để sau này mở rộng dùng API FromISR không bị lỗi.
        */
        uint8_t UART_IrqPriority;
        // Hai trường dưới để 0 (mặc định) thì UART chạy y như trước: nhận bằng ngắt, gửi bằng polling
        Driver_UART_RxMode_t UART_RxMode;
        Driver_UART_TxMode_t UART_TxMode;
    } Driver_UART_Config_t;

    /*Các API của UART*/
    // Bật clock, cấu hình chân GPIO, cấu hình USART và bật nhận (ngắt hoặc DMA)
    Driver_UART_Status_t Driver_UART_Init(const Driver_UART_Config_t *UART_Config);
    // Tắt UART, trả chân GPIO về trạng thái mặc định, giải phóng kênh DMA
    Driver_UART_Status_t Driver_UART_DeInit(Driver_UART_Port_t UART_Port);
    // Gửi 1 byte (chờ thanh ghi truyền rỗng, có timeout). Trả về drvUartBusy nếu đang gửi DMA
    Driver_UART_Status_t Driver_UART_SendByte(Driver_UART_Port_t UART_Port, uint8_t Data);
    // Gửi một mảng byte, chờ đến khi truyền xong hoàn toàn
    Driver_UART_Status_t Driver_UART_Send(Driver_UART_Port_t UART_Port, const uint8_t *Data, uint16_t Length);
    // Gửi chuỗi ký tự kết thúc bằng '\0' (không gửi ký tự '\0')
    Driver_UART_Status_t Driver_UART_SendString(Driver_UART_Port_t UART_Port, const char *String);
    // Gửi Length byte bằng DMA và trả về ngay (cần UART_TxMode = drvUartTxDma).
    // Bộ đệm Data phải còn hợp lệ cho tới khi gửi xong. Trả về drvUartBusy nếu lần gửi trước chưa xong
    Driver_UART_Status_t Driver_UART_SendDma(Driver_UART_Port_t UART_Port, const uint8_t *Data, uint16_t Length);
    // Đăng ký callback khi gửi DMA xong. Callback = 0 để bỏ đăng ký
    Driver_UART_Status_t Driver_UART_SetTxCallback(Driver_UART_Port_t UART_Port, Driver_UART_TxCallback_t Callback, void *Arg);
    // 1: đang gửi (DMA đang chạy hoặc byte cuối chưa ra hết khỏi chân), 0: rảnh
    uint8_t Driver_UART_IsTxBusy(Driver_UART_Port_t UART_Port);
    // Chờ gửi xong hoàn toàn (có timeout)
    Driver_UART_Status_t Driver_UART_WaitTxComplete(Driver_UART_Port_t UART_Port);
    // Số byte đang chờ trong bộ đệm nhận
    uint16_t Driver_UART_Available(Driver_UART_Port_t UART_Port);
    // Lấy 1 byte từ bộ đệm nhận (không chờ). Trả về drvUartEmpty nếu rỗng
    Driver_UART_Status_t Driver_UART_ReadByte(Driver_UART_Port_t UART_Port, uint8_t *Data);
    // Lấy tối đa MaxLength byte từ bộ đệm nhận (không chờ), trả về số byte đã lấy
    uint16_t Driver_UART_Read(Driver_UART_Port_t UART_Port, uint8_t *Buffer, uint16_t MaxLength);
    // Xóa toàn bộ dữ liệu trong bộ đệm nhận
    void Driver_UART_FlushRx(Driver_UART_Port_t UART_Port);
    // Số byte nhận bị mất do bộ đệm đầy (chỉ có nghĩa ở chế độ nhận bằng ngắt)
    uint32_t Driver_UART_GetRxDropped(Driver_UART_Port_t UART_Port);
#ifdef __cplusplus
}
#endif
#endif