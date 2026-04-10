#include "lvgl.h"
#include <string.h>
#include <stdio.h>

#ifdef LVGL_LIVE_PREVIEW

/* ── State ── */
static bool has_request = false;
static char req_person[16]  = "";
static int  req_urgency     = 0;
static char req_reason[16]  = "";

/* ── Theme ── */
static const uint32_t THEME_BG      = 0x000000;
static const uint32_t THEME_SURFACE = 0x0A0A0A;
static const uint32_t THEME_ACCENT  = 0x13F0EC;
static const uint32_t THEME_TEXT    = 0xFFFFFF;
static const uint32_t THEME_MUTED   = 0x7A7A7A;

/* ── Forward declarations ── */
static void create_idle_screen(void);
static void create_request_screen(const char *person, int urgency, const char *reason);
static void create_result_screen(bool approved);

/* ── Pill button helper ── */
static lv_obj_t *make_pill_btn(lv_obj_t *parent, const char *txt,
                                uint32_t bg_color, int w, int h) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 15, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 25, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(btn, lv_color_lighten(lv_color_hex(bg_color), 40),
                              LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ═══════════════════════════════════════════════
 *  Result screen  (after approve / deny)
 * ═══════════════════════════════════════════════ */

static void return_idle_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    create_idle_screen();
}

static void create_result_screen(bool approved) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
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
    lv_obj_set_style_text_color(msg, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 20);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
    lv_timer_create(return_idle_timer_cb, 3000, NULL);
}

/* ═══════════════════════════════════════════════
 *  Request screen  (approve / deny incoming visit)
 * ═══════════════════════════════════════════════ */

static void approve_click_cb(lv_event_t *e) {
    (void)e;
    /* In real device: send MSG_APPROVE via ESP-NOW */
    create_result_screen(true);
}

static void deny_click_cb(lv_event_t *e) {
    (void)e;
    /* In real device: send MSG_DENY via ESP-NOW */
    create_result_screen(false);
}

static void create_request_screen(const char *person, int urgency,
                                   const char *reason) {
    strncpy(req_person, person, sizeof(req_person) - 1);
    req_urgency = urgency;
    strncpy(req_reason, reason, sizeof(req_reason) - 1);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* ── Bell icon ── */
    lv_obj_t *bell = lv_label_create(scr);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(bell, lv_color_hex(THEME_ACCENT), 0);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_36, 0);
    lv_obj_align(bell, LV_ALIGN_TOP_MID, 0, 20);

    /* ── Title ── */
    lv_obj_t *title = lv_label_create(scr);
    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "%s is at the door!", person);
    lv_label_set_text(title, tbuf);
    lv_obj_set_style_text_color(title, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 65);

    /* ── Info row ── */
    lv_obj_t *info_row = lv_obj_create(scr);
    lv_obj_set_size(info_row, 600, 70);
    lv_obj_align(info_row, LV_ALIGN_TOP_MID, 0, 115);
    lv_obj_set_style_bg_color(info_row, lv_color_hex(THEME_SURFACE), 0);
    lv_obj_set_style_bg_opa(info_row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_row, 12, 0);
    lv_obj_set_style_border_width(info_row, 1, 0);
    lv_obj_set_style_border_color(info_row, lv_color_hex(THEME_ACCENT), 0);
    lv_obj_set_style_pad_left(info_row, 30, 0);
    lv_obj_set_style_pad_right(info_row, 30, 0);
    lv_obj_set_flex_flow(info_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(info_row, LV_SCROLLBAR_MODE_OFF);

    /* Urgency badge */
    static const uint32_t urg_colors[] = {0x055E5C, 0x0A8F8D, 0x13C7C3};
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
    lv_obj_set_style_text_color(urg_lbl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(urg_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(urg_lbl);

    /* Reason badge */
    lv_obj_t *rsn_badge = lv_obj_create(info_row);
    lv_obj_set_size(rsn_badge, 160, 40);
    lv_obj_set_style_bg_color(rsn_badge, lv_color_hex(THEME_ACCENT), 0);
    lv_obj_set_style_bg_opa(rsn_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(rsn_badge, 10, 0);
    lv_obj_set_style_border_width(rsn_badge, 0, 0);
    lv_obj_set_scrollbar_mode(rsn_badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *rsn_lbl = lv_label_create(rsn_badge);
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "Reason: %s", reason);
    lv_label_set_text(rsn_lbl, rbuf);
    lv_obj_set_style_text_color(rsn_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(rsn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(rsn_lbl);

    /* ── Approve / Deny buttons ── */
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

/* ═══════════════════════════════════════════════
 *  Idle screen  (waiting for requests)
 * ═══════════════════════════════════════════════ */

/* In preview: tap the "Simulate" button to fake a request */
static void simulate_click_cb(lv_event_t *e) {
    (void)e;
    create_request_screen("Mom", 2, "Entry");
}

static void create_idle_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* Door icon */
    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(icon, lv_color_hex(THEME_ACCENT), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -60);

    /* Status text */
    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "No visitors right now");
    lv_obj_set_style_text_color(status, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_22, 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, 10);

    /* Subtitle */
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "You'll be notified when someone is at the door");
    lv_obj_set_style_text_color(sub, lv_color_hex(THEME_MUTED), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 45);

    /* Preview-only: simulate button */
    lv_obj_t *sim = make_pill_btn(scr, LV_SYMBOL_BELL "  Simulate Request",
                                   THEME_ACCENT, 250, 50);
    lv_obj_align(sim, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(sim, simulate_click_cb, LV_EVENT_CLICKED, NULL);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
}

/* ── Entry point ── */
void lvgl_live_preview_init(void) {
    create_idle_screen();
}
#endif
