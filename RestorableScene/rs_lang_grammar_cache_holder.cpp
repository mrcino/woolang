#define _CRT_SECURE_NO_WARNINGS

#define RS_LANG_GRAMMAR_LR1_IMPL

#include "rs_lang_grammar_lr1_autogen.hpp"

#ifdef RS_LANG_GRAMMAR_LR1_AUTO_GENED

namespace rs
{
    void rs_read_lr1_to(rs::grammar::lr1table_t& out_lr1table)
    {
        // READ GOTO
        for (auto& goto_act : rslang_lr1_act_goto)
        {
            for (int i = 1; i < sizeof(goto_act) / sizeof(goto_act[0]); i++)
            {
                if (goto_act[i] != -1)
                    out_lr1table[goto_act[0]][grammar::nt(rslang_id_nonterm_list[i])]
                    .insert(grammar::action{ grammar::action::act_type::state_goto,
                        grammar::te{lex_type::l_eof},
                        (size_t)goto_act[i] });
            }
        }

        // READ R-S
        for (auto& red_sta_act : rslang_lr1_act_stack_reduce)
        {
            for (int i = 1; i < sizeof(red_sta_act) / sizeof(red_sta_act[0]); i++)
            {
                if (red_sta_act[i] != 0)
                {
                    if (red_sta_act[i] > 0)
                    {
                        //push
                        out_lr1table[red_sta_act[0]][grammar::te(rslang_id_term_list[i])]
                            .insert(grammar::action{ grammar::action::act_type::push_stack,
                        grammar::te{lex_type::l_eof},
                        (size_t)red_sta_act[i] - 1 });
                    }
                    else if (red_sta_act[i] < 0)
                    {
                        //redu
                        out_lr1table[red_sta_act[0]][grammar::te(rslang_id_term_list[i])]
                            .insert(grammar::action{ grammar::action::act_type::reduction,
                        grammar::te{lex_type::l_eof},
                        (size_t)(-red_sta_act[i]) - 1 });
                    }
                }
            }
        }

        // READ ACC
        out_lr1table[rslang_accept_state][grammar::te(rslang_id_term_list[rslang_accept_term])]
            .insert(grammar::action{ grammar::action::act_type::accept,
                        grammar::te{lex_type::l_eof},
                        (size_t)0 });

    }
    void rs_read_follow_set_to(rs::grammar::sym_nts_t& out_followset)
    {
        for (auto& followset : rslang_follow_sets)
        {
            for (int i = 1; i < sizeof(followset) / sizeof(followset[0]) && followset[i] != 0; i++)
            {
                out_followset[grammar::nt(rslang_id_nonterm_list[followset[0]])].insert(
                    grammar::te(rslang_id_term_list[followset[i]])
                );
            }
        }

    }
    void rs_read_origin_p_to(std::vector<rs::grammar::rule>& out_origin_p)
    {
        for (auto& origin_p : rslang_origin_p)
        {
            grammar::symlist rule_symlist;

            for (int i = 0; i < origin_p[2]; i++)
            {
                if (origin_p[2 + i] > 0)
                    rule_symlist.push_back(grammar::te(rslang_id_term_list[origin_p[2 + i]]));
                else
                    rule_symlist.push_back(grammar::nt(rslang_id_nonterm_list[-origin_p[2 + i]]));

            }

            out_origin_p.push_back(rs::grammar::rule{
                grammar::nt(rslang_id_nonterm_list[origin_p[0]],origin_p[1]),
                rule_symlist
                });
        }
    }
}

#endif
