#ifndef DRIVER_I2C_H
#define DRIVER_I2C_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include "stm32f10x.h"
#include "stm32f10x_i2c.h"
#include "stm32f10x_rcc.h"

    /*
    Số vòng lặp tối đa khi chờ một cờ I2C (timeout), có thể ghi đè bằng -D trong Makefile
    */
#ifndef DRIVER_I2C_TIMEOUT_LOOPS
#define DRIVER_I2C_TIMEOUT_LOOPS 100000UL
#endif

    // Số vòng lặp tối đa khi chờ một giao dịch DMA kết thúc (Driver_I2C_WaitComplete)
#ifndef DRIVER_I2C_DMA_TIMEOUT_LOOPS
#define DRIVER_I2C_DMA_TIMEOUT_LOOPS 20000000UL
#endif

    /*
    Trạng thái trả về của các hàm I2C
    */

    typedef enum
    {
        drvI2cOk = 0,  // Thành công
        drvI2cError,   // Tham số sai, I2C chưa khởi tạo, hoặc lỗi bus/DMA
        drvI2cTimeout, // Hết thời gian chờ (driver đã tự khôi phục bus)
        drvI2cNack,    // Slave không ACK (sai địa chỉ hoặc thiết bị không có mặt)
        drvI2cBusy     // Đang có một giao dịch DMA chưa xong
    } Driver_I2C_Status_t;

    /*
    Cổng I2C và chân tương ứng (STM32F103, không remap)
        Port1: SCL = PB6,  SDA = PB7                        DMA: TX = kênh 6, RX = kênh 7
        Port2: SCL = PB10, SDA = PB11 (trùng USART3)        DMA: TX = kênh 4, RX = kênh 5
    Chân chạy ở chế độ open-drain nên BẮT BUỘC có điện trở kéo lên ngoài (thường 4.7k)
    */

    typedef enum
    {
        drvI2cPort1 = 0,
        drvI2cPort2,
        drvI2cPortCount // Chỉ dùng để đếm, không truyền vào hàm
    } Driver_I2C_Port_t;

    /*
    Kích thước địa chỉ thanh ghi của slave (dùng cho MemRead / MemWrite)
    */

    typedef enum
    {
        drvI2cRegAddr8b = 0, // Địa chỉ thanh ghi 8 bit (cảm biến, RTC...)
        drvI2cRegAddr16b     // Địa chỉ thanh ghi 16 bit (EEPROM 24Cxx lớn...), gửi byte cao trước
    } Driver_I2C_RegAddrSize_t;

    /*
    Có dùng DMA không. Khi bật, driver chiếm 2 kênh DMA (TX và RX) của cổng này,
    đồng thời bật ngắt sự kiện (EV) và ngắt lỗi (ER) của I2C để kết thúc giao dịch DMA.
    Các hàm polling vẫn dùng được khi DMA rảnh
    */

    typedef enum
    {
        drvI2cDmaDisable = 0,
        drvI2cDmaEnable
    } Driver_I2C_DmaMode_t;

    /*
    Callback khi một giao dịch DMA kết thúc (đã phát STOP). Status: drvI2cOk, drvI2cNack hoặc drvI2cError.
    LƯU Ý: callback chạy trong ngữ cảnh NGẮT, nếu gọi API FreeRTOS phải dùng bản FromISR và
    I2C_IrqPriority phải >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4 (thường là 5).
    */

    typedef void (*Driver_I2C_Callback_t)(Driver_I2C_Port_t I2C_Port, Driver_I2C_Status_t Status, void *Arg);

    /*Cấu trúc khởi tạo cho I2C (chế độ master, địa chỉ 7 bit)*/

    typedef struct
    {
        Driver_I2C_Port_t I2C_Port;
        /*
        Tốc độ SCL (Hz), tối đa 400000.
        <= 100000: standard mode, > 100000: fast mode (nên có PCLK1 chia hết cho 10 MHz)
        */
        uint32_t I2C_ClockSpeed;
        // Hai trường dưới để 0 (mặc định) thì I2C chạy y như trước, không dùng DMA
        Driver_I2C_DmaMode_t I2C_DmaMode;
        // Mức ưu tiên ngắt I2C (EV, ER) và ngắt DMA. Với NVIC_PriorityGroup_4 giá trị 0..15
        uint8_t I2C_IrqPriority;
    } Driver_I2C_Config_t;

    /*
    Các API của I2C.
    DeviceAddress là địa chỉ 7 bit KHÔNG dịch (ví dụ MPU6050 = 0x68), driver tự thêm bit R/W.
    Driver không thread-safe: nếu nhiều task dùng chung một bus phải bảo vệ bằng mutex.
    */
    // Bật clock, cấu hình chân GPIO (AF open-drain) và peripheral I2C (và kênh DMA nếu bật)
    Driver_I2C_Status_t Driver_I2C_Init(const Driver_I2C_Config_t *I2C_Config);
    // Tắt I2C, trả chân GPIO về trạng thái mặc định, giải phóng kênh DMA
    Driver_I2C_Status_t Driver_I2C_DeInit(Driver_I2C_Port_t I2C_Port);
    // Gửi Length byte tới slave (START - địa chỉ - dữ liệu - STOP)
    Driver_I2C_Status_t Driver_I2C_Write(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, const uint8_t *Data, uint16_t Length);
    // Đọc Length byte từ slave (Length >= 1)
    Driver_I2C_Status_t Driver_I2C_Read(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint8_t *Data, uint16_t Length);
    // Ghi vào thanh ghi: START - địa chỉ - RegAddress - dữ liệu - STOP
    Driver_I2C_Status_t Driver_I2C_MemWrite(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, const uint8_t *Data, uint16_t Length);
    // Đọc từ thanh ghi: START - địa chỉ - RegAddress - repeated START - đọc dữ liệu - STOP
    Driver_I2C_Status_t Driver_I2C_MemRead(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, uint8_t *Data, uint16_t Length);
    // Kiểm tra thiết bị có ACK địa chỉ không. drvI2cOk: có, drvI2cNack: không có
    Driver_I2C_Status_t Driver_I2C_IsDeviceReady(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress);

    /*
    Các hàm DMA (cần I2C_DmaMode = drvI2cDmaEnable). Phần địa chỉ (START, địa chỉ slave, địa chỉ thanh ghi)
    được gửi ngay trong lời gọi hàm bằng polling, phần dữ liệu do DMA đảm nhiệm. Hàm trả về ngay sau khi
    DMA bắt đầu; kết thúc được báo bằng callback hoặc kiểm tra bằng IsBusy / WaitComplete.
    Bộ đệm phải còn hợp lệ cho tới khi giao dịch xong. Trả về drvI2cBusy nếu giao dịch trước chưa xong.
    */
    // Gửi Length byte bằng DMA (Length >= 1)
    Driver_I2C_Status_t Driver_I2C_WriteDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, const uint8_t *Data, uint16_t Length);
    // Đọc Length byte bằng DMA. Length >= 2 (đọc 1 byte hãy dùng Driver_I2C_Read)
    Driver_I2C_Status_t Driver_I2C_ReadDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint8_t *Data, uint16_t Length);
    // Ghi vào thanh ghi bằng DMA (Length >= 1)
    Driver_I2C_Status_t Driver_I2C_MemWriteDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, const uint8_t *Data, uint16_t Length);
    // Đọc từ thanh ghi bằng DMA. Length >= 2 (đọc 1 byte hãy dùng Driver_I2C_MemRead)
    Driver_I2C_Status_t Driver_I2C_MemReadDma(Driver_I2C_Port_t I2C_Port, uint8_t DeviceAddress, uint16_t RegAddress, Driver_I2C_RegAddrSize_t RegAddressSize, uint8_t *Data, uint16_t Length);
    // Đăng ký callback khi giao dịch DMA xong. Callback = 0 để bỏ đăng ký
    Driver_I2C_Status_t Driver_I2C_SetCallback(Driver_I2C_Port_t I2C_Port, Driver_I2C_Callback_t Callback, void *Arg);
    // 1: đang có giao dịch DMA chạy, 0: rảnh
    uint8_t Driver_I2C_IsBusy(Driver_I2C_Port_t I2C_Port);
    // Chờ giao dịch DMA xong và trả về kết quả của nó. Hết thời gian thì hủy giao dịch và trả về drvI2cTimeout
    Driver_I2C_Status_t Driver_I2C_WaitComplete(Driver_I2C_Port_t I2C_Port);
#ifdef __cplusplus
}
#endif
#endif