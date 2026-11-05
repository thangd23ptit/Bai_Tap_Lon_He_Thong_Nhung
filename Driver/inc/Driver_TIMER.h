#ifndef DRIVER_TIMER_H
#define DRIVER_TIMER_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
#include "stm32f10x.h"
#include "stm32f10x_tim.h"
#include "stm32f10x_rcc.h"

    /*
    Trạng thái trả về của các hàm Timer
    */

    typedef enum
    {
        drvTimerOk = 0, // Thành công
        drvTimerError   // Tham số sai hoặc Timer chưa được khởi tạo
    } Driver_Timer_Status_t;

    /*
    Các Timer đa dụng của STM32F103 (đều nằm trên APB1)
    Chân PWM mặc định (không remap):
        Timer2: CH1 = PA0, CH2 = PA1, CH3 = PA2, CH4 = PA3  (CH3/CH4 trùng chân USART2)
        Timer3: CH1 = PA6, CH2 = PA7, CH3 = PB0, CH4 = PB1
        Timer4: CH1 = PB6, CH2 = PB7, CH3 = PB8, CH4 = PB9
    Timer1 (advanced) chưa được hỗ trợ trong driver này
    */

    typedef enum
    {
        drvTimer2 = 0,
        drvTimer3,
        drvTimer4,
        drvTimerCount // Chỉ dùng để đếm, không truyền vào hàm
    } Driver_Timer_Port_t;

    /*
    Kênh so sánh (output compare) của Timer
    */

    typedef enum
    {
        drvTimerChannel1 = 0,
        drvTimerChannel2,
        drvTimerChannel3,
        drvTimerChannel4
    } Driver_Timer_Channel_t;

    /*
    Cực tính của PWM: duty được tính theo phần thời gian chân ở mức "active"
    */

    typedef enum
    {
        drvTimerPwmActiveHigh = 0, // Active = mức cao
        drvTimerPwmActiveLow       // Active = mức thấp
    } Driver_Timer_PwmPolarity_t;

    /*
    Hàm callback được gọi mỗi khi Timer tràn (hết một chu kỳ).
    LƯU Ý: callback chạy trong ngữ cảnh NGẮT. Nếu bên trong gọi API FreeRTOS
    thì phải dùng bản FromISR (xSemaphoreGiveFromISR, xQueueSendFromISR, ...)
    và Timer_IrqPriority phải >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY >> 4
    (thường là 5). Callback phải ngắn gọn, không được chặn hay delay.
    */

    typedef void (*Driver_Timer_Callback_t)(void *Arg);

    /*Cấu trúc khởi tạo cho Timer (cấu hình time base)*/

    typedef struct
    {
        Driver_Timer_Port_t Timer_Port;
        /*
        Chu kỳ tràn của Timer, tính bằng micro giây.
        Ví dụ 1000 = 1 ms. Giá trị tối đa phụ thuộc xung nhịp Timer
        (với 72 MHz khoảng 59 giây). Chu kỳ này cũng chính là chu kỳ của PWM
        trên mọi kênh của cùng Timer.
        */
        uint32_t Timer_PeriodUs;
        /*
        Mức ưu tiên ngắt (preemption priority).
        Với NVIC_PriorityGroup_4 giá trị 0..15
        */
        uint8_t Timer_IrqPriority;
    } Driver_Timer_Config_t;

    /*Cấu trúc khởi tạo cho một kênh PWM*/

    typedef struct
    {
        Driver_Timer_Port_t Timer_Port;
        Driver_Timer_Channel_t Timer_Channel;
        Driver_Timer_PwmPolarity_t Timer_PwmPolarity;
        uint16_t Timer_DutyPermille; // Độ rộng xung ban đầu, 0..1000 (1000 = 100%)
    } Driver_Timer_PwmConfig_t;

    /*Các API của Timer*/
    // Bật clock, cấu hình time base (chưa chạy, cần gọi Driver_Timer_Start)
    Driver_Timer_Status_t Driver_Timer_Init(const Driver_Timer_Config_t *Timer_Config);
    // Tắt Timer, trả các chân PWM đã dùng về trạng thái mặc định
    Driver_Timer_Status_t Driver_Timer_DeInit(Driver_Timer_Port_t Timer_Port);
    // Bắt đầu / dừng bộ đếm
    Driver_Timer_Status_t Driver_Timer_Start(Driver_Timer_Port_t Timer_Port);
    Driver_Timer_Status_t Driver_Timer_Stop(Driver_Timer_Port_t Timer_Port);
    // Đổi chu kỳ tràn, bộ đếm được đặt về 0. Giá trị duty đã đặt trước đó cần gọi lại PwmSetDuty
    Driver_Timer_Status_t Driver_Timer_SetPeriodUs(Driver_Timer_Port_t Timer_Port, uint32_t PeriodUs);
    // Đăng ký callback cho ngắt tràn. Truyền Callback = 0 để tắt ngắt
    Driver_Timer_Status_t Driver_Timer_SetCallback(Driver_Timer_Port_t Timer_Port, Driver_Timer_Callback_t Callback, void *Arg);
    // Đọc giá trị bộ đếm hiện tại
    uint16_t Driver_Timer_GetCounter(Driver_Timer_Port_t Timer_Port);
    // Tần số đếm thực tế của bộ đếm (Hz), mỗi tick = 1 / tần số này
    uint32_t Driver_Timer_GetTickFrequency(Driver_Timer_Port_t Timer_Port);
    // Cấu hình một kênh PWM (cấu hình chân GPIO, chế độ PWM mode 1). Timer phải đã Init
    Driver_Timer_Status_t Driver_Timer_PwmInit(const Driver_Timer_PwmConfig_t *PwmConfig);
    // Đổi độ rộng xung của kênh PWM, DutyPermille = 0..1000
    Driver_Timer_Status_t Driver_Timer_PwmSetDuty(Driver_Timer_Port_t Timer_Port, Driver_Timer_Channel_t Timer_Channel, uint16_t DutyPermille);
#ifdef __cplusplus
}
#endif
#endif