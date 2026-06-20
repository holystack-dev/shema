#include "button_bsp.h"
#include "multi_button.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "user_config.h"

// The 1.85C V2 round board has a single user button: BOOT (GPIO0, active-low).
//   single click  -> boot_groups bit0  (wake screen / go Home)
//   long press    -> pwr_groups  bit1  (power off / deep sleep)

EventGroupHandle_t boot_groups;
EventGroupHandle_t pwr_groups;

static Button boot_btn;
#define BOOT_KEY_GPIO    EXAMPLE_PIN_NUM_BOOT_BTN
#define boot_btn_id      1
#define boot_btn_active  0

static void on_boot_single_click(Button* btn_handle);
static void on_boot_double_click(Button* btn_handle);
static void on_boot_long_press_start(Button* btn_handle);
static void on_boot_press_up(Button* btn_handle);

static void clock_task_callback(void *arg)
{
  button_ticks();
}

static uint8_t read_button_GPIO(uint8_t button_id)
{
  switch (button_id) {
    case boot_btn_id: return gpio_get_level(BOOT_KEY_GPIO);
    default: break;
  }
  return 1;
}

static void gpio_init(void)
{
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_INPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)0x01 << BOOT_KEY_GPIO);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
}

void button_Init(void)
{
  boot_groups = xEventGroupCreate();
  pwr_groups = xEventGroupCreate();
  gpio_init();

  button_init(&boot_btn, read_button_GPIO, boot_btn_active, boot_btn_id);
  button_attach(&boot_btn, BTN_SINGLE_CLICK, on_boot_single_click);
  button_attach(&boot_btn, BTN_PRESS_REPEAT, on_boot_double_click);
  button_attach(&boot_btn, BTN_LONG_PRESS_START, on_boot_long_press_start);
  button_attach(&boot_btn, BTN_PRESS_UP, on_boot_press_up);

  const esp_timer_create_args_t clock_tick_timer_args =
  {
    .callback = &clock_task_callback,
    .name = "clock_task",
    .arg = NULL,
  };
  esp_timer_handle_t clock_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&clock_tick_timer_args, &clock_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(clock_tick_timer, 1000 * 5));  // 5ms
  button_start(&boot_btn);
}

/* single click -> home/wake */
static void on_boot_single_click(Button* btn_handle)
{
  xEventGroupSetBits(boot_groups, set_bit_button(0));
}

/* double click (unused) */
static void on_boot_double_click(Button* btn_handle)
{
  xEventGroupSetBits(boot_groups, set_bit_button(1));
}

/* long press -> power off */
static void on_boot_long_press_start(Button* btn_handle)
{
  xEventGroupSetBits(pwr_groups, set_bit_button(1));
}

/* release */
static void on_boot_press_up(Button* btn_handle)
{
  xEventGroupSetBits(boot_groups, set_bit_button(3));
}

uint8_t user_button_get_repeat_count(void)
{
  return (button_get_repeat_count(&boot_btn));
}

uint8_t user_boot_get_repeat_count(void)
{
  return (button_get_repeat_count(&boot_btn));
}
