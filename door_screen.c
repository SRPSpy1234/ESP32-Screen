#include "lvgl.h"
#include <string.h>
#include <stdio.h>

#ifdef LVGL_LIVE_PREVIEW

/* ── State ── */
static const char *selected_person = NULL;
static int         selected_urgency = 0;

/* ── Theme ── */
static const uint32_t THEME_BG      = 0x000000;
static const uint32_t THEME_SURFACE = 0x0A0A0A;
static const uint32_t THEME_ACCENT  = 0x13F0EC;
static const uint32_t THEME_TEXT    = 0xFFFFFF;
static const uint32_t THEME_MUTED   = 0x7A7A7A;

/* ── Forward declarations ── */
static void create_home_screen(void);
static void create_details_screen(const char *person);

/* ── Helpers to style a "pill" button ── */
static lv_obj_t *make_pill_btn(lv_obj_t *parent, const char *txt,
                                uint32_t bg_color, int w, int h) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 15, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 20, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
    /* pressed */
    lv_obj_set_style_bg_color(btn, lv_color_lighten(lv_color_hex(bg_color), 40),
                              LV_STATE_PRESSED);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return btn;
}

/* ═══════════════════════════════════════════════
 *  Details screen  (urgency 1-3 + reason)
 * ═══════════════════════════════════════════════ */

/* Urgency button colours: green / orange / red */
static const uint32_t urgency_colors[] = {0x055E5C, 0x0A8F8D, 0x13C7C3};

static void urgency_click_cb(lv_event_t *e) {
    int level = (int)(intptr_t)lv_event_get_user_data(e);
    selected_urgency = level;

    /* Highlight selected, dim the others */
    lv_obj_t *cont = lv_obj_get_parent(lv_event_get_target(e));
    uint32_t cnt = lv_obj_get_child_count(cont);
    for(uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(cont, i);
        if((int)(intptr_t)lv_obj_get_user_data(child) == level) {
            lv_obj_set_style_opa(child, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(child, 3, 0);
            lv_obj_set_style_border_color(child, lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_opa(child, LV_OPA_60, 0);
            lv_obj_set_style_border_width(child, 0, 0);
        }
    }
}

/* Timer callback to return home */
static void return_home_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    create_home_screen();
}

static void back_click_cb(lv_event_t *e) {
    (void)e;
    create_home_screen();
}

/* ── Response result screens ── */
static void create_response_screen(bool approved) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *icon = lv_label_create(scr);
    lv_label_set_text(icon, approved ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(icon,
        lv_color_hex(approved ? 0x27AE60 : 0xE74C3C), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, approved ? "Come in!" : "Not right now");
    lv_obj_set_style_text_color(msg, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, approved
        ? "The door has been unlocked"
        : "Please try again later");
    lv_obj_set_style_text_color(sub, lv_color_hex(THEME_MUTED), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 50);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
    lv_timer_create(return_home_timer_cb, 4000, NULL);
}

/* Preview-only: simulate an approved response */
static void simulate_approved_timer_cb(lv_timer_t *t) {
    lv_timer_del(t);
    create_response_screen(true);
}

static void reason_click_real_cb(lv_event_t *e) {
    const char *reason = (const char *)lv_event_get_user_data(e);
    if(selected_urgency == 0) selected_urgency = 1; /* default */

    /* ── Waiting for response screen ── */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* Spinner */
    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *msg = lv_label_create(scr);
    lv_label_set_text(msg, "Waiting for response...");
    lv_obj_set_style_text_color(msg, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_24, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *detail = lv_label_create(scr);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s  |  Urgency: %d  |  %s",
             selected_person, selected_urgency, reason);
    lv_label_set_text(detail, buf);
    lv_obj_set_style_text_color(detail, lv_color_hex(THEME_MUTED), 0);
    lv_obj_set_style_text_font(detail, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(detail, LV_ALIGN_CENTER, 0, 60);

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);

    /* In real device: send MSG_REQUEST via ESP-NOW here.
       For preview: simulate an "approved" response after 2s */
    lv_timer_create(simulate_approved_timer_cb, 2000, NULL);
}

static void create_details_screen(const char *person) {
    selected_person  = person;
    selected_urgency = 0;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* ── Back button ── */
    lv_obj_t *back = lv_btn_create(scr);
    lv_obj_set_size(back, 80, 36);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 15, 10);
    lv_obj_set_style_bg_color(back, lv_color_hex(THEME_SURFACE), 0);
    lv_obj_set_style_radius(back, 10, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(THEME_ACCENT), 0);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(THEME_TEXT), 0);
    lv_obj_center(bl);

    /* ── Header ── */
    lv_obj_t *header = lv_label_create(scr);
    char hbuf[64];
    snprintf(hbuf, sizeof(hbuf), "Hi %s!", person);
    lv_label_set_text(header, hbuf);
    lv_obj_set_style_text_color(header, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_24, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 12);

    /* ── Urgency label ── */
    lv_obj_t *urg_lbl = lv_label_create(scr);
    lv_label_set_text(urg_lbl, "How much do you need me?");
    lv_obj_set_style_text_color(urg_lbl, lv_color_hex(THEME_MUTED), 0);
    lv_obj_set_style_text_font(urg_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(urg_lbl, LV_ALIGN_TOP_MID, 0, 55);

    /* Urgency button row */
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

    static const char *urg_labels[] = {"1 - Low", "2 - Med", "3 - High"};
    for(int i = 0; i < 3; i++) {
        lv_obj_t *btn = make_pill_btn(urg_row, urg_labels[i],
                                       urgency_colors[i], 140, 46);
        lv_obj_set_user_data(btn, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(btn, urgency_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i + 1));
        lv_obj_set_style_opa(btn, LV_OPA_60, 0);
    }

    /* ── Reason label ── */
    lv_obj_t *rsn_lbl = lv_label_create(scr);
    lv_label_set_text(rsn_lbl, "What's the reason?");
    lv_obj_set_style_text_color(rsn_lbl, lv_color_hex(THEME_MUTED), 0);
    lv_obj_set_style_text_font(rsn_lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(rsn_lbl, LV_ALIGN_TOP_MID, 0, 155);

    /* Reason buttons — single row of 4 */
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

    static const char  *reasons[]      = {"Dinner", "Help", "Entry", "Other"};
    static const char  *reason_icons[] = {LV_SYMBOL_HOME, LV_SYMBOL_CALL,
                                          LV_SYMBOL_RIGHT, LV_SYMBOL_LIST};
    static const uint32_t reason_colors[] = {0x0A8F8D, 0x0C7A78,
                                              0x0E6664, 0x105250};
    for(int i = 0; i < 4; i++) {
        char lbl_buf[32];
        snprintf(lbl_buf, sizeof(lbl_buf), "%s  %s", reason_icons[i], reasons[i]);
        lv_obj_t *btn = make_pill_btn(rsn_row, lbl_buf,
                                       reason_colors[i], 170, 55);
        lv_obj_add_event_cb(btn, reason_click_real_cb, LV_EVENT_CLICKED,
                            (void *)reasons[i]);
    }

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, true);
}

