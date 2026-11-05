#ifndef DRIVER_ADC_H
#define DRIVER_ADC_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include "stm32f10x.h"
#include "stm32f10x_adc.h"
#include "stm32f10x_rcc.h"

    /*
    Số vòng lặp tối đa khi chờ ADC (timeout), có thể ghi đè bằng -D trong Makefile
    */
#ifndef DRIVER_ADC_TIMEOUT_LOOPS
#define DRIVER_ADC_TIMEOUT_LOOPS 100000UL
#endif

    /*
    Kênh ADC (STM32F103) và chân tương ứng:
        Kênh 0..7   : PA0..PA7
        Kênh 8..9   : PB0..PB1
        Kênh 10..15 : PC0..PC5   (chỉ có trên chip từ 64 chân trở lên, không có trên Blue Pill)
        Kênh 16     : cảm biến nhiệt độ nội (chỉ ADC1)
        Kênh 17     : điện áp tham chiếu nội Vrefint (chỉ ADC1)
    Chân PA0..PA3 trùng TIM2, PA2/PA3 trùng USART2, PA5..PA7 trùng SPI1, PA6/PA7 và PB0/PB1 trùng TIM3
    */
#define DRIVER_ADC_CHANNEL_TEMP 16U
#define DRIVER_ADC_CHANNEL_VREF 17U

    // Tạo bit trong ADC_ChannelMask cho kênh Channel, ví dụ DRIVER_ADC_CHANNEL_BIT(0) | DRIVER_ADC_CHANNEL_BIT(1)
#define DRIVER_ADC_CHANNEL_BIT(Channel) (1UL << (Channel))

    // Số kênh tối đa trong một chuỗi quét bằng DMA
