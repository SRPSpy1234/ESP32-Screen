/*  door_device.ino
 *  ────────────────
 *  ESP32 + CrowPanel ADVANCE 7" IPS (800×480) V1.3 on the bedroom door.
 *  Connects to WiFi, sends UDP requests to the controller,
 *  listens for approve/deny responses.
 *
 *  Board:  ESP32S3 Dev Module
 *  Arduino IDE Settings:
 *    Flash Size: 16MB (128Mb)        ← Advance has 16MB flash!
 *    PSRAM: OPI PSRAM
 *    Partition Scheme: Default 4MB with spiffs (or 16MB)
 *    USB CDC On Boot: Disabled
 *  Library deps:  lvgl 9.x, LovyanGFX 1.1.16, WiFi
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
 *  CrowPanel ADVANCE 7.0" IPS display driver (LovyanGFX)
 *  Pin mapping & timing from official Elecrow V1.3 source
 *  Driver IC: SC7277 (pure RGB, no SPI init needed)
 *  Backlight: STC8H1K28 MCU at I2C 0x30
 * ═══════════════════════════════════════════════ */

class LGFX : public lgfx::LGFX_Device {
public:
    lgfx::Bus_RGB      _bus_instance;
    lgfx::Panel_RGB    _panel_instance;
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
            auto cfg = _panel_instance.config_detail();
            cfg.use_psram = 1;   /* official Elecrow value */
            _panel_instance.config_detail(cfg);
        }
        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;

            cfg.pin_d0  = GPIO_NUM_21;   // B0
            cfg.pin_d1  = GPIO_NUM_47;   // B1
            cfg.pin_d2  = GPIO_NUM_48;   // B2
            cfg.pin_d3  = GPIO_NUM_45;   // B3
            cfg.pin_d4  = GPIO_NUM_38;   // B4

            cfg.pin_d5  = GPIO_NUM_9;    // G0
            cfg.pin_d6  = GPIO_NUM_10;   // G1
            cfg.pin_d7  = GPIO_NUM_11;   // G2
            cfg.pin_d8  = GPIO_NUM_12;   // G3
            cfg.pin_d9  = GPIO_NUM_13;   // G4
            cfg.pin_d10 = GPIO_NUM_14;   // G5

            cfg.pin_d11 = GPIO_NUM_7;    // R0
            cfg.pin_d12 = GPIO_NUM_17;   // R1
            cfg.pin_d13 = GPIO_NUM_18;   // R2
            cfg.pin_d14 = GPIO_NUM_3;    // R3
            cfg.pin_d15 = GPIO_NUM_46;   // R4

            cfg.pin_henable = GPIO_NUM_42;
            cfg.pin_vsync   = GPIO_NUM_41;
            cfg.pin_hsync   = GPIO_NUM_40;
            cfg.pin_pclk    = GPIO_NUM_39;
            cfg.freq_write  = 14000000;   /* 14 MHz: stable on esp32 2.0.14 */

            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;    /* official: 8 */
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 8;    /* official: 8 */

            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 8;    /* official: 8 */
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 8;    /* official: 8 */

            cfg.pclk_idle_high    = 1;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }
        /* No Light_PWM — backlight is controlled via STC8H1K28 I2C MCU */
        {
            auto cfg = _touch_instance.config();
            cfg.x_min      = 0;
            cfg.x_max      = 800;
            cfg.y_min      = 0;
            cfg.y_max      = 480;
            cfg.pin_int    = -1;
            cfg.pin_rst    = -1;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port   = I2C_NUM_0;
            cfg.pin_sda    = GPIO_NUM_15;
            cfg.pin_scl    = GPIO_NUM_16;
            cfg.freq       = 400000;
            cfg.i2c_addr   = 0x5D;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }
        setPanel(&_panel_instance);
    }
};

static LGFX lcd;

/* ─── STC8H1K28 backlight/touch controller at I2C 0x30 ─── */
#define STC_ADDR  0x30

static void stc_send_cmd(uint8_t cmd) {
    Wire.beginTransmission(STC_ADDR);
    Wire.write(cmd);
    Wire.endTransmission();
}

static bool i2c_device_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return (Wire.endTransmission() == 0);
}

/* ─── Networking ─── */
static WiFiUDP udp;
static IPAddress controller_ip(255, 255, 255, 255); /* broadcast until discovered */
static bool     controller_found = false;

