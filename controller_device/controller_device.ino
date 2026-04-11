/*  controller_device.ino
 *  ─────────────────────
 *  ESP32 + 5" capacitive touch (800×480) — your bedside controller.
 *  Connects to the same WiFi, listens for UDP requests from the door,
 *  lets you approve or deny, and sends the response back.
 *
 *  Board:  ESP32-S3 (or whichever your 5" screen uses)
 *  Library deps:  lvgl, LovyanGFX (or your display driver), WiFi
 */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <lvgl.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <driver/i2c.h>
#include <Wire.h>
#include "protocol.h"   /* shared header one level up */

/* ═══════════════════════════════════════════════
 *  CrowPanel 5.0" display driver (LovyanGFX)
 *  Pin mapping & timing from Elecrow official code
 * ═══════════════════════════════════════════════ */

class LGFX : public lgfx::LGFX_Device {
public:
    lgfx::Bus_RGB      _bus_instance;
    lgfx::Panel_RGB    _panel_instance;
    lgfx::Light_PWM    _light_instance;
    lgfx::Touch_GT911  _touch_instance;

    LGFX(void) {
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width  = 800;
            cfg.memory_height = 480;
            cfg.panel_width   = 800;
            cfg.panel_height  = 480;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            _panel_instance.config(cfg);
        }
        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;

            cfg.pin_d0  = GPIO_NUM_8;   // B0
            cfg.pin_d1  = GPIO_NUM_3;   // B1
            cfg.pin_d2  = GPIO_NUM_46;  // B2
            cfg.pin_d3  = GPIO_NUM_9;   // B3
            cfg.pin_d4  = GPIO_NUM_1;   // B4

            cfg.pin_d5  = GPIO_NUM_5;   // G0
            cfg.pin_d6  = GPIO_NUM_6;   // G1
            cfg.pin_d7  = GPIO_NUM_7;   // G2
            cfg.pin_d8  = GPIO_NUM_15;  // G3
            cfg.pin_d9  = GPIO_NUM_16;  // G4
            cfg.pin_d10 = GPIO_NUM_4;   // G5

            cfg.pin_d11 = GPIO_NUM_45;  // R0
            cfg.pin_d12 = GPIO_NUM_48;  // R1
            cfg.pin_d13 = GPIO_NUM_47;  // R2
            cfg.pin_d14 = GPIO_NUM_21;  // R3
            cfg.pin_d15 = GPIO_NUM_14;  // R4

            cfg.pin_henable = GPIO_NUM_40;
            cfg.pin_vsync   = GPIO_NUM_41;
            cfg.pin_hsync   = GPIO_NUM_39;
            cfg.pin_pclk    = GPIO_NUM_0;
            cfg.freq_write  = 12000000;

            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 43;

            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 8;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 12;

