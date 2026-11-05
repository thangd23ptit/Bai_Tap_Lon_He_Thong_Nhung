#ifndef DRIVER_SPI_H
#define DRIVER_SPI_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include "stm32f10x.h"
#include "stm32f10x_spi.h"
#include "stm32f10x_rcc.h"

    /*
    Số vòng lặp tối đa khi chờ một cờ SPI (timeout), có thể ghi đè bằng -D trong Makefile
    */
#ifndef DRIVER_SPI_TIMEOUT_LOOPS
#define DRIVER_SPI_TIMEOUT_LOOPS 100000UL
#endif

    // Số vòng lặp tối đa khi chờ một lần truyền DMA kết thúc (Driver_SPI_WaitComplete)
#ifndef DRIVER_SPI_DMA_TIMEOUT_LOOPS
#define DRIVER_SPI_DMA_TIMEOUT_LOOPS 20000000UL
#endif

    /*
    Trạng thái trả về của các hàm SPI
    */

    typedef enum
    {
        drvSpiOk = 0,  // Thành công
        drvSpiError,   // Tham số sai hoặc SPI chưa được khởi tạo
        drvSpiTimeout, // Hết thời gian chờ
        drvSpiBusy     // Đang có một lần truyền DMA chưa xong
    } Driver_SPI_Status_t;

    /*
    Cổng SPI và chân tương ứng (STM32F103, không remap), chế độ master
        Port1 (APB2, tối đa 36 MHz): SCK = PA5,  MISO = PA6,  MOSI = PA7  (PA6/PA7 trùng TIM3 CH1/CH2)
                                     DMA: RX = kênh 2, TX = kênh 3
        Port2 (APB1, tối đa 18 MHz): SCK = PB13, MISO = PB14, MOSI = PB15
                                     DMA: RX = kênh 4, TX = kênh 5
    Chân CS (NSS) do người dùng tự điều khiển bằng Driver_GPIO
    */

    typedef enum
    {
        drvSpiPort1 = 0,
        drvSpiPort2,
        drvSpiPortCount // Chỉ dùng để đếm, không truyền vào hàm
    } Driver_SPI_Port_t;

    /*
    Chế độ SPI (CPOL / CPHA)
        Mode0: CPOL = 0, CPHA = 0 (xung nhịp nghỉ ở mức thấp, lấy mẫu cạnh lên)
        Mode1: CPOL = 0, CPHA = 1
        Mode2: CPOL = 1, CPHA = 0
        Mode3: CPOL = 1, CPHA = 1
    */

    typedef enum
    {
        drvSpiMode0 = 0,
        drvSpiMode1,
        drvSpiMode2,
        drvSpiMode3
    } Driver_SPI_Mode_t;

    /*
    Thứ tự bit
    */

    typedef enum
    {
        drvSpiMsbFirst = 0,
        drvSpiLsbFirst
    } Driver_SPI_BitOrder_t;

    /*
    Có dùng DMA không. Khi bật, driver chiếm 2 kênh DMA (RX và TX) của cổng này
    để dùng với Driver_SPI_TransferDma. Các hàm polling vẫn dùng được khi DMA rảnh
    */

    typedef enum
    {
        drvSpiDmaDisable = 0,
        drvSpiDmaEnable
    } Driver_SPI_DmaMode_t;

    /*
    Callback khi một lần truyền DMA kết thúc, Status là drvSpiOk hoặc drvSpiError.
    Callback được gọi khi DMA nhận đã đủ dữ liệu; bit BSY có thể còn vài chu kỳ nữa mới xóa,
    nếu cần nhả CS an toàn hãy gọi Driver_SPI_WaitComplete (hoặc chờ BSY) trước khi nhả.
    LƯU Ý: callback chạy trong ngữ cảnh NGẮT, nếu gọi API FreeRTOS phải dùng bản FromISR và
    SPI_DmaIrqPriority phải >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4 (thường là 5).
    */

    typedef void (*Driver_SPI_Callback_t)(Driver_SPI_Port_t SPI_Port, Driver_SPI_Status_t Status, void *Arg);

    /*Cấu trúc khởi tạo cho SPI (master, full-duplex, 8 bit dữ liệu)*/

    typedef struct
    {
        Driver_SPI_Port_t SPI_Port;
        Driver_SPI_Mode_t SPI_Mode;
        /*
        Tốc độ SCK mong muốn (Hz). Driver chọn hệ số chia nhỏ nhất sao cho
        tốc độ thực tế KHÔNG vượt quá giá trị này (chia 2, 4, ... 256)
        */
        uint32_t SPI_BaudRate;
        Driver_SPI_BitOrder_t SPI_BitOrder;
        // Hai trường dưới để 0 (mặc định) thì SPI chạy y như trước, không dùng DMA
        Driver_SPI_DmaMode_t SPI_DmaMode;
        // Mức ưu tiên ngắt DMA. Với NVIC_PriorityGroup_4 giá trị 0..15
        uint8_t SPI_DmaIrqPriority;
    } Driver_SPI_Config_t;

    /*
    Các API của SPI.
    Driver không thread-safe: nếu nhiều task dùng chung một bus phải bảo vệ bằng mutex.
    */
    // Bật clock, cấu hình chân GPIO và peripheral SPI (và kênh DMA nếu bật)
    Driver_SPI_Status_t Driver_SPI_Init(const Driver_SPI_Config_t *SPI_Config);
    // Tắt SPI, trả chân GPIO về trạng thái mặc định, giải phóng kênh DMA
    Driver_SPI_Status_t Driver_SPI_DeInit(Driver_SPI_Port_t SPI_Port);
    // Đổi tốc độ SCK khi chạy (dùng cho bus có nhiều thiết bị khác tốc độ)
    Driver_SPI_Status_t Driver_SPI_SetBaudRate(Driver_SPI_Port_t SPI_Port, uint32_t BaudRate);
    // Tốc độ SCK thực tế đang dùng (Hz)
    uint32_t Driver_SPI_GetBaudRate(Driver_SPI_Port_t SPI_Port);
    // Gửi 1 byte và nhận 1 byte cùng lúc. RxData có thể là 0 nếu không cần
    Driver_SPI_Status_t Driver_SPI_TransferByte(Driver_SPI_Port_t SPI_Port, uint8_t TxData, uint8_t *RxData);
    // Gửi/nhận Length byte. TxData = 0: gửi 0xFF. RxData = 0: bỏ dữ liệu nhận
    Driver_SPI_Status_t Driver_SPI_Transfer(Driver_SPI_Port_t SPI_Port, const uint8_t *TxData, uint8_t *RxData, uint16_t Length);
    // Chỉ gửi
    Driver_SPI_Status_t Driver_SPI_Write(Driver_SPI_Port_t SPI_Port, const uint8_t *TxData, uint16_t Length);
    // Chỉ nhận (gửi 0xFF làm dữ liệu giả)
    Driver_SPI_Status_t Driver_SPI_Read(Driver_SPI_Port_t SPI_Port, uint8_t *RxData, uint16_t Length);
    /*
    Gửi/nhận Length byte bằng DMA và trả về ngay (cần SPI_DmaMode = drvSpiDmaEnable).
    TxData = 0: gửi 0xFF. RxData = 0: bỏ dữ liệu nhận. Các bộ đệm phải còn hợp lệ cho tới khi xong.
    Trả về drvSpiBusy nếu lần truyền trước chưa xong.
    */
    Driver_SPI_Status_t Driver_SPI_TransferDma(Driver_SPI_Port_t SPI_Port, const uint8_t *TxData, uint8_t *RxData, uint16_t Length);
    // Đăng ký callback khi truyền DMA xong. Callback = 0 để bỏ đăng ký
    Driver_SPI_Status_t Driver_SPI_SetCallback(Driver_SPI_Port_t SPI_Port, Driver_SPI_Callback_t Callback, void *Arg);
    // 1: đang có lần truyền DMA chạy, 0: rảnh
    uint8_t Driver_SPI_IsBusy(Driver_SPI_Port_t SPI_Port);
    // Chờ lần truyền DMA xong và bit BSY xóa (an toàn để nhả CS). Trả về kết quả của lần truyền đó.
    // Hết thời gian thì hủy lần truyền và trả về drvSpiTimeout
    Driver_SPI_Status_t Driver_SPI_WaitComplete(Driver_SPI_Port_t SPI_Port);
#ifdef __cplusplus
}
#endif
#endif