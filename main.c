/* --------------------------------------------------------------
   Application: Ride Safety Node - Proof of Concept
   Class: Real Time Systems - Fa 2025
   Author: [J Crawford]
   Email:  [evancrawford@ucf.edu]
   Company Context:
     The Walt Disney Company (Orlando) - roller coaster ride safety node.
     This ESP32 monitors track load and an E-Stop button, drives indicator
     LEDs, and streams telemetry to a higher-level ride controller.

   AI Use:
     ChatGPT (OpenAI) on 2025-12-05.
     // [AI-ASSIST] markers indicate locations that were AI-influenced.
---------------------------------------------------------------*/

#include <stdio.h>
#include <string.h>
#include <stdarg.h>           // for va_list // [AI-ASSIST]
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"

// ------------------ Pin / config definitions ------------------
// STATUS LED: soft RT heartbeat (node alive)
#define LED_STATUS   GPIO_NUM_2
// ALARM LED: overload / safety alert
#define LED_ALARM    GPIO_NUM_4
// BRAKE LED: emergency stop applied
#define LED_BRAKE    GPIO_NUM_5

// E-Stop pushbutton (active LOW, pull-up enabled)
#define ESTOP_PIN    GPIO_NUM_18
#define RESET_PIN GPIO_NUM_19   // Manual reset button

// "Track load" sensor (potentiometer on ADC1_CHANNEL_6 / GPIO34)
#define LOAD_ADC_CHANNEL  ADC1_CHANNEL_6

// Real-time parameters (all periods / deadlines in ms)
#define LOAD_SAMPLE_PERIOD_MS   50   // Hard RT (load sensor)
#define LOAD_SAMPLE_DEADLINE_MS 50
#define HEARTBEAT_PERIOD_MS     500  // Soft RT
#define TELEMETRY_PERIOD_MS     200  // Soft RT
#define ESTOP_REACTION_DEADLINE_MS 5 // Hard RT (E-stop → BRAKE_LED on)

// Threshold for overload (tune in Wokwi)
#define LOAD_THRESHOLD_RAW      3000

// Queue depth for overload events
#define ALERT_QUEUE_LEN         10

// ------------------ Types ------------------
typedef struct {
    uint32_t ts_ms;   // timestamp (ms since boot)
    int      adc_raw; // sensor value at time of alert
} ride_alert_t;

// ------------------ Sync handles ------------------
static SemaphoreHandle_t sem_estop   = NULL;  // binary sem signaled by ISR (hard path)
static SemaphoreHandle_t print_mutex = NULL;  // mutex for UART/printf
static SemaphoreHandle_t adc_mutex   = NULL;  // mutex for shared ADC reading
static QueueHandle_t     alert_queue = NULL;  // queue for overload alerts
static SemaphoreHandle_t sem_reset = NULL;

// Shared state (protected by adc_mutex when accessed by multiple tasks)
static int latest_adc_raw = 0;

// Emergency latch flag (written by estop_task, read by others)
static volatile bool emergency_latched = false;
static volatile bool overload_active = false;

// ------------------ Utility: timestamp helper ------------------
static uint32_t ms_since_boot(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// ------------------ Utility: safe printf wrapper ------------------
static void safe_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    if (xSemaphoreTake(print_mutex, pdMS_TO_TICKS(1000))) {
        vprintf(fmt, args);
        xSemaphoreGive(print_mutex);
    } else {
        // fallback if mutex couldn't be taken
        vprintf(fmt, args);
    }

    va_end(args);
}

// ------------------ ISR: E-Stop button ------------------
// Hard RT path: ISR must be short, only wakes estop_task.
static void IRAM_ATTR estop_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    xSemaphoreGiveFromISR(sem_estop, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}
// ------------------ ISR: Reset button ------------------
static void IRAM_ATTR reset_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Wake reset task
    xSemaphoreGiveFromISR(sem_reset, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

// ------------------ Task: Heartbeat (Soft RT) ------------------
// Company mapping: "Ride Node Status LED"
// Period: 500 ms; Deadline: 500 ms (Soft - missed blink is cosmetic)
static void heartbeat_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS);

    while (1) {
        gpio_set_level(LED_STATUS, 1);
        vTaskDelayUntil(&last_wake, period_ticks / 2);
        gpio_set_level(LED_STATUS, 0);
        vTaskDelayUntil(&last_wake, period_ticks / 2);
    }
}