            cfg.pclk_active_neg   = 0;
            cfg.de_idle_high      = 0;
            cfg.pclk_idle_high    = 0;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = GPIO_NUM_2;
            _light_instance.config(cfg);
            _panel_instance.light(&_light_instance);
        }
        {
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = 799;
            cfg.y_min      = 0;
            cfg.y_max      = 479;
            cfg.pin_int    = -1;
            cfg.pin_rst    = -1;
            cfg.bus_shared = true;
            cfg.offset_rotation = 0;
            cfg.i2c_port   = I2C_NUM_1;
            cfg.pin_sda    = GPIO_NUM_19;
            cfg.pin_scl    = GPIO_NUM_20;
            cfg.freq       = 400000;
            cfg.i2c_addr   = 0x14;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

static LGFX lcd;

/* ─── PCA9557 I/O expander (display reset/enable on V3.0 boards) ─── */
#define PCA9557_ADDR       0x18
#define PCA9557_OUTPUT_REG 0x01
#define PCA9557_CONFIG_REG 0x03

static void pca9557_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(PCA9557_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

/* ─── Networking ─── */
static WiFiUDP   udp;
static IPAddress door_ip;
static bool      door_known = false;

/* ─── Request state ─── */
static char req_person[16]  = "";
static int  req_urgency     = 0;
static char req_reason[16]  = "";

/* ─── Forward declarations (UI) ─── */
static void create_idle_screen(void);
static void create_request_screen(const char *person, int urgency,
                                   const char *reason);
static void create_result_screen(bool approved);

/* ════════════════════════════════════════════════
 *  Networking
 * ════════════════════════════════════════════════ */

static void wifi_connect(void) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void send_response(uint8_t msg_type) {
    if (!door_known) return;

    door_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = msg_type;
    /* person/urgency/reason not needed for responses but zero-filled */

    udp.beginPacket(door_ip, COMM_PORT);
    udp.write((const uint8_t *)&pkt, sizeof(pkt));
    udp.endPacket();
}

/* Polled by an LVGL timer every 100 ms */
static void check_incoming(lv_timer_t *t) {
    (void)t;
    int len = udp.parsePacket();
    if (len < 1) return;

    door_packet_t pkt;
    udp.read((uint8_t *)&pkt, sizeof(pkt));

    /* Remember send's IP */
    door_ip    = udp.remoteIP();
    door_known = true;

    if (pkt.msg_type == MSG_REQUEST) {
        /* Null-terminate just in case */
        pkt.person[sizeof(pkt.person) - 1] = '\0';
        pkt.reason[sizeof(pkt.reason) - 1] = '\0';

        strncpy(req_person, pkt.person, sizeof(req_person) - 1);
        req_urgency = pkt.urgency;
        strncpy(req_reason, pkt.reason, sizeof(req_reason) - 1);

        create_request_screen(req_person, req_urgency, req_reason);
    }
}

/* ════════════════════════════════════════════════
 *  Pill-button helper
 * ════════════════════════════════════════════════ */

static lv_obj_t *make_pill_btn(lv_obj_t *parent, const char *txt,
                                uint32_t bg, int w, int h) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 15, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 25, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(btn, lv_color_lighten(lv_color_hex(bg), 40),
                              LV_STATE_PRESSED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ════════════════════════════════════════════════
 *  Result screen  (after you approve / deny)
 * ════════════════════════════════════════════════ */

static void return_idle_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    create_idle_screen();
}

static void create_result_screen(bool approved) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, approved ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(icon,
        lv_color_hex(approved ? 0x27AE60 : 0xE74C3C), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *msg = lv_label_create(scr);
    char buf[64];
    snprintf(buf, sizeof(buf), approved ? "%s may enter" : "%s was denied",
             req_person);
    lv_label_set_text(msg, buf);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 20);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
    lv_timer_create(return_idle_timer_cb, 3000, NULL);
}

/* ════════════════════════════════════════════════
 *  Request screen  (someone is at the door!)
 * ════════════════════════════════════════════════ */

static void approve_click_cb(lv_event_t *e) {
    (void)e;
    send_response(MSG_APPROVE);
    create_result_screen(true);
}

static void deny_click_cb(lv_event_t *e) {
    (void)e;
    send_response(MSG_DENY);
    create_result_screen(false);
}

static void create_request_screen(const char *person, int urgency,
                                   const char *reason) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* Bell */
    lv_obj_t *bell = lv_label_create(scr);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(bell, lv_color_hex(0x13F0EC), 0);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_36, 0);
    lv_obj_align(bell, LV_ALIGN_TOP_MID, 0, 20);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "%s is at the door!", person);
    lv_label_set_text(title, tbuf);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 65);

    /* Info row */
    lv_obj_t *info_row = lv_obj_create(scr);
    lv_obj_set_size(info_row, 600, 70);
    lv_obj_align(info_row, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_style_bg_color(info_row, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(info_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_row, 12, 0);
    lv_obj_set_style_border_width(info_row, 0, 0);
    lv_obj_set_style_pad_left(info_row, 30, 0);
    lv_obj_set_style_pad_right(info_row, 30, 0);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(info_row, LV_SCROLLBAR_MODE_OFF);

    /* Urgency badge */
    static const uint32_t urg_colors[] = {0x27AE60, 0xF39C12, 0xE74C3C};
    static const char *urg_text[]      = {"Low", "Medium", "High"};
    int idx = (urgency >= 1 && urgency <= 3) ? urgency - 1 : 0;

    lv_obj_t *urg_badge = lv_obj_create(info_row);
    lv_obj_set_size(urg_badge, 160, 40);
    lv_obj_set_style_bg_color(urg_badge, lv_color_hex(urg_colors[idx]), 0);
    lv_obj_set_style_bg_opa(urg_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(urg_badge, 10, 0);
    lv_obj_set_style_border_width(urg_badge, 0, 0);
    lv_obj_set_scrollbar_mode(urg_badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *urg_lbl = lv_label_create(urg_badge);
    char ubuf[32];
    snprintf(ubuf, sizeof(ubuf), "Urgency: %s", urg_text[idx]);
    lv_label_set_text(urg_lbl, ubuf);
    lv_obj_set_style_text_color(urg_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(urg_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(urg_lbl);

    /* Reason badge */
    lv_obj_t *rsn_badge = lv_obj_create(info_row);
    lv_obj_set_size(rsn_badge, 160, 40);
    lv_obj_set_style_bg_color(rsn_badge, lv_color_hex(0x0E8E8B), 0);
    lv_obj_set_style_bg_opa(rsn_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rsn_badge, 10, 0);
    lv_obj_set_style_border_width(rsn_badge, 0, 0);
    lv_obj_set_scrollbar_mode(rsn_badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *rsn_lbl = lv_label_create(rsn_badge);
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "Reason: %s", reason);
    lv_label_set_text(rsn_lbl, rbuf);
    lv_obj_set_style_text_color(rsn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(rsn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(rsn_lbl);

    /* Approve / Deny buttons */
    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, 500, 120);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(btn_row, 40, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(btn_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *approve = make_pill_btn(btn_row, LV_SYMBOL_OK "  Approve",
                                       0x27AE60, 200, 70);
    lv_obj_add_event_cb(approve, approve_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *deny = make_pill_btn(btn_row, LV_SYMBOL_CLOSE "  Deny",
                                    0xE74C3C, 200, 70);
    lv_obj_add_event_cb(deny, deny_click_cb, LV_EVENT_CLICKED, NULL);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, true);
}

/* ════════════════════════════════════════════════
 *  Idle screen  (waiting for visitors)
 * ════════════════════════════════════════════════ */

static void create_idle_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x13F0EC), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "No visitors right now");
    lv_obj_set_style_text_color(status, lv_color_hex(0x7A7A7A), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_22, 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "You'll be notified when someone is at the door");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 45);

    /* WiFi status indicator */
    lv_obj_t *wifi_dot = lv_obj_create(scr);
    lv_obj_set_size(wifi_dot, 14, 14);
    lv_obj_align(wifi_dot, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_style_radius(wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(wifi_dot, 0, 0);
    lv_obj_set_style_bg_color(wifi_dot,
        lv_color_hex(WiFi.status() == WL_CONNECTED ? 0x27AE60 : 0xE74C3C), 0);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
}

/* ════════════════════════════════════════════════
 *  Arduino setup() / loop()
 * ════════════════════════════════════════════════
 *
 *  ⚠  YOU MUST add your display & touch driver init here.
 *     This depends on your specific 5" screen hardware.
 */

static void my_disp_flush(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    lcd.pushImage(area->x1, area->y1, w, h, (uint16_t *)px_map);
    lv_display_flush_ready(disp);
}

static void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    uint16_t tx, ty;
    if (lcd.getTouch(&tx, &ty)) {
        data->point.x = tx;
        data->point.y = ty;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t my_tick_get(void) {
    return (uint32_t)millis();
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== CrowPanel Controller Starting ===");

    /* PCA9557 I/O expander init (required for CrowPanel V3.0 boards)
     * Controls display reset/enable signals via I2C GPIO expander. */
    Wire.begin(19, 20);
    pca9557_write(PCA9557_CONFIG_REG, 0x00);  /* all pins = output */
    pca9557_write(PCA9557_OUTPUT_REG, 0x00);  /* IO0=LOW, IO1=LOW */
    delay(20);
    pca9557_write(PCA9557_OUTPUT_REG, 0x01);  /* IO0=HIGH (enable display) */
    delay(100);
    pca9557_write(PCA9557_CONFIG_REG, 0x02);  /* IO1=input (touch IRQ) */
    Serial.println("[OK] PCA9557 init done");

    /* Display hardware (LovyanGFX — handles backlight + touch) */
    lcd.init();
    lcd.setRotation(0);
    lcd.fillScreen(TFT_BLACK);
    Serial.println("[OK] LCD init done");

    /* WiFi */
    wifi_connect();

    /* UDP listener */
    udp.begin(COMM_PORT);

    /* LVGL init */
    lv_init();
    lv_tick_set_cb(my_tick_get);

    /* Display (800×480 for 5" — adjust if yours differs) */
    static lv_display_t *disp = lv_display_create(800, 480);
    static uint8_t buf[800 * 40 * 2];
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_disp_flush);

    /* Touch */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_read);

    /* Poll for incoming UDP every 100 ms */
    lv_timer_create(check_incoming, 100, NULL);

    /* Start on idle screen */
    create_idle_screen();
}

void loop() {
    lv_timer_handler();
    delay(5);
}
