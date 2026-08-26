#include "qt/theme/disasm_theme_tokens.hpp"

#include "qt/theme/aida_tokens.hpp"
#include "qt/widgets/aida_paint_utils.hpp"

namespace aida::qt::theme {

namespace {

AidaDisasmTheme g_snapshot;
quint64 g_revision = 0;
bool g_initialized = false;

QColor with_alpha_u8(const QColor& color, int alpha)
{
    QColor result = color;
    result.setAlpha(alpha & 0xFF);
    return result;
}

void populate(AidaDisasmTheme& theme)
{
    const tokens_t& t = tokens();
    theme.segment = t.text_dim;
    theme.address = t.syn_address;
    theme.bytes = t.text_dim;
    theme.mnemonic = t.syn_keyword;
    theme.reg = t.syn_register;
    theme.reg_ptr = t.syn_register;
    theme.immediate_num = t.syn_number;
    theme.immediate_offset = t.text_secondary;
    theme.string_ref = t.syn_string;
    theme.imp_func = t.error;
    theme.external_func = t.error;
    theme.library_func = t.info;
    theme.lumina_func = t.success;
    theme.sub_label = t.syn_function;
    theme.loc_label = t.syn_function;
    theme.func_name = t.syn_function;
    theme.data_ref = t.syn_register;
    theme.comment = t.syn_comment;
    theme.xref = with_alpha_u8(t.success, 235);
    theme.separator = t.border_subtle;
    theme.banner = t.accent;
    theme.var_decl = t.syn_register;
    theme.var_use = t.syn_identifier;
    theme.keyword = t.syn_keyword;
    theme.directive = t.syn_preprocessor;
    theme.selection_bg = t.selection;
    theme.cursor_line_bg = t.hover_wash;
    theme.gutter_bg = t.bg_elevated;
    theme.panel_bg = t.bg_base;
    theme.arrow_up = t.success;
    theme.arrow_down = t.warning;

    theme.data_byte = t.syn_number;
    theme.data_word = t.syn_number;
    theme.data_dword = t.syn_number;
    theme.data_qword = t.syn_number;
    theme.data_xmmword = t.syn_type;
    theme.data_ymmword = t.syn_type;
    theme.data_zmmword = t.syn_type;
    theme.data_tbyte = t.warning;
    theme.data_fword = t.warning;
    theme.string_ascii = t.syn_string;
    theme.string_unicode = t.syn_string;
    theme.struct_ref = t.syn_type;
    theme.array_ref = t.syn_type;
    theme.offset_ref = t.text_secondary;
    theme.segment_ref = t.text_dim;
    theme.pointer_ref = t.info;
    theme.data_unknown = t.text_dim;
    theme.align_directive = t.syn_comment;
    theme.jump_thunk = t.error;
    theme.case_label = t.syn_register;
    theme.default_case = t.warning;
    theme.stack_var = t.syn_register;
    theme.stack_arg = t.syn_identifier;
    theme.saved_reg = t.text_dim;
    theme.restored_reg = t.text_secondary;
    theme.section_text = t.info;
    theme.section_data = t.syn_register;
    theme.section_rdata = t.syn_type;
    theme.section_bss = t.text_dim;
    theme.section_rsrc = t.warning;
    theme.section_other = t.text_secondary;
    theme.custom_struct = t.syn_type;
    theme.enum_value = t.syn_number;
    theme.typelib_type = t.syn_type;
    theme.mnem_branch = t.warning;
    theme.mnem_call = t.syn_keyword;
    theme.mnem_ret = t.error;
    theme.mnem_arith = t.syn_keyword;
    theme.mnem_logic = t.syn_keyword;
    theme.mnem_data = t.syn_keyword;
    theme.mnem_sse = t.syn_type;
    theme.mnem_string = t.syn_string;
    theme.mnem_priv = t.warning;
    theme.mnem_nop = t.text_dim;
    theme.mnem_int = t.error;
    theme.mnem_other = t.syn_keyword;
    theme.entry_point = t.accent;
    theme.main_function = t.success;
    theme.winmain_function = t.success;
    theme.dllmain_function = t.success;
}

}

QColor AidaDisasmTheme::color_for_kind(int kind) const
{
    switch (kind) {
    case kind_regular_function: return sub_label;
    case kind_library_function: return library_func;
    case kind_lumina_function: return lumina_func;
    case kind_external_import: return imp_func;
    case kind_instruction: return mnemonic;
    case kind_data: return data_ref;
    case kind_string: return string_ref;
    case kind_label: return loc_label;
    case kind_register_op: return reg;
    case kind_immediate: return immediate_num;
    case kind_comment: return comment;
    case kind_data_byte: return data_byte;
    case kind_data_word: return data_word;
    case kind_data_dword: return data_dword;
    case kind_data_qword: return data_qword;
    case kind_data_xmmword: return data_xmmword;
    case kind_data_ymmword: return data_ymmword;
    case kind_data_zmmword: return data_zmmword;
    case kind_data_tbyte: return data_tbyte;
    case kind_data_fword: return data_fword;
    case kind_string_ascii: return string_ascii;
    case kind_string_unicode: return string_unicode;
    case kind_struct_ref: return struct_ref;
    case kind_array_ref: return array_ref;
    case kind_offset_ref: return offset_ref;
    case kind_segment_ref: return segment_ref;
    case kind_pointer_ref: return pointer_ref;
    case kind_data_unknown: return data_unknown;
    case kind_align_directive: return align_directive;
    case kind_jump_thunk: return jump_thunk;
    case kind_case_label: return case_label;
    case kind_default_case: return default_case;
    case kind_stack_var: return stack_var;
    case kind_stack_arg: return stack_arg;
    case kind_saved_reg: return saved_reg;
    case kind_restored_reg: return restored_reg;
    case kind_section_text: return section_text;
    case kind_section_data: return section_data;
    case kind_section_rdata: return section_rdata;
    case kind_section_bss: return section_bss;
    case kind_section_rsrc: return section_rsrc;
    case kind_section_other: return section_other;
    case kind_custom_struct: return custom_struct;
    case kind_enum_value: return enum_value;
    case kind_typelib_type: return typelib_type;
    case kind_mnem_branch: return mnem_branch;
    case kind_mnem_call: return mnem_call;
    case kind_mnem_ret: return mnem_ret;
    case kind_mnem_arith: return mnem_arith;
    case kind_mnem_logic: return mnem_logic;
    case kind_mnem_data: return mnem_data;
    case kind_mnem_sse: return mnem_sse;
    case kind_mnem_string: return mnem_string;
    case kind_mnem_priv: return mnem_priv;
    case kind_mnem_nop: return mnem_nop;
    case kind_mnem_int: return mnem_int;
    case kind_mnem_other: return mnem_other;
    case kind_imp_function: return imp_func;
    case kind_entry_point: return entry_point;
    case kind_main_function: return main_function;
    case kind_winmain_function: return winmain_function;
    case kind_dllmain_function: return dllmain_function;
    case kind_unknown:
    default: return address;
    }
}

QColor AidaDisasmTheme::color_for_section_name(const QString& section_name) const
{
    if (section_name.isEmpty() || section_name[0] != u'.')
        return section_other;
    if (section_name.size() > 4 && section_name[1] == u't' && section_name[2] == u'e' &&
        section_name[3] == u'x' && section_name[4] == u't')
        return section_text;
    if (section_name.size() > 5 && section_name[1] == u'r' && section_name[2] == u'd' &&
        section_name[3] == u'a' && section_name[4] == u't' && section_name[5] == u'a')
        return section_rdata;
    if (section_name.size() > 4 && section_name[1] == u'd' && section_name[2] == u'a' &&
        section_name[3] == u't' && section_name[4] == u'a')
        return section_data;
    if (section_name.size() > 3 && section_name[1] == u'b' && section_name[2] == u's' &&
        section_name[3] == u's')
        return section_bss;
    if (section_name.size() > 4 && section_name[1] == u'r' && section_name[2] == u's' &&
        section_name[3] == u'r' && section_name[4] == u'c')
        return section_rsrc;
    return section_other;
}

const AidaDisasmTheme& disasm_theme_snapshot()
{
    if (!g_initialized)
        refresh_disasm_theme_tokens();
    return g_snapshot;
}

void refresh_disasm_theme_tokens()
{
    populate(g_snapshot);
    g_initialized = true;
    ++g_revision;
}

quint64 disasm_theme_revision()
{
    if (!g_initialized)
        refresh_disasm_theme_tokens();
    return g_revision;
}

QColor disasm_with_alpha(QColor color, qreal alpha)
{
    color.setAlphaF(widgets::clamp01(alpha) * color.alphaF());
    return color;
}

}
