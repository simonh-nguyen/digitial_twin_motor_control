// Includes
#include "motor_controller.hpp"

static constexpr char *TAG = "Motor";

static MotorController *motor_obj;

MotorController::MotorController()
{
  motor_obj = this;

  timer_hdl = nullptr;
  oper_hdl = nullptr;
  cmpr_hdl = nullptr;
  gen_hdl = nullptr;
  unit_hdl = nullptr;
  channel_a_hdl = nullptr;
  channel_b_hdl = nullptr;

  update_task_hdl = NULL;
  display_task_hdl = NULL;

  timestamp = 0;
  direction = STOPPED;
  duty_cycle = 0;
  velocity = 0;
  velocity_ema = 0;
  position = 0;
}

void MotorController::init()
{
  ESP_LOGI(TAG, "Setting up output to ENA.");
  mcpwm_timer_config_t timer_config = {
      .group_id = 0,
      .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
      .resolution_hz = TIMER_RES,
      .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
      .period_ticks = TIMER_PERIOD,
  };
  ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &timer_hdl));

  mcpwm_operator_config_t oper_config = {
      .group_id = 0,
  };
  ESP_ERROR_CHECK(mcpwm_new_operator(&oper_config, &oper_hdl));
  ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper_hdl, timer_hdl));

  mcpwm_comparator_config_t cmpr_config = {
      .flags = {
          .update_cmp_on_tez = true,
      },
  };
  ESP_ERROR_CHECK(mcpwm_new_comparator(oper_hdl, &cmpr_config, &cmpr_hdl));

  mcpwm_generator_config_t gen_config = {
      .gen_gpio_num = GPIO_ENA,
      .flags = {
          .pull_down = 1,
      },
  };
  ESP_ERROR_CHECK(mcpwm_new_generator(oper_hdl, &gen_config, &gen_hdl));

  ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpr_hdl, TIMER_PERIOD * 0.5));
  ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(gen_hdl, MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
  ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(gen_hdl, MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, cmpr_hdl, MCPWM_GEN_ACTION_LOW)));

  ESP_ERROR_CHECK(mcpwm_timer_enable(timer_hdl));
  ESP_ERROR_CHECK(mcpwm_timer_start_stop(timer_hdl, MCPWM_TIMER_START_NO_STOP));

  ESP_LOGI(TAG, "Setting up outputs to IN1 and IN2.");
  gpio_config_t output_config = {
      .pin_bit_mask = ((1ULL << GPIO_IN1) | (1ULL << GPIO_IN2)),
      .mode = GPIO_MODE_OUTPUT,
      .pull_down_en = GPIO_PULLDOWN_ENABLE,
  };
  gpio_config(&output_config);

  ESP_LOGI(TAG, "Setting up inputs for encoder A and B.");
  pcnt_unit_config_t unit_config = {
      .low_limit = ENCODER_LOW_LIMIT,
      .high_limit = ENCODER_HIGH_LIMIT,
      .flags = {
          .accum_count = 1,
      },
  };
  ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &unit_hdl));

  pcnt_glitch_filter_config_t filter_config = {
      .max_glitch_ns = ENCODER_GLITCH_NS,
  };
  ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(unit_hdl, &filter_config));

  pcnt_chan_config_t channel_a_config = {
      .edge_gpio_num = GPIO_C1,
      .level_gpio_num = GPIO_C2,
  };
  ESP_ERROR_CHECK(pcnt_new_channel(unit_hdl, &channel_a_config, &channel_a_hdl));
  pcnt_chan_config_t channel_b_config = {
      .edge_gpio_num = GPIO_C2,
      .level_gpio_num = GPIO_C1,
  };
  ESP_ERROR_CHECK(pcnt_new_channel(unit_hdl, &channel_b_config, &channel_b_hdl));

  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(channel_a_hdl, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(channel_a_hdl, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
  ESP_ERROR_CHECK(pcnt_channel_set_edge_action(channel_b_hdl, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
  ESP_ERROR_CHECK(pcnt_channel_set_level_action(channel_b_hdl, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

  ESP_ERROR_CHECK(pcnt_unit_add_watch_point(unit_hdl, ENCODER_LOW_LIMIT));
  ESP_ERROR_CHECK(pcnt_unit_add_watch_point(unit_hdl, ENCODER_HIGH_LIMIT));

  ESP_ERROR_CHECK(pcnt_unit_enable(unit_hdl));
  ESP_ERROR_CHECK(pcnt_unit_clear_count(unit_hdl));
  ESP_ERROR_CHECK(pcnt_unit_start(unit_hdl));

  ESP_LOGI(TAG, "Setting up update task.");
  xTaskCreatePinnedToCore(update_trampoline, "Update Task", update_config.stack_size, nullptr, update_config.priority, &update_task_hdl, update_config.core);

  ESP_LOGI(TAG, "Setting up display task.");
  xTaskCreatePinnedToCore(display_task, "Display Task", display_config.stack_size, nullptr, display_config.priority, &display_task_hdl, display_config.core);
  vTaskSuspend(display_task_hdl);
}

void MotorController::update_trampoline(void *arg)
{
  while (1)
  {
    motor_obj->update_task();
    vTaskDelay(update_config.delay / portTICK_PERIOD_MS);
  }
}

void MotorController::update_task()
{
  static uint64_t time_prev = 0;
  static uint64_t time_curr = 0;
  static uint64_t time_diff = 0;

  static int pcnt_prev = 0;
  static int pcnt_curr = 0;
  static int pcnt_diff = 0;

  time_curr = esp_timer_get_time();
  ESP_ERROR_CHECK(pcnt_unit_get_count(unit_hdl, &pcnt_curr));

  time_diff = time_curr - time_prev;
  pcnt_diff = pcnt_curr - pcnt_prev;

  if (pcnt_diff < 0)
    direction = COUNTERCLOCKWISE;
  else if (pcnt_diff > 0)
    direction = CLOCKWISE;
  else
    direction = STOPPED;

  timestamp = (double)time_curr / US_TO_MS;
  velocity = ((double)abs(pcnt_diff) / (double)time_diff) * PPUS_TO_RAD_S;
  velocity_ema = (alpha * velocity) + (1.0 - alpha) * velocity_ema;
  position = (double)pcnt_curr * PULSE_TO_RAD;

  ESP_ERROR_CHECK(pcnt_unit_get_count(unit_hdl, &pcnt_prev));
  time_prev = esp_timer_get_time();
}

void MotorController::display_task(void *arg)
{
  while (1)
  {
    ESP_LOGI(TAG, "Timestamp (ms): %.3f, Direction: %d, Velocity (rad/s): %.3f , Position (rad): %.3f, Velocity EMA (rad/s): %.3f", motor_obj->timestamp, motor_obj->direction, motor_obj->velocity, motor_obj->position, motor_obj->velocity_ema);
    vTaskDelay(display_config.delay / portTICK_PERIOD_MS);
  }
}

void MotorController::enable_display()
{
  ESP_LOGI(TAG, "Enabling display.");
  vTaskResume(display_task_hdl);
}

void MotorController::disable_display()
{
  ESP_LOGI(TAG, "Disabling display.");
  vTaskSuspend(display_task_hdl);
}

void MotorController::stop_motor()
{
  ESP_LOGI(TAG, "Stopping motor.");
  gpio_set_level(GPIO_IN1, 0);
  gpio_set_level(GPIO_IN2, 0);
}

void MotorController::set_direction(MotorDirection dir)
{
  if (dir == CLOCKWISE)
  {
    ESP_LOGI(TAG, "Setting motor direction to clockwise.");
    gpio_set_level(GPIO_IN2, 0);
    gpio_set_level(GPIO_IN1, 1);
  }
  else if (dir == COUNTERCLOCKWISE)
  {
    ESP_LOGI(TAG, "Setting motor direction to counter-clockwise.");
    gpio_set_level(GPIO_IN1, 0);
    gpio_set_level(GPIO_IN2, 1);
  }
}

void MotorController::set_duty_cycle(double dc)
{
  // ESP_LOGI(TAG, "Setting motor duty cycle to %.3f.", duty_cycle);
  duty_cycle = dc;
  dc = (dc * MIN_DUTY_CYCLE) + MIN_DUTY_CYCLE; // Changes scale
  ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(cmpr_hdl, TIMER_PERIOD * dc));
}

double MotorController::get_timestamp()
{
  return timestamp;
}

double MotorController::get_duty_cycle()
{
  return duty_cycle;
}

int8_t MotorController::get_direction()
{
  return direction;
}

double MotorController::get_velocity()
{
  return velocity;
}

double MotorController::get_velocity_ema()
{
  return velocity_ema;
}

double MotorController::get_position()
{
  return position;
}

void MotorController::pid_velocity(double set_point)
{
  static uint64_t time_prev = 0;
  static uint64_t timer_curr = 0;
  static double dt = 0;

  static double error_prev = 0;
  static double error = 0;
  static double integral = 0;
  static double derivative = 0;
  static double prev_output = 0;
  static double output = 0;

  timer_curr = esp_timer_get_time();
  dt = (timer_curr - time_prev) / US_TO_S;

  error = set_point - get_velocity_ema();
  integral += error * dt;
  derivative = (error - error_prev) / dt;

  // output = kc * (error + (1 / ti) * integral + td * derivative);
  output = kp * error + ki * integral + kd * derivative;

  // Keep output within range
  if (output > PID_MAX_OUTPUT)
    output = PID_MAX_OUTPUT;
  else if (output < PID_MIN_OUTPUT)
    output = PID_MIN_OUTPUT;

  // Only sets new output when error is large enough
  if (fabs(error) <= PID_HYSTERESIS)
    output = prev_output;

  set_duty_cycle(output);

  prev_output = output;
  error_prev = set_point - get_velocity_ema();
  time_prev = esp_timer_get_time();
}