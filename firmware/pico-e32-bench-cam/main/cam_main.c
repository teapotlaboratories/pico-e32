/* pico-e32-bench-cam — the bench capture camera (M5Stack Timer Camera F).
 *
 * Hardware-in-the-loop verification: the camera is aimed at the panel under test. It joins
 * WiFi and serves a still JPEG at  GET http://<ip>/capture  so the dev box can run a
 * flash -> capture -> inspect loop (tools/capture_frame.sh).
 *
 * This is NOT part of the handheld firmware — it is bench equipment.
 * Pin map: espressif/arduino-esp32 CameraWebServer camera_pins.h.
 *
 * WiFi credentials are NOT stored in the tree — they are passed as build-time macros:
 *   make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 \
 *        WIFI_SSID='my ssid' WIFI_PASS='my pass'                                    */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"

#if !defined(WIFI_SSID) || !defined(WIFI_PASS)
#error "WIFI_SSID/WIFI_PASS not defined. Pass them on the build command (they are never stored in the tree): make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 WIFI_SSID='ssid' WIFI_PASS='pass'"
#endif

static const char *TAG = "bench-cam";

/* ---- Camera pin map: M5Stack Timer Camera X / F (OV3660) ----
 * Source: m5stack/TimerCam-arduino, src/utility/Camera_Class.h:6-22 (the vendor's own
 * library for this board) — all 16 signals verified against it 2026-07-15. If the bench
 * camera is ever swapped, the failure path below prints which of the known maps the
 * attached hardware answers to. */
#define PIN_PWDN  -1
#define PIN_RESET 15
#define PIN_XCLK  27
#define PIN_SIOD  25
#define PIN_SIOC  23
#define PIN_D7    19
#define PIN_D6    36
#define PIN_D5    18
#define PIN_D4    39
#define PIN_D3     5
#define PIN_D2    34
#define PIN_D1    35
#define PIN_D0    32
#define PIN_VSYNC 22
#define PIN_HREF  26
#define PIN_PCLK  21

/* The Timer Camera latches its own power rail: the board only stays up while GPIO 33 is
 * driven high. USB masks this — the rail is held anyway — but on battery the board powers
 * itself off, which is exactly how the vendor implements powerOff() (drive the same pin
 * low). Source: m5stack/TimerCam-arduino, src/utility/Power_Class.h:13 (POWER_HOLD_PIN 33)
 * and Power_Class.cpp:7-8 (pinMode OUTPUT + digitalWrite HIGH in begin()). */
#define PIN_POWER_HOLD 33

static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "wifi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "==== bench-cam ready:  http://" IPSTR "/capture  ====", IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Latch the power rail on. Must run before anything else that matters, and before any
 * long-running work, so an unplugged board does not drop out mid-capture. */
static void power_hold(void)
{
    gpio_config_t h = {
        .pin_bit_mask = 1ULL << PIN_POWER_HOLD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&h);
    gpio_set_level(PIN_POWER_HOLD, 1);
}

static esp_err_t camera_start(void)
{
    camera_config_t c = {
        .pin_pwdn = PIN_PWDN, .pin_reset = PIN_RESET, .pin_xclk = PIN_XCLK,
        .pin_sccb_sda = PIN_SIOD, .pin_sccb_scl = PIN_SIOC,
        .pin_d7 = PIN_D7, .pin_d6 = PIN_D6, .pin_d5 = PIN_D5, .pin_d4 = PIN_D4,
        .pin_d3 = PIN_D3, .pin_d2 = PIN_D2, .pin_d1 = PIN_D1, .pin_d0 = PIN_D0,
        .pin_vsync = PIN_VSYNC, .pin_href = PIN_HREF, .pin_pclk = PIN_PCLK,
        .xclk_freq_hz = 20000000,   /* 20 MHz -> 10 MHz JPEG PCLK. TESTED 2026-07-18: raising to 24 MHz
                                     * (12 MHz PCLK) corrupted EVERY frame incl. SVGA (bad JPEG markers,
                                     * lost MCU blocks) for only ~5-8% more fps -- the classic-ESP32
                                     * I2S/DVP path overruns above 10 MHz PCLK here. Keep 20 MHz. And
                                     * fps at these sizes is bandwidth-bound (~6 Mbit/s), not PCLK-bound,
                                     * so a faster pixel clock wouldn't help even if it were clean. */
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_UXGA,   /* 1600x1200 — the panel is a small part of frame */
        .jpeg_quality = 8,                /* lower = better quality */
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&c);
}

