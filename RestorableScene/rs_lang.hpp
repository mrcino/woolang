#pragma once

#include "rs_basic_type.hpp"
#include "rs_lang_ast_builder.hpp"
#include "rs_compiler_ir.hpp"

#include <unordered_map>
#include <unordered_set>
namespace rs
{
    struct lang_symbol
    {
        enum class symbol_type
        {
            typing,
            variable,
            function,
        };
        symbol_type type;
        std::wstring name;

        ast::ast_decl_attribute* attribute;
        lang_scope* defined_in_scope;
        bool define_in_function = false;
        bool static_symbol = false;
        bool has_been_defined_in_pass2 = false;
        bool is_constexpr = false;
        bool has_been_assigned = false;
        bool is_ref = false;

        union
        {
            rs_integer_t stackvalue_index_in_funcs = 0;
            size_t global_index_in_lang;
        };

        union
        {
            ast::ast_value* variable_value;
            ast::ast_type* type_informatiom;
        };

        std::vector<ast::ast_value_function_define*> function_overload_sets;
    };

    struct lang_scope
    {
        bool stop_searching_in_last_scope_flag;

        enum scope_type
        {
            namespace_scope,    // namespace xx{}
            function_scope,     // func xx(){}
            just_scope,         //{} if{} while{}
        };

        scope_type type;
        lang_scope* belong_namespace;
        lang_scope* parent_scope;
        std::wstring scope_namespace;
        std::unordered_map<std::wstring, lang_symbol*> symbols;

        // Only used when this scope is a namespace.
        std::unordered_map<std::wstring, lang_scope*> sub_namespaces;

        std::vector<ast::ast_using_namespace*> used_namespace;
        std::vector<lang_symbol*> in_function_symbols;

        ast::ast_value_function_define* function_node;

        size_t max_used_stack_size_in_func = 0; // only used in function_scope
        size_t used_stackvalue_index = 0; // only used in function_scope

        size_t this_block_used_stackvalue_count = 0;

        size_t assgin_stack_index(lang_symbol* in_func_variable)
        {
            rs_assert(type == scope_type::function_scope);
            in_function_symbols.push_back(in_func_variable);

            if (used_stackvalue_index + 1 > max_used_stack_size_in_func)
                max_used_stack_size_in_func = used_stackvalue_index + 1;
            return used_stackvalue_index++;
        }

        void reduce_function_used_stack_size_at(rs_integer_t canceled_stack_pos)
        {
            max_used_stack_size_in_func--;
            for (auto* infuncvars : in_function_symbols)
            {
                rs_assert(infuncvars->type == lang_symbol::symbol_type::variable);

                if (!infuncvars->static_symbol)
                {
                    if (infuncvars->stackvalue_index_in_funcs == canceled_stack_pos)
                        infuncvars->stackvalue_index_in_funcs = 0;
                    else if (infuncvars->stackvalue_index_in_funcs > canceled_stack_pos)
                        infuncvars->stackvalue_index_in_funcs--;
                }

            }
        }

    };


    class lang
    {
    private:
        lexer* lang_anylizer;
        std::vector<lang_scope*> lang_scopes_buffers;
        std::vector<lang_symbol*> lang_symbols; // only used for storing symbols to release
        std::vector<opnum::opnumbase*> generated_opnum_list_for_clean;
        std::forward_list<grammar::ast_base*> generated_ast_nodes_buffers;
        std::unordered_set<grammar::ast_base*> traving_node;
        std::vector<lang_scope*> lang_scopes; // it is a stack like list;
        lang_scope* now_namespace = nullptr;

        ast::ast_value_function_define* now_function_in_final_anylize = nullptr;
    public:
        lang(lexer& lex) :
            lang_anylizer(&lex)
        {
            begin_namespace(L"");   // global namespace
        }
        ~lang()
        {
            clean_and_close_lang();
        }

        void fully_update_type(ast::ast_type* type)
        {
            if (type->is_custom())
            {
                if (type->is_complex())
                    fully_update_type(type->complex_type);
                if (type->is_func())
                    for (auto& a_t : type->argument_types)
                        fully_update_type(a_t);

                // ready for update..
                if (ast::ast_type::is_custom_type(type->type_name))
                {
                    auto* type_sym = find_type_in_this_scope(type);
                    if (type_sym)
                    {
                        fully_update_type(type_sym->type_informatiom);



                        if (type->is_func())
                            type->set_ret_type(type_sym->type_informatiom);
                        else
                            type->set_type(type_sym->type_informatiom);
                    }
                }
            }
        }

