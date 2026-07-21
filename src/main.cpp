// ============================================================
// Fall Detection Firmware - Main Logic
// Architecture:
//   Core 1 (loop): IMU Sampling (50ms) + DoG Filter + TFLite FSM
//   Core 0 (networkTask): Wi-Fi/ERa keep-alive + Telegram alert
//   IPC: FreeRTOS Queue (systemEventQueue)
// ============================================================

#define DATA_COLLECTION_MODE 0
#if !DATA_COLLECTION_MODE

#define DEFAULT_MQTT_HOST "mqtt1.eoh.io"
#define ERA_AUTH_TOKEN "6b536517-4be6-48c7-87d9-3ade27854876"
#define ERA_VIRTUAL_WRITE_LEGACY

#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <ERaSimpleEsp32.hpp>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "DeviceIMU.h"
#include "DeviceWifi.h"
#include "model.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// ============================================================
// Hardware Configuration
// ============================================================
#define LOOP_DELAY_MS   50      // IMU sampling interval = 20 Hz
#define GPIO_BUTTON     18      // Cancel button (active LOW)
#define GPIO_BUZZER     19      // Buzzer output

// ============================================================
// Algorithm Configuration
// ============================================================
#define INFERENCE_WINDOW_SIZE   64
#define DOG_RADIUS              60
#define DOG_SIZE                (DOG_RADIUS * 2 + 1)
#define CIRCULARBUFFER_SIZE     192
#define FALL_FEATURE_THRESHOLD  0.07f
#define CBC                     4       // Circular buffer channels
#define SBC                     2       // Supporting buffer channels

// ============================================================
// Alert Timing
// ============================================================
#define COUNTDOWN_DURATION_MS   15000   // 15 s before fall is confirmed
#define ALERTING_DURATION_MS    40000   // 40 s of siren then auto-reset

// ============================================================
// Logging
// ============================================================
#define LOGGING 1
#if LOGGING
#define LOG(x) Serial.println(x)
#else
#define LOG(x)
#endif

// ============================================================
// ERa Virtual Pin Map
// ============================================================
#define VPIN_FSM_STATE    V0   // String  – current FSM state name
#define VPIN_ALERT_STATUS V1   // Integer – 0=OK, 1=FALL DETECTED
#define VPIN_LAST_FALL    V2   // String  – uptime (ms) when last fall occurred
#define VPIN_RSSI         V3   // Integer – Wi-Fi RSSI (dBm)
#define VPIN_RESET        V4   // Button  – remote reset (only active in ALERTING)
#define VPIN_UPTIME       V5   // Integer – device uptime in seconds
#define VPIN_CONFIDENCE   V6   // Double  – AI confidence percentage (0-100)
#define VPIN_ALERT_COUNT  V7   // Integer – total number of confirmed falls

// ============================================================
// FSM States
// ============================================================
enum SystemState : uint8_t {
    STATE_MONITORING,   // Continuously reading IMU & running naive filter
    STATE_TRIGGER,      // Shock detected; buffering 32 more samples post-shock
    STATE_INFERENCE,    // Running TFLite classification on 64-sample window
    STATE_COUNTDOWN,    // Fall confirmed by AI; 15 s cancel window (local only)
    STATE_ALERTING,     // Alarm: local siren + Cloud alerts sent
    STATE_CANCELLED     // User cancelled via hardware button
};

const char* stateNames[] = {
    "MONITORING", "TRIGGER", "INFERENCE",
    "COUNTDOWN",  "ALERTING", "CANCELLED"
};

// ============================================================
// IPC: FreeRTOS Queue between Core 1 (FSM) and Core 0 (Network)
// ============================================================
enum EventType : uint8_t {
    EVT_STATE_CHANGE,       // Carry new state for ERa telemetry
    EVT_FALL_CONFIRMED,     // Trigger Telegram + ERa alert (once)
    EVT_ALERT_CLEARED       // Carry clear status after reset
};