/* Diagnostic for a failed probe: esp_camera_init() reports the same 0x106 whether nothing
 * answered on the bus or a sensor answered with an unrecognised PID, and its probe loop
 * swallows per-address failures. Scan the bus ourselves to tell those apart:
 *   no ACK at all  -> power/wiring/XCLK (sensor is not talking)
 *   ACK at 0x30    -> sensor is alive; the PID read is what's wrong
 * A failed init leaves XCLK disabled, and the sensor cannot clock its SCCB logic without
 * it, so drive the clock here or every address will read as dead. */
static void sccb_scan(int xclk_hz)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = xclk_hz, .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t te = ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = PIN_XCLK, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1, .duty = 1, .hpoint = 0,
    };
    esp_err_t ce = ledc_channel_config(&ch);
    ESP_LOGW(TAG, "scan: XCLK %d Hz on GPIO %d (timer=%s channel=%s)", xclk_hz, PIN_XCLK,
             esp_err_to_name(te), esp_err_to_name(ce));
    vTaskDelay(pdMS_TO_TICKS(20));

    i2c_driver_delete(I2C_NUM_1);   /* camera init may have left it installed */
    i2c_config_t ic = {
        .mode = I2C_MODE_MASTER, .sda_io_num = PIN_SIOD, .scl_io_num = PIN_SIOC,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    if (i2c_param_config(I2C_NUM_1, &ic) != ESP_OK ||
        i2c_driver_install(I2C_NUM_1, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "scan: i2c setup failed");
        return;
    }

    int found = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t r = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (r == ESP_OK) { ESP_LOGW(TAG, "scan:   ACK at 0x%02x", a); found++; }
    }
    ESP_LOGW(TAG, "scan: sda=%d scl=%d xclk=%d -> %d device(s) [OV2640 expected at 0x30]",
             PIN_SIOD, PIN_SIOC, xclk_hz, found);
    i2c_driver_delete(I2C_NUM_1);
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
}

/* Is the sensor even electrically there? An idle I2C bus with a powered sensor has external
 * pull-ups holding both lines high. Fight each line with the (much weaker ~45k) internal
 * pull-down: still reads high -> an external pull-up is winning -> bus is wired and powered.
 * Reads low -> the line is floating -> nothing is connected on these pins. */
static void bus_idle_test(void)
{
    const int pins[2] = { PIN_SIOD, PIN_SIOC };
    const char *names[2] = { "SDA/SIOD", "SCL/SIOC" };
    for (int i = 0; i < 2; i++) {
        gpio_config_t d = {
            .pin_bit_mask = 1ULL << pins[i], .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&d);
        vTaskDelay(pdMS_TO_TICKS(5));
        int pulled = gpio_get_level(pins[i]);
        ESP_LOGW(TAG, "bus: %s (GPIO %2d) with internal pull-down = %d  (%s)",
                 names[i], pins[i], pulled,
                 pulled ? "HIGH -> external pull-up present, bus is alive"
                        : "LOW  -> floating, nothing pulling this line up");
    }
}

/* Probe one address on an arbitrary pin pair. Returns true on ACK. */
static bool probe_at(int sda, int scl, uint8_t addr)
{
    i2c_driver_delete(I2C_NUM_1);
    i2c_config_t ic = {
        .mode = I2C_MODE_MASTER, .sda_io_num = sda, .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    if (i2c_param_config(I2C_NUM_1, &ic) != ESP_OK) return false;
    if (i2c_driver_install(I2C_NUM_1, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) return false;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t r = i2c_master_cmd_begin(I2C_NUM_1, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    i2c_driver_delete(I2C_NUM_1);
    return r == ESP_OK;
}

/* The documented pin map is confirmed by two Espressif sources, but the board is EOL and its
 * schematics are gone — so verify rather than trust: sweep every output-capable pin pair for
 * a sensor answer. A hit means this board's SCCB pins differ from the documented map.
 * (34-39 are input-only and 6-11 are the SPI flash, so both are excluded.) */
static void sccb_sweep(void)
{
    static const int cand[] = { 0, 2, 4, 5, 12, 13, 14, 15, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 };
    static const uint8_t addrs[] = { 0x30, 0x3C };   /* OV2640 / OV5640-OV3660 */
    const int n = sizeof(cand) / sizeof(cand[0]);
    int hits = 0;

    /* sccb_scan() stops the clock on its way out; without XCLK every pin looks dead. */
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = 20000000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = PIN_XCLK, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1, .duty = 1, .hpoint = 0,
    };
    ledc_channel_config(&ch);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGW(TAG, "sweep: trying %d x %d pin pairs for a sensor at 0x30/0x3C ...", n, n - 1);
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            for (int k = 0; k < 2; k++) {
                if (probe_at(cand[i], cand[j], addrs[k])) {
                    ESP_LOGW(TAG, "sweep:  *** ACK 0x%02x on sda=%d scl=%d ***",
                             addrs[k], cand[i], cand[j]);
                    hits++;
                }
            }
        }
    }
    ESP_LOGW(TAG, "sweep: done -> %d hit(s). 0 hits = sensor not powered/connected, "
                  "not a pin-map error.", hits);
}

