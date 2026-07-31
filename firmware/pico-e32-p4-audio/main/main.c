/* pico-e32-p4-audio — minimal ES8311 tone test (no fake-08, no display).
 *
 * Brings up the ES8311 codec + an I2S standard TX channel and plays a continuous sine, so we can tell
 * whether the AUDIO HARDWARE path (codec -> DAC -> speaker amp -> speaker) works at all, independent of
 * fake-08's audio buffering. If this tone is audible but Celeste is silent, the bug is in the fake-08
 * wiring; if this is ALSO silent, it's the codec/amp/routing (most likely a speaker-amp ENABLE pin).
 *
 * Pins (docs/hardware/pico-e32-guition-jc4880p443c-p4.md): I2C SDA7/SCL8 @0x18; I2S MCLK13/BCLK12/WS10/DOUT9.
 *   make build APP=pico-e32-p4-audio BOARD=guition-jc4880p443c
 * The board has no documented PA-enable pin. To hunt for it, drive a candidate GPIO high before playing:
 *   make build ... DEFS='-D PA_GPIO=16'    (then flash + listen; sweep candidates)
 */
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#define I2C_SDA   GPIO_NUM_7
#define I2C_SCL   GPIO_NUM_8
#define A_MCLK    GPIO_NUM_13
#define A_BCLK    GPIO_NUM_12
#define A_WS      GPIO_NUM_10
#define A_DOUT    GPIO_NUM_9
#ifndef SR
#define SR        48000        /* override to match fake-08's 22050 via DEFS='-D SR=22050' */
#endif
#ifndef TONE_HZ
#define TONE_HZ   1000         /* pick a non-60Hz-multiple so mains hum can't be mistaken for the tone */
#endif
#ifndef PA_GPIO
#define PA_GPIO   (-1)         /* -1 = don't drive any amp-enable pin (test the always-on assumption) */
#endif

static const char *TAG = "p4-audio";

void app_main(void)
{
    ESP_LOGI(TAG, "minimal ES8311 tone test — SR=%d tone=%dHz PA_GPIO=%d", SR, TONE_HZ, PA_GPIO);

    /* Optional speaker-amp enable pin (sweep candidates via -D PA_GPIO=NN). #if, not runtime if, so the
     * default PA_GPIO=-1 doesn't compile a negative shift. */
#if PA_GPIO >= 0
    {
        gpio_config_t io = { .pin_bit_mask = 1ULL << (PA_GPIO), .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(PA_GPIO, 1);
        ESP_LOGW(TAG, "drove GPIO%d HIGH as a candidate speaker-amp enable", PA_GPIO);
    }
#endif

    /* I2C master bus (shared touch/codec bus on the real board). */
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bcfg, &bus));

    /* I2S standard TX channel (ESP is the master). */
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t ccfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&ccfg, &tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = A_MCLK, .bclk = A_BCLK, .ws = A_WS, .dout = A_DOUT, .din = I2S_GPIO_UNUSED },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));

    /* ES8311 via esp_codec_dev. */
    audio_codec_i2c_cfg_t icfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&icfg);
    audio_codec_i2s_cfg_t dcfg = { .port = I2S_NUM_0, .tx_handle = tx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&dcfg);
    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl, .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .master_mode = false,
        .use_mclk = true, .pa_pin = -1, .mclk_div = 256,
    };
    const audio_codec_if_t *es_if = es8311_codec_new(&es);
    esp_codec_dev_cfg_t devcfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es_if, .data_if = data };
    esp_codec_dev_handle_t dev = esp_codec_dev_new(&devcfg);
    esp_codec_dev_sample_info_t si = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = SR };
    ESP_ERROR_CHECK(esp_codec_dev_open(dev, &si));
    esp_codec_dev_set_out_vol(dev, 90);
    ESP_LOGI(TAG, "ES8311 open OK — playing a looping arpeggio at volume 90 (listen at the speaker)");

    /* A short rising/falling arpeggio, looped — a CHANGING pitch is far easier to pick out by ear from
     * the bench's steady mains hum than a single tone. Each note is generated on the fly (stereo S16) with
     * a few-ms fade in/out so note boundaries don't click. */
    static const int NOTES[] = { 392, 523, 659, 784, 988, 784, 659, 523 };  /* G4 C5 E5 G5 B5 G5 E5 C5 */
    const int NN = sizeof(NOTES) / sizeof(NOTES[0]);
    const int note_frames = SR / 5;          /* 200 ms per note */
    const int fade = SR / 200;               /* 5 ms fade */
    int16_t *buf = malloc((size_t)note_frames * 2 * sizeof(int16_t));
    uint32_t loops = 0;
    for (int idx = 0; ; idx++) {
        int f = NOTES[idx % NN];
        for (int i = 0; i < note_frames; i++) {
            double env = 1.0;
            if (i < fade)                     env = (double)i / fade;
            else if (i > note_frames - fade)  env = (double)(note_frames - i) / fade;
            int16_t s = (int16_t)(14000.0 * env * sin(2.0 * M_PI * f * i / SR));
            buf[i * 2] = s; buf[i * 2 + 1] = s;
        }
        esp_codec_dev_write(dev, buf, (int)((size_t)note_frames * 2 * sizeof(int16_t)));
        if (idx % NN == (NN - 1)) ESP_LOGI(TAG, "arpeggio loop %lu", (unsigned long)(++loops));
    }
}