/* ─── LVGL state ─── */
static const char *selected_person  = NULL;
static int         selected_urgency = 0;
static lv_timer_t *timeout_timer    = NULL;

/* ─── Forward declarations (UI) ─── */
static void create_home_screen(void);
static void create_details_screen(const char *person);
static void create_waiting_screen(const char *person, int urgency, const char *reason);
static void create_response_screen(bool approved);

/* ─── Forward declarations (network) ─── */
static void wifi_connect(void);
static void send_request(const char *person, int urgency, const char *reason);
static void check_incoming(lv_timer_t *t);

/* ════════════════════════════════════════════════
 *  Networking
 * ════════════════════════════════════════════════ */

static void wifi_connect(void) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    /* Non-blocking: UI shows while connecting.
       In production add a status indicator. */
}

static void send_request(const char *person, int urgency, const char *reason) {
    door_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.msg_type = MSG_REQUEST;
    strncpy(pkt.person, person, sizeof(pkt.person) - 1);
    pkt.urgency = (uint8_t)urgency;
    strncpy(pkt.reason, reason, sizeof(pkt.reason) - 1);

    /* Broadcast on the local subnet so controller picks it up
       regardless of its IP.  Once we get a reply we learn its IP. */
    udp.beginPacket(controller_ip, COMM_PORT);
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

    /* Remember controller's IP for future unicast */
    if (!controller_found) {
        controller_ip   = udp.remoteIP();
        controller_found = true;
    }

    if (pkt.msg_type == MSG_APPROVE) {
        if (timeout_timer) { lv_timer_del(timeout_timer); timeout_timer = NULL; }
        create_response_screen(true);
    } else if (pkt.msg_type == MSG_DENY) {
        if (timeout_timer) { lv_timer_del(timeout_timer); timeout_timer = NULL; }
        create_response_screen(false);
    }
    /* MSG_PONG could be handled here for connectivity UI */
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
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_lighten(lv_color_hex(bg), 40),
                              LV_STATE_PRESSED);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ════════════════════════════════════════════════
 *  Response screen  (approved / denied)
 * ════════════════════════════════════════════════ */

static void return_home_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    create_home_screen();
}

static void create_response_screen(bool approved) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, approved ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(icon,
        lv_color_hex(approved ? 0x27AE60 : 0xE74C3C), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, approved ? "Come in!" : "Not right now");
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, approved ? "The door has been unlocked"
                                     : "Please try again later");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7A7A7A), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 50);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    lv_timer_create(return_home_timer_cb, 4000, NULL);
}

/* ════════════════════════════════════════════════
 *  Waiting screen  (spinner while waiting for reply)
 * ════════════════════════════════════════════════ */

static void timeout_cb(lv_timer_t *t) {
    lv_timer_del(t);
    timeout_timer = NULL;
    /* No response after 30 s — show denied */
    create_response_screen(false);
}

static void create_waiting_screen(const char *person, int urgency,
                                   const char *reason) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Waiting for response...");
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_24, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *detail = lv_label_create(scr);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s  |  Urgency: %d  |  %s",
             person, urgency, reason);
    lv_label_set_text(detail, buf);
    lv_obj_set_style_text_color(detail, lv_color_hex(0x7A7A7A), 0);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 60);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);

    /* Send the UDP request */
    send_request(person, urgency, reason);

    /* 30-second timeout */
    timeout_timer = lv_timer_create(timeout_cb, 30000, NULL);
}

/* ════════════════════════════════════════════════
 *  Urgency / Reason callbacks
 * ════════════════════════════════════════════════ */

static const uint32_t urgency_colors[] = {0x27AE60, 0xF39C12, 0xE74C3C};

static void urgency_click_cb(lv_event_t *e) {
    int level = (int)(intptr_t)lv_event_get_user_data(e);
    selected_urgency = level;
    lv_obj_t *cont = lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e));
    uint32_t cnt = lv_obj_get_child_count(cont);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(cont, i);
        if ((int)(intptr_t)lv_obj_get_user_data(child) == level) {
            lv_obj_set_style_opa(child, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(child, 3, 0);
            lv_obj_set_style_border_color(child, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_opa(child, LV_OPA_60, 0);
            lv_obj_set_style_border_width(child, 0, 0);
        }
    }
}

static void reason_click_cb(lv_event_t *e) {
    const char *reason = (const char *)lv_event_get_user_data(e);
    if (selected_urgency == 0) selected_urgency = 1;
    create_waiting_screen(selected_person, selected_urgency, reason);
}

