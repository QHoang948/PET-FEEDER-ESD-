/* Blue Pill + DS1307 + LCD I2C (PCF8574) + Keypad 4x4 PA0..PA7
 * Timer2 1 Hz, đọc RTC mỗi giây, 3 mốc thời gian kích ngõ ra PA8
 */
#include "main.h"          // đảm bảo main.h include "stm32f1xx_hal.h"
#include <stdio.h>

#include "lcd_i2c.h"
#include "ds1307.h"
#include "keypad.h"

/* ====== Handle HAL ====== */
I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim2;

/* ====== Ứng dụng ====== */
#define LCD_ADDR_DEFAULT   0x27   // đổi 0x3F nếu backpack khác
LCD_I2C_Handle lcd = { .hi2c = &hi2c1, .addr7bit = LCD_ADDR_DEFAULT, .backlight = 0x08 };

typedef struct { uint8_t h,m,s; } TimeHMSS;
static volatile uint8_t tick1s = 0;
static TimeHMSS now_soft = {0}, alarm1={0}, alarm2={0}, alarm3={0};

/* ====== Prototype ====== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM2_Init(void);
static void show_time(const TimeHMSS *t);
static uint8_t time_equal(const TimeHMSS *a, const TimeHMSS *b);
static void activate_output_ms(uint32_t ms);
static void ui_set_time(TimeHMSS *t, const char *title);
static void ui_set_rtc_via_keypad(void);



/* ====== TIM2 callback 1 Hz ====== */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2) {
    tick1s = 1;
    // Đồng hồ mềm (phòng khi chưa có RTC); vẫn sẽ bị ghi đè bởi DS1307 bên dưới
    now_soft.s++;
    if (now_soft.s==60){ now_soft.s=0; now_soft.m++; }
    if (now_soft.m==60){ now_soft.m=0; now_soft.h++; }
    if (now_soft.h==24){ now_soft.h=0; }
  }
}

/* ====== Hiển thị ====== */
static void show_time(const TimeHMSS *t)
{
  char buf[17];
  LCD_SetCursor(&lcd,0,0);
  snprintf(buf,sizeof(buf)," NOW: %02u:%02u:%02u", t->h,t->m,t->s);
  LCD_Print(&lcd, buf);
  LCD_SetCursor(&lcd,1,0);
  LCD_Print(&lcd,"1 2 3 RTC");
}

static uint8_t time_equal(const TimeHMSS *a, const TimeHMSS *b)
{
  return (a->h==b->h && a->m==b->m && a->s==b->s);
}

/* ====== Kích ngõ ra PA8 trong ms (thay servo AVR) ====== */
static void activate_output_ms(uint32_t ms)
{
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
  HAL_Delay(ms);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
}
// Đọc 1 lần/phím: debounce + chờ nhả
static int read_key_once(uint8_t *key)
{
  uint8_t k1, k2;

  if (!Keypad_Scan(&k1)) return 0;   // chưa bấm

  HAL_Delay(15);                     // debounce nhấn
  if (!Keypad_Scan(&k2) || k1 != k2) return 0; // chưa ổn định

  *key = k1;                         // chốt phím vừa nhấn

  // chờ tới khi nhả phím hoàn toàn rồi mới cho lần kế
  do { HAL_Delay(10); } while (Keypad_Scan(&k2));

  HAL_Delay(15);                     // debounce nhả
  return 1;
}