struct SystemEvent {
    EventType type;
    SystemState state;
    float confidence;
};

static QueueHandle_t systemEventQueue;

// ============================================================
// Shared flags (written by ERa callback on Core 0,
// read by FSM on Core 1 – atomic uint8_t is sufficient on ESP32)
// ============================================================
static volatile uint8_t remoteResetFlag = 0;
static int alertCount = 0;

// ============================================================
// Sensor & Algorithm Buffers
// ============================================================
static float circular_buffer[CIRCULARBUFFER_SIZE * CBC]          = {};
static float supporting_circular_buffer[CIRCULARBUFFER_SIZE * SBC] = {};
static float dog_kernel[DOG_SIZE];

// ============================================================
// TFLite objects
// ============================================================
static tflite::ErrorReporter*    error_reporter = nullptr;
static const tflite::Model*      tfl_model      = nullptr;
static tflite::MicroInterpreter* interpreter    = nullptr;
static TfLiteTensor*             tfl_input      = nullptr;
static TfLiteTensor*             tfl_output     = nullptr;

#define kTensorArenaSize 20000
static uint8_t tensor_arena[kTensorArenaSize];

// ============================================================
// ERa remote-reset callback
// Only effective when FSM is in STATE_ALERTING (safety rule)
// ============================================================
ERA_WRITE(VPIN_RESET) {
    if (param.getInt() == 1) {
        remoteResetFlag = 1;
        LOG("[ERa] Remote reset requested");
    }
}