/* Last unknown: the attached camera may gate its sensor with a power-down/reset line that
 * the configured pin map gets wrong. A sensor held in power-down answers on no pin, so the
 * bus sweep above cannot see past that. Rather than hand-roll the power sequencing, hand
 * each known ESP32 camera board's pin map to the vendor driver and let it do the
 * PWDN/RESET dance. A success identifies which board is actually attached; all of them
 * failing is strong evidence the fault is in the hardware, not the configuration.
 * Maps from espressif/arduino-esp32,
 * libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h — each verified against it
 * 2026-07-15. */
typedef struct { const char *name; camera_config_t cfg; } board_map_t;

#define MAP(nm, pwdn, rst, xclk, sd, sc, d7, d6, d5, d4, d3, d2, d1, d0, vs, hr, pc) \
    { nm, { .pin_pwdn = pwdn, .pin_reset = rst, .pin_xclk = xclk,                    \
            .pin_sccb_sda = sd, .pin_sccb_scl = sc,                                  \
            .pin_d7 = d7, .pin_d6 = d6, .pin_d5 = d5, .pin_d4 = d4,                  \
            .pin_d3 = d3, .pin_d2 = d2, .pin_d1 = d1, .pin_d0 = d0,                  \
            .pin_vsync = vs, .pin_href = hr, .pin_pclk = pc,                         \
            .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0,                    \
            .ledc_channel = LEDC_CHANNEL_0, .pixel_format = PIXFORMAT_JPEG,          \
            .frame_size = FRAMESIZE_QVGA, .jpeg_quality = 12, .fb_count = 1,         \
            .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST } }

static void identify_board(void)
{
    static const board_map_t maps[] = {
        MAP("ESP-EYE (documented)", -1, -1,  4, 18, 23, 36, 37, 38, 39, 35, 14, 13, 34,  5, 27, 25),
        MAP("AI-Thinker ESP32-CAM", 32, -1,  0, 26, 27, 35, 34, 39, 36, 21, 19, 18,  5, 25, 23, 22),
        MAP("ESP32 WROVER-KIT",     -1, -1, 21, 26, 27, 35, 34, 39, 36, 19, 18,  5,  4, 25, 23, 22),
        MAP("M5Stack PSRAM",        -1, 15, 27, 25, 23, 19, 36, 18, 39,  5, 34, 35, 32, 22, 26, 21),
        MAP("M5Stack WIDE",         -1, 15, 27, 22, 23, 19, 36, 18, 39,  5, 34, 35, 32, 25, 26, 21),
        /* TTGO T-Journal is deliberately omitted: its D0 is GPIO 17, which is a PSRAM pin on
         * WROVER modules — probing it takes PSRAM out and panics the board into a boot loop. */
    };
    const int n = sizeof(maps) / sizeof(maps[0]);

    ESP_LOGW(TAG, "identify: trying %d known board pin maps ...", n);
    for (int i = 0; i < n; i++) {
        esp_camera_deinit();              /* clear any state the previous attempt left */
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_err_t r = esp_camera_init(&maps[i].cfg);
        if (r == ESP_OK) {
            sensor_t *s = esp_camera_sensor_get();
            ESP_LOGW(TAG, "identify:  *** %s WORKS (sensor PID=0x%02x) — this is the board ***",
                     maps[i].name, s ? s->id.PID : 0);
            return;
        }
        ESP_LOGW(TAG, "identify:  %-22s -> 0x%x (%s)", maps[i].name, r, esp_err_to_name(r));
    }
    ESP_LOGW(TAG, "identify: no known pin map detects a sensor.");
}

/* Read one unsigned query parameter; returns `dflt` if absent/unparseable. */
static int query_int(const char *q, const char *key, int dflt)
{
    char buf[16];
    if (!q || httpd_query_key_value(q, key, buf, sizeof(buf)) != ESP_OK) return dflt;
    char *end;
    long v = strtol(buf, &end, 10);
    return (end == buf) ? dflt : (int)v;
}

