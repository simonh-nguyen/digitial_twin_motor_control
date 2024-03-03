// Includes
#include "communication.hpp"

static constexpr char *TAG = "communication";

// UART properties
static constexpr uint32_t UART_BAUD_RATE = 115200;
static constexpr uint32_t BUFFER_SIZE = 1024;

// TX task properties
static constexpr uint8_t TX_RATE = 1000; // Monitoring sample rate in ms
static constexpr int32_t TX_STACK_SIZE = 1024 * 4;
static constexpr UBaseType_t TX_TASK_PRIO = configMAX_PRIORITIES - 2; // High priority
static constexpr int8_t TX_TASK_CORE = 0;                             // Run task on Core 1

// RX task properties
static constexpr uint8_t RX_RATE = 1000; // Monitoring sample rate in ms
static constexpr int32_t RX_STACK_SIZE = 1024 * 4;
static constexpr UBaseType_t RX_TASK_PRIO = configMAX_PRIORITIES - 2; // High priority
static constexpr int8_t RX_TASK_CORE = 0;                             // Run task on Core 1

static Communication *comm_obj;

Communication::Communication()
{
  comm_obj = this;

  tx_task_hdl = NULL;
  rx_task_hdl = NULL;
}

void Communication::init()
{
  ESP_LOGI(TAG, "Setting up UART.");
  uart_config_t uart_config = {
      .baud_rate = UART_BAUD_RATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUFFER_SIZE, BUFFER_SIZE, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, GPIO_TX, GPIO_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  ESP_LOGI(TAG, "Setting up transmitter task.");
  // xTaskCreatePinnedToCore(tx_trampoline, "Transmitter", TX_STACK_SIZE, nullptr, TX_TASK_PRIO, &tx_task_hdl, TX_TASK_CORE);

  ESP_LOGI(TAG, "Setting up receiver task.");
  xTaskCreatePinnedToCore(rx_trampoline, "RX Data Task", rx_config.stack_size, nullptr, rx_config.priority, &rx_task_hdl, rx_config.core);
}

void Communication::send_data(char *data)
{
  // sprintf(data, "%d,%s,%d", FRAME_START, data, FRAME_END); // Encapsulates data frame
  uart_write_bytes(UART_NUM_1, data, strlen(data));
}

void Communication::tx_trampoline(void *arg)
{
  while (1)
  {
    comm_obj->tx();
    vTaskDelay(TX_RATE / portTICK_PERIOD_MS);
  }
}

void Communication::tx()
{
  // send_data("test\n");
}

void Communication::rx_trampoline(void *arg)
{
  while (1)
  {
    comm_obj->rx();
    vTaskDelay(rx_config.delay / portTICK_PERIOD_MS);
  }
}

void Communication::rx()
{
}