// ------------------ Task: Load Sensor Monitor (Hard/Firm RT) ------------------
// Company mapping: "Track Segment Load Monitor"
// Period: 50 ms; Deadline: 50 ms (Hard/Firm - you will justify in README)
// At each sample, if load exceeds threshold, send alert to queue.
static void load_sensor_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period_ticks = pdMS_TO_TICKS(LOAD_SAMPLE_PERIOD_MS);

    int prev_above = 0;

    while (1) {
        int adc_val = adc1_get_raw(LOAD_ADC_CHANNEL);

        // Update shared ADC value (mutex protected)
        if (xSemaphoreTake(adc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            latest_adc_raw = adc_val;
            xSemaphoreGive(adc_mutex);
        }

        int above = (adc_val > LOAD_THRESHOLD_RAW) ? 1 : 0;

        // NEW: track continuous overload state
        overload_active = above;

        // Rising-edge detection (only trigger once per overload event)
        if (above && !prev_above) {
            ride_alert_t alert;
            alert.ts_ms = ms_since_boot();
            alert.adc_raw = adc_val;

            BaseType_t res = xQueueSendToBack(alert_queue, &alert, 0);
            if (res != pdTRUE) {
                safe_printf("[%8u ms] [LoadMon] ALERT DROPPED - queue full\n",
                            ms_since_boot());
            } else {
                safe_printf("[%8u ms] [LoadMon] Overload detected, adc=%d\n",
                            alert.ts_ms, adc_val);
            }
        }

        prev_above = above;

        vTaskDelayUntil(&last_wake, period_ticks);
    }
}


// ------------------ Task: E-Stop Handler (Hard RT) ------------------
// Company mapping: "Ride Emergency Stop Handler"
// Deadline: ~5 ms from ISR event to BRAKE_LED ON (Hard RT)
// Woken by sem_estop signaled in ISR.
static void estop_task(void *pvParameters)
{
    while (1) {
        // Block until E-Stop semaphore is given by ISR
        if (xSemaphoreTake(sem_estop, portMAX_DELAY) == pdTRUE) {
            uint32_t ts = ms_since_boot();

            // Latch emergency state
            emergency_latched = true;

            // Turn on BRAKE and ALARM LEDs immediately
            gpio_set_level(LED_BRAKE, 1);
            gpio_set_level(LED_ALARM, 1);

            safe_printf("[%8u ms] [E-STOP] Emergency stop activated! BRAKES ON.\n", ts);
        }
    }
}

// ------------------ Task: Reset Handler (Soft/Firm RT) ------------------
// Company mapping: "Ride Operator Reset Handler"
// Triggered by reset button ISR via sem_reset.
static void reset_task(void *pvParameters)
{
    while (1) {
        // Block until reset semaphore is given by ISR
        if (xSemaphoreTake(sem_reset, portMAX_DELAY) == pdTRUE) {

            uint32_t ts = ms_since_boot();

            // Clear emergency latch and release brakes
            emergency_latched = false;

            gpio_set_level(LED_BRAKE, 0);

            // ALARM LED returns to overload-based behavior
            safe_printf("[%8u ms] [RESET] Operator reset received. System out of E-STOP.\n",
                        ts);
        }
    }
}

