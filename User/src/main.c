
#include "stm32f10x.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdint.h>

/* =========================================================
 * 1. DINH NGHIA CHAN GPIO
 * ========================================================= */

/* Nut chon LED: PB12 */
#define BTN_SELECT GPIO_Pin_12

/* Nut tang do sang: PB13 */
#define BTN_UP GPIO_Pin_13

/* Nut giam do sang: PB15 */
#define BTN_DOWN GPIO_Pin_15

/* Do sang toi da la 100% */
#define PWM_MAX 100U

/* Moi lan nhan nut tang/giam 10% */
#define PWM_STEP 10U

/* =========================================================
 * 2. DINH NGHIA SU KIEN VA DU LIEU QUEUE
 * ========================================================= */

/* Cac loai su kien do TaskButton gui di */
typedef enum
{
    EVT_UP = 1, /* Su kien tang do sang */
    EVT_DOWN    /* Su kien giam do sang */
} EventType_t;

/* Cau truc du lieu truyen qua Queue */
typedef struct
{
    uint8_t type; /* Loai su kien: EVT_UP hoac EVT_DOWN */
    uint8_t led;  /* LED can dieu khien: 0, 1 hoac 2 */
} LEDEvent_t;

/* =========================================================
 * 3. KHAI BAO BIEN TOAN CUC
 * ========================================================= */

/* Queue duy nhat dung de truyen su kien tu TaskButton */
static QueueHandle_t qLED = NULL;

/* Handle cua TaskLED2 va TaskLED3 de gui Task Notification */
static TaskHandle_t taskLED2Handle = NULL;
static TaskHandle_t taskLED3Handle = NULL;

/* Mang luu do sang hien tai cua tung LED, don vi % */
static uint8_t brightness[3] = {0U, 0U, 0U};

/* LED dang duoc chon:
 * 0 = LED1, 1 = LED2, 2 = LED3
 */
static uint8_t selectedLED = 0U;

/* =========================================================
 * 4. KHOI TAO PWM CHO BA LED
 * ========================================================= */

static void PWM_Init(void)
{
    GPIO_InitTypeDef gpio;
    TIM_TimeBaseInitTypeDef timer;
    TIM_OCInitTypeDef oc;

    /* Cap clock cho GPIOA va GPIOB */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA |
                               RCC_APB2Periph_GPIOB,
                           ENABLE);

    /* Cap clock cho Timer 3 */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    /* PA6 = TIM3_CH1, PA7 = TIM3_CH2 */
    gpio.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP; /* Ngo ra alternate function */
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* PB0 = TIM3_CH3 */
    gpio.GPIO_Pin = GPIO_Pin_0;
    GPIO_Init(GPIOB, &gpio);

    /* Cau hinh bo dem TIM3:
     * Timer clock = 72MHz
     * PSC = 71 -> tan so dem = 72MHz / 72 = 1MHz
     * ARR = 999 -> PWM = 1MHz / 1000 = 1kHz
     */
    timer.TIM_Prescaler = 71U;
    timer.TIM_Period = 999U;
    timer.TIM_CounterMode = TIM_CounterMode_Up;
    timer.TIM_ClockDivision = TIM_CKD_DIV1;
    timer.TIM_RepetitionCounter = 0U;
    TIM_TimeBaseInit(TIM3, &timer);

    /* Cau hinh PWM mode 1 cho ca ba kenh */
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;

    /* Ban dau xung PWM bang 0, LED tat */
    oc.TIM_Pulse = 0U;

    /* Khoi tao ba kenh PWM */
    TIM_OC1Init(TIM3, &oc);
    TIM_OC2Init(TIM3, &oc);
    TIM_OC3Init(TIM3, &oc);

    /* Cho phep preload gia tri CCR */
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);

    /* Cho phep preload thanh ghi ARR */
    TIM_ARRPreloadConfig(TIM3, ENABLE);

    /* Bat Timer 3 */
    TIM_Cmd(TIM3, ENABLE);
}

/* =========================================================
 * 5. KHOI TAO BA NUT NHAN
 * ========================================================= */

static void Button_Init(void)
{
    GPIO_InitTypeDef gpio;

    /* Cap clock cho GPIOB */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    /* Cau hinh PB12, PB13, PB15 la ngo vao keo len noi */
    gpio.GPIO_Pin = BTN_SELECT | BTN_UP | BTN_DOWN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOB, &gpio);

    /* Khi tha nut: muc logic 1
     * Khi nhan nut: chan GPIO bi keo xuong muc 0
     */
}

/* =========================================================
 * 6. HAM CAP NHAT DO SANG LED
 * ========================================================= */