/* ====== Nhập HH:MM:SS bằng keypad (gọn, chống sai) ====== */
/* ====== Nhập HH:MM:SS với con trỏ nhảy tuần tự qua 6 digit ====== */
static void ui_set_time(TimeHMSS *t, const char *title)
{
  // 6 vị trí nhập: cột trên LCD tương ứng "  00:00:00"
  //                0 1 2 3 4 5 6 7 8 9 10 11 12 13...
  // ta in ở hàng 1, bắt đầu tại col=2 -> các cột là 2,3,5,6,8,9
  const uint8_t POS_COL[6] = {2,3,5,6,8,9};

  uint8_t hh = t->h;      // lấy giá trị cũ từ biến đầu vào
  uint8_t mm = t->m;
  uint8_t ss = t->s;
  uint8_t pos=0;              // 0..5 = Ht,Hu, Mt,Mu, St,Su
  char buf[17];

  LCD_Clear(&lcd);
  LCD_SetCursor(&lcd,0,0);
  LCD_Print(&lcd, title);
  LCD_SetCursor(&lcd,1,0);
  LCD_Print(&lcd, "  00:00:00  (15=OK)");
  // Hiển thị giá trị cũ ngay sau khi vào menu
  LCD_SetCursor(&lcd,1,2);
  snprintf(buf,sizeof(buf),"%02u:%02u:%02u", hh,mm,ss);
  LCD_Print(&lcd, buf);

  // Bật con trỏ (nếu driver hỗ trợ)
  #ifdef LCD_CursorOn
  LCD_CursorOn(&lcd, 1);
  #endif
  #ifdef LCD_BlinkOn
  LCD_BlinkOn(&lcd, 1);
  #endif

  while (1) {
    // In lại thời gian hiện có
    LCD_SetCursor(&lcd,1,2);
    snprintf(buf,sizeof(buf),"%02u:%02u:%02u", hh,mm,ss);
    LCD_Print(&lcd, buf);

    // Đặt con trỏ tới đúng "ô" đang nhập
    LCD_SetCursor(&lcd,1, POS_COL[pos]);

    // Đọc phím
    uint8_t key;
    if (!read_key_once(&key)) { HAL_Delay(5); continue; }

    if (key <= 9) {
      // Gán digit vừa bấm vào đúng nửa-digit hiện tại
      if (pos == 0) {               // Hour tens
        hh = (uint8_t)(key*10 + (hh%10));
        if (hh > 23) hh = 20;       // tạm giữ tens hợp lệ
        pos = 1;
      }
      else if (pos == 1) {          // Hour units
        hh = (uint8_t)(((hh/10)*10) + key);
        if (hh > 23) hh = 23;       // chốt phạm vi giờ
        pos = 2;
      }
      else if (pos == 2) {          // Minute tens
        mm = (uint8_t)(key*10 + (mm%10));
        if (mm > 59) mm = 50;
        pos = 3;
      }
      else if (pos == 3) {          // Minute units
        mm = (uint8_t)(((mm/10)*10) + key);
        if (mm > 59) mm = 59;
        pos = 4;
      }
      else if (pos == 4) {          // Second tens
        ss = (uint8_t)(key*10 + (ss%10));
        if (ss > 59) ss = 50;
        pos = 5;
      }
      else /* pos == 5 */ {         // Second units
        ss = (uint8_t)(((ss/10)*10) + key);
        if (ss > 59) ss = 59;
        pos = 0;                    // quay lại đầu, nhảy đủ 6 ô
      }
    }
    else if (key == 10) {
      // Back 1 ô: lùi pos và xóa riêng phần đang nhập
      if (pos == 0) pos = 5; else pos--;
      if (pos >= 4)        ss = 0;  // đang chỉnh giây
      else if (pos >= 2)   mm = 0;  // đang chỉnh phút
      else                 hh = 0;  // đang chỉnh giờ
    }
    else if (key == 15) {
      // OK: trả kết quả
      t->h = hh; t->m = mm; t->s = ss;
      HAL_Delay(120);

      // Tắt nháy (nếu có)
      #ifdef LCD_CursorOn
      LCD_CursorOn(&lcd, 0);
      #endif
      #ifdef LCD_BlinkOn
      LCD_BlinkOn(&lcd, 0);
      #endif
      return;
    }
  }
}

static void rtc_write_and_sync(uint8_t hh, uint8_t mm, uint8_t ss)
{
  // 1) Ghi xuống DS1307 (giả sử DS1307_WriteTime() đã CH=0 & BCD)
  DS1307_Time tt = {0};
  tt.hour = hh; tt.min = mm; tt.sec = ss;
  tt.day  = 1;  tt.date= 1;  tt.month=1; tt.year=24;

  // Tạm khóa ngắt rất ngắn tránh now_soft tự tăng trong lúc set
  __disable_irq();
  DS1307_WriteTime(&hi2c1, &tt);
  __enable_irq();

  // 2) Đợi nhẹ rồi đọc lại để chắc chắn
  HAL_Delay(10);
  DS1307_Time rd;
  if (DS1307_ReadTime(&hi2c1, &rd) == HAL_OK) {
    now_soft.h = rd.hour;
    now_soft.m = rd.min;
    now_soft.s = rd.sec;
  } else {
    // fallback: dùng đúng giá trị vừa ghi
    now_soft.h = hh;
    now_soft.m = mm;
    now_soft.s = ss;
  }

  // 3) Căn lại mốc giây của TIM2 để không lệch nửa chừng
  __HAL_TIM_SET_COUNTER(&htim2, 0);
  tick1s = 0;

  // 4) Cập nhật LCD ngay (không chờ tick kế tiếp)
  show_time(&now_soft);
}