static void back_click_cb(lv_event_t *e) {
    (void)e;
    create_home_screen();
}

/* ════════════════════════════════════════════════
 *  Details screen  (urgency + reason)
 * ════════════════════════════════════════════════ */

static void create_details_screen(const char *person) {
    selected_person  = person;
    selected_urgency = 0;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Back */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 80, 36);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 15, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(bl);

    /* Header */
    lv_obj_t *header = lv_label_create(scr);
    char hbuf[64];
    snprintf(hbuf, sizeof(hbuf), "Hi %s!", person);
    lv_label_set_text(header, hbuf);
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 12);

    /* Urgency label */
    lv_obj_t *urg_lbl = lv_label_create(scr);
    lv_label_set_text(urg_lbl, "How much do you need me?");
    lv_obj_set_style_text_color(urg_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(urg_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(urg_lbl, LV_ALIGN_TOP_MID, 0, 55);

    /* Urgency row */
    lv_obj_t *urg_row = lv_obj_create(scr);
    lv_obj_set_size(urg_row, 480, 60);
    lv_obj_align(urg_row, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_bg_opa(urg_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(urg_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(urg_row, 20, 0);
    lv_obj_set_flex_flow(urg_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(urg_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(urg_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(urg_row, LV_OBJ_FLAG_SCROLLABLE);

    static const char *urg_labels[] = {"1 - Low", "2 - Med", "3 - High"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = make_pill_btn(urg_row, urg_labels[i],
                                       urgency_colors[i], 140, 46);
        lv_obj_set_user_data(btn, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(btn, urgency_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i + 1));
        lv_obj_set_style_opa(btn, LV_OPA_60, 0);
    }

    /* Reason label */
    lv_obj_t *rsn_lbl = lv_label_create(scr);
    lv_label_set_text(rsn_lbl, "What's the reason?");
    lv_obj_set_style_text_color(rsn_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(rsn_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(rsn_lbl, LV_ALIGN_TOP_MID, 0, 155);

    /* Reason row */
    lv_obj_t *rsn_row = lv_obj_create(scr);
    lv_obj_set_size(rsn_row, 760, 200);
    lv_obj_align(rsn_row, LV_ALIGN_TOP_MID, 0, 185);
    lv_obj_set_style_bg_opa(rsn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(rsn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(rsn_row, 15, 0);
    lv_obj_set_style_pad_row(rsn_row, 12, 0);
    lv_obj_set_flex_flow(rsn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(rsn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(rsn_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(rsn_row, LV_OBJ_FLAG_SCROLLABLE);

    static const char *reasons[]       = {"Dinner", "Help", "Entry", "Other"};
    static const char *reason_icons[]  = {LV_SYMBOL_HOME, LV_SYMBOL_CALL,
                                          LV_SYMBOL_RIGHT, LV_SYMBOL_LIST};
    static const uint32_t reason_clrs[] = {0x2980B9, 0x8E44AD,
                                            0x16A085, 0x7F8C8D};
    for (int i = 0; i < 4; i++) {
        char lbl_buf[32];
        snprintf(lbl_buf, sizeof(lbl_buf), "%s  %s", reason_icons[i], reasons[i]);
        lv_obj_t *btn = make_pill_btn(rsn_row, lbl_buf,
                                       reason_clrs[i], 170, 55);
        lv_obj_add_event_cb(btn, reason_click_cb, LV_EVENT_CLICKED,
                            (void *)reasons[i]);
    }

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

/* ════════════════════════════════════════════════
 *  Home screen  (Mom / Dad / Mason)
 * ════════════════════════════════════════════════ */

static void person_click_cb(lv_event_t *e) {
    const char *person = (const char *)lv_event_get_user_data(e);
    create_details_screen(person);
}

static void create_home_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_label_create(scr);
    lv_label_set_text(header, "Who's visiting?");
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_28, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Please select your name");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x7A7A7A), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 80);

    /* WiFi status dot (top-right corner) */
    lv_obj_t *wifi_dot = lv_obj_create(scr);
    lv_obj_set_size(wifi_dot, 14, 14);
    lv_obj_align(wifi_dot, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_style_radius(wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(wifi_dot, 0, 0);
    lv_obj_set_style_pad_all(wifi_dot, 0, 0);
    lv_obj_set_scrollbar_mode(wifi_dot, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(wifi_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(wifi_dot,
        lv_color_hex(WiFi.status() == WL_CONNECTED ? 0x27AE60 : 0xE74C3C), 0);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 700, 280);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_column(cont, 40, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    static const uint32_t colors[] = {0x055E5C, 0x0A8F8D, 0x13C7C3};
    static const char *names[]     = {"Mom", "Dad", "Mason"};
    static const char *icons[]     = {LV_SYMBOL_HOME, LV_SYMBOL_SETTINGS,
                                      LV_SYMBOL_AUDIO};

    for (int i = 0; i < 3; i++) {
        lv_obj_t *card = lv_obj_create(cont);
        lv_obj_set_size(card, 180, 220);
        lv_obj_set_style_bg_color(card, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, person_click_cb, LV_EVENT_CLICKED,
                            (void *)names[i]);

        lv_obj_t *ic = lv_label_create(card);
        lv_label_set_text(ic, icons[i]);
        lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);

        lv_obj_t *nm = lv_label_create(card);
        lv_label_set_text(nm, names[i]);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
    }

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

/* ════════════════════════════════════════════════
 *  Arduino setup() / loop()
 * ════════════════════════════════════════════════ */

static void my_disp_flush(lv_display_t *disp, const lv_area_t *area,
                           uint8_t *px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    lcd.pushImage(area->x1, area->y1, w, h, (lgfx::rgb565_t *)px_map);
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
    Serial.println("\n\n=== CrowPanel ADVANCE 7\" Door Device Starting (V1.3) ===");

    /* GPIO19 is SPI CS on the Advance board — set as output */
    pinMode(19, OUTPUT);

    /* I2C bus for STC8H1K28 backlight controller + GT911 touch
       SDA = GPIO15, SCL = GPIO16 on the Advance board */
    Wire.begin(15, 16);
    delay(50);

    /* Wait for STC8H1K28 (0x30) and GT911 touch (0x5D) to come up */
    Serial.println("[INIT] Waiting for STC8H1K28 & GT911...");
    for (int retries = 0; retries < 20; retries++) {
        if (i2c_device_present(STC_ADDR) && i2c_device_present(0x5D)) {
            Serial.println("[OK] STC8H1K28 (0x30) and GT911 (0x5D) detected");
            break;
        }
        /* Touch controller not responding — reset it via GPIO1 */
        stc_send_cmd(250);      /* activate touch screen */
        pinMode(1, OUTPUT);
        digitalWrite(1, LOW);
        delay(120);
        pinMode(1, INPUT);
        delay(100);
        Serial.printf("[INIT] Retry %d...\n", retries + 1);
    }

    /* Turn on backlight: 0 = max brightness, 245 = off */
    stc_send_cmd(0);
    Serial.println("[OK] Backlight ON (max brightness)");

    /* Display hardware */
    lcd.init();
    lcd.fillScreen(TFT_BLACK);
    Serial.println("[OK] LCD init done");

    /* WiFi */
    wifi_connect();

    /* UDP listener */
    udp.begin(COMM_PORT);

    /* LVGL init */
    lv_init();
    lv_tick_set_cb(my_tick_get);

    /* Display (800×480, 16-bit color)
       Internal SRAM draw buffers avoid PSRAM bus contention with
       the LCD DMA that continuously reads the panel framebuffer.
       Copy path: SRAM draw buf → PSRAM panel FB (pushImage). */
    static lv_display_t *disp = lv_display_create(800, 480);
    static const size_t buf_lines = 48;   /* 800×48×2 = 76 800 bytes */
    static const size_t buf_size  = 800 * buf_lines * sizeof(lv_color16_t);
    static void *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL);
    static void *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL);
    if (!buf1 || !buf2) {
        Serial.printf("[FATAL] SRAM alloc failed: buf1=%p buf2=%p\n", buf1, buf2);
        while (1) delay(1000);
    }
    Serial.printf("[OK] LVGL buffers: buf1=%p buf2=%p (%u bytes each, internal SRAM)\n",
                  buf1, buf2, (unsigned)buf_size);
    lv_display_set_buffers(disp, buf1, buf2, buf_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, my_disp_flush);

    /* Touch input */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_read);

    /* Poll for incoming UDP packets every 100 ms */
    lv_timer_create(check_incoming, 100, NULL);

    /* Show home screen */
    create_home_screen();
    Serial.println("[OK] Door device ready");
}

void loop() {
    lv_timer_handler();
    delay(1);
}