#define DRIVER_ADC_MAX_SCAN_CHANNELS 16U

    /*
    Trạng thái trả về của các hàm ADC
    */

    typedef enum
    {
        drvAdcOk = 0,  // Thành công
        drvAdcError,   // Tham số sai, kênh chưa đăng ký hoặc ADC chưa được khởi tạo
        drvAdcTimeout, // Hết thời gian chờ
        drvAdcBusy     // Đang quét bằng DMA nên không đọc đơn lẻ được
    } Driver_ADC_Status_t;

    /*
    Bộ ADC (ADC3 trên dòng high-density chưa được hỗ trợ).
    Chỉ ADC1 dùng được DMA (kênh DMA 1) trên dòng STM32F103 medium-density
    */

    typedef enum
    {
        drvAdc1 = 0,
        drvAdc2,
        drvAdcCount // Chỉ dùng để đếm, không truyền vào hàm
    } Driver_ADC_Port_t;

    /*
    Thời gian lấy mẫu (chu kỳ xung ADC). Nguồn tín hiệu trở kháng cao
    hoặc cảm biến nhiệt độ nội cần thời gian lấy mẫu dài
    */

    typedef enum
    {
        drvAdcSampleTime1p5 = 0,
        drvAdcSampleTime7p5,
        drvAdcSampleTime13p5,
        drvAdcSampleTime28p5,
        drvAdcSampleTime41p5,
        drvAdcSampleTime55p5,
        drvAdcSampleTime71p5,
        drvAdcSampleTime239p5
    } Driver_ADC_SampleTime_t;

    /*Cấu trúc khởi tạo cho ADC (đọc từng kênh bằng software trigger, kết quả căn phải 12 bit)*/

    typedef struct
    {
        Driver_ADC_Port_t ADC_Port;
        /*
        Mặt nạ các kênh sẽ dùng. Chân GPIO của các kênh 0..15 được cấu hình analog,
        kênh 16/17 sẽ bật cảm biến nhiệt độ / Vrefint
        */
        uint32_t ADC_ChannelMask;
        Driver_ADC_SampleTime_t ADC_SampleTime;
    } Driver_ADC_Config_t;

    /*
    Chế độ quét bằng DMA
        Once    : quét đủ ADC_BufferLength mẫu rồi tự dừng
        Circular: quét liên tục, DMA ghi vòng quanh bộ đệm cho tới khi gọi Driver_ADC_StopDma
    */

    typedef enum
    {
        drvAdcDmaOnce = 0,
        drvAdcDmaCircular
    } Driver_ADC_DmaMode_t;

    /*
    Sự kiện của quét DMA. Với chế độ Circular, hai sự kiện Half và Complete cho phép xử lý
    kiểu ping-pong: nửa đầu bộ đệm an toàn để đọc khi có Complete, nửa sau khi có Half.
    */

    typedef enum
    {
        drvAdcDmaEventHalf = 0, // Đã ghi xong nửa đầu bộ đệm
        drvAdcDmaEventComplete, // Đã ghi xong toàn bộ bộ đệm (hết một vòng ở chế độ Circular)
        drvAdcDmaEventError     // Lỗi DMA, quét đã dừng
    } Driver_ADC_DmaEvent_t;

    /*
    Callback của quét DMA.
    LƯU Ý: callback chạy trong ngữ cảnh NGẮT, nếu gọi API FreeRTOS phải dùng bản FromISR và
    ADC_DmaIrqPriority phải >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4 (thường là 5).
    */

    typedef void (*Driver_ADC_DmaCallback_t)(Driver_ADC_Port_t ADC_Port, Driver_ADC_DmaEvent_t Event, void *Arg);

    /*
    Cấu hình quét nhiều kênh bằng DMA. Mỗi vòng quét đọc lần lượt các kênh trong ADC_Channels
    (đã được đăng ký trong ADC_ChannelMask lúc Init), kết quả ghi liên tiếp vào ADC_Buffer:
        ADC_Buffer = { vòng0[kênh0, kênh1, ...], vòng1[kênh0, kênh1, ...], ... }
    */

    typedef struct
    {
        Driver_ADC_Port_t ADC_Port;  // Chỉ drvAdc1
        const uint8_t *ADC_Channels; // Thứ tự các kênh cần quét
        uint8_t ADC_ChannelCount;    // 1 .. DRIVER_ADC_MAX_SCAN_CHANNELS
        uint16_t *ADC_Buffer;        // Bộ đệm nhận kết quả (giá trị thô 12 bit trong uint16_t)
        uint16_t ADC_BufferLength;   // Số mẫu của bộ đệm, phải là bội số của ADC_ChannelCount
        Driver_ADC_DmaMode_t ADC_DmaMode;
        uint8_t ADC_DmaIrqPriority; // Mức ưu tiên ngắt DMA. Với NVIC_PriorityGroup_4 giá trị 0..15
    } Driver_ADC_DmaConfig_t;

    /*
    Các API của ADC.
    Driver không thread-safe: nếu nhiều task cùng đọc một bộ ADC phải bảo vệ bằng mutex.
    */
    // Bật clock, cấu hình chân analog, cấu hình ADC và hiệu chuẩn (calibration)
    Driver_ADC_Status_t Driver_ADC_Init(const Driver_ADC_Config_t *ADC_Config);
    // Tắt ADC (dừng quét DMA nếu đang chạy), trả các chân GPIO về trạng thái mặc định
    Driver_ADC_Status_t Driver_ADC_DeInit(Driver_ADC_Port_t ADC_Port);
    // Đọc một lần, giá trị thô 12 bit (0..4095). Trả về drvAdcBusy nếu đang quét DMA
    Driver_ADC_Status_t Driver_ADC_Read(Driver_ADC_Port_t ADC_Port, uint8_t Channel, uint16_t *Value);
    // Đọc Samples lần liên tiếp và lấy trung bình (giảm nhiễu)
    Driver_ADC_Status_t Driver_ADC_ReadAverage(Driver_ADC_Port_t ADC_Port, uint8_t Channel, uint16_t Samples, uint16_t *Value);
    // Đọc và đổi ra mV. VrefMillivolt là điện áp VDDA thực tế, ví dụ 3300
    Driver_ADC_Status_t Driver_ADC_ReadMillivolt(Driver_ADC_Port_t ADC_Port, uint8_t Channel, uint32_t VrefMillivolt, uint32_t *Millivolt);

    // Bắt đầu quét nhiều kênh liên tục bằng DMA và trả về ngay
    Driver_ADC_Status_t Driver_ADC_StartDma(const Driver_ADC_DmaConfig_t *ADC_DmaConfig);
    // Dừng quét DMA, trả ADC về chế độ đọc đơn lẻ và giải phóng kênh DMA
    Driver_ADC_Status_t Driver_ADC_StopDma(Driver_ADC_Port_t ADC_Port);
    // Đăng ký callback cho quét DMA (có thể gọi trước hoặc sau StartDma). Callback = 0 để bỏ đăng ký
    Driver_ADC_Status_t Driver_ADC_SetDmaCallback(Driver_ADC_Port_t ADC_Port, Driver_ADC_DmaCallback_t Callback, void *Arg);
    // 1: đang quét DMA, 0: rảnh (ở chế độ Once là đã quét xong)
    uint8_t Driver_ADC_IsDmaRunning(Driver_ADC_Port_t ADC_Port);
#ifdef __cplusplus
}
#endif
#endif