/* ====== Nhập thời gian và ghi xuống DS1307 ====== */
/* ====== Nhập thời gian và ghi xuống DS1307 (giữ lại giá trị cũ) ====== */
static void ui_set_rtc_via_keypad(void)
{
  DS1307_Time tds;
  TimeHMSS temp = {0};

  // 1) Đọc thời gian hiện tại (nếu có) để làm giá trị khởi tạo
  if (DS1307_ReadTime(&hi2c1, &tds) == HAL_OK) {
    temp.h = tds.hour;
    temp.m = tds.min;
    temp.s = tds.sec;
  } else {
    // nếu DS1307 lỗi, lấy giá trị đang chạy trong now_soft
    temp = now_soft;
  }

  // 2) Gọi giao diện nhập, hiển thị sẵn giờ hiện tại
  ui_set_time(&temp, " SET RTC (15=OK)");

  // 3) Ghi xuống DS1307 và đồng bộ ngay
  rtc_write_and_sync(temp.h, temp.m, temp.s);
}


/* ====== main ====== */
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_TIM2_Init();

  LCD_Init(&lcd);
  Keypad_Init();

  // đọc RTC lần đầu
  DS1307_Time tds;
  if (DS1307_ReadTime(&hi2c1,&tds)==HAL_OK){
    now_soft.h=tds.hour; now_soft.m=tds.min; now_soft.s=tds.sec;
  }

  LCD_Clear(&lcd);
  LCD_SetCursor(&lcd,0,0); LCD_Print(&lcd,"RTC CONTROL");
  LCD_SetCursor(&lcd,1,0); LCD_Print(&lcd,"12/13/14=ALARM 15=RTC");
  HAL_Delay(1000);

  while (1) {
    if (tick1s){
      tick1s=0;

      if (DS1307_ReadTime(&hi2c1,&tds)==HAL_OK){
        now_soft.h=tds.hour; now_soft.m=tds.min; now_soft.s=tds.sec;
      }
      show_time(&now_soft);

      if ( time_equal(&now_soft,&alarm1) ||
           time_equal(&now_soft,&alarm2) ||
           time_equal(&now_soft,&alarm3) ) {
        activate_output_ms(800);   // bật PA8 ~0.8s (thay servo)
      }
    }

    // menu đơn giản bằng phím chức năng: 12/13/14/15
    uint8_t key;
    if (Keypad_Scan(&key)) {
      if (key==12)      ui_set_time(&alarm1, " SET ALARM1");
      else if (key==13) ui_set_time(&alarm2, " SET ALARM2");
      else if (key==14) ui_set_time(&alarm3, " SET ALARM3");
      else if (key==15) ui_set_rtc_via_keypad();
    }
  }
}

/* ====== Clock: dùng HSI 8 MHz cho gọn ====== */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_AFIO_CLK_ENABLE();
  __HAL_RCC_PWR_CLK_ENABLE();

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_OFF;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|
                                RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;    // 8 MHz
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) Error_Handler();

  __HAL_AFIO_REMAP_SWJ_NOJTAG(); // tùy chọn
}

/* ====== GPIO: Keypad + Output PA8 ====== */
static void MX_GPIO_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  GPIO_InitTypeDef g={0};

  // Rows: PA0..PA3 input pull-up
  g.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA,&g);

  // Cols: PA4..PA7 output open-drain, set HIGH
  g.Pin = GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  g.Mode = GPIO_MODE_OUTPUT_OD;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA,&g);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_SET);

  // Output điều khiển (thay servo)
  g.Pin = GPIO_PIN_8;
  g.Mode = GPIO_MODE_OUTPUT_PP;
  g.Pull = GPIO_NOPULL;
  g.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA,&g);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
}

/* ====== I2C1: PB6/PB7 @100kHz ====== */
static void MX_I2C1_Init(void)
{
  __HAL_RCC_I2C1_CLK_ENABLE();

  GPIO_InitTypeDef g={0};
  g.Pin = GPIO_PIN_6|GPIO_PIN_7;     // PB6=SCL, PB7=SDA
  g.Mode = GPIO_MODE_AF_OD;
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB,&g);

  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1)!=HAL_OK) Error_Handler();
}

/* ====== TIM2 base 1 Hz (SYSCLK=8MHz) ======
 * 8MHz / 8000 / 1000 = 1 Hz
 */
static void MX_TIM2_Init(void)
{
  __HAL_RCC_TIM2_CLK_ENABLE();

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 8000-1;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 1000-1;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  if (HAL_TIM_Base_Init(&htim2)!=HAL_OK) Error_Handler();

  HAL_NVIC_SetPriority(TIM2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
  HAL_TIM_Base_Start_IT(&htim2);
}

void TIM2_IRQHandler(void){ HAL_TIM_IRQHandler(&htim2); }

void Error_Handler(void)
{
  __disable_irq();
  while(1){}
}