// ------------------ Task: Alert Handler (Firm RT) ------------------
// Company mapping: "Safety Alert Aggregator"
// Consumes overload events from queue, blinks ALARM LED, logs info.
static void alert_handler_task(void *pvParameters)
{
    ride_alert_t alert;

    while (1) {

        // Check for new overload events
        if (xQueueReceive(alert_queue, &alert, pdMS_TO_TICKS(20)) == pdTRUE) {

            if (emergency_latched) {
                safe_printf("[%8u ms] [Alert] Overload AFTER E-STOP, adc=%d\n",
                            ms_since_boot(), alert.adc_raw);
            } else {
                safe_printf("[%8u ms] [Alert] Overload BEFORE E-STOP, adc=%d\n",
                            ms_since_boot(), alert.adc_raw);
            }
        }

        // NEW: Drive ALARM LED continuously based on overload flag
        if (!emergency_latched) {
            if (overload_active) {
                gpio_set_level(LED_ALARM, 1);
            } else {
                gpio_set_level(LED_ALARM, 0);
            }
        }

        // If emergency latched, LED already forced ON by estop_task
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ------------------ Task: Telemetry / Logger (Soft RT, variable time) ------------------
// Company mapping: "Ride Telemetry / Logger"
// Period: 200 ms (Soft RT). Execution time varies with system load:
//   - does extra work proportional to queue depth (variable-time task).
// ------------------ Task: Telemetry / Logger (Soft RT, variable time) ------------------
// Company mapping: "Ride Telemetry / Logger"
// Option B: Telemetry slows during emergency to reduce bandwidth and noise.
static void telemetry_task(void *pvParameters)
{
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        uint32_t now = ms_since_boot();
        int adc_snapshot = 0;

        if (xSemaphoreTake(adc_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            adc_snapshot = latest_adc_raw;
            xSemaphoreGive(adc_mutex);
        }

        UBaseType_t pending_alerts = uxQueueMessagesWaiting(alert_queue);

        // Variable workload (diagnostic simulation)
        for (UBaseType_t i = 0; i < pending_alerts; i++) {
            volatile int spin = 0;
            for (int j = 0; j < 1000; j++) {
                spin += j;
            }
        }

        // Normal telemetry printout
        safe_printf("[%8u ms] [Telemetry] adc=%4d, alerts_in_queue=%2u, emergency=%d\n",
                    now, adc_snapshot, (unsigned)pending_alerts,
                    emergency_latched ? 1 : 0);

        // ---------------------------
        // Slow telemetry if emergency latched // [AI-ASSIST]
        // ---------------------------
        TickType_t delay_ticks =
            emergency_latched ?
            pdMS_TO_TICKS(1000) :        // 1 second during emergency
            pdMS_TO_TICKS(TELEMETRY_PERIOD_MS); // normal (200ms)

        vTaskDelayUntil(&last_wake, delay_ticks);
    }
}


// ------------------ Main ------------------
void app_main(void)
{
    // ------------- GPIO config -------------
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_STATUS) |
                        (1ULL << LED_ALARM)  |
                        (1ULL << LED_BRAKE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);

    gpio_config_t estop_conf = {
        .pin_bit_mask = (1ULL << ESTOP_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // active LOW button
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE     // falling edge on press
    };
    gpio_config(&estop_conf);

    gpio_config_t reset_conf = {
        .pin_bit_mask = (1ULL << RESET_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE       // falling edge = pressed
    };
    gpio_config(&reset_conf);

    // ------------- ADC config -------------
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LOAD_ADC_CHANNEL, ADC_ATTEN_DB_12);

    // ------------- Sync primitives -------------
    sem_estop = xSemaphoreCreateBinary();
    sem_reset = xSemaphoreCreateBinary(); // [AI-ASSIST]
    print_mutex = xSemaphoreCreateMutex();
    adc_mutex   = xSemaphoreCreateMutex();
    alert_queue = xQueueCreate(ALERT_QUEUE_LEN, sizeof(ride_alert_t));

    if (!sem_estop || !sem_reset || !print_mutex || !adc_mutex || !alert_queue) {
        printf("Failed to create sync primitives\n");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // ------------- ISR service -------------
    gpio_install_isr_service(0);  // default flags
    gpio_isr_handler_add(ESTOP_PIN, estop_isr_handler, NULL);
    gpio_isr_handler_add(RESET_PIN, reset_isr_handler, NULL);

    // ------------- Task creation & priorities -------------
    // Priority convention (higher number = higher priority):
    // 4: estop_task (Hard RT)
    // 3: load_sensor_task (Hard/Firm RT), reset_task (Soft/Firm RT)
    // 2: alert_handler_task (Firm RT)
    // 1: telemetry_task, heartbeat_task (Soft RT)

    xTaskCreate(heartbeat_task,    "heartbeat",    2048, NULL, 1, NULL);
    xTaskCreate(telemetry_task,    "telemetry",    4096, NULL, 1, NULL);
    xTaskCreate(alert_handler_task,"alert_handler",4096, NULL, 2, NULL);
    xTaskCreate(load_sensor_task,  "load_sensor",  4096, NULL, 3, NULL);
    xTaskCreate(estop_task,        "estop",        4096, NULL, 4, NULL);
    xTaskCreate(reset_task,        "reset",        2048, NULL, 3, NULL);  // [AI-ASSIST]


    safe_printf("[%8u ms] [BOOT] Ride Safety Node started. Threshold=%d\n",
                ms_since_boot(), LOAD_THRESHOLD_RAW);
}