/* Apply a ?size= value (shared by /stream and /capture). Names cover the OV3660's full range
 * qvga..qxga so the fps-vs-resolution ceiling can be swept from the wire. Returns true if it set
 * a size (caller then settles). Unknown value -> false (left unchanged). */
static bool apply_size(sensor_t *s, const char *v)
{
    framesize_t fs;
    if      (!strcmp(v, "qvga")) fs = FRAMESIZE_QVGA;   /* 320x240  */
    else if (!strcmp(v, "vga"))  fs = FRAMESIZE_VGA;    /* 640x480  */
    else if (!strcmp(v, "svga")) fs = FRAMESIZE_SVGA;   /* 800x600  */
    else if (!strcmp(v, "xga"))  fs = FRAMESIZE_XGA;    /* 1024x768 */
    else if (!strcmp(v, "hd"))   fs = FRAMESIZE_HD;     /* 1280x720  (720p) */
    else if (!strcmp(v, "sxga")) fs = FRAMESIZE_SXGA;   /* 1280x1024 */
    else if (!strcmp(v, "uxga")) fs = FRAMESIZE_UXGA;   /* 1600x1200 */
    else if (!strcmp(v, "fhd"))  fs = FRAMESIZE_FHD;    /* 1920x1080 (1080p) */
    else if (!strcmp(v, "qxga")) fs = FRAMESIZE_QXGA;   /* 2048x1536 (sensor max) */
    else return false;
    s->set_framesize(s, fs);
    return true;
}


/* GET /stream -> MJPEG (multipart/x-mixed-replace). Open it in a browser to aim the rig
 * live instead of capturing one frame at a time. Honours whatever exposure/AWB the last
 * /capture set, so what you see is what a capture will grab.
 * Tip: aiming is easier at a smaller frame size -- /stream?size=svga is much smoother than
 * UXGA, and resolution does not matter for pointing the camera. */
#define PART_BOUNDARY "pico-e32-bench-cam-frame"
static const char *STREAM_TYPE     = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART     = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_get(httpd_req_t *req)
{
    char q[64] = { 0 };
    sensor_t *s = esp_camera_sensor_get();
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && s) {
        char v[16];
        if (httpd_query_key_value(q, "size", v, sizeof v) == ESP_OK && apply_size(s, v))
            vTaskDelay(pdMS_TO_TICKS(300));   /* let the sensor settle after a size change */
    }
    esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    ESP_LOGI(TAG, "stream: client connected");

    char part[80];
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }
        int hlen = snprintf(part, sizeof part, STREAM_PART, (unsigned)fb->len);
        if ((res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY))) != ESP_OK ||
            (res = httpd_resp_send_chunk(req, part, hlen)) != ESP_OK ||
            (res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len)) != ESP_OK) {
            esp_camera_fb_return(fb);
            break;                                  /* client closed the tab */
        }
        esp_camera_fb_return(fb);
    }
    ESP_LOGI(TAG, "stream: client gone");
    return res;
}

/* GET /capture -> a fresh still JPEG
 *
 * Optional query params tune the sensor for the subject we actually shoot: a bright,
 * emissive LCD in a dark box. Auto-exposure meters for the surrounding darkness and blows
 * the panel out to flat white, so manual exposure is the norm here, not the exception.
 *   ?exp=<0-1200>  manual exposure; lower = darker      (default: auto)
 *   ?gain=<0-30>   manual gain; lower = less noise/bloom (default: auto)
 *   ?auto=1        force auto exposure/gain back on
 *   ?awb=0|1       auto white balance off/on -- OFF to judge real colour on a panel
 *   ?sat=-2..2     saturation   ?con=-2..2  contrast   ?sharp=-2..2  sharpness
 *   ?size=uxga|xga|svga|vga  frame size -- uxga (1600x1200) for detail; restores it if a
 *                            prior /stream?size=svga (aiming) left the sensor small
 * Values persist in the sensor until changed, so a bare /capture reuses the last setting. */
