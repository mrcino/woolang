#pragma once

#include "rs_basic_type.hpp"
#include "rs_lang_ast_builder.hpp"

#include <unordered_map>
#include <unordered_set>
namespace rs
{
    struct lang_symbol
    {
        enum class symbol_type
        {
            variable,
            function,
        };
        symbol_type type;
        std::wstring name;
        bool static_symbol;

        ast::ast_value* variable_value;
        std::vector<ast::ast_value*> function_overload_sets;
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

        ast::ast_value_function_define* function_node;
    };

    class lang
    {
    private:
        lexer* lang_anylizer;
        std::vector<lang_scope*> lang_namespaces; // only used for storing namespaces to release
        std::vector<lang_symbol*> lang_symbols; // only used for storing symbols to release

        std::vector<lang_scope*> lang_scopes; // it is a stack like list;
        lang_scope* now_namespace;

    public:
        lang(lexer& lex) :
            lang_anylizer(&lex)
        {
            begin_namespace(L"");   // global namespace
        }

        std::unordered_set<grammar::ast_base*> traving_node;

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
                    define_variable_in_this_scope(varref.ident_name, varref.init_val);

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
            else if (ast_value_variable* a_value_var = dynamic_cast<ast_value_variable*>(ast_node))
            {
                auto* sym = find_symbol_in_this_scope(a_value_var, rs::lang_symbol::symbol_type::variable);
                if (sym)
                {
                    a_value_var->value_type = sym->variable_value->value_type;
                }
                a_value_var->searching_begin_namespace_in_pass2 = now_scope();
            }
            else if (ast_value_type_cast* a_value_cast = dynamic_cast<ast_value_type_cast*>(ast_node))
            {
                analyze_pass1(a_value_cast->_be_cast_value_node);
                a_value_cast->add_child(a_value_cast->_be_cast_value_node);
            }
            else if (ast_value_function_define* a_value_func = dynamic_cast<ast_value_function_define*>(ast_node))
            {
                begin_function(a_value_func);

                auto arg_child = a_value_func->argument_list->children;
                while (arg_child)
                {
                    if (ast_value_arg_define* argdef = dynamic_cast<ast_value_arg_define*>(arg_child))
                    {
                        define_variable_in_this_scope(argdef->arg_name, argdef);
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
            else if (ast_return* a_ret = dynamic_cast<ast_return*>(ast_node))
            {
                auto* located_function_scope = in_function();
                if (!located_function_scope)
                    lang_anylizer->lang_error(0x0000, a_ret, L"Invalid return, cannot do return ouside of function.");
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
                                        lang_anylizer->lang_warning(0x0000, a_ret, L"Incompatible with the return type, the return value will be determined to be 'dynamic'.");
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
                                lang_anylizer->lang_error(0x0000, a_ret, L"Cannot return 'void' and '%s' at same time.", located_function_scope->function_node->value_type->type_name.c_str());
                            }
                        }
                        else
                        {
                            if (!located_function_scope->function_node->value_type->is_void())
                                lang_anylizer->lang_error(0x0000, a_ret, L"Cannot return 'void' and '%s' at same time.", located_function_scope->function_node->value_type->type_name.c_str());
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
            else
            {
                grammar::ast_base* child = ast_node->children;
                while (child)
                {
                    analyze_pass1(child);
                    child = child->sibling;
                }
            }
        }

        void analyze_pass2(grammar::ast_base* ast_node)
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

            if (ast_value* a_value = dynamic_cast<ast_value*>(ast_node))
            {
                if (a_value->value_type->is_pending())
                {
                    if (ast_value_variable* a_value_var = dynamic_cast<ast_value_variable*>(a_value))
                    {
                        auto* sym = find_symbol_in_this_scope(a_value_var,
                            rs::lang_symbol::symbol_type::variable);

                        if (sym)
                        {
                            analyze_pass2(sym->variable_value);
                            a_value_var->value_type = sym->variable_value->value_type;
                            if (a_value_var->value_type->is_pending())
                            {
                                if (!a_value_var->value_type->is_pending_function())
                                    lang_anylizer->lang_error(0x0000, a_value_var, L"Unable to decide value type.");
                            }

                        }
                        else
                        {
                            lang_anylizer->lang_error(0x0000, a_value_var, L"Unknown identifier '%s'.", a_value_var->var_name.c_str());
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
                            lang_anylizer->lang_error(0x0000, a_value_bin, L"Failed to analyze the type.");
                            a_value_bin->value_type = new ast_type(L"pending");
                        }
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
                                rs_test(called_funcsymb->symbol);

                                if (!called_funcsymb->symbol->function_overload_sets.empty())
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
                                        while (form_args)
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

                                                break;
                                            }

                                            if (!real_arg)
                                                break;// real_args count didn't match, break..

                                            if (real_arg->value_type->is_same(form_arg->value_type))
                                                ;// do nothing..
                                            else if (ast_type::check_castable(form_arg->value_type, real_arg->value_type))
                                                best_match = false;
                                            else
                                                break; // bad match, break..


                                            real_args = real_args->sibling;
                                        match_check_end_for_variadic_func:
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
                                        }
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
                                                    + L"' at ("
                                                    + std::to_wstring(judge_sets->at(index)->row_no)
                                                    + L","
                                                    + std::to_wstring(judge_sets->at(index)->col_no)
                                                    + L")";

                                                if (index + 1 != judge_sets->size())
                                                {
                                                    acceptable_func += L" or ";
                                                }
                                            }
                                            this->lang_anylizer->lang_error(0x0000, a_value_funccall, L"Cannot judge which function override to call, maybe: %s.", acceptable_func.c_str());
                                        }
                                        else
                                        {
                                            a_value_funccall->called_func = judge_sets->front();
                                            analyze_pass2(a_value_funccall->called_func);
                                        }
                                    }
                                    else
                                    {
                                        this->lang_anylizer->lang_error(0x0000, a_value_funccall, L"No matched function override to call.");
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
                    }
                    else
                    {
                        lang_anylizer->lang_error(0x0000, a_value, L"Unknown type '%s'.", a_value->value_type->get_type_name().c_str());
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
                            lang_anylizer->lang_error(0x0000, a_value, L"Cannot find overload of '%s' with type '%s' .",
                                a_variable_sym->var_name.c_str(),
                                a_value_typecast->value_type->get_type_name().c_str());
                        }
                    }
                    else
                    {
                    just_do_simple_type_cast:
                        if (!ast_type::check_castable(a_value_typecast->value_type, origin_value->value_type))
                        {
                            lang_anylizer->lang_error(0x0000, a_value, L"Cannot cast '%s' to '%s'.",
                                origin_value->value_type->get_type_name().c_str(),
                                a_value_typecast->value_type->get_type_name().c_str()
                            );
                        }
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
                                    lang_anylizer->lang_warning(0x0000, a_ret, L"Incompatible with the return type, the return value will be determined to be 'dynamic'.");
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
                            lang_anylizer->lang_error(0x0000, a_ret, L"Cannot return 'void' and '%s' at same time.", a_ret->located_function->value_type->type_name.c_str());
                        }
                    }
                    else
                    {
                        if (!a_ret->located_function->value_type->is_void())
                            lang_anylizer->lang_error(0x0000, a_ret, L"Cannot return 'void' and '%s' at same time.", a_ret->located_function->value_type->type_name.c_str());
                    }
                }

            }
            else if (ast_sentence_block* a_sentence_blk = dynamic_cast<ast_sentence_block*>(ast_node))
            {
                analyze_pass2(a_sentence_blk->sentence_list);
            }
            grammar::ast_base* child = ast_node->children;
            while (child)
            {
                analyze_pass2(child);
                child = child->sibling;
            }

        }

        lang_scope* begin_namespace(const std::wstring& scope_namespace)
        {
            if (now_namespace)
            {
                auto fnd = now_namespace->sub_namespaces.find(scope_namespace);
                if (fnd != now_namespace->sub_namespaces.end())
                {
                    lang_scopes.push_back(fnd->second);
                    return now_namespace = fnd->second;
                }
            }

            lang_scope* scope = new lang_scope;
            lang_namespaces.push_back(scope);

            scope->stop_searching_in_last_scope_flag = false;
            scope->type = lang_scope::scope_type::namespace_scope;
            scope->belong_namespace = now_namespace;
            scope->parent_scope = lang_scopes.empty() ? nullptr : lang_scopes.back();
            scope->scope_namespace = scope_namespace;

            if (now_namespace)
                now_namespace->sub_namespaces[scope_namespace] = scope;

            lang_scopes.push_back(scope);
            return now_namespace = scope;
        }

        void end_namespace()
        {
            rs_assert(lang_scopes.back()->type == lang_scope::scope_type::namespace_scope);
            lang_scopes.pop_back();

            now_namespace = now_namespace->belong_namespace;
        }

        lang_scope* begin_scope()
        {
            lang_scope* scope = new lang_scope;

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
            lang_scopes.pop_back();
        }

        lang_scope* begin_function(ast::ast_value_function_define* ast_value_funcdef)
        {
            lang_scope* scope = new lang_scope;

            scope->stop_searching_in_last_scope_flag = false;
            scope->type = lang_scope::scope_type::function_scope;
            scope->belong_namespace = now_namespace;
            scope->parent_scope = lang_scopes.empty() ? nullptr : lang_scopes.back();
            scope->function_node = ast_value_funcdef;

            if (ast_value_funcdef->function_name != L"")
            {
                // Not anymous function, define func-symbol..
                define_variable_in_this_scope(ast_value_funcdef->function_name, ast_value_funcdef);
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

        lang_symbol* define_variable_in_this_scope(const std::wstring& names, ast::ast_value* init_val)
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
                        sym->type = lang_symbol::symbol_type::variable;
                        sym->name = names;

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
                        sym->function_overload_sets.push_back(func_def);
                        return sym;
                    }
                }
            }

            if (lang_scopes.back()->symbols.find(names) != lang_scopes.back()->symbols.end())
            {
                auto* last_func_symbol = lang_scopes.back()->symbols[names];

                lang_anylizer->lang_error(0x0000, init_val, L"Redefined '%s' in this scope.", names.c_str());
                return last_func_symbol;

            }
            else
            {
                lang_symbol* sym = lang_scopes.back()->symbols[names] = new lang_symbol;
                sym->type = lang_symbol::symbol_type::variable;
                sym->name = names;
                sym->variable_value = init_val;

                if (in_function())
                    sym->static_symbol = false;
                else
                    sym->static_symbol = true;

                lang_symbols.push_back(sym);
                return sym;
            }
        }
        lang_symbol* find_symbol_in_this_scope(ast::ast_value_variable* var_ident, lang_symbol::symbol_type need_type)
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

            while (searching)
            {
                // search_in 
                if (var_ident->scope_namespaces.size())
                {
                    size_t namespace_index = 0;
                    lang_scope* begin_namespace = nullptr;
                    if (searching->type != lang_scope::scope_type::namespace_scope)
                        searching = searching->belong_namespace;

                    auto* stored_scope_for_next_try = searching;

                    while (namespace_index < var_ident->scope_namespaces.size())
                    {
                        if (auto fnd = searching->sub_namespaces.find(var_ident->scope_namespaces[namespace_index]);
                            fnd != searching->sub_namespaces.end())
                        {
                            namespace_index++;
                            searching = fnd->second;
                        }
                        else
                        {
                            searching = stored_scope_for_next_try;
                            goto there_is_no_such_namespace;
                        }
                    }
                }

                if (auto fnd = searching->symbols.find(var_ident->var_name);
                    fnd != searching->symbols.end())
                {
                    if (fnd->second->type == need_type)
                    {
                        return var_ident->symbol = fnd->second;
                    }
                }

            there_is_no_such_namespace:
                searching = searching->parent_scope;
            }

            return nullptr;
        }
    };
}