#include "rs_compiler_ir.hpp"
#include "rs_lang_ast_builder.hpp"

namespace rs
{
    void program_debug_data_info::generate(grammar::ast_base* ast_node, ir_compiler* compiler)
    {
        // funcdef should not genrate val..
        if (dynamic_cast<ast::ast_value_function_define*>(ast_node)
            || dynamic_cast<ast::ast_list*>(ast_node)
            || dynamic_cast<ast::ast_namespace*>(ast_node)
            || dynamic_cast<ast::ast_sentence_block*>(ast_node)
            || dynamic_cast<ast::ast_if*>(ast_node)
            || dynamic_cast<ast::ast_while*>(ast_node))
            return;

        auto& row_buff = _general_src_data_buf_a[ast_node->source_file][ast_node->row_no];
        if (row_buff.find(ast_node->col_no) == row_buff.end())
            row_buff[ast_node->col_no] = SIZE_MAX;

        auto& old_ip = row_buff[ast_node->col_no];
        if (compiler->get_now_ip() < old_ip)
            old_ip = compiler->get_now_ip();


    }
    void program_debug_data_info::finalize_pdd()
    {
        for (auto& [filename, rowbuf] : _general_src_data_buf_a)
        {
            for (auto& [rowno, colbuf] : rowbuf)
            {
                for (auto& [colno, ipxx] : colbuf)
                {
                    // if (_general_src_data_buf_b.find(ipxx) == _general_src_data_buf_b.end())
                    _general_src_data_buf_b[ipxx] = location{ rowno , colno ,filename };
                }
            }
        }
    }
    const program_debug_data_info::location& program_debug_data_info::get_src_location(byte_t* rt_pos) const
    {
        const size_t FAIL_INDEX = SIZE_MAX;
        static program_debug_data_info::location     FAIL_LOC;

        size_t result = FAIL_INDEX;
        auto byte_offset = (rt_pos - runtime_codes_base) + 1;
        do
        {
            --byte_offset;
            if (auto fnd = pdd_rt_code_byte_offset_to_ir.find(byte_offset);
                fnd != pdd_rt_code_byte_offset_to_ir.end())
            {
                result = fnd->second;
                break;
            }
        } while (byte_offset > 0);

        if (result == FAIL_INDEX)
            return FAIL_LOC;

        while (_general_src_data_buf_b.find(result) == _general_src_data_buf_b.end())
        {
            result--;
        }

        return _general_src_data_buf_b.at(result);
    }
    size_t program_debug_data_info::get_ip_by_src_location(const std::string& src_name, size_t rowno)const
    {
        const size_t FAIL_INDEX = SIZE_MAX;

        auto fnd = _general_src_data_buf_a.find(src_name);
        if (fnd == _general_src_data_buf_a.end())
            return FAIL_INDEX;

        size_t result = FAIL_INDEX;
        for (auto& [rowid, linebuf] : fnd->second)
        {
            if (rowid >= rowno)
            {
                for (auto [colno, ip] : linebuf)
                    if (ip < result)
                        result = ip;
                return result;
            }
        }
        return FAIL_INDEX;
    }
    size_t program_debug_data_info::get_ip_index(byte_t* rt_pos) const
    {
        const size_t FAIL_INDEX = SIZE_MAX;
        static location     FAIL_LOC;

        size_t result = FAIL_INDEX;
        auto byte_offset = (rt_pos - runtime_codes_base) + 1;
        do
        {
            --byte_offset;
            if (auto fnd = pdd_rt_code_byte_offset_to_ir.find(byte_offset);
                fnd != pdd_rt_code_byte_offset_to_ir.end())
            {
                result = fnd->second;
                break;
            }
        } while (byte_offset > 0);

        return result;
    }
}