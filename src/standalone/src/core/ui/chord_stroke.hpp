#pragma once

#include <cstdint>

namespace aida::ui {

using chord_stroke_t = std::uint32_t;

namespace chord {

constexpr chord_stroke_t mod_ctrl = 0x1000;
constexpr chord_stroke_t mod_shift = 0x2000;
constexpr chord_stroke_t mod_alt = 0x4000;
constexpr chord_stroke_t mod_super = 0x8000;
constexpr chord_stroke_t mod_mask = 0xF000;

constexpr chord_stroke_t k_tab = 512;
constexpr chord_stroke_t k_left_arrow = 513;
constexpr chord_stroke_t k_right_arrow = 514;
constexpr chord_stroke_t k_up_arrow = 515;
constexpr chord_stroke_t k_down_arrow = 516;
constexpr chord_stroke_t k_page_up = 517;
constexpr chord_stroke_t k_page_down = 518;
constexpr chord_stroke_t k_home = 519;
constexpr chord_stroke_t k_end = 520;
constexpr chord_stroke_t k_insert = 521;
constexpr chord_stroke_t k_delete = 522;
constexpr chord_stroke_t k_backspace = 523;
constexpr chord_stroke_t k_space = 524;
constexpr chord_stroke_t k_enter = 525;
constexpr chord_stroke_t k_escape = 526;
constexpr chord_stroke_t k_left_ctrl = 527;
constexpr chord_stroke_t k_left_shift = 528;
constexpr chord_stroke_t k_left_alt = 529;
constexpr chord_stroke_t k_left_super = 530;
constexpr chord_stroke_t k_right_ctrl = 531;
constexpr chord_stroke_t k_right_shift = 532;
constexpr chord_stroke_t k_right_alt = 533;
constexpr chord_stroke_t k_right_super = 534;
constexpr chord_stroke_t k_menu = 535;
constexpr chord_stroke_t k_0 = 536;
constexpr chord_stroke_t k_1 = 537;
constexpr chord_stroke_t k_2 = 538;
constexpr chord_stroke_t k_3 = 539;
constexpr chord_stroke_t k_4 = 540;
constexpr chord_stroke_t k_5 = 541;
constexpr chord_stroke_t k_6 = 542;
constexpr chord_stroke_t k_7 = 543;
constexpr chord_stroke_t k_8 = 544;
constexpr chord_stroke_t k_9 = 545;
constexpr chord_stroke_t k_a = 546;
constexpr chord_stroke_t k_b = 547;
constexpr chord_stroke_t k_c = 548;
constexpr chord_stroke_t k_d = 549;
constexpr chord_stroke_t k_e = 550;
constexpr chord_stroke_t k_f = 551;
constexpr chord_stroke_t k_g = 552;
constexpr chord_stroke_t k_h = 553;
constexpr chord_stroke_t k_i = 554;
constexpr chord_stroke_t k_j = 555;
constexpr chord_stroke_t k_k = 556;
constexpr chord_stroke_t k_l = 557;
constexpr chord_stroke_t k_m = 558;
constexpr chord_stroke_t k_n = 559;
constexpr chord_stroke_t k_o = 560;
constexpr chord_stroke_t k_p = 561;
constexpr chord_stroke_t k_q = 562;
constexpr chord_stroke_t k_r = 563;
constexpr chord_stroke_t k_s = 564;
constexpr chord_stroke_t k_t = 565;
constexpr chord_stroke_t k_u = 566;
constexpr chord_stroke_t k_v = 567;
constexpr chord_stroke_t k_w = 568;
constexpr chord_stroke_t k_x = 569;
constexpr chord_stroke_t k_y = 570;
constexpr chord_stroke_t k_z = 571;
constexpr chord_stroke_t k_f1 = 572;
constexpr chord_stroke_t k_f2 = 573;
constexpr chord_stroke_t k_f3 = 574;
constexpr chord_stroke_t k_f4 = 575;
constexpr chord_stroke_t k_f5 = 576;
constexpr chord_stroke_t k_f6 = 577;
constexpr chord_stroke_t k_f7 = 578;
constexpr chord_stroke_t k_f8 = 579;
constexpr chord_stroke_t k_f9 = 580;
constexpr chord_stroke_t k_f10 = 581;
constexpr chord_stroke_t k_f11 = 582;
constexpr chord_stroke_t k_f12 = 583;
constexpr chord_stroke_t k_f13 = 584;
constexpr chord_stroke_t k_f14 = 585;
constexpr chord_stroke_t k_f15 = 586;
constexpr chord_stroke_t k_f16 = 587;
constexpr chord_stroke_t k_f17 = 588;
constexpr chord_stroke_t k_f18 = 589;
constexpr chord_stroke_t k_f19 = 590;
constexpr chord_stroke_t k_f20 = 591;
constexpr chord_stroke_t k_f21 = 592;
constexpr chord_stroke_t k_f22 = 593;
constexpr chord_stroke_t k_f23 = 594;
constexpr chord_stroke_t k_f24 = 595;
constexpr chord_stroke_t k_apostrophe = 596;
constexpr chord_stroke_t k_comma = 597;
constexpr chord_stroke_t k_minus = 598;
constexpr chord_stroke_t k_period = 599;
constexpr chord_stroke_t k_slash = 600;
constexpr chord_stroke_t k_semicolon = 601;
constexpr chord_stroke_t k_equal = 602;
constexpr chord_stroke_t k_left_bracket = 603;
constexpr chord_stroke_t k_backslash = 604;
constexpr chord_stroke_t k_right_bracket = 605;
constexpr chord_stroke_t k_grave_accent = 606;
constexpr chord_stroke_t k_caps_lock = 607;
constexpr chord_stroke_t k_scroll_lock = 608;
constexpr chord_stroke_t k_num_lock = 609;
constexpr chord_stroke_t k_print_screen = 610;
constexpr chord_stroke_t k_pause = 611;
constexpr chord_stroke_t k_keypad_0 = 612;
constexpr chord_stroke_t k_keypad_1 = 613;
constexpr chord_stroke_t k_keypad_2 = 614;
constexpr chord_stroke_t k_keypad_3 = 615;
constexpr chord_stroke_t k_keypad_4 = 616;
constexpr chord_stroke_t k_keypad_5 = 617;
constexpr chord_stroke_t k_keypad_6 = 618;
constexpr chord_stroke_t k_keypad_7 = 619;
constexpr chord_stroke_t k_keypad_8 = 620;
constexpr chord_stroke_t k_keypad_9 = 621;
constexpr chord_stroke_t k_keypad_decimal = 622;
constexpr chord_stroke_t k_keypad_divide = 623;
constexpr chord_stroke_t k_keypad_multiply = 624;
constexpr chord_stroke_t k_keypad_subtract = 625;
constexpr chord_stroke_t k_keypad_add = 626;
constexpr chord_stroke_t k_keypad_enter = 627;
constexpr chord_stroke_t k_keypad_equal = 628;
constexpr chord_stroke_t k_app_back = 629;
constexpr chord_stroke_t k_app_forward = 630;
constexpr chord_stroke_t k_oem102 = 631;
constexpr chord_stroke_t k_gamepad_start = 632;

static_assert(k_tab == 512 && k_gamepad_start == 632);
static_assert(mod_ctrl == (1u << 12) && mod_shift == (1u << 13) &&
              mod_alt == (1u << 14) && mod_super == (1u << 15) &&
              mod_mask == 0xF000);

}

constexpr bool valid_chord_stroke(chord_stroke_t stroke) noexcept {
    const chord_stroke_t key = stroke & ~chord::mod_mask;
    return key >= chord::k_tab && key < chord::k_gamepad_start &&
           key != chord::k_left_ctrl && key != chord::k_right_ctrl &&
           key != chord::k_left_shift && key != chord::k_right_shift &&
           key != chord::k_left_alt && key != chord::k_right_alt &&
           key != chord::k_left_super && key != chord::k_right_super;
}

inline const char* chord_key_name(chord_stroke_t key) noexcept {
    static constexpr const char* k_names[] = {
        "Tab", "LeftArrow", "RightArrow", "UpArrow", "DownArrow", "PageUp",
        "PageDown", "Home", "End", "Insert", "Delete", "Backspace", "Space",
        "Enter", "Escape", "LeftCtrl", "LeftShift", "LeftAlt", "LeftSuper",
        "RightCtrl", "RightShift", "RightAlt", "RightSuper", "Menu",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
        "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
        "U", "V", "W", "X", "Y", "Z",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10",
        "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19",
        "F20", "F21", "F22", "F23", "F24",
        "Apostrophe", "Comma", "Minus", "Period", "Slash", "Semicolon",
        "Equal", "LeftBracket", "Backslash", "RightBracket", "GraveAccent",
        "CapsLock", "ScrollLock", "NumLock", "PrintScreen", "Pause",
        "Keypad0", "Keypad1", "Keypad2", "Keypad3", "Keypad4", "Keypad5",
        "Keypad6", "Keypad7", "Keypad8", "Keypad9", "KeypadDecimal",
        "KeypadDivide", "KeypadMultiply", "KeypadSubtract", "KeypadAdd",
        "KeypadEnter", "KeypadEqual", "AppBack", "AppForward", "Oem102",
    };
    static_assert((chord::k_gamepad_start - chord::k_tab) ==
                  (sizeof(k_names) / sizeof(k_names[0])));
    if (key < chord::k_tab || key >= chord::k_gamepad_start)
        return nullptr;
    return k_names[key - chord::k_tab];
}

}