static esp_err_t capture_get(httpd_req_t *req)
{
    char q[64] = { 0 };
    bool have_q = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK;
    sensor_t *s = esp_camera_sensor_get();

    if (s && have_q) {
        /* Frame size persists in the sensor, so an earlier /stream?size=svga (used for aiming) leaves
         * /capture stuck at SVGA -- soft on the small panel. Accept ?size= here too so one capture URL
         * can restore full resolution, matching /stream. (Init default is UXGA; see FRAMESIZE_UXGA.) */
        char v[16];
        if (httpd_query_key_value(q, "size", v, sizeof v) == ESP_OK && apply_size(s, v))
            vTaskDelay(pdMS_TO_TICKS(300));   /* let the sensor settle after a size change */
        int jq = query_int(q, "q", -1);       /* JPEG quality 0..63 (lower = better/bigger). */
        if (jq >= 0) s->set_quality(s, jq);
        if (query_int(q, "auto", 0)) {
            s->set_exposure_ctrl(s, 1);
            s->set_gain_ctrl(s, 1);
            ESP_LOGI(TAG, "capture: auto exposure/gain");
        }
        int exp = query_int(q, "exp", -1);
        if (exp >= 0) {
            s->set_exposure_ctrl(s, 0);      /* manual */
            s->set_aec_value(s, exp);
        }
        int gain = query_int(q, "gain", -1);
        if (gain >= 0) {
            s->set_gain_ctrl(s, 0);          /* manual */
            s->set_agc_gain(s, gain);
        }
        /* Auto white balance *lies about colour*: it renormalises a red screen toward grey,
         * which is exactly the judgement we use the rig for (is that red or cyan?). Turn it
         * off to read the panel's real hue. ?awb=1 restores it. */
        int awb = query_int(q, "awb", -1);
        if (awb >= 0) {
            s->set_whitebal(s, awb);
            s->set_awb_gain(s, awb);
        }
        int sat = query_int(q, "sat", -99);  if (sat != -99) s->set_saturation(s, sat);  /* -2..2 */
        int con = query_int(q, "con", -99);  if (con != -99) s->set_contrast(s, con);    /* -2..2 */
        int sharp = query_int(q, "sharp", -99); if (sharp != -99) s->set_sharpness(s, sharp);
        int hmir  = query_int(q, "hmir",  -1);  if (hmir  >= 0)   s->set_hmirror(s, hmir);    /* 0/1 */
        int vfl   = query_int(q, "vflip", -1);  if (vfl   >= 0)   s->set_vflip(s, vfl);       /* 0/1 */
        if (exp >= 0 || gain >= 0) ESP_LOGI(TAG, "capture: exp=%d gain=%d awb=%d", exp, gain, awb);
    }

    /* Drop frames so we return a *current* image: one to clear the buffered frame, plus
     * extra when settings just changed, since the sensor needs a few frames to apply them. */
    int drop = have_q ? 4 : 1;
    for (int i = 0; i < drop; i++) {
        camera_fb_t *stale = esp_camera_fb_get();
        if (stale) esp_camera_fb_return(stale);
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return r;
}

static void http_start(void)
{
    httpd_handle_t s = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 4;      /* /stream holds one open indefinitely */
    cfg.stack_size = 8192;
    if (httpd_start(&s, &cfg) == ESP_OK) {
        httpd_uri_t u = { .uri = "/capture", .method = HTTP_GET, .handler = capture_get };
        httpd_register_uri_handler(s, &u);
        httpd_uri_t st = { .uri = "/stream", .method = HTTP_GET, .handler = stream_get };
        httpd_register_uri_handler(s, &st);
        ESP_LOGI(TAG, "http server up (GET /capture, GET /stream)");
    } else ESP_LOGE(TAG, "httpd_start failed");
}

void app_main(void)
{
    power_hold();   /* first: keep the board alive if it is ever run off USB power */

    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    }
    esp_err_t err = camera_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: 0x%x (%s)", err, esp_err_to_name(err));
        sccb_scan(20000000);
        sccb_scan(10000000);   /* some modules only answer at the lower clock */
        bus_idle_test();
        sccb_sweep();
        identify_board();
        return;
    }
    ESP_LOGI(TAG, "camera up (SVGA JPEG)");
    /* The OV3660 h-mirrors by default on this rig, which had been silently cancelling the panel's own
     * display mirror — so a real display bug read as "fine" in captures (caught 2026-07-18). The
     * orientation verified against the real panel is hmirror OFF, vflip ON; combined with the capture
     * tool's `convert -rotate 90` (the 90° mount) a capture then reads the same way round as the panel.
     * Overridable per-request via /capture?hmir=0|1 & ?vflip=0|1. */
    { sensor_t *s = esp_camera_sensor_get(); if (s) { s->set_hmirror(s, 0); s->set_vflip(s, 1); } }
    wifi_start();
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    http_start();
    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