// ============================================================
// Network Task (Core 0)
// Responsibilities:
//   1. Non-blocking Wi-Fi & ERa keep-alive
//   2. Periodic telemetry to ERa (every 5 s)
//   3. Fire-and-forget Telegram + ERa alert on FALL event
// ============================================================
static void networkTask(void* /*param*/) {
    LOG("[Net] Task started on Core 0");

    // --- Connect Wi-Fi (blocking once at startup) ---
    DeviceWifi.connect(1);
    DeviceWifi.waitForConnect(1);

    // --- Configure NTP Time ---
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    LOG("[Net] NTP configured");

    // --- Configure ERa ---
    ERa.begin(ssid, password);

    unsigned long lastTelemetry = 0;

    for (;;) {
        // 1. Keep Wi-Fi & ERa alive
        if (WiFi.status() != WL_CONNECTED) {
            LOG("[Net] Wi-Fi lost – reconnecting...");
            DeviceWifi.connect(0);
            DeviceWifi.waitForConnect(0);
        }
        ERa.run();

        // 2. Periodic telemetry (every 5 s)
        if (millis() - lastTelemetry >= 5000 && ERa.connected()) {
            lastTelemetry = millis();
            int rssi = WiFi.RSSI();
            int wifi_percentage = 0;
            if (rssi <= -100) wifi_percentage = 0;
            else if (rssi >= -50) wifi_percentage = 100;
            else wifi_percentage = 2 * (rssi + 100);

            ERa.virtualWrite(VPIN_RSSI,        wifi_percentage);
            ERa.virtualWrite(VPIN_UPTIME,      (int)(millis() / 1000));
            ERa.virtualWrite(VPIN_ALERT_COUNT, alertCount);
        }

        // 3. Consume events from FSM (non-blocking, 100 ms timeout)
        SystemEvent ev;
        if (xQueueReceive(systemEventQueue, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (ev.type) {
                // --- Update ERa state label & AI confidence ---
                case EVT_STATE_CHANGE:
                    if (ERa.connected()) {
                        ERa.virtualWrite(VPIN_FSM_STATE, stateNames[ev.state]);
                        ERa.virtualWrite(VPIN_CONFIDENCE, ev.confidence);
                    }
                    break;

                // --- Send fall alert (ERa V1 + Telegram, max 3 retries each) ---
                case EVT_FALL_CONFIRMED: {
                    LOG("[Net] FALL event received – sending alerts");

                    // Increment and update ERa alert count and status
                    alertCount++;
                    if (ERa.connected()) {
                        time_t now = time(nullptr);
                        struct tm timeinfo;
                        char timeStr[32] = "-";
                        if (getLocalTime(&timeinfo)) {
                            strftime(timeStr, sizeof(timeStr), "%H:%M:%S %d/%m/%Y", &timeinfo);
                        } else {
                            snprintf(timeStr, sizeof(timeStr), "Uptime %lu s", millis() / 1000);
                        }

                        ERa.virtualWrite(VPIN_ALERT_STATUS, 1);
                        ERa.virtualWrite(VPIN_FSM_STATE,    "ALERTING");
                        ERa.virtualWrite(VPIN_LAST_FALL,    timeStr);
                        ERa.virtualWrite(VPIN_CONFIDENCE,   ev.confidence);
                        ERa.virtualWrite(VPIN_ALERT_COUNT,  alertCount);
                        LOG("[Net] Fall time logged to ERa (string): " + String(timeStr));
                    }

                    // Send Telegram HTTPS POST (retry up to 3 times, 2 s apart)
                    const int   MAX_RETRIES    = 3;
                    const int   RETRY_DELAY_MS = 2000;
                    const char* MSG = "⚠️ CẢNH BÁO TÉ NGÃ! Thiết bị phát hiện cú ngã khẩn cấp. Vui lòng kiểm tra ngay!";

                    bool sent = false;
                    for (int attempt = 1; attempt <= MAX_RETRIES && !sent; attempt++) {
                        if (WiFi.status() != WL_CONNECTED) {
                            LOG("[Net] No Wi-Fi – Telegram retry " + String(attempt));
                            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                            continue;
                        }

                        WiFiClientSecure client;
                        client.setInsecure(); // skip CA verification (acceptable for student project)
                        HTTPClient http;

                        String url = "https://api.telegram.org/bot";
                        url += telegram_bot_token;
                        url += "/sendMessage";

                        String payload = "{\"chat_id\":\"";
                        payload += telegram_chat_id;
                        payload += "\",\"text\":\"";
                        payload += MSG;
                        payload += "\"}";

                        http.begin(client, url);
                        http.addHeader("Content-Type", "application/json");
                        int code = http.POST(payload);
                        http.end();

                        if (code == 200 || code == 201) {
                            sent = true;
                            LOG("[Net] Telegram sent OK (attempt " + String(attempt) + ")");
                        } else {
                            LOG("[Net] Telegram fail – HTTP " + String(code) + " (attempt " + String(attempt) + ")");
                            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                        }
                    }
                    break;
                }

                // --- Clear alert on ERa Dashboard ---
                case EVT_ALERT_CLEARED:
                    if (ERa.connected()) {
                        ERa.virtualWrite(VPIN_ALERT_STATUS, 0);
                        ERa.virtualWrite(VPIN_FSM_STATE,    "MONITORING");
                    }
                    break;
            }
        }
    }
}

// ============================================================
// Helpers
// ============================================================
static void sendEvent(EventType type, SystemState state = STATE_MONITORING, float confidence = 0.0f) {
    SystemEvent ev = { type, state, confidence };
    xQueueSend(systemEventQueue, &ev, 0 /*don't block*/);
}

// ============================================================
// setup()
// ============================================================
void setup() {
#if LOGGING
    Serial.begin(115200);
    delay(1000);
    LOG("[SYS] Fall Detector starting...");
    LOG("[SYS] sizeof(SystemEvent) = " + String(sizeof(SystemEvent)) + " bytes");
#endif

    // --- TFLite setup ---
    static tflite::MicroErrorReporter micro_reporter;
    error_reporter = &micro_reporter;

    tfl_model = tflite::GetModel(model_tflite);
    if (tfl_model->version() != TFLITE_SCHEMA_VERSION) {
        LOG("[TFL] Schema version mismatch!");
        while (true) {}
    }

    static tflite::AllOpsResolver resolver;
    static tflite::MicroInterpreter static_interp(
        tfl_model, resolver, tensor_arena, kTensorArenaSize, error_reporter);
    interpreter = &static_interp;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        LOG("[TFL] AllocateTensors failed!");
        while (true) {}
    }
    tfl_input  = interpreter->input(0);
    tfl_output = interpreter->output(0);

    // --- IMU setup ---
    DeviceIMU.setup(21, 22); // SDA=21, SCL=22

    // --- DoG kernel precompute ---
    const float sigma_s = 6.6f;
    const float sigma_l = sigma_s * 3.0f;
    float sum = 0;
    float sg[DOG_SIZE], lg[DOG_SIZE];
    for (int i = -DOG_RADIUS; i <= DOG_RADIUS; i++) {
        sg[i + DOG_RADIUS] = expf(-0.5f * powf(i / sigma_s, 2));
        sum += sg[i + DOG_RADIUS];
    }
    for (int i = 0; i < DOG_SIZE; i++) sg[i] /= sum;
    sum = 0;
    for (int i = -DOG_RADIUS; i <= DOG_RADIUS; i++) {
        lg[i + DOG_RADIUS] = expf(-0.5f * powf(i / sigma_l, 2));
        sum += lg[i + DOG_RADIUS];
    }
    for (int i = 0; i < DOG_SIZE; i++) { lg[i] /= sum; dog_kernel[i] = sg[i] - lg[i]; }

    // --- GPIO setup ---
    pinMode(GPIO_BUTTON, INPUT_PULLUP);
    pinMode(GPIO_BUZZER, OUTPUT);
    tone(GPIO_BUZZER, 1200, 200); // startup beep

    // --- IPC Queue (capacity 8 events) ---
    systemEventQueue = xQueueCreate(8, sizeof(SystemEvent));

    // --- Launch Network Task on Core 0 ---
    xTaskCreatePinnedToCore(networkTask, "NetTask", 8192, nullptr, 1, nullptr, 0);

    LOG("[SYS] Setup complete. Entering monitoring loop on Core 1.");
}