static void PWM_SetBrightness(uint8_t led, uint8_t percent)
{
    uint16_t compare;

    /* Gioi han do sang trong khoang 0-100% */
    if (percent > PWM_MAX)
    {
        percent = PWM_MAX;
    }

    /* Chuyen phan tram sang gia tri CCR:
     * 0%   -> CCR = 0
     * 50%  -> CCR = 500
     * 100% -> CCR = 1000
     */
    compare = ((uint32_t)percent * 1000U) / PWM_MAX;

    /* Cap nhat dung kenh PWM cua LED */
    switch (led)
    {
    case 0U:
        TIM_SetCompare1(TIM3, compare); /* LED1 - PA6 */
        break;

    case 1U:
        TIM_SetCompare2(TIM3, compare); /* LED2 - PA7 */
        break;

    case 2U:
        TIM_SetCompare3(TIM3, compare); /* LED3 - PB0 */
        break;

    default:
        /* Khong lam gi neu chi so LED khong hop le */
        break;
    }
}

/* =========================================================
 * 7. HAM GUI SU KIEN VAO QUEUE
 * ========================================================= */

static void SendEvent(uint8_t type, uint8_t led)
{
    LEDEvent_t event;

    /* Dong goi loai su kien va LED duoc chon */
    event.type = type;
    event.led = led;

    /* Gui su kien vao Queue duy nhat.
     * Neu Queue day, TaskButton se cho den khi co cho trong.
     */
    xQueueSend(qLED, &event, portMAX_DELAY);
}

/* =========================================================
 * 8. HAM CHUYEN SU KIEN DEN TASK LED
 * ========================================================= */