        void analyze_pass1(grammar::ast_base* ast_node)
        {
            if (traving_node.find(ast_node) != traving_node.end())
                return;

            struct traving_guard
            {
                lang* _lang;
                grammar::ast_base* _tving_node;
                traving_guard(lang* _lg, grammar::ast_base* ast_ndoe)
                    :_tving_node(ast_ndoe)
                    , _lang(_lg)
                {
                    _lang->traving_node.insert(_tving_node);
                }
                ~traving_guard()
                {
                    _lang->traving_node.erase(_tving_node);
                }
            };

            traving_guard g1(this, ast_node);

            using namespace ast;

            if (!ast_node)return;

            if (ast_symbolable_base* a_symbol_ob = dynamic_cast<ast_symbolable_base*>(ast_node))
            {
                a_symbol_ob->searching_begin_namespace_in_pass2 = now_scope();
            }

            ///////////////////////////////////////////////////////////////////////////////////////////

            if (ast_namespace* a_namespace = dynamic_cast<ast_namespace*>(ast_node))
            {
                begin_namespace(a_namespace->scope_name);
                a_namespace->add_child(a_namespace->in_scope_sentence);
                grammar::ast_base* child = a_namespace->in_scope_sentence->children;
                while (child)
                {
                    analyze_pass1(child);
                    child = child->sibling;
                }
                end_namespace();
            }
            else if (ast_varref_defines* a_varref_defs = dynamic_cast<ast_varref_defines*>(ast_node))
            {
                for (auto& varref : a_varref_defs->var_refs)
                {
                    analyze_pass1(varref.init_val);
                    varref.symbol = define_variable_in_this_scope(varref.ident_name, varref.init_val, a_varref_defs->declear_attribute);
                    varref.symbol->is_ref = varref.is_ref;
                    a_varref_defs->add_child(varref.init_val);
                }
            }
            else if (ast_value_binary* a_value_bin = dynamic_cast<ast_value_binary*>(ast_node))
            {
                analyze_pass1(a_value_bin->left);
                analyze_pass1(a_value_bin->right);

                a_value_bin->add_child(a_value_bin->left);
                a_value_bin->add_child(a_value_bin->right);

                a_value_bin->value_type = pass_binary_op::binary_upper_type(
                    a_value_bin->left->value_type,
                    a_value_bin->right->value_type
                );

                if (nullptr == a_value_bin->value_type)
                    a_value_bin->value_type = new ast_type(L"pending");
            }
            else if (ast_value_index* a_value_idx = dynamic_cast<ast_value_index*>(ast_node))
            {
                analyze_pass1(a_value_idx->from);
                analyze_pass1(a_value_idx->index);
            }
            else if (ast_value_assign* a_value_assi = dynamic_cast<ast_value_assign*>(ast_node))
            {
                analyze_pass1(a_value_assi->left);
                analyze_pass1(a_value_assi->right);

                a_value_assi->add_child(a_value_assi->left);
                a_value_assi->add_child(a_value_assi->right);

                if (auto lsymb = dynamic_cast<ast_value_symbolable_base*>(a_value_assi->left)
                    ; lsymb && lsymb->symbol)
                    lsymb->symbol->has_been_assigned = true;

                a_value_assi->value_type = new ast_type(L"pending");
                a_value_assi->value_type->set_type(a_value_assi->left->value_type);

                if (!a_value_assi->value_type->is_pending() && !a_value_assi->right->value_type->is_pending())
                {
                    if (!ast_type::check_castable(a_value_assi->left->value_type, a_value_assi->right->value_type, false))
                    {
                        lang_anylizer->lang_error(0x0000, a_value_assi, RS_ERR_CANNOT_ASSIGN_TYPE_TO_TYPE,
                            a_value_assi->right->value_type->get_type_name().c_str(),
                            a_value_assi->left->value_type->get_type_name().c_str());
                    }
                }
            }
            else if (ast_value_logical_binary* a_value_logic_bin = dynamic_cast<ast_value_logical_binary*>(ast_node))
            {
                analyze_pass1(a_value_logic_bin->left);
                analyze_pass1(a_value_logic_bin->right);

                a_value_logic_bin->add_child(a_value_logic_bin->left);
                a_value_logic_bin->add_child(a_value_logic_bin->right);
            }
            else if (ast_value_variable* a_value_var = dynamic_cast<ast_value_variable*>(ast_node))
            {
                auto* sym = find_value_in_this_scope(a_value_var);
                if (sym)
                {
                    a_value_var->value_type = sym->variable_value->value_type;
                }
            }
            else if (ast_value_type_cast* a_value_cast = dynamic_cast<ast_value_type_cast*>(ast_node))
            {
                analyze_pass1(a_value_cast->_be_cast_value_node);
                a_value_cast->add_child(a_value_cast->_be_cast_value_node);
            }
            else if (ast_value_type_judge* ast_value_judge = dynamic_cast<ast_value_type_judge*>(ast_node))
            {
                if (ast_value_judge->is_mark_as_using_ref)
                    ast_value_judge->_be_cast_value_node->is_mark_as_using_ref = true;

                analyze_pass1(ast_value_judge->_be_cast_value_node);
               
            }
            else if (ast_value_function_define* a_value_func = dynamic_cast<ast_value_function_define*>(ast_node))
            {
                a_value_func->this_func_scope = begin_function(a_value_func);

                auto arg_child = a_value_func->argument_list->children;
                while (arg_child)
                {
                    if (ast_value_arg_define* argdef = dynamic_cast<ast_value_arg_define*>(arg_child))
                    {
                        argdef->symbol = define_variable_in_this_scope(argdef->arg_name, argdef, argdef->declear_attribute);
                        argdef->symbol->is_ref = argdef->is_ref;
                    }
                    else
                    {
                        rs_assert(dynamic_cast<ast_token*>(arg_child));
                    }

                    arg_child = arg_child->sibling;
                }

                if (a_value_func->in_function_sentence)
                {
                    analyze_pass1(a_value_func->in_function_sentence);
                    a_value_func->add_child(a_value_func->in_function_sentence);
                }
                end_function();
            }
            else if (ast_fakevalue_unpacked_args* a_fakevalue_unpacked_args = dynamic_cast<ast_fakevalue_unpacked_args*>(ast_node))
            {
                analyze_pass1(a_fakevalue_unpacked_args->unpacked_pack);
            }
            else if (ast_value_funccall* a_value_funccall = dynamic_cast<ast_value_funccall*>(ast_node))
            {
                analyze_pass1(a_value_funccall->called_func);
                analyze_pass1(a_value_funccall->arguments);
                a_value_funccall->add_child(a_value_funccall->called_func);
                a_value_funccall->add_child(a_value_funccall->arguments);

                // function call should be 'pending' type, then do override judgement in pass2
            }
            else if (ast_value_array* a_value_arr = dynamic_cast<ast_value_array*>(ast_node))
            {
                analyze_pass1(a_value_arr->array_items);
                a_value_arr->add_child(a_value_arr->array_items);
            }
            else if (ast_value_mapping* a_value_map = dynamic_cast<ast_value_mapping*>(ast_node))
            {
                analyze_pass1(a_value_map->mapping_pairs);
                a_value_map->add_child(a_value_map->mapping_pairs);
            }
            else if (ast_value_indexed_variadic_args* a_value_variadic_args_idx = dynamic_cast<ast_value_indexed_variadic_args*>(ast_node))
            {
                analyze_pass1(a_value_variadic_args_idx->argindex);
            }
            else if (ast_return* a_ret = dynamic_cast<ast_return*>(ast_node))
            {
                auto* located_function_scope = in_function();
                if (!located_function_scope)
                    lang_anylizer->lang_error(0x0000, a_ret, RS_ERR_CANNOT_DO_RET_OUSIDE_FUNC);
                else
                {
                    a_ret->located_function = located_function_scope->function_node;
                    if (a_ret->return_value)
                    {
                        analyze_pass1(a_ret->return_value);

                        if (a_ret->return_value->value_type->is_pending() == false)
                        {
                            auto* func_return_type = located_function_scope->function_node->value_type->get_return_type();

                            if (func_return_type->is_pending())
                            {
                                located_function_scope->function_node->value_type->set_ret_type(a_ret->return_value->value_type);
                            }
                            else if (located_function_scope->function_node->auto_adjust_return_type)
                            {
                                if (!func_return_type->is_same(a_ret->return_value->value_type))
                                {
                                    auto* mixed_type = pass_binary_op::binary_upper_type(func_return_type, a_ret->return_value->value_type);
                                    if (mixed_type)
                                    {
                                        located_function_scope->function_node->value_type->set_type_with_name(mixed_type->type_name);
                                    }
                                    else
                                    {
                                        located_function_scope->function_node->value_type->set_type_with_name(L"dynamic");
                                        lang_anylizer->lang_warning(0x0000, a_ret, RS_WARN_FUNC_WILL_RETURN_DYNAMIC);
                                    }
                                }
                            }
                            else
                            {
                                if (!func_return_type->is_same(a_ret->return_value->value_type))
                                {
                                    auto* cast_return_type = pass_type_cast::do_cast(*lang_anylizer, a_ret->return_value, func_return_type);
                                    cast_return_type->col_no = a_ret->col_no;
                                    cast_return_type->row_no = a_ret->row_no;
                                    cast_return_type->source_file = a_ret->source_file;

                                    analyze_pass1(cast_return_type);

                                    a_ret->return_value = cast_return_type;
                                }
                            }
                        }

                        a_ret->add_child(a_ret->return_value);
                    }
                    else
                    {
                        if (located_function_scope->function_node->auto_adjust_return_type)
                        {
                            if (located_function_scope->function_node->value_type->is_pending())
                            {
                                located_function_scope->function_node->value_type->set_type_with_name(L"void");
                                located_function_scope->function_node->auto_adjust_return_type = false;
                            }
                            else
                            {
                                lang_anylizer->lang_error(0x0000, a_ret, RS_ERR_CANNOT_RET_TYPE_AND_TYPE_AT_SAME_TIME, L"void", located_function_scope->function_node->value_type->type_name.c_str());
                            }
                        }
                        else
                        {
                            if (!located_function_scope->function_node->value_type->is_void())
                                lang_anylizer->lang_error(0x0000, a_ret, RS_ERR_CANNOT_RET_TYPE_AND_TYPE_AT_SAME_TIME, L"void", located_function_scope->function_node->value_type->type_name.c_str());
                        }
                    }
                }

            }
            else if (ast_sentence_block* a_sentence_blk = dynamic_cast<ast_sentence_block*>(ast_node))
            {
                this->begin_scope();
                analyze_pass1(a_sentence_blk->sentence_list);
                this->end_scope();
            }
            else if (ast_if* ast_if_sentence = dynamic_cast<ast_if*>(ast_node))
            {
                analyze_pass1(ast_if_sentence->judgement_value);
                analyze_pass1(ast_if_sentence->execute_if_true);
                if (ast_if_sentence->execute_else)
                    analyze_pass1(ast_if_sentence->execute_else);
            }
            else if (ast_while* ast_while_sentence = dynamic_cast<ast_while*>(ast_node))
            {
                analyze_pass1(ast_while_sentence->judgement_value);
                analyze_pass1(ast_while_sentence->execute_sentence);
            }
            else if (ast_value_unary* a_value_unary = dynamic_cast<ast_value_unary*>(ast_node))
            {
                analyze_pass1(a_value_unary->val);
                a_value_unary->add_child(a_value_unary->val);

                if (a_value_unary->operate == +lex_type::l_lnot)
                    a_value_unary->value_type = new ast_type(L"int");
                else if (!a_value_unary->val->value_type->is_pending())
                    a_value_unary->value_type = a_value_unary->val->value_type;
            }
            else if (ast_mapping_pair* a_mapping_pair = dynamic_cast<ast_mapping_pair*>(ast_node))
            {
                analyze_pass1(a_mapping_pair->key);
                analyze_pass1(a_mapping_pair->val);
            }
            else if (ast_using_namespace* a_using_namespace = dynamic_cast<ast_using_namespace*>(ast_node))
            {
                // do using namespace op..
                // do check..
                auto* parent_child = a_using_namespace->parent->children;
                while (parent_child)
                {
                    if (auto* using_namespace_ch = dynamic_cast<ast_using_namespace*>(parent_child))
                    {
                        if (using_namespace_ch == a_using_namespace)
                            break;
                    }
                    else
                    {
                        lang_anylizer->lang_error(0x0000, a_using_namespace, RS_ERR_ERR_PLACE_FOR_USING_NAMESPACE);
                        break;
                    }

                    parent_child = parent_child->sibling;
                }
                now_scope()->used_namespace.push_back(a_using_namespace);
            }
            else if (ast_using_type_as* a_using_type_as = dynamic_cast<ast_using_type_as*>(ast_node))
            {
                // now_scope()->used_namespace.push_back(a_using_namespace);
                define_type_in_this_scope(a_using_type_as->new_type_identifier, a_using_type_as->old_type, a_using_type_as->declear_attribute);
            }
            else
            {
                grammar::ast_base* child = ast_node->children;
                while (child)
                {
                    analyze_pass1(child);
                    child = child->sibling;
                }
            }
            if (ast_value* a_val = dynamic_cast<ast_value*>(ast_node))
            {
                if (a_val->value_type->is_custom())
                {
                    // ready for update..
                    fully_update_type(a_val->value_type);
                }
                a_val->value_type->searching_begin_namespace_in_pass2 = now_scope();
                // end if (ast_value* a_val = dynamic_cast<ast_value*>(ast_node))
            }
        }
        bool analyze_pass2(grammar::ast_base* ast_node)
        {
            rs_assert(ast_node);

            if (ast_node->completed_in_pass2)
                return true;

            if (traving_node.find(ast_node) != traving_node.end())
                return true;

            struct traving_guard
            {
                lang* _lang;
                grammar::ast_base* _tving_node;
                traving_guard(lang* _lg, grammar::ast_base* ast_ndoe)
                    :_tving_node(ast_ndoe)
                    , _lang(_lg)
                {
                    _lang->traving_node.insert(_tving_node);
                }
                ~traving_guard()
                {
                    _lang->traving_node.erase(_tving_node);
                }
            };

            traving_guard g1(this, ast_node);

            using namespace ast;

            if (ast_value* a_value = dynamic_cast<ast_value*>(ast_node))
            {
                if (a_value->value_type->is_custom())
                {
                    // ready for update..
                    fully_update_type(a_value->value_type);
                }


                if (a_value->value_type->is_pending())
                {
                    if (ast_value_variable* a_value_var = dynamic_cast<ast_value_variable*>(a_value))
                    {
                        auto* sym = find_value_in_this_scope(a_value_var);

                        if (sym)
                        {
                            if (sym->define_in_function && !sym->has_been_defined_in_pass2)
                                lang_anylizer->lang_error(0x0000, a_value_var, RS_ERR_UNKNOWN_IDENTIFIER, a_value_var->var_name.c_str());

                            analyze_pass2(sym->variable_value);
                            a_value_var->value_type = sym->variable_value->value_type;
                            a_value_var->symbol = sym;

                            if (a_value_var->value_type->is_pending())
                            {
                                if (a_value_var->symbol->type != lang_symbol::symbol_type::function)
                                    lang_anylizer->lang_error(0x0000, a_value_var, RS_ERR_UNABLE_DECIDE_VAR_TYPE);
                                else if (a_value_var->symbol->function_overload_sets.size() == 1)
                                {
                                    // only you~
                                    a_value_var->value_type = sym->function_overload_sets.front()->value_type;
                                }
                            }

                        }
                        else
                        {
                            lang_anylizer->lang_error(0x0000, a_value_var, RS_ERR_UNKNOWN_IDENTIFIER, a_value_var->var_name.c_str());
                            a_value_var->value_type = new ast_type(L"pending");
                        }
                    }
                    else if (ast_value_binary* a_value_bin = dynamic_cast<ast_value_binary*>(a_value))
                    {
                        analyze_pass2(a_value_bin->left);
                        analyze_pass2(a_value_bin->right);

                        a_value_bin->value_type = pass_binary_op::binary_upper_type(
                            a_value_bin->left->value_type,
                            a_value_bin->right->value_type
                        );

                        if (nullptr == a_value_bin->value_type)
                        {
                            lang_anylizer->lang_error(0x0000, a_value_bin, RS_ERR_CANNOT_CALC_WITH_L_AND_R);
                            a_value_bin->value_type = new ast_type(L"pending");
                        }
                    }
                    else if (ast_value_index* a_value_idx = dynamic_cast<ast_value_index*>(ast_node))
                    {
                        analyze_pass2(a_value_idx->from);
                        analyze_pass2(a_value_idx->index);
                    }
                    else if (ast_value_assign* a_value_assi = dynamic_cast<ast_value_assign*>(ast_node))
                    {
                        analyze_pass2(a_value_assi->left);
                        analyze_pass2(a_value_assi->right);

                        a_value_assi->value_type = new ast_type(L"pending");
                        a_value_assi->value_type->set_type(a_value_assi->left->value_type);

                        if (!a_value_assi->value_type->is_pending() && !a_value_assi->right->value_type->is_pending())
                        {
                            if (!ast_type::check_castable(a_value_assi->left->value_type, a_value_assi->right->value_type, false))
                            {
                                lang_anylizer->lang_error(0x0000, a_value_assi, RS_ERR_CANNOT_ASSIGN_TYPE_TO_TYPE,
                                    a_value_assi->right->value_type->get_type_name().c_str(),
                                    a_value_assi->left->value_type->get_type_name().c_str());
                            }
                        }

                        //if (a_value_assi->value_type->is_pending())
                        //{
                            /*lang_anylizer->lang_error(0x0000, a_value_assi, L"Failed to analyze the type.");*/
                            // do nothing
                        //}
                    }
                    else if (ast_value_function_define* a_value_funcdef = dynamic_cast<ast_value_function_define*>(a_value))
                    {
                        if (a_value_funcdef->in_function_sentence)
                        {
                            analyze_pass2(a_value_funcdef->in_function_sentence);
                        }
                        if (a_value_funcdef->value_type->is_pending())
                        {
                            // There is no return in function  return void
                            if (a_value_funcdef->auto_adjust_return_type)
                                a_value_funcdef->value_type->set_type_with_name(L"void");
                        }
                    }
                    else if (ast_value_funccall* a_value_funccall = dynamic_cast<ast_value_funccall*>(ast_node))
                    {
                        analyze_pass2(a_value_funccall->called_func);
                        analyze_pass2(a_value_funccall->arguments);

                        // judge the function override..
                        if (a_value_funccall->called_func->value_type->is_pending())
                        {
                            // function call for witch overrride not judge. do it.
                            if (auto* called_funcsymb = dynamic_cast<ast_value_symbolable_base*>(a_value_funccall->called_func))
                            {
                                if (nullptr == called_funcsymb->symbol)
                                {
                                    // do nothing..
                                }
                                else if (!called_funcsymb->symbol->function_overload_sets.empty())
                                {

                                    // have override set, judge with following rule:
                                    // 1. best match
                                    // 2. need cast
                                    // 3. variadic func
                                    // -  bad match

                                    std::vector<ast_value_function_define*> best_match_sets;
                                    std::vector<ast_value_function_define*> need_cast_sets;
                                    std::vector<ast_value_function_define*> variadic_sets;

                                    for (auto* _override_func : called_funcsymb->symbol->function_overload_sets)
                                    {
                                        auto* override_func = dynamic_cast<ast_value_function_define*>(_override_func);
                                        rs_test(override_func);

                                        bool best_match = true;

                                        auto* real_args = a_value_funccall->arguments->children;
                                        auto* form_args = override_func->argument_list->children;
                                        do
                                        {
                                            auto* form_arg = dynamic_cast<ast_value_arg_define*>(form_args);
                                            auto* real_arg = dynamic_cast<ast_value*>(real_args);

                                            rs_test(real_args == real_arg);

                                            if (!form_arg)
                                            {
                                                // variadic..
                                                if (nullptr == real_arg) // arg count match, just like best match/ need cast match
                                                    goto match_check_end_for_variadic_func;
                                                else
                                                    variadic_sets.push_back(override_func);

                                                goto this_function_override_checking_over;
                                            }

                                            if (!real_arg)
                                                goto this_function_override_checking_over;// real_args count didn't match, break..

                                            if (auto* a_fakevalue_unpack_args = dynamic_cast<ast_fakevalue_unpacked_args*>(real_arg))
                                            {
                                                best_match = false;
                                                auto ecount = a_fakevalue_unpack_args->expand_count;
                                                if (0 == ecount)
                                                {
                                                    // all in!!!
                                                    variadic_sets.push_back(override_func);
                                                    goto this_function_override_checking_over;
                                                }

                                                while (ecount)
                                                {
                                                    if (form_arg)
                                                    {
                                                        form_args = form_arg->sibling;
                                                        form_arg = dynamic_cast<ast_value_arg_define*>(form_args);
                                                    }
                                                    else if (form_args)
                                                    {
                                                        // is variadic
                                                        variadic_sets.push_back(override_func);
                                                        goto this_function_override_checking_over;
                                                    }
                                                    else
                                                    {
                                                        // not match , over..
                                                        goto this_function_override_checking_over;
                                                    }
                                                    ecount--;
                                                }

                                            }

                                            if (real_arg->value_type->is_same(form_arg->value_type))
                                                ;// do nothing..
                                            else if (ast_type::check_castable(form_arg->value_type, real_arg->value_type, false))
                                                best_match = false;
                                            else
                                                break; // bad match, break..


                                            real_args = real_args->sibling;
                                        match_check_end_for_variadic_func:
                                            if (form_args)
                                                form_args = form_args->sibling;


                                            if (form_args == nullptr)
                                            {
                                                // finish match check, add it to set
                                                if (real_args == nullptr)
                                                {
                                                    if (best_match)
                                                        best_match_sets.push_back(override_func);
                                                    else
                                                        need_cast_sets.push_back(override_func);

                                                }
                                                // else: bad match..
                                            }
                                        } while (form_args);

                                    this_function_override_checking_over:;

                                    }

                                    std::vector<ast_value_function_define*>* judge_sets = nullptr;
                                    if (best_match_sets.size())
                                        judge_sets = &best_match_sets;
                                    else if (need_cast_sets.size())
                                        judge_sets = &need_cast_sets;
                                    else if (variadic_sets.size())
                                        judge_sets = &variadic_sets;

                                    if (judge_sets)
                                    {
                                        if (judge_sets->size() > 1)
                                        {
                                            std::wstring acceptable_func;
                                            for (size_t index = 0; index < judge_sets->size(); index++)
                                            {
                                                acceptable_func += L"'" + judge_sets->at(index)->function_name + L":"
                                                    + judge_sets->at(index)->value_type->get_type_name()
                                                    + L"' " RS_TERM_AT L" ("
                                                    + std::to_wstring(judge_sets->at(index)->row_no)
                                                    + L","
                                                    + std::to_wstring(judge_sets->at(index)->col_no)
                                                    + L")";

                                                if (index + 1 != judge_sets->size())
                                                {
                                                    acceptable_func += L" " RS_TERM_OR L" ";
                                                }
                                            }
                                            this->lang_anylizer->lang_error(0x0000, a_value_funccall, RS_ERR_UNABLE_DECIDE_FUNC_OVERRIDE, acceptable_func.c_str());
                                        }
                                        else
                                        {
                                            a_value_funccall->called_func = judge_sets->front();
                                            analyze_pass2(a_value_funccall->called_func);
                                        }
                                    }
                                    else
                                    {
                                        this->lang_anylizer->lang_error(0x0000, a_value_funccall, RS_ERR_NO_MATCH_FUNC_OVERRIDE);
                                    }
                                }
                            }
                        }

                        if (!a_value_funccall->called_func->value_type->is_pending())
                        {
                            if (a_value_funccall->called_func->value_type->is_func())
                            {
                                a_value_funccall->value_type = a_value_funccall->called_func->value_type->get_return_type();
                            }
                        }
                        else
                        {
                            /*
                            // for recurrence function callen, this check will cause lang error, just ignore the call type.
                            // - if function's type can be judge, it will success outside.

                            if(a_value_funccall->called_func->value_type->is_pending_function())
                                lang_anylizer->lang_error(0x0000, a_value, L"xxx '%s'.", a_value->value_type->get_type_name().c_str());
                            */
                        }

                        if (a_value_funccall->called_func
                            && a_value_funccall->called_func->value_type->is_func()
                            && !a_value_funccall->called_func->value_type->is_pending())
                        {
                            auto* real_args = a_value_funccall->arguments->children;
                            a_value_funccall->arguments->remove_allnode();

                            for (auto a_type_index = a_value_funccall->called_func->value_type->argument_types.begin();
                                a_type_index != a_value_funccall->called_func->value_type->argument_types.end();
                                a_type_index++)
                            {
                                if (!real_args)
                                {
                                    // default arg mgr here, now just kill
                                    lang_anylizer->lang_error(0x0000, a_value_funccall, RS_ERR_ARGUMENT_TOO_FEW, a_value_funccall->called_func->value_type->get_type_name().c_str());
                                }
                                else
                                {
                                    auto tmp_sib = real_args->sibling;

                                    real_args->parent = nullptr;
                                    real_args->sibling = nullptr;

                                    auto* arg_val = dynamic_cast<ast_value*>(real_args);
                                    real_args = tmp_sib;

                                    if (auto* a_fakevalue_unpack_args = dynamic_cast<ast_fakevalue_unpacked_args*>(arg_val))
                                    {
                                        a_value_funccall->arguments->add_child(a_fakevalue_unpack_args);

                                        auto ecount = a_fakevalue_unpack_args->expand_count;
                                        if (0 == ecount)
                                        {
                                            // all in!!!
                                            ecount =
                                                (rs_integer_t)(
                                                    a_value_funccall->called_func->value_type->argument_types.end()
                                                    - a_type_index);
                                            a_fakevalue_unpack_args->expand_count = -ecount;
                                        }

                                        while (ecount)
                                        {
                                            if (a_type_index != a_value_funccall->called_func->value_type->argument_types.end())
                                            {
                                                a_type_index++;
                                            }
                                            else if (a_value_funccall->called_func->value_type->is_variadic_function_type)
                                            {
                                                // is variadic
                                                break;
                                            }
                                            else
                                            {
                                                lang_anylizer->lang_error(0x0000, a_value_funccall, RS_ERR_ARGUMENT_TOO_MANY, a_value_funccall->called_func->value_type->get_type_name().c_str());
                                                break;
                                            }
                                            ecount--;
                                        }

                                        a_type_index--;
                                    }
                                    else
                                    {
                                        if (!arg_val->value_type->is_pending() && !arg_val->value_type->is_same(*a_type_index))
                                        {
                                            auto* cast_arg_type = pass_type_cast::do_cast(*lang_anylizer, arg_val, *a_type_index);
                                            cast_arg_type->col_no = arg_val->col_no;
                                            cast_arg_type->row_no = arg_val->row_no;
                                            cast_arg_type->source_file = arg_val->source_file;
                                            analyze_pass2(cast_arg_type);

                                            a_value_funccall->arguments->add_child(cast_arg_type);
                                        }
                                        else
                                        {
                                            a_value_funccall->arguments->add_child(arg_val);
                                        }
                                    }
                                }
                            }
                            if (a_value_funccall->called_func->value_type->is_variadic_function_type)
                            {
                                while (real_args)
                                {
                                    auto tmp_sib = real_args->sibling;

                                    real_args->parent = nullptr;
                                    real_args->sibling = nullptr;

                                    a_value_funccall->arguments->add_child(real_args);
                                    real_args = tmp_sib;
                                }
                            }
                            if (real_args)
                            {
                                lang_anylizer->lang_error(0x0000, a_value_funccall, RS_ERR_ARGUMENT_TOO_MANY, a_value_funccall->called_func->value_type->get_type_name().c_str());
                            }

                        }
                        else
                        {
                            lang_anylizer->lang_error(0x0000, a_value, RS_ERR_TYPE_CANNOT_BE_CALL,
                                a_value_funccall->called_func->value_type->get_type_name().c_str());
                        }
                    }
                    else if (ast_value_unary* a_value_unary = dynamic_cast<ast_value_unary*>(ast_node))
                    {
                        analyze_pass2(a_value_unary->val);

                        if (a_value_unary->operate == +lex_type::l_lnot)
                            a_value_unary->value_type = new ast_type(L"int");
                        else if (!a_value_unary->val->value_type->is_pending())
                            a_value_unary->value_type = a_value_unary->val->value_type;
                        // else
                            // not need to manage, if val is pending, other place will give error.
                    }
                    else
                    {
                        lang_anylizer->lang_error(0x0000, a_value, RS_ERR_UNKNOWN_TYPE, a_value->value_type->get_type_name().c_str());
                    }

                }

                //
                if (ast_value_function_define* a_value_funcdef = dynamic_cast<ast_value_function_define*>(a_value))
                {
                    // return-type adjust complete. do 'return' cast;
                    a_value_funcdef->auto_adjust_return_type = false;// stop using auto-adjust
                    if (a_value_funcdef->in_function_sentence)
                    {
                        analyze_pass2(a_value_funcdef->in_function_sentence);
                    }
                }
                else if (ast_value_assign* a_value_assi = dynamic_cast<ast_value_assign*>(ast_node))
                {
                    analyze_pass2(a_value_assi->right);

                    if (auto lsymb = dynamic_cast<ast_value_symbolable_base*>(a_value_assi->left)
                        ; lsymb && lsymb->symbol)
                        lsymb->symbol->has_been_assigned = true;

                    if (a_value_assi->right->value_type->is_pending_function())
                    {
                        auto* try_finding_override = pass_type_cast::do_cast(*lang_anylizer, a_value_assi->right, a_value_assi->value_type);
                        try_finding_override->col_no = a_value_assi->right->col_no;
                        try_finding_override->row_no = a_value_assi->right->row_no;
                        try_finding_override->source_file = a_value_assi->right->source_file;

                        analyze_pass2(try_finding_override);

                        a_value_assi->right = try_finding_override;
                    }

                    if (!ast_type::check_castable(a_value_assi->left->value_type, a_value_assi->right->value_type, false))
                    {
                        lang_anylizer->lang_error(0x0000, a_value_assi, RS_ERR_CANNOT_ASSIGN_TYPE_TO_TYPE,
                            a_value_assi->right->value_type->get_type_name().c_str(),
                            a_value_assi->left->value_type->get_type_name().c_str());
                    }
                }
                else if (ast_value_type_cast* a_value_typecast = dynamic_cast<ast_value_type_cast*>(a_value))
                {
                    // check: cast is valid?
                    ast_value* origin_value = a_value_typecast->_be_cast_value_node;
                    analyze_pass2(a_value_typecast->value_type);
                    analyze_pass2(origin_value);

                    if (auto* a_variable_sym = dynamic_cast<ast_value_variable*>(origin_value);
                        a_variable_sym && a_variable_sym->value_type->is_pending_function())
                    {
                        // this function is in adjust..

                        ast_value* final_adjust_func_overload = nullptr;
                        if (a_value_typecast->value_type->is_func())
                        {
                            auto& func_symbol = a_variable_sym->symbol->function_overload_sets;
                            if (func_symbol.size())
                            {
                                for (auto func_overload : func_symbol)
                                {
                                    auto* overload_func = dynamic_cast<ast_value_function_define*>(func_overload);
                                    if (overload_func->value_type->is_same(a_value_typecast->value_type))
                                    {
                                        a_value_typecast->_be_cast_value_node = overload_func;
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                // symbol is not a function-symbol, can not do adjust, goto simple-cast;
                                goto just_do_simple_type_cast;
                            }
                        }
                        if (a_value_typecast->_be_cast_value_node->value_type->is_pending())
                        {
                            lang_anylizer->lang_error(0x0000, a_value, RS_ERR_CANNOT_GET_FUNC_OVERRIDE_WITH_TYPE,
                                a_variable_sym->var_name.c_str(),
                                a_value_typecast->value_type->get_type_name().c_str());
                        }
                    }
                    else
                    {
                    just_do_simple_type_cast:
                        if (!ast_type::check_castable(a_value_typecast->value_type, origin_value->value_type, !a_value_typecast->implicit))
                        {
                            if (a_value_typecast->implicit)
                                lang_anylizer->lang_error(0x0000, a_value, RS_ERR_CANNOT_IMPLCAST_TYPE_TO_TYPE,
                                    origin_value->value_type->get_type_name().c_str(),
                                    a_value_typecast->value_type->get_type_name().c_str()
                                );
                            else
                                lang_anylizer->lang_error(0x0000, a_value, RS_ERR_CANNOT_CAST_TYPE_TO_TYPE,
                                    origin_value->value_type->get_type_name().c_str(),
                                    a_value_typecast->value_type->get_type_name().c_str()
                                );
                        }
                    }
                }
                else if (ast_value_type_judge* ast_value_judge = dynamic_cast<ast_value_type_judge*>(ast_node))
                {
                    if (ast_value_judge->is_mark_as_using_ref)
                        ast_value_judge->_be_cast_value_node->is_mark_as_using_ref = true;

                    analyze_pass2(ast_value_judge->_be_cast_value_node);

                    if (ast_value_judge->_be_cast_value_node->value_type->is_pending()
                        || ast_value_judge->_be_cast_value_node->value_type->is_dynamic())
                    {
                        if (ast_value_judge->value_type->is_func())
                            lang_anylizer->lang_error(0x0000, ast_value_judge, RS_ERR_CANNOT_AS_COMPLEX_TYPE);
                    }
                    else if (!ast_value_judge->value_type->is_same(ast_value_judge->_be_cast_value_node->value_type))
                    {
                        lang_anylizer->lang_error(0x0000, ast_value_judge, RS_ERR_CANNOT_AS_TYPE,
                            ast_value_judge->_be_cast_value_node->value_type->get_type_name().c_str(),
                            ast_value_judge->value_type->get_type_name().c_str());
                    }
                    if (ast_value_judge->value_type->is_dynamic())
                    {
                        lang_anylizer->lang_error(0x0000, ast_value_judge, RS_ERR_CANNOT_AS_DYNAMIC);
                    }
                }
                else if (ast_value_array* a_value_arr = dynamic_cast<ast_value_array*>(a_value))
                {
                    analyze_pass2(a_value_arr->array_items);
                }
                else if (ast_value_mapping* a_value_map = dynamic_cast<ast_value_mapping*>(ast_node))
                {
                    analyze_pass2(a_value_map->mapping_pairs);
                }
                else if (ast_value_index* a_value_index = dynamic_cast<ast_value_index*>(ast_node))
                {
                    analyze_pass2(a_value_index->from);
                    analyze_pass2(a_value_index->index);

                    if (!a_value_index->from->value_type->is_array()
                        && !a_value_index->from->value_type->is_map()
                        && !a_value_index->from->value_type->is_string()
                        && !a_value_index->from->value_type->is_dynamic())
                    {
                        lang_anylizer->lang_error(0x0000, a_value_index->from, RS_ERR_UNINDEXABLE_TYPE
                            , a_value_index->from->value_type->get_type_name().c_str());
                    }
                }
                else if (ast_value_indexed_variadic_args* a_value_variadic_args_idx = dynamic_cast<ast_value_indexed_variadic_args*>(ast_node))
                {
                    analyze_pass2(a_value_variadic_args_idx->argindex);

                    if (!a_value_variadic_args_idx->argindex->value_type->is_integer())
                    {
                        auto* cast_return_type = pass_type_cast::do_cast(*lang_anylizer, a_value_variadic_args_idx->argindex, new ast_type(L"int"));
                        cast_return_type->col_no = a_value_variadic_args_idx->col_no;
                        cast_return_type->row_no = a_value_variadic_args_idx->row_no;
                        cast_return_type->source_file = a_value_variadic_args_idx->source_file;

                        analyze_pass2(cast_return_type);
                        a_value_variadic_args_idx->argindex = cast_return_type;
                    }
                }
                else if (ast_fakevalue_unpacked_args* a_fakevalue_unpacked_args = dynamic_cast<ast_fakevalue_unpacked_args*>(ast_node))
                {
                    analyze_pass2(a_fakevalue_unpacked_args->unpacked_pack);
                    if (!a_fakevalue_unpacked_args->unpacked_pack->value_type->is_array() && !a_fakevalue_unpacked_args->unpacked_pack->value_type->is_dynamic())
                    {
                        lang_anylizer->lang_error(0x0000, a_fakevalue_unpacked_args, RS_ERR_NEED_TYPES, L"array");
                    }
                }
            }

            if (ast_value_logical_binary* a_value_logic_bin = dynamic_cast<ast_value_logical_binary*>(ast_node))
            {
                analyze_pass2(a_value_logic_bin->left);
                analyze_pass2(a_value_logic_bin->right);

                a_value_logic_bin->add_child(a_value_logic_bin->left);
                a_value_logic_bin->add_child(a_value_logic_bin->right);
            }
            else if (ast_mapping_pair* a_mapping_pair = dynamic_cast<ast_mapping_pair*>(ast_node))
            {
                analyze_pass2(a_mapping_pair->key);
                analyze_pass2(a_mapping_pair->val);
            }
            else if (ast_return* a_ret = dynamic_cast<ast_return*>(ast_node))
            {
                if (a_ret->return_value)
                {
                    analyze_pass2(a_ret->return_value);

                    if (a_ret->return_value->value_type->is_pending())
                    {
                        // error will report in analyze_pass2(a_ret->return_value), so here do nothing.. 
                    }
                    else
                    {
                        auto* func_return_type = a_ret->located_function->value_type->get_return_type();

                        if (func_return_type->is_pending())
                        {
                            a_ret->located_function->value_type->set_ret_type(a_ret->return_value->value_type);
                        }
                        else if (a_ret->located_function->auto_adjust_return_type)
                        {
                            if (!func_return_type->is_same(a_ret->return_value->value_type))
                            {
                                auto* mixed_type = pass_binary_op::binary_upper_type(func_return_type, a_ret->return_value->value_type);
                                if (mixed_type)
                                {
                                    a_ret->located_function->value_type->set_type_with_name(mixed_type->type_name);
                                }
                                else
                                {
                                    a_ret->located_function->value_type->set_type_with_name(L"dynamic");
                                    lang_anylizer->lang_warning(0x0000, a_ret, RS_WARN_FUNC_WILL_RETURN_DYNAMIC);
                                }
                            }
                        }
                        else
                        {
                            if (!func_return_type->is_same(a_ret->return_value->value_type))
                            {
                                auto* cast_return_type = pass_type_cast::do_cast(*lang_anylizer, a_ret->return_value, func_return_type);
                                cast_return_type->col_no = a_ret->col_no;
                                cast_return_type->row_no = a_ret->row_no;
                                cast_return_type->source_file = a_ret->source_file;

                                analyze_pass2(cast_return_type);

                                a_ret->return_value = cast_return_type;
                            }
                        }
                    }
                }
                else
                {
                    if (a_ret->located_function->auto_adjust_return_type)
                    {
                        if (a_ret->located_function->value_type->is_pending())
                        {
                            a_ret->located_function->value_type->set_type_with_name(L"void");
                            a_ret->located_function->auto_adjust_return_type = false;
                        }
                        else
                        {
                            lang_anylizer->lang_error(0x0000, a_ret, RS_ERR_CANNOT_RET_TYPE_AND_TYPE_AT_SAME_TIME, L"void", a_ret->located_function->value_type->type_name.c_str());
                        }
                    }
                    else
                    {
                        if (!a_ret->located_function->value_type->is_void())
                            lang_anylizer->lang_error(0x0000, a_ret, RS_ERR_CANNOT_RET_TYPE_AND_TYPE_AT_SAME_TIME, L"void", a_ret->located_function->value_type->type_name.c_str());
                    }
                }

            }
            else if (ast_sentence_block* a_sentence_blk = dynamic_cast<ast_sentence_block*>(ast_node))
            {
                analyze_pass2(a_sentence_blk->sentence_list);
            }
            else if (ast_if* ast_if_sentence = dynamic_cast<ast_if*>(ast_node))
            {
                analyze_pass2(ast_if_sentence->judgement_value);
                analyze_pass2(ast_if_sentence->execute_if_true);
                if (ast_if_sentence->execute_else)
                    analyze_pass2(ast_if_sentence->execute_else);
            }
            else if (ast_while* ast_while_sentence = dynamic_cast<ast_while*>(ast_node))
            {
                analyze_pass2(ast_while_sentence->judgement_value);
                analyze_pass2(ast_while_sentence->execute_sentence);
            }
            else if (ast_varref_defines* a_varref_defs = dynamic_cast<ast_varref_defines*>(ast_node))
            {
                for (auto& varref : a_varref_defs->var_refs)
                {
                    varref.symbol->has_been_defined_in_pass2 = true;
                }
            }

            ast_value_type_judge* a_value_type_judge_for_attrb = dynamic_cast<ast_value_type_judge*>(ast_node);
            ast_value_symbolable_base* a_value_base_for_attrb = dynamic_cast<ast_value_symbolable_base*>(ast_node);
            if (ast_value_symbolable_base* a_value_base = a_value_base_for_attrb;
                a_value_base ||
                (a_value_type_judge_for_attrb &&
                    (a_value_base = dynamic_cast<ast_value_symbolable_base*>(a_value_type_judge_for_attrb->_be_cast_value_node))
                    )
                )
            {
                if (a_value_base->is_mark_as_using_ref)
                    if (a_value_base->symbol)
                        a_value_base->symbol->has_been_assigned = true;

                // DONOT SWAP THESE TWO SENTENCES, BECAUSE has_been_assigned IS NOT 
                // DECIDED BY a_value_base->symbol->is_ref

                if (a_value_base->symbol && a_value_base->symbol->is_ref)
                    a_value_base->is_ref_ob_in_finalize = true;

                if (!a_value_base_for_attrb)
                {
                    a_value_type_judge_for_attrb->is_mark_as_using_ref = a_value_base->is_mark_as_using_ref;
                    a_value_type_judge_for_attrb->is_ref_ob_in_finalize = a_value_base->is_ref_ob_in_finalize;
                }
            }

            grammar::ast_base* child = ast_node->children;
            while (child)
            {
                analyze_pass2(child);
                child = child->sibling;
            }

            return ast_node->completed_in_pass2 = true;
        }

        void clean_and_close_lang()
        {
            for (auto* created_scopes : lang_scopes_buffers)
            {
                for (auto& [symbool_name, created_symbols] : created_scopes->symbols)
                    delete created_symbols;
                delete created_scopes;
            }
            for (auto* created_temp_opnum : generated_opnum_list_for_clean)
                delete created_temp_opnum;
            for (auto generated_ast_node : generated_ast_nodes_buffers)
                delete generated_ast_node;

            lang_scopes_buffers.clear();
            generated_opnum_list_for_clean.clear();
            generated_ast_nodes_buffers.clear();
        }

        // register mapping.... fxxk
        std::vector<bool> assigned_t_register_list = std::vector<bool>(opnum::reg::T_REGISTER_COUNT);   // will assign t register
        std::vector<bool> assigned_r_register_list = std::vector<bool>(opnum::reg::R_REGISTER_COUNT);   // will assign r register

        opnum::opnumbase& get_useable_register_for_pure_value()
        {
            using namespace ast;
            using namespace opnum;
#define RS_NEW_OPNUM(...) (*generated_opnum_list_for_clean.emplace_back(new __VA_ARGS__))
            for (size_t i = 0; i < opnum::reg::T_REGISTER_COUNT; i++)
            {
                if (!assigned_t_register_list[i])
                {
                    assigned_t_register_list[i] = true;
                    return RS_NEW_OPNUM(reg(reg::t0 + i));
                }
            }
            rs_error("cannot get a useable register..");
            return RS_NEW_OPNUM(reg(reg::cr));
        }
        void _complete_using_register_for_pure_value(opnum::opnumbase& completed_reg)
        {
            using namespace ast;
            using namespace opnum;
            if (auto* reg_ptr = dynamic_cast<opnum::reg*>(&completed_reg);
                reg_ptr && reg_ptr->id >= 0 && reg_ptr->id < opnum::reg::T_REGISTER_COUNT)
            {
                rs_test(assigned_t_register_list[reg_ptr->id]);
                assigned_t_register_list[reg_ptr->id] = false;
            }
        }
        void _complete_using_all_register_for_pure_value()
        {
            for (size_t i = 0; i < opnum::reg::T_REGISTER_COUNT; i++)
            {
                assigned_t_register_list[i] = 0;
            }
        }

        opnum::opnumbase& get_useable_register_for_ref_value()
        {
            using namespace ast;
            using namespace opnum;
#define RS_NEW_OPNUM(...) (*generated_opnum_list_for_clean.emplace_back(new __VA_ARGS__))
            for (size_t i = 0; i < opnum::reg::R_REGISTER_COUNT; i++)
            {
                if (!assigned_r_register_list[i])
                {
                    assigned_r_register_list[i] = true;
                    return RS_NEW_OPNUM(reg(reg::r0 + i));
                }
            }
            rs_error("cannot get a useable register..");
            return RS_NEW_OPNUM(reg(reg::cr));
        }
        void _complete_using_register_for_ref_value(opnum::opnumbase& completed_reg)
        {
            using namespace ast;
            using namespace opnum;
            if (auto* reg_ptr = dynamic_cast<opnum::reg*>(&completed_reg);
                reg_ptr && reg_ptr->id >= opnum::reg::T_REGISTER_COUNT
                && reg_ptr->id < opnum::reg::T_REGISTER_COUNT + opnum::reg::R_REGISTER_COUNT)
            {
                rs_test(assigned_r_register_list[reg_ptr->id - opnum::reg::T_REGISTER_COUNT]);
                assigned_r_register_list[reg_ptr->id - opnum::reg::T_REGISTER_COUNT] = false;
            }
        }
        void _complete_using_all_register_for_ref_value()
        {
            for (size_t i = 0; i < opnum::reg::R_REGISTER_COUNT; i++)
            {
                assigned_r_register_list[i] = 0;
            }
        }

        void complete_using_register(opnum::opnumbase& completed_reg)
        {
            _complete_using_register_for_pure_value(completed_reg);
            _complete_using_register_for_ref_value(completed_reg);
        }

        void complete_using_all_register()
        {
            _complete_using_all_register_for_pure_value();
            _complete_using_all_register_for_ref_value();
        }

        bool is_cr_reg(opnum::opnumbase& op_num)
        {
            using namespace opnum;
            if (auto* regist = dynamic_cast<reg*>(&op_num))
            {
                if (regist->id == reg::cr)
                    return true;
            }
            return false;
        }

        bool is_non_ref_tem_reg(opnum::opnumbase& op_num)
        {
            using namespace opnum;
            if (auto* regist = dynamic_cast<reg*>(&op_num))
            {
                if (regist->id >= reg::t0 && regist->id <= reg::t15)
                    return true;
            }
            return false;
        }

        opnum::opnumbase& mov_value_to_cr(opnum::opnumbase& op_num, ir_compiler* compiler)
        {
            using namespace ast;
            using namespace opnum;
            if (last_value_stored_to_cr)
                return op_num;

            if (auto* regist = dynamic_cast<reg*>(&op_num))
            {
                if (regist->id == reg::cr)
                    return op_num;
            }
            compiler->set(reg(reg::cr), op_num);
            return RS_NEW_OPNUM(reg(reg::cr));
        }

        std::vector<ast::ast_value_function_define* > in_used_functions;

        opnum::opnumbase& get_opnum_by_symbol(grammar::ast_base* error_prud, lang_symbol* symb, ir_compiler* compiler, bool get_pure_value = false)
        {
            using namespace opnum;

            if (symb->is_constexpr)
                return analyze_value(symb->variable_value, compiler, get_pure_value);

            if (symb->type == lang_symbol::symbol_type::variable)
            {
                if (symb->static_symbol)
                {
                    if (!get_pure_value)
                        return RS_NEW_OPNUM(global(symb->global_index_in_lang));
                    else
                    {
                        auto& loaded_pure_glb_opnum = get_useable_register_for_pure_value();
                        compiler->set(loaded_pure_glb_opnum, global(symb->global_index_in_lang));
                        return loaded_pure_glb_opnum;
                    }
                }
                else
                {
                    if (!get_pure_value)
                    {
                        if (symb->stackvalue_index_in_funcs <= 64 || symb->stackvalue_index_in_funcs >= -63)
                            return RS_NEW_OPNUM(reg(reg::bp_offset(-(int8_t)symb->stackvalue_index_in_funcs)));
                        else
                        {
                            auto& ldr_aim = get_useable_register_for_ref_value();
                            compiler->ldsr(ldr_aim, imm(-(int16_t)symb->stackvalue_index_in_funcs));
                            return ldr_aim;
                        }
                    }
                    else
                    {
                        if (symb->stackvalue_index_in_funcs <= 64 || symb->stackvalue_index_in_funcs >= -63)
                        {
                            auto& loaded_pure_glb_opnum = get_useable_register_for_pure_value();
                            compiler->set(loaded_pure_glb_opnum, reg(reg::bp_offset(-(int8_t)symb->stackvalue_index_in_funcs)));
                            return loaded_pure_glb_opnum;
                        }
                        else
                        {
                            auto& lds_aim = get_useable_register_for_ref_value();
                            compiler->lds(lds_aim, imm(-(int16_t)symb->stackvalue_index_in_funcs));
                            return lds_aim;
                        }
                    }
                }
            }
            else
            {
                if (symb->function_overload_sets.size() == 1)
                    return analyze_value(symb->function_overload_sets.front(), compiler, get_pure_value);
                else
                {
                    lang_anylizer->lang_error(0x0000, error_prud, RS_ERR_UNABLE_DECIDE_FUNC_SYMBOL);
                    return RS_NEW_OPNUM(imm(0));
                }
            }
        }

        bool last_value_stored_to_cr = false;

        opnum::opnumbase& analyze_value(ast::ast_value* value, ir_compiler* compiler, bool get_pure_value = false)
        {
            compiler->pdb_info->generate_debug_info_at_astnode(value, compiler);

            last_value_stored_to_cr = false;
            using namespace ast;
            using namespace opnum;
            if (value->is_constant)
            {
                auto const_value = value->get_constant_value();
                switch (const_value.type)
                {
                case value::valuetype::integer_type:
                    if (!get_pure_value)
                        return RS_NEW_OPNUM(imm(const_value.integer));
                    else
                    {
                        auto& treg = get_useable_register_for_pure_value();
                        compiler->set(treg, imm(const_value.integer));
                        return treg;
                    }
                case value::valuetype::real_type:
                    if (!get_pure_value)
                        return RS_NEW_OPNUM(imm(const_value.real));
                    else
                    {
                        auto& treg = get_useable_register_for_pure_value();
                        compiler->set(treg, imm(const_value.real));
                        return treg;
                    }
                case value::valuetype::handle_type:
                    if (!get_pure_value)
                        return RS_NEW_OPNUM(imm((void*)const_value.handle));
                    else
                    {
                        auto& treg = get_useable_register_for_pure_value();
                        compiler->set(treg, imm((void*)const_value.handle));
                        return treg;
                    }
                case value::valuetype::string_type:
                    if (!get_pure_value)
                        return RS_NEW_OPNUM(imm(const_value.string->c_str()));
                    else
                    {
                        auto& treg = get_useable_register_for_pure_value();
                        compiler->set(treg, imm(const_value.string->c_str()));
                        return treg;
                    }
                case value::valuetype::invalid:  // for nil
                    if (!get_pure_value)
                        return RS_NEW_OPNUM(reg(reg::ni));
                    else
                    {
                        auto& treg = get_useable_register_for_pure_value();
                        compiler->set(treg, reg(reg::ni));
                        return treg;
                    }
                default:
                    rs_error("error constant type..");
                    break;
                }
            }
            else if (auto* a_value_literal = dynamic_cast<ast_value_literal*>(value))
            {
                rs_error("ast_value_literal should be 'constant'..");
            }
            else if (auto* a_value_binary = dynamic_cast<ast_value_binary*>(value))
            {
                // if mixed type, do opx
                value::valuetype optype = value::valuetype::invalid;
                if (a_value_binary->left->value_type->is_same(a_value_binary->right->value_type)
                    && !a_value_binary->left->value_type->is_dynamic())
                    optype = a_value_binary->left->value_type->value_type;

                auto& beoped_left_opnum = analyze_value(a_value_binary->left, compiler, true);
                auto& op_right_opnum = analyze_value(a_value_binary->right, compiler);

                switch (a_value_binary->operate)
                {
                case lex_type::l_add:
                    if (optype == value::valuetype::invalid)
                        compiler->addx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->addi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->addr(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::handle_type:
                            compiler->addh(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::string_type:
                            compiler->adds(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_sub:
                    if (optype == value::valuetype::invalid)
                        compiler->subx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->subi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->subr(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::handle_type:
                            compiler->subh(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_mul:
                    if (optype == value::valuetype::invalid)
                        compiler->mulx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->muli(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->mulr(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_div:
                    if (optype == value::valuetype::invalid)
                        compiler->divx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->divi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->divr(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_mod:
                    if (optype == value::valuetype::invalid)
                        compiler->modx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->modi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->modr(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                default:
                    rs_error("Do not support this operator..");
                    break;
                }
                complete_using_register(op_right_opnum);
                last_value_stored_to_cr = true;
                return beoped_left_opnum;
            }
            else if (auto* a_value_assign = dynamic_cast<ast_value_assign*>(value))
            {
                // if mixed type, do opx
                bool same_type = a_value_assign->left->value_type->is_same(a_value_assign->right->value_type);
                value::valuetype optype = value::valuetype::invalid;
                if (same_type && !a_value_assign->left->value_type->is_dynamic())
                    optype = a_value_assign->left->value_type->value_type;

                if (auto symb_left = dynamic_cast<ast_value_symbolable_base*>(a_value_assign->left);
                    symb_left && symb_left->symbol->attribute->is_constant_attr())
                {
                    lang_anylizer->lang_error(0x0000, value, RS_ERR_CANNOT_ASSIGN_TO_CONSTANT);
                }

                auto& beoped_left_opnum = analyze_value(a_value_assign->left, compiler);
                auto& op_right_opnum = analyze_value(a_value_assign->right, compiler);

                switch (a_value_assign->operate)
                {
                case lex_type::l_assign:
                    if (!a_value_assign->left->value_type->is_func()
                        && optype == value::valuetype::invalid
                        && !a_value_assign->left->value_type->is_dynamic())
                        // TODO : NEED WARNING..
                        compiler->movx(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->mov(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_add_assign:
                    if (optype == value::valuetype::invalid)
                        // TODO : NEED WARNING..
                        compiler->addx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->addi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->addr(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::handle_type:
                            compiler->addh(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::string_type:
                            compiler->adds(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::array_type:
                            compiler->addx(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::mapping_type:
                            compiler->addx(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_sub_assign:
                    if (optype == value::valuetype::invalid)
                        // TODO : NEED WARNING..
                        compiler->subx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->subi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->subr(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::handle_type:
                            compiler->subh(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_mul_assign:
                    if (optype == value::valuetype::invalid)
                        // TODO : NEED WARNING..
                        compiler->mulx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->muli(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->mulr(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_div_assign:
                    if (optype == value::valuetype::invalid)
                        // TODO : NEED WARNING..
                        compiler->divx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->divi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->divr(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                case lex_type::l_mod_assign:
                    if (optype == value::valuetype::invalid)
                        // TODO : NEED WARNING..
                        compiler->modx(beoped_left_opnum, op_right_opnum);
                    else
                    {
                        switch (optype)
                        {
                        case rs::value::valuetype::integer_type:
                            compiler->modi(beoped_left_opnum, op_right_opnum); break;
                        case rs::value::valuetype::real_type:
                            compiler->modr(beoped_left_opnum, op_right_opnum); break;
                        default:
                            rs_error("Do not support this type..");
                            break;
                        }
                    }
                    break;
                default:
                    rs_error("Do not support this operator..");
                    break;
                }
                complete_using_register(op_right_opnum);
                last_value_stored_to_cr = true;
                return beoped_left_opnum;
            }
            else if (auto* a_value_variable = dynamic_cast<ast_value_variable*>(value))
            {
                // ATTENTION: HERE JUST VALUE , NOT JUDGE FUNCTION
                auto symb = a_value_variable->symbol;
                return get_opnum_by_symbol(a_value_variable, symb, compiler, get_pure_value);
            }
            else if (auto* a_value_type_cast = dynamic_cast<ast_value_type_cast*>(value))
            {
                if (a_value_type_cast->value_type->is_dynamic()
                    || a_value_type_cast->value_type->is_same(a_value_type_cast->_be_cast_value_node->value_type)
                    || a_value_type_cast->value_type->is_func())
                    // no cast, just as origin value
                    return analyze_value(a_value_type_cast->_be_cast_value_node, compiler, get_pure_value);

                auto& treg = get_useable_register_for_pure_value();

                compiler->setcast(treg,
                    analyze_value(a_value_type_cast->_be_cast_value_node, compiler),
                    a_value_type_cast->value_type->value_type);
                last_value_stored_to_cr = true;
                return treg;

            }
            else if (ast_value_type_judge* a_value_type_judge = dynamic_cast<ast_value_type_judge*>(value))
            {
                auto& result = analyze_value(a_value_type_judge->_be_cast_value_node, compiler);
                if (!a_value_type_judge->value_type->is_same(a_value_type_judge->_be_cast_value_node->value_type))
                {
                    rs_test(a_value_type_judge->value_type->value_type != value::valuetype::invalid);
                    compiler->typeas(result, a_value_type_judge->value_type->value_type);
                }
                return result;
            }
            else if (auto* a_value_function_define = dynamic_cast<ast_value_function_define*>(value))
            {
                // function defination
                if (a_value_function_define->ir_func_has_been_generated == false)
                {
                    in_used_functions.push_back(a_value_function_define);
                    a_value_function_define->ir_func_has_been_generated = true;
                }
                return RS_NEW_OPNUM(opnum::tagimm_rsfunc(a_value_function_define->get_ir_func_signature_tag()));
            }
            else if (auto* a_value_funccall = dynamic_cast<ast_value_funccall*>(value))
            {
                if (now_function_in_final_anylize && now_function_in_final_anylize->value_type->is_variadic_function_type)
                    compiler->psh(reg(reg::tc));


                std::vector<ast_value* >arg_list;
                auto arg = a_value_funccall->arguments->children;

                bool full_unpack_arguments = false;
                rs_integer_t extern_unpack_arg_count = 0;

                while (arg)
                {
                    ast_value* arg_val = dynamic_cast<ast_value*>(arg);
                    rs_assert(arg_val);

                    if (full_unpack_arguments)
                    {
                        lang_anylizer->lang_error(0x0000, arg_val, RS_ERR_ARG_DEFINE_AFTER_VARIADIC);
                        break;
                    }

                    if (auto* a_fakevalue_unpacked_args = dynamic_cast<ast_fakevalue_unpacked_args*>(arg_val))
                    {
                        if (a_fakevalue_unpacked_args->expand_count <= 0)
                            full_unpack_arguments = true;
                    }

                    arg_list.insert(arg_list.begin(), arg_val);
                    arg = arg->sibling;
                }
                for (auto* argv : arg_list)
                {
                    if (auto* a_fakevalue_unpacked_args = dynamic_cast<ast_fakevalue_unpacked_args*>(argv))
                    {
                        auto& packing = analyze_value(a_fakevalue_unpacked_args->unpacked_pack, compiler,
                            a_fakevalue_unpacked_args->expand_count <= 0);

                        if (a_fakevalue_unpacked_args->expand_count <= 0)
                            compiler->set(reg(reg::tc), imm(arg_list.size() + extern_unpack_arg_count - 1));
                        else
                            extern_unpack_arg_count += a_fakevalue_unpacked_args->expand_count - 1;

                        compiler->ext_unpackargs(packing,
                            a_fakevalue_unpacked_args->expand_count);
                    }
                    else
                    {
                        if (argv->is_mark_as_using_ref)
                            compiler->pshr(analyze_value(argv, compiler));
                        else
                            compiler->psh(analyze_value(argv, compiler));
                    }
                }

                auto* called_func_aim = &analyze_value(a_value_funccall->called_func, compiler);
                if (is_cr_reg(*called_func_aim))
                {
                    auto& callaimreg = get_useable_register_for_pure_value();
                    compiler->set(callaimreg, *called_func_aim);
                    called_func_aim = &callaimreg;
                    complete_using_register(callaimreg);
                }

                if (!full_unpack_arguments && (!dynamic_cast<opnum::immbase*>(called_func_aim)
                    || a_value_funccall->called_func->value_type->is_variadic_function_type))
                    compiler->set(reg(reg::tc), imm(arg_list.size() + extern_unpack_arg_count));

                compiler->call(*called_func_aim);

                compiler->pop(arg_list.size());
                if (now_function_in_final_anylize && now_function_in_final_anylize->value_type->is_variadic_function_type)
                    compiler->pop(reg(reg::tc));

                last_value_stored_to_cr = true;
                if (!get_pure_value)
                {
                    return RS_NEW_OPNUM(reg(reg::cr));
                }
                else
                {
                    auto& funcresult = get_useable_register_for_pure_value();
                    compiler->set(funcresult, reg(reg::cr));
                    return funcresult;
                }
            }
            else if (auto* a_value_logical_binary = dynamic_cast<ast_value_logical_binary*>(value))
            {
                value::valuetype optype = value::valuetype::invalid;
                if (a_value_logical_binary->left->value_type->is_same(a_value_logical_binary->right->value_type)
                    && !a_value_logical_binary->left->value_type->is_dynamic())
                    optype = a_value_logical_binary->left->value_type->value_type;
                bool left_in_cr = false;
                auto* _beoped_left_opnum = &analyze_value(a_value_logical_binary->left, compiler);
                if (is_cr_reg(*_beoped_left_opnum))
                {
                    auto* _tmp_beoped_left_opnum = &get_useable_register_for_pure_value();
                    compiler->set(*_tmp_beoped_left_opnum, *_beoped_left_opnum);
                    _beoped_left_opnum = _tmp_beoped_left_opnum;
                    left_in_cr = true;
                }
                auto& beoped_left_opnum = *_beoped_left_opnum;
                auto& op_right_opnum = analyze_value(a_value_logical_binary->right, compiler);

                switch (a_value_logical_binary->operate)
                {
                case lex_type::l_equal:
                    if (a_value_logical_binary->left->value_type->is_nil()
                        || a_value_logical_binary->right->value_type->is_nil()
                        || a_value_logical_binary->left->value_type->is_func()
                        || a_value_logical_binary->right->value_type->is_func()
                        || a_value_logical_binary->left->value_type->is_gc_type()
                        || a_value_logical_binary->right->value_type->is_gc_type()
                        || optype == value::valuetype::integer_type
                        || optype == value::valuetype::handle_type)
                        compiler->equb(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->equx(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_not_equal:
                    if (a_value_logical_binary->left->value_type->is_nil()
                        || a_value_logical_binary->right->value_type->is_nil()
                        || a_value_logical_binary->left->value_type->is_func()
                        || a_value_logical_binary->right->value_type->is_func()
                        || a_value_logical_binary->left->value_type->is_gc_type()
                        || a_value_logical_binary->right->value_type->is_gc_type()
                        || optype == value::valuetype::integer_type
                        || optype == value::valuetype::handle_type)
                        compiler->nequb(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->nequx(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_less:
                    if (optype == value::valuetype::integer_type)
                        compiler->lti(beoped_left_opnum, op_right_opnum);
                    else if (optype == value::valuetype::real_type)
                        compiler->ltr(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->ltx(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_less_or_equal:
                    if (optype == value::valuetype::integer_type)
                        compiler->elti(beoped_left_opnum, op_right_opnum);
                    else if (optype == value::valuetype::real_type)
                        compiler->eltr(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->eltx(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_larg:
                    if (optype == value::valuetype::integer_type)
                        compiler->gti(beoped_left_opnum, op_right_opnum);
                    else if (optype == value::valuetype::real_type)
                        compiler->gtr(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->gtx(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_larg_or_equal:
                    if (optype == value::valuetype::integer_type)
                        compiler->egti(beoped_left_opnum, op_right_opnum);
                    else if (optype == value::valuetype::real_type)
                        compiler->egtr(beoped_left_opnum, op_right_opnum);
                    else
                        compiler->egtx(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_land:
                    compiler->land(beoped_left_opnum, op_right_opnum);
                    break;
                case lex_type::l_lor:
                    compiler->lor(beoped_left_opnum, op_right_opnum);
                    break;
                default:
                    rs_error("Do not support this operator..");
                    break;
                }

                complete_using_register(op_right_opnum);
                last_value_stored_to_cr = true;

                if (!get_pure_value)
                    return RS_NEW_OPNUM(reg(reg::cr));
                else
                {
                    if (left_in_cr)
                    {
                        compiler->set(beoped_left_opnum, reg(reg::cr));
                        return beoped_left_opnum;
                    }
                    else
                    {
                        auto& result = get_useable_register_for_pure_value();
                        compiler->set(result, reg(reg::cr));
                        return result;
                    }
                }
            }
            else if (auto* a_value_array = dynamic_cast<ast_value_array*>(value))
            {
                auto* _arr_item = a_value_array->array_items->children;
                std::vector<ast_value*> arr_list;
                while (_arr_item)
                {
                    auto* arr_val = dynamic_cast<ast_value*>(_arr_item);
                    rs_test(arr_val);

                    arr_list.insert(arr_list.begin(), arr_val);

                    _arr_item = _arr_item->sibling;
                }

                for (auto* in_arr_val : arr_list)
                {
                    if (in_arr_val->is_mark_as_using_ref)
                        compiler->pshr(analyze_value(in_arr_val, compiler));
                    else
                        compiler->psh(analyze_value(in_arr_val, compiler));
                }

                auto& treg = get_useable_register_for_pure_value();
                compiler->mkarr(treg, imm(arr_list.size()));
                return treg;

            }
            else if (auto* a_value_mapping = dynamic_cast<ast_value_mapping*>(value))
            {
                auto* _map_item = a_value_mapping->mapping_pairs->children;
                size_t map_pair_count = 0;
                while (_map_item)
                {
                    auto* _map_pair = dynamic_cast<ast_mapping_pair*>(_map_item);
                    rs_test(_map_pair);

                    compiler->psh(analyze_value(_map_pair->key, compiler));

                    if (_map_pair->val->is_mark_as_using_ref)
                        compiler->pshr(analyze_value(_map_pair->val, compiler));
                    else
                        compiler->psh(analyze_value(_map_pair->val, compiler));

                    _map_item = _map_item->sibling;
                    map_pair_count++;
                }

                auto& treg = get_useable_register_for_pure_value();
                compiler->mkmap(treg, imm(map_pair_count));
                return treg;

            }
            else if (auto* a_value_index = dynamic_cast<ast_value_index*>(value))
            {
                bool left_in_cr = false;
                auto* _beoped_left_opnum = &analyze_value(a_value_index->from, compiler);

                // TODO: IF a_value_index->index IS A SINGLE AST, DO NOT MOVE LEFT TO CR
                if (is_cr_reg(*_beoped_left_opnum) && !a_value_index->index->is_constant)
                {
                    auto* _tmp_beoped_left_opnum = &get_useable_register_for_pure_value();
                    compiler->set(*_tmp_beoped_left_opnum, *_beoped_left_opnum);
                    _beoped_left_opnum = _tmp_beoped_left_opnum;
                    left_in_cr = true;
                }
                auto& beoped_left_opnum = *_beoped_left_opnum;
                auto& op_right_opnum = analyze_value(a_value_index->index, compiler);

                last_value_stored_to_cr = true;

                compiler->idx(beoped_left_opnum, op_right_opnum);

                if (!get_pure_value)
                    return RS_NEW_OPNUM(reg(reg::cr));
                else
                {
                    if (left_in_cr)
                    {
                        compiler->set(beoped_left_opnum, reg(reg::cr));
                        return beoped_left_opnum;
                    }
                    else
                    {
                        auto& result = get_useable_register_for_pure_value();
                        compiler->set(result, reg(reg::cr));
                        return result;
                    }
                }
            }
            else if (auto* a_value_packed_variadic_args = dynamic_cast<ast_value_packed_variadic_args*>(value))
            {
                if (!now_function_in_final_anylize
                    || !now_function_in_final_anylize->value_type->is_variadic_function_type)
                {
                    lang_anylizer->lang_error(0x0000, a_value_packed_variadic_args, RS_ERR_USING_VARIADIC_IN_NON_VRIDIC_FUNC);
                    return RS_NEW_OPNUM(reg(reg::cr));
                }
                else
                {
                    auto& packed = get_useable_register_for_pure_value();

                    compiler->ext_packargs(packed, imm(
                        now_function_in_final_anylize->value_type->argument_types.size()
                    ));
                    return packed;
                }
            }
            else if (auto* a_value_indexed_variadic_args = dynamic_cast<ast_value_indexed_variadic_args*>(value))
            {
                if (!now_function_in_final_anylize
                    || !now_function_in_final_anylize->value_type->is_variadic_function_type)
                {
                    lang_anylizer->lang_error(0x0000, a_value_packed_variadic_args, RS_ERR_USING_VARIADIC_IN_NON_VRIDIC_FUNC);
                    return RS_NEW_OPNUM(reg(reg::cr));
                }

                if (!get_pure_value)
                {
                    if (a_value_indexed_variadic_args->argindex->is_constant)
                    {
                        auto _cv = a_value_indexed_variadic_args->argindex->get_constant_value();
                        if (_cv.integer <= 63 - 2)
                            return RS_NEW_OPNUM(reg(reg::bp_offset(_cv.integer + 2
                                + now_function_in_final_anylize->value_type->argument_types.size())));
                        else
                        {
                            auto& result = get_useable_register_for_ref_value();
                            compiler->ldsr(result, imm(_cv.integer + 2
                                + now_function_in_final_anylize->value_type->argument_types.size()));
                            return result;
                        }
                    }
                    else
                    {
                        auto& index = analyze_value(a_value_indexed_variadic_args->argindex, compiler, true);
                        compiler->addi(index, imm(2
                            + now_function_in_final_anylize->value_type->argument_types.size()));
                        complete_using_register(index);
                        auto& result = get_useable_register_for_ref_value();
                        compiler->ldsr(result, index);
                        return result;
                    }
                }
                else
                {
                    auto& result = get_useable_register_for_pure_value();

                    if (a_value_indexed_variadic_args->argindex->is_constant)
                    {
                        auto _cv = a_value_indexed_variadic_args->argindex->get_constant_value();
                        if (_cv.integer <= 63 - 2)
                        {
                            last_value_stored_to_cr = true;
                            compiler->set(result, reg(reg::bp_offset(_cv.integer + 2
                                + now_function_in_final_anylize->value_type->argument_types.size())));
                        }
                        else
                        {
                            compiler->lds(result, imm(_cv.integer + 2
                                + now_function_in_final_anylize->value_type->argument_types.size()));
                        }
                    }
                    else
                    {
                        auto& index = analyze_value(a_value_indexed_variadic_args->argindex, compiler, true);
                        compiler->addi(index, imm(2
                            + now_function_in_final_anylize->value_type->argument_types.size()));
                        complete_using_register(index);
                        compiler->lds(result, index);

                    }
                    return result;
                }
            }
            else if (auto* a_fakevalue_unpacked_args = dynamic_cast<ast_fakevalue_unpacked_args*>(value))
            {
                lang_anylizer->lang_error(0x0000, a_fakevalue_unpacked_args, RS_ERR_UNPACK_ARGS_OUT_OF_FUNC_CALL);
                return RS_NEW_OPNUM(reg(reg::cr));
            }
            else if (auto* a_value_unary = dynamic_cast<ast_value_unary*>(value))
            {
                switch (a_value_unary->operate)
                {
                case lex_type::l_lnot:
                    compiler->lnot(analyze_value(a_value_unary->val, compiler));
                    break;
                case lex_type::l_sub:
                    if (a_value_unary->val->value_type->is_dynamic())
                    {
                        auto& result = analyze_value(a_value_unary->val, compiler, true);
                        compiler->set(reg(reg::cr), imm(0));
                        compiler->subx(reg(reg::cr), result);
                        complete_using_register(result);
                    }
                    else if (a_value_unary->val->value_type->is_integer())
                    {
                        auto& result = analyze_value(a_value_unary->val, compiler, true);
                        compiler->set(reg(reg::cr), imm(0));
                        compiler->subi(reg(reg::cr), result);
                        complete_using_register(result);
                    }
                    else if (a_value_unary->val->value_type->is_real())
                    {
                        auto& result = analyze_value(a_value_unary->val, compiler, true);
                        compiler->set(reg(reg::cr), imm(0));
                        compiler->subi(reg(reg::cr), result);
                        complete_using_register(result);
                    }
                    else
                        lang_anylizer->lang_error(0x0000, a_value_unary, RS_ERR_TYPE_CANNOT_NEGATIVE, a_value_unary->val->value_type->get_type_name());
                    break;
                default:
                    rs_error("Do not support this operator..");
                    break;
                }
                last_value_stored_to_cr = true;
                if (!get_pure_value)
                    return RS_NEW_OPNUM(reg(reg::cr));
                else
                {
                    auto& result = get_useable_register_for_pure_value();
                    compiler->set(result, reg(reg::cr));
                    return result;
                }
            }
            else
            {
                rs_error("unknown value type..");
            }

            rs_error("run to err place..");
            static opnum::opnumbase err;
            return err;
#undef RS_NEW_OPNUM
        }

        opnum::opnumbase& auto_analyze_value(ast::ast_value* value, ir_compiler* compiler, bool get_pure_value = false)
        {
            auto& result = analyze_value(value, compiler, get_pure_value);
            complete_using_all_register();

            return result;
        }

        void real_analyze_finalize(grammar::ast_base* ast_node, ir_compiler* compiler)
        {
            compiler->pdb_info->generate_debug_info_at_astnode(ast_node, compiler);

            if (traving_node.find(ast_node) != traving_node.end())
            {
                lang_anylizer->lang_error(0x0000, ast_node, L"Bad ast node.");
                return;
            }

            struct traving_guard
            {
                lang* _lang;
                grammar::ast_base* _tving_node;
                traving_guard(lang* _lg, grammar::ast_base* ast_ndoe)
                    :_tving_node(ast_ndoe)
                    , _lang(_lg)
                {
                    _lang->traving_node.insert(_tving_node);
                }
                ~traving_guard()
                {
                    _lang->traving_node.erase(_tving_node);
                }
            };

            traving_guard g1(this, ast_node);

            using namespace ast;
            using namespace opnum;

            if (auto* a_varref_defines = dynamic_cast<ast_varref_defines*>(ast_node))
            {
                for (auto& varref_define : a_varref_defines->var_refs)
                {
                    if (varref_define.is_ref)
                    {
                        auto& ref_ob = get_opnum_by_symbol(a_varref_defines, varref_define.symbol, compiler);
                        auto& aim_ob = auto_analyze_value(varref_define.init_val, compiler);

                        if (is_non_ref_tem_reg(aim_ob))
                            lang_anylizer->lang_error(0x0000, varref_define.init_val, RS_ERR_NOT_REFABLE_INIT_ITEM);

                        compiler->ext_setref(ref_ob, aim_ob);
                    }
                    else
                    {
                        if (!varref_define.symbol->is_constexpr)
                            compiler->mov(get_opnum_by_symbol(a_varref_defines, varref_define.symbol, compiler),
                                auto_analyze_value(varref_define.init_val, compiler));
                    }
                }
            }
            else if (auto* a_list = dynamic_cast<ast_list*>(ast_node))
            {
                auto* child = a_list->children;
                while (child)
                {
                    real_analyze_finalize(child, compiler);
                    child = child->sibling;
                }
            }
            else if (auto* a_if = dynamic_cast<ast_if*>(ast_node))
            {
                mov_value_to_cr(auto_analyze_value(a_if->judgement_value, compiler), compiler);

                auto ifelse_tag = "if_else_" + compiler->get_unique_tag_based_command_ip();
                auto ifend_tag = "if_end_" + compiler->get_unique_tag_based_command_ip();

                compiler->jf(tag(ifelse_tag));

                real_analyze_finalize(a_if->execute_if_true, compiler);
                if (a_if->execute_else)
                    compiler->jmp(tag(ifend_tag));
                compiler->tag(ifelse_tag);
                if (a_if->execute_else)
                {
                    real_analyze_finalize(a_if->execute_else, compiler);
                    compiler->tag(ifend_tag);
                }

            }
            else if (auto* a_while = dynamic_cast<ast_while*>(ast_node))
            {
                auto while_begin_tag = "while_begin_" + compiler->get_unique_tag_based_command_ip();
                auto while_end_tag = "while_end_" + compiler->get_unique_tag_based_command_ip();

                compiler->tag(while_begin_tag);
                mov_value_to_cr(auto_analyze_value(a_while->judgement_value, compiler), compiler);
                compiler->jf(tag(while_end_tag));

                real_analyze_finalize(a_while->execute_sentence, compiler);

                compiler->jmp(tag(while_begin_tag));
                compiler->tag(while_end_tag);
            }
            else if (auto* a_value = dynamic_cast<ast_value*>(ast_node))
            {
                auto_analyze_value(a_value, compiler);
            }
            else if (auto* a_sentence_block = dynamic_cast<ast_sentence_block*>(ast_node))
            {
                real_analyze_finalize(a_sentence_block->sentence_list, compiler);
            }
            else if (auto* a_return = dynamic_cast<ast_return*>(ast_node))
            {
                if (a_return->return_value)
                {
                    mov_value_to_cr(auto_analyze_value(a_return->return_value, compiler), compiler);
                }
                compiler->jmp(tag(a_return->located_function->get_ir_func_signature_tag() + "_do_ret"));
            }
            else if (auto* a_namespace = dynamic_cast<ast_namespace*>(ast_node))
            {
                real_analyze_finalize(a_namespace->in_scope_sentence, compiler);
            }
            else if (ast_using_namespace* a_using_namespace = dynamic_cast<ast_using_namespace*>(ast_node))
            {
                // do nothing
            }
            else if (ast_using_type_as* a_using_type_as = dynamic_cast<ast_using_type_as*>(ast_node))
            {
                // do nothing
            }
            else
                lang_anylizer->lang_error(0x0000, ast_node, L"Bad ast node.");
        }

        void analyze_finalize(grammar::ast_base* ast_node, ir_compiler* compiler)
        {
            size_t public_block_begin = compiler->get_now_ip();
            auto res_ip = compiler->reserved_stackvalue();                      // reserved..
            real_analyze_finalize(ast_node, compiler);
            auto used_tmp_regs = compiler->update_all_temp_regist_to_stack(public_block_begin);
            compiler->reserved_stackvalue(res_ip, used_tmp_regs); // set reserved size

            compiler->jmp(opnum::tag("__rsir_rtcode_seg_function_define_end"));
            while (!in_used_functions.empty())
            {
                auto tmp_build_func_list = in_used_functions;
                in_used_functions.clear();
                for (auto* funcdef : tmp_build_func_list)
                {
                    size_t funcbegin_ip = compiler->get_now_ip();
                    now_function_in_final_anylize = funcdef;

                    compiler->tag(funcdef->get_ir_func_signature_tag());
                    compiler->pdb_info->generate_func_begin(funcdef, compiler);
                    auto res_ip = compiler->reserved_stackvalue();                      // reserved..

                    // apply args.
                    int arg_count = 0;
                    auto arg_index = funcdef->argument_list->children;
                    while (arg_index)
                    {
                        if (auto* a_value_arg_define = dynamic_cast<ast::ast_value_arg_define*>(arg_index))
                        {
                            if (a_value_arg_define->is_ref || !a_value_arg_define->symbol->has_been_assigned)
                            {
                                funcdef->this_func_scope->
                                    reduce_function_used_stack_size_at(a_value_arg_define->symbol->stackvalue_index_in_funcs);

                                rs_assert(0 == a_value_arg_define->symbol->stackvalue_index_in_funcs);
                                a_value_arg_define->symbol->stackvalue_index_in_funcs = -2 - arg_count;

                            }
                            else
                                compiler->set(get_opnum_by_symbol(a_value_arg_define, a_value_arg_define->symbol, compiler),
                                    opnum::reg(opnum::reg::bp_offset(+2 + arg_count)));
                        }
                        else//variadic
                            break;
                        arg_count++;
                        arg_index = arg_index->sibling;
                    }

                    real_analyze_finalize(funcdef->in_function_sentence, compiler);

                    auto reserved_stack_size =
                        funcdef->this_func_scope->max_used_stack_size_in_func
                        + compiler->update_all_temp_regist_to_stack(funcbegin_ip);

                    compiler->reserved_stackvalue(res_ip, reserved_stack_size); // set reserved size

                    compiler->set(opnum::reg(opnum::reg::cr), opnum::reg(opnum::reg::ni));

                    compiler->tag(funcdef->get_ir_func_signature_tag() + "_do_ret");
                    // compiler->pop(reserved_stack_size);
                    compiler->ret();                                            // do return
                    compiler->pdb_info->generate_func_end(funcdef, compiler);

                }
            }
            compiler->tag("__rsir_rtcode_seg_function_define_end");
            compiler->pdb_info->finalize_generate_debug_info();

            rs::grammar::ast_base::pickout_this_thread_ast(generated_ast_nodes_buffers);
        }

        lang_scope* begin_namespace(const std::wstring& scope_namespace)
        {
            if (lang_scopes.size())
            {
                auto fnd = lang_scopes.back()->sub_namespaces.find(scope_namespace);
                if (fnd != lang_scopes.back()->sub_namespaces.end())
                {
                    lang_scopes.push_back(fnd->second);
                    return now_namespace = lang_scopes.back();
                }
            }

            lang_scope* scope = new lang_scope;
            lang_scopes_buffers.push_back(scope);

            scope->stop_searching_in_last_scope_flag = false;
            scope->type = lang_scope::scope_type::namespace_scope;
            scope->belong_namespace = now_namespace;
            scope->parent_scope = lang_scopes.empty() ? nullptr : lang_scopes.back();
            scope->scope_namespace = scope_namespace;

            if (lang_scopes.size())
                lang_scopes.back()->sub_namespaces[scope_namespace] = scope;

            lang_scopes.push_back(scope);
            return now_namespace = lang_scopes.back();
        }

        void end_namespace()
        {
            rs_assert(lang_scopes.back()->type == lang_scope::scope_type::namespace_scope);

            now_namespace = lang_scopes.back()->belong_namespace;
            lang_scopes.pop_back();
        }

        lang_scope* begin_scope()
        {
            lang_scope* scope = new lang_scope;
            lang_scopes_buffers.push_back(scope);

            scope->stop_searching_in_last_scope_flag = false;
            scope->type = lang_scope::scope_type::just_scope;
            scope->belong_namespace = now_namespace;
            scope->parent_scope = lang_scopes.empty() ? nullptr : lang_scopes.back();

            lang_scopes.push_back(scope);
            return scope;
        }

        void end_scope()
        {
            rs_assert(lang_scopes.back()->type == lang_scope::scope_type::just_scope);

            auto scope = now_scope();
            if (auto* func = in_function())
            {
                func->used_stackvalue_index -= scope->this_block_used_stackvalue_count;
            }
            lang_scopes.pop_back();
        }

        lang_scope* begin_function(ast::ast_value_function_define* ast_value_funcdef)
        {
            lang_scope* scope = new lang_scope;
            lang_scopes_buffers.push_back(scope);

            scope->stop_searching_in_last_scope_flag = false;
            scope->type = lang_scope::scope_type::function_scope;
            scope->belong_namespace = now_namespace;
            scope->parent_scope = lang_scopes.empty() ? nullptr : lang_scopes.back();
            scope->function_node = ast_value_funcdef;

            if (ast_value_funcdef->function_name != L"")
            {
                // Not anymous function, define func-symbol..
                define_variable_in_this_scope(ast_value_funcdef->function_name, ast_value_funcdef, ast_value_funcdef->declear_attribute);
            }

            lang_scopes.push_back(scope);
            return scope;
        }

        void end_function()
        {
            rs_assert(lang_scopes.back()->type == lang_scope::scope_type::function_scope);
            lang_scopes.pop_back();
        }

        lang_scope* now_scope() const
        {
            rs_assert(!lang_scopes.empty());
            return lang_scopes.back();
        }

        lang_scope* in_function() const
        {
            for (auto rindex = lang_scopes.rbegin(); rindex != lang_scopes.rend(); rindex++)
            {
                if ((*rindex)->type == lang_scope::scope_type::function_scope)
                    return *rindex;
            }
            return nullptr;
        }

        size_t global_symbol_index = 0;

        lang_symbol* define_variable_in_this_scope(const std::wstring& names, ast::ast_value* init_val, ast::ast_decl_attribute* attr)
        {
            rs_assert(lang_scopes.size());

            if (auto* func_def = dynamic_cast<ast::ast_value_function_define*>(init_val))
            {
                if (func_def->function_name != L"")
                {
                    lang_symbol* sym;
                    if (lang_scopes.back()->symbols.find(names) != lang_scopes.back()->symbols.end())
                    {
                        sym = lang_scopes.back()->symbols[names];
                    }
                    else
                    {
                        sym = lang_scopes.back()->symbols[names] = new lang_symbol;
                        sym->type = lang_symbol::symbol_type::function;
                        sym->name = names;
                        sym->defined_in_scope = lang_scopes.back();
                        sym->attribute = new ast::ast_decl_attribute();
                        sym->attribute->add_attribute(lang_anylizer, +lex_type::l_const); // for stop: function = xxx;

                        auto* pending_function = new ast::ast_value_function_define;
                        pending_function->value_type = new ast::ast_type(L"pending");
                        pending_function->value_type->set_as_function_type();
                        pending_function->value_type->set_as_variadic_arg_func();
                        pending_function->in_function_sentence = nullptr;
                        pending_function->auto_adjust_return_type = false;
                        pending_function->function_name = func_def->function_name;
                        pending_function->symbol = sym;
                        sym->variable_value = pending_function;
                    }

                    if (dynamic_cast<ast::ast_value_function_define*>(sym->variable_value)
                        && dynamic_cast<ast::ast_value_function_define*>(sym->variable_value)->function_name != L"")
                    {
                        func_def->symbol = sym;
                        sym->function_overload_sets.push_back(func_def);
                        return sym;
                    }
                }
            }

            if (lang_scopes.back()->symbols.find(names) != lang_scopes.back()->symbols.end())
            {
                auto* last_func_symbol = lang_scopes.back()->symbols[names];

                lang_anylizer->lang_error(0x0000, init_val, RS_ERR_REDEFINED, names.c_str());
                return last_func_symbol;
            }
            else
            {
                lang_symbol* sym = lang_scopes.back()->symbols[names] = new lang_symbol;
                sym->attribute = attr;
                sym->type = lang_symbol::symbol_type::variable;
                sym->name = names;
                sym->variable_value = init_val;
                sym->defined_in_scope = lang_scopes.back();

                if (auto* func = in_function())
                {
                    sym->define_in_function = true;
                    sym->static_symbol = false;

                    // TODO:
                    // const var xxx = 0;
                    // const var ddd = xxx;

                    if (!attr->is_constant_attr() || !init_val->is_constant)
                    {
                        sym->stackvalue_index_in_funcs = func->assgin_stack_index(sym);
                        lang_scopes.back()->this_block_used_stackvalue_count++;
                    }
                    else
                        sym->is_constexpr = true;

                }
                else
                {
                    sym->define_in_function = false;
                    sym->static_symbol = true;
                    if (!attr->is_constant_attr() || !init_val->is_constant)
                        sym->global_index_in_lang = global_symbol_index++;
                    else
                        sym->is_constexpr = true;

                }

                lang_symbols.push_back(sym);
                return sym;
            }
        }
        lang_symbol* define_type_in_this_scope(const std::wstring& names, ast::ast_type* as_type, ast::ast_decl_attribute* attr)
        {
            rs_assert(lang_scopes.size());

            if (lang_scopes.back()->symbols.find(names) != lang_scopes.back()->symbols.end())
            {
                auto* last_func_symbol = lang_scopes.back()->symbols[names];

                lang_anylizer->lang_error(0x0000, as_type, RS_ERR_REDEFINED, names.c_str());
                return last_func_symbol;
            }
            else
            {
                lang_symbol* sym = lang_scopes.back()->symbols[names] = new lang_symbol;
                sym->attribute = attr;
                sym->type = lang_symbol::symbol_type::typing;
                sym->name = names;
                sym->type_informatiom = as_type;
                sym->defined_in_scope = lang_scopes.back();

                lang_symbols.push_back(sym);
                return sym;
            }
        }
        lang_symbol* find_symbol_in_this_scope(ast::ast_symbolable_base* var_ident, const std::wstring& ident_str)
        {
            rs_assert(lang_scopes.size());

            auto* searching = var_ident->search_from_global_namespace ?
                lang_scopes.front()
                :
                (
                    var_ident->searching_begin_namespace_in_pass2 ?
                    var_ident->searching_begin_namespace_in_pass2
                    :
                    lang_scopes.back()
                    );

            std::vector<lang_scope*> searching_namespace;
            searching_namespace.push_back(searching);

            auto* _searching_in_all = searching;
            while (_searching_in_all)
            {
                for (auto* a_using_namespace : _searching_in_all->used_namespace)
                {
                    if (!a_using_namespace->from_global_namespace)
                    {
                        auto* finding_namespace = _searching_in_all;
                        while (finding_namespace)
                        {
                            auto* _deep_in_namespace = finding_namespace;
                            for (auto& nspace : a_using_namespace->used_namespace_chain)
                            {
                                if (auto fnd = _deep_in_namespace->sub_namespaces.find(nspace);
                                    fnd != _deep_in_namespace->sub_namespaces.end())
                                    _deep_in_namespace = fnd->second;
                                else
                                {
                                    // fail
                                    goto failed_in_this_namespace;
                                }
                            }
                            // ok!
                            searching_namespace.push_back(_deep_in_namespace);
                        failed_in_this_namespace:;
                            finding_namespace = finding_namespace->belong_namespace;
                        }
                    }
                    else
                    {
                        auto* _deep_in_namespace = lang_scopes.front();
                        for (auto& nspace : a_using_namespace->used_namespace_chain)
                        {
                            if (auto fnd = _deep_in_namespace->sub_namespaces.find(nspace);
                                fnd != _deep_in_namespace->sub_namespaces.end())
                                _deep_in_namespace = fnd->second;
                            else
                            {
                                // fail
                                goto failed_in_this_namespace_from_global;
                            }
                        }
                        // ok!
                        searching_namespace.push_back(_deep_in_namespace);
                    failed_in_this_namespace_from_global:;
                    }
                }
                _searching_in_all = _searching_in_all->parent_scope;
            }
            std::set<lang_symbol*> searching_result;
            std::set<lang_scope*> searched_scopes;
            for (auto _searching : searching_namespace)
            {
                while (_searching)
                {
                    // search_in 
                    if (var_ident->scope_namespaces.size())
                    {
                        size_t namespace_index = 0;
                        lang_scope* begin_namespace = nullptr;
                        if (_searching->type != lang_scope::scope_type::namespace_scope)
                            _searching = _searching->belong_namespace;

                        auto* stored_scope_for_next_try = _searching;

                        while (namespace_index < var_ident->scope_namespaces.size())
                        {
                            if (auto fnd = _searching->sub_namespaces.find(var_ident->scope_namespaces[namespace_index]);
                                fnd != _searching->sub_namespaces.end() && searched_scopes.find(fnd->second) == searched_scopes.end())
                            {
                                namespace_index++;
                                _searching = fnd->second;
                            }
                            else
                            {
                                _searching = stored_scope_for_next_try;
                                goto there_is_no_such_namespace;
                            }
                        }
                    }

                    searched_scopes.insert(_searching);
                    if (auto fnd = _searching->symbols.find(ident_str);
                        fnd != _searching->symbols.end())
                    {
                        searching_result.insert(var_ident->symbol = fnd->second);
                        goto next_searching_point;
                    }

                there_is_no_such_namespace:
                    _searching = _searching->parent_scope;
                }

            next_searching_point:;
            }

            if (searching_result.empty())
                return nullptr;

            if (searching_result.size() > 1)
            {
                std::wstring err_info = RS_ERR_SYMBOL_IS_AMBIGUOUS;
                size_t fnd_count = 0;
                for (auto fnd_result : searching_result)
                {
                    auto _full_namespace_ = rs::str_to_wstr(get_belong_namespace_path_with_lang_scope(fnd_result->defined_in_scope));
                    if (_full_namespace_ == L"")
                        err_info += RS_TERM_GLOBAL_NAMESPACE;
                    else
                        err_info += L"'" + _full_namespace_ + L"'";
                    fnd_count++;
                    if (fnd_count + 1 == searching_result.size())
                        err_info += L" " RS_TERM_AND L" ";
                    else
                        err_info += L", ";
                }

                lang_anylizer->lang_error(0x0000, var_ident, err_info.c_str(), ident_str.c_str());
            }

            return *searching_result.begin();
        }
        lang_symbol* find_type_in_this_scope(ast::ast_type* var_ident)
        {
            auto* result = find_symbol_in_this_scope(var_ident, var_ident->type_name);
            if (result && result->type != lang_symbol::symbol_type::typing)
            {
                lang_anylizer->lang_error(0x0000, var_ident, RS_ERR_IS_NOT_A_TYPE, var_ident->type_name.c_str());
                return nullptr;
            }
            return result;
        }
        lang_symbol* find_value_in_this_scope(ast::ast_value_variable* var_ident)
        {
            auto* result = find_symbol_in_this_scope(var_ident, var_ident->var_name);
            if (result && result->type == lang_symbol::symbol_type::typing)
            {
                lang_anylizer->lang_error(0x0000, var_ident, RS_ERR_IS_A_TYPE, var_ident->var_name.c_str());
                return nullptr;
            }
            return result;
        }
        bool has_compile_error()const
        {
            return !lang_anylizer->lex_error_list.empty();
        }
    };
}