// ============================================================
// loop() – runs entirely on Core 1
// Tick rate: 50 ms (20 Hz)
// ============================================================
void loop() {
    // ---- Timing gate: enforce exact 50 ms tick ----
    static unsigned long prevTick = 0;
    unsigned long now = millis();
    if (now - prevTick < LOOP_DELAY_MS) return;
    prevTick = now;

    // ---- Read accelerometer ----
    AccelData a;
    DeviceIMU.device.update();
    DeviceIMU.device.getAccel(&a);

    const float ax = a.accelX;
    const float ay = a.accelY;
    const float az = a.accelZ;

    static int          windex     = 0;
    static long long    loop_count = 0;

    windex = (windex + 1) % CIRCULARBUFFER_SIZE;
    circular_buffer[windex * CBC + 0] = ax;
    circular_buffer[windex * CBC + 1] = ay;
    circular_buffer[windex * CBC + 2] = az;
    circular_buffer[windex * CBC + 3] = 0.0f; // placeholder for custom feature

    // ---- DoG + Angle custom feature computation ----
    // We compute the feature for index fi (DOG_RADIUS steps behind windex)
    const int fi = (windex - DOG_RADIUS + CIRCULARBUFFER_SIZE) % CIRCULARBUFFER_SIZE;
    float final_feature = 0.0f;
    {
        const float mag_acc = sqrtf(ax * ax + ay * ay + az * az);
        supporting_circular_buffer[windex * SBC] = mag_acc;

        static float sx = 0, sy = 0, sz = 0, ang_sum = 0;

        const int fiP28 = (fi + 28)  % CIRCULARBUFFER_SIZE;
        const int fiM12 = (fi - 12 + CIRCULARBUFFER_SIZE) % CIRCULARBUFFER_SIZE;
        const int fiP8  = (fi +  8)  % CIRCULARBUFFER_SIZE;
        const int fiM8  = (fi -  8 + CIRCULARBUFFER_SIZE) % CIRCULARBUFFER_SIZE;

        sx += circular_buffer[fiP28 * CBC];     sx -= circular_buffer[fiM12 * CBC];
        sy += circular_buffer[fiP28 * CBC + 1]; sy -= circular_buffer[fiM12 * CBC + 1];
        sz += circular_buffer[fiP28 * CBC + 2]; sz -= circular_buffer[fiM12 * CBC + 2];

        const float lx = sx / 40.f, ly = sy / 40.f, lz = sz / 40.f;
        const float px = circular_buffer[fiP8 * CBC],
                    py = circular_buffer[fiP8 * CBC + 1],
                    pz = circular_buffer[fiP8 * CBC + 2];

        const float mag_lp = sqrtf(lx*lx + ly*ly + lz*lz);
        const float mag_p8 = supporting_circular_buffer[fiP8 * SBC];
        const float cos_a  = (px*lx + py*ly + pz*lz) / (mag_p8 * mag_lp + 1e-6f);
        const float angle  = acosf(fminf(fmaxf(cos_a, -1.f), 1.f));

        supporting_circular_buffer[fiP8 * SBC + 1] = angle;
        ang_sum += angle;
        ang_sum -= supporting_circular_buffer[fiM8 * SBC + 1];
        const float angle_lp = ang_sum / 16.f;

        float dog_val = 0;
        for (int i = 0; i < DOG_SIZE; i++) {
            const int idx = (fi - DOG_RADIUS + i + CIRCULARBUFFER_SIZE) % CIRCULARBUFFER_SIZE;
            dog_val += supporting_circular_buffer[idx * SBC] * dog_kernel[i];
        }
        final_feature = dog_val * angle_lp;
        circular_buffer[fi * CBC + 3] = final_feature;
    }

    loop_count++;

    // ============================================================
    // FSM
    // ============================================================
    static SystemState  state              = STATE_MONITORING;
    static SystemState  prev_state         = STATE_MONITORING;
    static float        max_feature        = -1.f;
    static long long    shock_peak_index   = 0;
    static long long    trigger_start_loop = 0;
    static unsigned long state_enter_time  = 0;
    static float        latest_confidence  = 0.0f;

    // ---- Broadcast state change to Network Task ----
    if (state != prev_state) {
        sendEvent(EVT_STATE_CHANGE, state, latest_confidence);
        prev_state = state;
        state_enter_time = millis();
        LOG(String("[FSM] -> ") + stateNames[state]);
    }

    // ---- Hardware button: cancel fall during COUNTDOWN ----
    const bool btn_pressed = (digitalRead(GPIO_BUTTON) == LOW);

    switch (state) {

    // ----------------------------------------------------------
    case STATE_MONITORING: {
        // Naive threshold detection: find peak of feature above threshold
        if (final_feature > FALL_FEATURE_THRESHOLD && loop_count > CIRCULARBUFFER_SIZE) {
            if (final_feature > max_feature) {
                max_feature      = final_feature;
                shock_peak_index = loop_count - DOG_RADIUS;
            }
        } else if (shock_peak_index != 0 && final_feature < FALL_FEATURE_THRESHOLD) {
            // Feature dropped back below threshold → shock event recorded
            trigger_start_loop = loop_count;
            max_feature        = -1.f;
            state              = STATE_TRIGGER;
        }
        break;
    }

    // ----------------------------------------------------------
    case STATE_TRIGGER: {
        // Buffer DOG_RADIUS more samples so the inference window is centered
        // on the shock peak and includes its aftermath
        if (loop_count >= shock_peak_index + (INFERENCE_WINDOW_SIZE / 2 + DOG_RADIUS)) {
            shock_peak_index = 0;
            state            = STATE_INFERENCE;
        }
        break;
    }

    // ----------------------------------------------------------
    case STATE_INFERENCE: {
        LOG("[TFL] Invoking classifier...");
        unsigned long t0 = millis();

        // Copy 64-sample window into TFLite input tensor
        const int start_w  = (windex - (DOG_RADIUS + INFERENCE_WINDOW_SIZE) + CIRCULARBUFFER_SIZE) % CIRCULARBUFFER_SIZE;
        const int part1    = min(CIRCULARBUFFER_SIZE - start_w, INFERENCE_WINDOW_SIZE);
        const int part2    = INFERENCE_WINDOW_SIZE - part1;
        memcpy(tfl_input->data.f,            &circular_buffer[start_w * CBC], part1 * CBC * sizeof(float));
        if (part2 > 0) memcpy(tfl_input->data.f + part1 * CBC, &circular_buffer[0],        part2 * CBC * sizeof(float));

        if (interpreter->Invoke() != kTfLiteOk) {
            LOG("[TFL] Invoke failed");
            state = STATE_MONITORING;
            break;
        }

        const float prob = tfl_output->data.f[0];
        LOG("[TFL] P(fall)=" + String(prob, 4) + " took " + String(millis() - t0) + " ms");
        latest_confidence = roundf(prob * 10000.0f) / 100.0f;

        state = (prob >= 0.5f) ? STATE_COUNTDOWN : STATE_MONITORING;
        break;
    }

    // ----------------------------------------------------------
    case STATE_COUNTDOWN: {
        const unsigned long elapsed = millis() - state_enter_time;

        // User pressed physical button → cancel
        if (btn_pressed) {
            state = STATE_CANCELLED;
            break;
        }

        // Countdown expired → escalate to full alert
        if (elapsed >= COUNTDOWN_DURATION_MS) {
            state = STATE_ALERTING;
            sendEvent(EVT_FALL_CONFIRMED, STATE_ALERTING, latest_confidence);
            break;
        }

        // Local audio feedback: beep frequency increases as countdown runs down
        {
            const unsigned long beepInterval = 1000UL - (900UL * elapsed) / COUNTDOWN_DURATION_MS;
            static unsigned long lastBeep = 0;
            if (millis() - lastBeep >= beepInterval) {
                lastBeep = millis();
                tone(GPIO_BUZZER, 2000, 80);
            }
        }
        break;
    }

    // ----------------------------------------------------------
    case STATE_ALERTING: {
        // SAFETY RULE: Remote Reset is only accepted in this state
        if (remoteResetFlag) {
            remoteResetFlag = 0;
            latest_confidence = 0.0f;
            sendEvent(EVT_ALERT_CLEARED);
            state = STATE_MONITORING;
            LOG("[FSM] Remote reset accepted");
            break;
        }

        // Auto-reset after 40 s
        if (millis() - state_enter_time >= ALERTING_DURATION_MS) {
            latest_confidence = 0.0f;
            sendEvent(EVT_ALERT_CLEARED);
            state = STATE_MONITORING;
            LOG("[FSM] Alert timeout – auto reset");
            break;
        }

        // Local siren: alternating tones 250 ms apart
        {
            static unsigned long lastSiren = 0;
            static bool hi = false;
            if (millis() - lastSiren >= 250) {
                lastSiren = millis();
                hi = !hi;
                tone(GPIO_BUZZER, hi ? 1600 : 1100, 200);
            }
        }
        break;
    }

    // ----------------------------------------------------------
    case STATE_CANCELLED: {
        // Rising confirmation melody, then return to monitoring
        tone(GPIO_BUZZER, 800,  120); delay(140);
        tone(GPIO_BUZZER, 1200, 120); delay(140);
        tone(GPIO_BUZZER, 1600, 250); delay(270);
        latest_confidence = 0.0f;
        sendEvent(EVT_ALERT_CLEARED);
        state = STATE_MONITORING;
        break;
    }

    } // end switch
}
#endif