static void NotifyLEDTask(TaskHandle_t handle, LEDEvent_t event)
{
    uint32_t value;

    /* Dong goi loai su kien va chi so LED vao mot so 32-bit */
    value = ((uint32_t)event.type << 8U) | event.led;

    /* Gui notification cho TaskLED2 hoac TaskLED3.
     * Neu Task dich dang co notification chua xu ly,
     * cho mot tick roi thu gui lai de tranh mat su kien.
     */
    while (xTaskNotify(handle, value, eSetValueWithoutOverwrite) != pdPASS)
    {
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}

/* =========================================================
 * 9. TASK BUTTON - DOC VA XU LY BA NUT NHAN
 * ========================================================= */

static void TaskButton(void *argument)
{
    /* Mang chua chan GPIO cua ba nut */
    const uint16_t pins[3] = {
        BTN_SELECT,
        BTN_UP,
        BTN_DOWN};

    /* Gia tri doc duoc o lan lay mau truoc */
    uint8_t last[3] = {1U, 1U, 1U};

    /* Trang thai da xac nhan sau chong doi */
    uint8_t stable[3] = {1U, 1U, 1U};

    /* Bo dem so lan lay mau lien tiep giong nhau */
    uint8_t count[3] = {0U, 0U, 0U};

    uint8_t sample;
    uint8_t i;

    (void)argument;

    for (;;)
    {
        /* Kiem tra lan luot ca ba nut nhan */
        for (i = 0U; i < 3U; i++)
        {
            /* Doc trang thai GPIO:
             * 0 = nhan nut, 1 = tha nut
             */
            sample = (GPIO_ReadInputDataBit(GPIOB, pins[i]) == Bit_RESET) ? 0U : 1U;

            /* Neu gia tri thay doi thi bat dau dem lai */
            if (sample != last[i])
            {
                last[i] = sample;
                count[i] = 0U;
            }
            else if (count[i] < 2U)
            {
                /* Tang bo dem neu gia tri van on dinh */
                count[i]++;
            }

            /* Chi chap nhan khi da lay mau on dinh */
            if ((count[i] >= 2U) && (sample != stable[i]))
            {
                stable[i] = sample;

                /* Chi xu ly tai canh nhan nut */
                if (stable[i] == 0U)
                {
                    if (i == 0U)
                    {
                        /* Nut PB12:
                         * Chuyen LED dang duoc dieu khien.
                         * LED1 -> LED2 -> LED3 -> LED1
                         */
                        selectedLED = (selectedLED + 1U) % 3U;
                    }
                    else if (i == 1U)
                    {
                        /* Nut PB13:
                         * Gui su kien tang do sang LED dang chon.
                         */
                        SendEvent(EVT_UP, selectedLED);
                    }
                    else
                    {
                        /* Nut PB15:
                         * Gui su kien giam do sang LED dang chon.
                         */
                        SendEvent(EVT_DOWN, selectedLED);
                    }
                }
            }
        }

        /* Lay mau nut moi 10ms de chong doi */
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

/* =========================================================
 * 10. TASK LED1 - NHAN QUEUE VA PHAN PHOI SU KIEN
 * ========================================================= */

static void TaskLED1(void *argument)
{
    LEDEvent_t event;

    (void)argument;

    for (;;)
    {
        /* Cho su kien moi tu TaskButton */
        if (xQueueReceive(qLED, &event, portMAX_DELAY) == pdPASS)
        {
            /* Neu su kien thuoc LED1 thi xu ly truc tiep */
            if (event.led == 0U)
            {
                if (event.type == EVT_UP && brightness[0] < PWM_MAX)
                {
                    /* Tang do sang LED1, toi da 100% */
                    brightness[0] += PWM_STEP;
                }
                else if (event.type == EVT_DOWN && brightness[0] >= PWM_STEP)
                {
                    /* Giam do sang LED1, toi thieu 0% */
                    brightness[0] -= PWM_STEP;
                }

                /* Cap nhat PWM LED1 */
                PWM_SetBrightness(0U, brightness[0]);
            }
            else if (event.led == 1U)
            {
                /* Su kien thuoc LED2:
                 * Chuyen tiep den TaskLED2 bang notification.
                 */
                NotifyLEDTask(taskLED2Handle, event);
            }
            else if (event.led == 2U)
            {
                /* Su kien thuoc LED3:
                 * Chuyen tiep den TaskLED3 bang notification.
                 */
                NotifyLEDTask(taskLED3Handle, event);
            }
        }
    }
}

/* =========================================================
 * 11. TASK LED2 - DIEU KHIEN LED2 DOC LAP
 * ========================================================= */

static void TaskLED2(void *argument)
{
    uint32_t value;
    uint8_t type;

    (void)argument;

    for (;;)
    {
        /* Cho notification tu TaskLED1 */
        if (xTaskNotifyWait(0U, 0xFFFFFFFFUL, &value, portMAX_DELAY) == pdPASS)
        {
            /* Tach loai su kien tu gia tri notification */
            type = (uint8_t)((value >> 8U) & 0xFFU);

            if (type == EVT_UP && brightness[1] < PWM_MAX)
            {
                /* Tang do sang LED2 */
                brightness[1] += PWM_STEP;
            }
            else if (type == EVT_DOWN && brightness[1] >= PWM_STEP)
            {
                /* Giam do sang LED2 */
                brightness[1] -= PWM_STEP;
            }

            /* Cap nhat PWM LED2 */
            PWM_SetBrightness(1U, brightness[1]);
        }
    }
}

/* =========================================================
 * 12. TASK LED3 - DIEU KHIEN LED3 DOC LAP
 * ========================================================= */

static void TaskLED3(void *argument)
{
    uint32_t value;
    uint8_t type;

    (void)argument;

    for (;;)
    {
        /* Cho notification tu TaskLED1 */
        if (xTaskNotifyWait(0U, 0xFFFFFFFFUL, &value, portMAX_DELAY) == pdPASS)
        {
            /* Tach loai su kien tu gia tri notification */
            type = (uint8_t)((value >> 8U) & 0xFFU);

            if (type == EVT_UP && brightness[2] < PWM_MAX)
            {
                /* Tang do sang LED3 */
                brightness[2] += PWM_STEP;
            }
            else if (type == EVT_DOWN && brightness[2] >= PWM_STEP)
            {
                /* Giam do sang LED3 */
                brightness[2] -= PWM_STEP;
            }

            /* Cap nhat PWM LED3 */
            PWM_SetBrightness(2U, brightness[2]);
        }
    }
}

/* =========================================================
 * 13. HAM MAIN - KHOI TAO VA TAO CAC TASK
 * ========================================================= */

int main(void)
{
    /* Khoi tao he thong va clock theo cau hinh startup */
    SystemInit();

    /* Khoi tao PWM va cac nut nhan */
    PWM_Init();
    Button_Init();

    /* Tao mot Queue chua toi da 8 su kien */
    qLED = xQueueCreate(8U, sizeof(LEDEvent_t));

    /* Neu tao Queue that bai thi dung chuong trinh */
    if (qLED == NULL)
    {
        while (1)
        {
        }
    }

    /* Tao Task doc nut nhan, uu tien 3 */
    if (xTaskCreate(TaskButton, "Button", 256U, NULL, 3U, NULL) != pdPASS)
    {
        while (1)
        {
        }
    }

    /* Tao TaskLED1:
     * Vua dieu khien LED1, vua phan phoi su kien.
     */
    if (xTaskCreate(TaskLED1, "LED1", 256U, NULL, 2U, NULL) != pdPASS)
    {
        while (1)
        {
        }
    }

    /* Tao TaskLED2 va luu handle */
    if (xTaskCreate(TaskLED2, "LED2", 256U, NULL, 2U, &taskLED2Handle) != pdPASS)
    {
        while (1)
        {
        }
    }

    /* Tao TaskLED3 va luu handle */
    if (xTaskCreate(TaskLED3, "LED3", 256U, NULL, 2U, &taskLED3Handle) != pdPASS)
    {
        while (1)
        {
        }
    }

    /* Bat dau bo lap lich FreeRTOS */
    vTaskStartScheduler();

    /* Neu Scheduler khong chay thi chuong trinh dung tai day */
    while (1)
    {
    }
}