/* ═══════════════════════════════════════════════
 *  Home screen  (Mom / Dad / Mason)
 * ═══════════════════════════════════════════════ */

static void person_click_cb(lv_event_t *e) {
    const char *person = (const char *)lv_event_get_user_data(e);
    create_details_screen(person);
}

static void create_home_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_BG), 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* ── Header: "Who's visiting?" ── */
    lv_obj_t *header = lv_label_create(scr);
    lv_label_set_text(header, "Who's visiting?");
    lv_obj_set_style_text_color(header, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_28, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 40);

    /* ── Subtitle ── */
    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Please select your name");
    lv_obj_set_style_text_color(sub, lv_color_hex(THEME_MUTED), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 80);

    /* ── Button container (flex row, centered) ── */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 700, 280);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(cont, 40, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* ── Card style values ── */
    static const uint32_t colors[] = {0x0A8F8D, 0x0C7A78, 0x0E6664};
    static const char *names[]     = {"Mom", "Dad", "Mason"};
    static const char *icons[]     = {LV_SYMBOL_HOME, LV_SYMBOL_SETTINGS,
                                      LV_SYMBOL_AUDIO};

    for(int i = 0; i < 3; i++) {
        /* Card */
        lv_obj_t *card = lv_obj_create(cont);
        lv_obj_set_size(card, 180, 220);
        lv_obj_set_style_bg_color(card, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 30, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(colors[i]), 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        /* Pressed effect */
        lv_obj_set_style_bg_color(card,
            lv_color_lighten(lv_color_hex(colors[i]), 40), LV_STATE_PRESSED);
        lv_obj_set_style_transform_width(card, -5, LV_STATE_PRESSED);
        lv_obj_set_style_transform_height(card, -5, LV_STATE_PRESSED);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        /* Navigate on click */
        lv_obj_add_event_cb(card, person_click_cb, LV_EVENT_CLICKED,
                            (void *)names[i]);

        /* Icon */
        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);

        /* Name label */
        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, names[i]);
        lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    }

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, true);
}

/* ── Entry point ── */
void lvgl_live_preview_init(void) {
    create_home_screen();
}
#endif
