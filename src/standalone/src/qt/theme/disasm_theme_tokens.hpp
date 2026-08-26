#pragma once

#include <QColor>
#include <QString>

#include <cstdint>

namespace aida::qt::theme {

enum disasm_kind_t {
    kind_unknown = 0,
    kind_regular_function = 1,
    kind_library_function = 2,
    kind_lumina_function = 3,
    kind_external_import = 4,
    kind_instruction = 5,
    kind_data = 6,
    kind_string = 7,
    kind_label = 8,
    kind_register_op = 9,
    kind_immediate = 10,
    kind_comment = 11,
    kind_data_byte = 12,
    kind_data_word = 13,
    kind_data_dword = 14,
    kind_data_qword = 15,
    kind_data_xmmword = 16,
    kind_data_ymmword = 17,
    kind_data_zmmword = 18,
    kind_data_tbyte = 19,
    kind_data_fword = 20,
    kind_string_ascii = 21,
    kind_string_unicode = 22,
    kind_struct_ref = 23,
    kind_array_ref = 24,
    kind_offset_ref = 25,
    kind_segment_ref = 26,
    kind_pointer_ref = 27,
    kind_data_unknown = 28,
    kind_align_directive = 29,
    kind_jump_thunk = 30,
    kind_case_label = 31,
    kind_default_case = 32,
    kind_stack_var = 33,
    kind_stack_arg = 34,
    kind_saved_reg = 35,
    kind_restored_reg = 36,
    kind_section_text = 37,
    kind_section_data = 38,
    kind_section_rdata = 39,
    kind_section_bss = 40,
    kind_section_rsrc = 41,
    kind_section_other = 42,
    kind_custom_struct = 43,
    kind_enum_value = 44,
    kind_typelib_type = 45,
    kind_mnem_branch = 46,
    kind_mnem_call = 47,
    kind_mnem_ret = 48,
    kind_mnem_arith = 49,
    kind_mnem_logic = 50,
    kind_mnem_data = 51,
    kind_mnem_sse = 52,
    kind_mnem_string = 53,
    kind_mnem_priv = 54,
    kind_mnem_nop = 55,
    kind_mnem_int = 56,
    kind_mnem_other = 57,
    kind_imp_function = 58,
    kind_entry_point = 59,
    kind_main_function = 60,
    kind_winmain_function = 61,
    kind_dllmain_function = 62
};

struct AidaDisasmTheme {
    QColor segment;
    QColor address;
    QColor bytes;
    QColor mnemonic;
    QColor reg;
    QColor reg_ptr;
    QColor immediate_num;
    QColor immediate_offset;
    QColor string_ref;
    QColor imp_func;
    QColor external_func;
    QColor library_func;
    QColor lumina_func;
    QColor sub_label;
    QColor loc_label;
    QColor func_name;
    QColor data_ref;
    QColor comment;
    QColor xref;
    QColor separator;
    QColor banner;
    QColor var_decl;
    QColor var_use;
    QColor keyword;
    QColor directive;
    QColor selection_bg;
    QColor cursor_line_bg;
    QColor gutter_bg;
    QColor panel_bg;
    QColor arrow_up;
    QColor arrow_down;

    QColor data_byte;
    QColor data_word;
    QColor data_dword;
    QColor data_qword;
    QColor data_xmmword;
    QColor data_ymmword;
    QColor data_zmmword;
    QColor data_tbyte;
    QColor data_fword;
    QColor string_ascii;
    QColor string_unicode;
    QColor struct_ref;
    QColor array_ref;
    QColor offset_ref;
    QColor segment_ref;
    QColor pointer_ref;
    QColor data_unknown;
    QColor align_directive;
    QColor jump_thunk;
    QColor case_label;
    QColor default_case;
    QColor stack_var;
    QColor stack_arg;
    QColor saved_reg;
    QColor restored_reg;
    QColor section_text;
    QColor section_data;
    QColor section_rdata;
    QColor section_bss;
    QColor section_rsrc;
    QColor section_other;
    QColor custom_struct;
    QColor enum_value;
    QColor typelib_type;
    QColor mnem_branch;
    QColor mnem_call;
    QColor mnem_ret;
    QColor mnem_arith;
    QColor mnem_logic;
    QColor mnem_data;
    QColor mnem_sse;
    QColor mnem_string;
    QColor mnem_priv;
    QColor mnem_nop;
    QColor mnem_int;
    QColor mnem_other;
    QColor entry_point;
    QColor main_function;
    QColor winmain_function;
    QColor dllmain_function;

    QColor color_for_kind(int kind) const;
    QColor color_for_section_name(const QString& section_name) const;
};

const AidaDisasmTheme& disasm_theme_snapshot();
void refresh_disasm_theme_tokens();
quint64 disasm_theme_revision();

QColor disasm_with_alpha(QColor color, qreal alpha);

}
