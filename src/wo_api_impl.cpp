// wo_api_impl.cpp
#define _CRT_SECURE_NO_WARNINGS
#include "wo_vm.hpp"
#include "wo_source_file_manager.hpp"
#include "wo_compiler_parser.hpp"
#include "wo_exceptions.hpp"
#include "wo_stdlib.hpp"
#include "wo_lang_grammar_loader.hpp"
#include "wo_lang.hpp"
#include "wo_utf8.hpp"
#include "wo_runtime_debuggee.hpp"
#include "wo_global_setting.hpp"
#include "wo_io.hpp"
#include "wo_roroutine_simulate_mgr.hpp"
#include "wo_roroutine_thread_mgr.hpp"

#include <csignal>
#include <sstream>
#include <new>

// TODO LIST
// 1. ALL GC_UNIT OPERATE SHOULD BE ATOMIC

#define WO_VERSION(DEV,MAIN,SUB,CORRECT) ((0x##DEV##ull)<<(3*16))|((0x##MAIN##ull)<<(2*16))|((0x##SUB##ull)<<(1*16))|((0x##CORRECT##ull)<<(0*16))
#define WO_VERSION_STR(DEV,MAIN,SUB,CORRECT) #DEV "." #MAIN "." #SUB "." #CORRECT "."

#ifdef _DEBUG
#define WO_DEBUG_SFX "debug"
#else
#define WO_DEBUG_SFX ""
#endif

constexpr wo_integer_t version = WO_VERSION(de, 1, 3, 0);
constexpr char         version_str[] = WO_VERSION_STR(de, 1, 3, 0) WO_DEBUG_SFX;

#undef WO_DEBUG_SFX
#undef WO_VERSION_STR
#undef WO_VERSION


#include <atomic>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <string>


void _default_fail_handler(wo_string_t src_file, uint32_t lineno, wo_string_t functionname, uint32_t rterrcode, wo_string_t reason)
{
    wo::wo_stderr << ANSI_HIR "WooLang Runtime happend a failure: "
        << ANSI_HIY << reason << " (E" << std::hex << rterrcode << std::dec << ")" << ANSI_RST << wo::wo_endl;
    wo::wo_stderr << "\tAt source: \t" << src_file << wo::wo_endl;
    wo::wo_stderr << "\tAt line: \t" << lineno << wo::wo_endl;
    wo::wo_stderr << "\tAt function: \t" << functionname << wo::wo_endl;
    wo::wo_stderr << wo::wo_endl;

    wo::wo_stderr << ANSI_HIR "callstack: " ANSI_RST << wo::wo_endl;

    if (wo::vmbase::_this_thread_vm)
        wo::vmbase::_this_thread_vm->dump_call_stack(32, true, std::cerr);

    wo::wo_stderr << wo::wo_endl;

    if ((rterrcode & WO_FAIL_TYPE_MASK) == WO_FAIL_MINOR)
    {
        wo::wo_stderr << ANSI_HIY "This is a minor failure, ignore it." ANSI_RST << wo::wo_endl;
        // Just ignore it..
    }
    else if ((rterrcode & WO_FAIL_TYPE_MASK) == WO_FAIL_MEDIUM)
    {
        // Just throw it..
        wo::wo_stderr << ANSI_HIY "This is a medium failure, it will be throw." ANSI_RST << wo::wo_endl;
        throw wo::rsruntime_exception(rterrcode, reason);
    }
    else if ((rterrcode & WO_FAIL_TYPE_MASK) == WO_FAIL_HEAVY)
    {
        // Just throw it..
        wo::wo_stderr << ANSI_HIY "This is a heavy failure, abort." ANSI_RST << wo::wo_endl;
        throw wo::rsruntime_exception(rterrcode, reason);
    }
    else
    {
        wo::wo_stderr << "This failure may cause a crash or nothing happens." << wo::wo_endl;
        wo::wo_stderr << "1) Abort program.(You can attatch debuggee.)" << wo::wo_endl;
        wo::wo_stderr << "2) Continue.(May cause unknown errors.)" << wo::wo_endl;
        wo::wo_stderr << "3) Roll back to last WO-EXCEPTION-RECOVERY.(Not immediatily)" << wo::wo_endl;
        wo::wo_stderr << "4) Halt (Not exactly safe, this vm will be abort.)" << wo::wo_endl;
        wo::wo_stderr << "5) Throw exception.(Not exactly safe)" << wo::wo_endl;
        do
        {
            int choice;
            wo::wo_stderr << "Please input your choice: " ANSI_HIY;
            std::cin >> choice;
            wo::wo_stderr << ANSI_RST;
            switch (choice)
            {
            case 1:
                wo_error(reason); break;
            case 2:
                return;
            case 3:
                if (wo::vmbase::_this_thread_vm)
                {
                    wo::vmbase::_this_thread_vm->er->set_gcunit_with_barrier(wo::value::valuetype::string_type);
                    wo::string_t::gc_new<wo::gcbase::gctype::eden>(wo::vmbase::_this_thread_vm->er->gcunit, reason);
                    wo::exception_recovery::rollback(wo::vmbase::_this_thread_vm);
                    return;
                }
                else
                    wo::wo_stderr << ANSI_HIR "No virtual machine running in this thread." ANSI_RST << wo::wo_endl;

                break;
            case 4:
                wo::wo_stderr << ANSI_HIR "Current virtual machine will abort." ANSI_RST << wo::wo_endl;
                throw wo::rsruntime_exception(rterrcode, reason);

                // in debug, if there is no catcher for wo_runtime_error, 
                // the program may continue working.
                // Abort here.
                wo_error(reason);
            case 5:
                throw wo::rsruntime_exception(WO_FAIL_MEDIUM, reason);

                // in debug, if there is no catcher for wo_runtime_error, 
                // the program may continue working.
                // Abort here.
                wo_error(reason);
            default:
                wo::wo_stderr << ANSI_HIR "Invalid choice" ANSI_RST << wo::wo_endl;
            }

            char _useless_for_clear = 0;
            std::cin.clear();
            while (std::cin.readsome(&_useless_for_clear, 1));

        } while (true);
    }
}
static std::atomic<wo_fail_handler> _wo_fail_handler_function = &_default_fail_handler;

wo_fail_handler wo_regist_fail_handler(wo_fail_handler new_handler)
{
    return _wo_fail_handler_function.exchange(new_handler);
}
void wo_cause_fail(wo_string_t src_file, uint32_t lineno, wo_string_t functionname, uint32_t rterrcode, wo_string_t reason)
{
    _wo_fail_handler_function.load()(src_file, lineno, functionname, rterrcode, reason);
}

void _wo_ctrl_c_signal_handler(int sig)
{
    // CTRL + C, 
    wo::wo_stderr << ANSI_HIR "CTRL+C:" ANSI_RST " Pause all virtual-machine by default debuggee immediately." << wo::wo_endl;

    std::lock_guard g1(wo::vmbase::_alive_vm_list_mx);
    for (auto vm : wo::vmbase::_alive_vm_list)
    {
        if (!wo_has_attached_debuggee((wo_vm)vm))
            wo_attach_default_debuggee((wo_vm)vm);
        wo_break_immediately((wo_vm)vm);
    }

    wo_handle_ctrl_c(_wo_ctrl_c_signal_handler);

}

void wo_handle_ctrl_c(void(*handler)(int))
{
    signal(SIGINT, handler ? handler : _wo_ctrl_c_signal_handler);
}

#undef wo_init

void wo_finish()
{
    bool scheduler_need_shutdown = true;

    // Ready to shutdown all vm & coroutine.
    do
    {
        if (scheduler_need_shutdown)
            wo_coroutine_stopall();

        do
        {
            std::lock_guard g1(wo::vmbase::_alive_vm_list_mx);

            for (auto& alive_vms : wo::vmbase::_alive_vm_list)
                alive_vms->interrupt(wo::vmbase::ABORT_INTERRUPT);
        } while (false);

        if (scheduler_need_shutdown)
        {
            wo::fvmscheduler::shutdown();
            scheduler_need_shutdown = false;
        }
        wo_gc_immediately();

        std::this_thread::yield();

        std::lock_guard g1(wo::vmbase::_alive_vm_list_mx);
        if (wo::vmbase::_alive_vm_list.empty())
            break;

    } while (true);

    wo_gc_stop();
}

void wo_init(int argc, char** argv)
{
    const char* basic_env_local = "en_US.UTF-8";
    bool enable_std_package = true;
    bool enable_ctrl_c_to_debug = true;
    bool enable_gc = true;
    size_t coroutine_mgr_thread_count = 4;

    for (int command_idx = 0; command_idx + 1 < argc; command_idx++)
    {
        std::string current_arg = argv[command_idx];
        if (current_arg.size() >= 2 && current_arg[0] == '-' && current_arg[1] == '-')
        {
            current_arg = current_arg.substr(2);
            if ("local" == current_arg)
                basic_env_local = argv[++command_idx];
            else if ("enable-std" == current_arg)
                enable_std_package = atoi(argv[++command_idx]);
            else if ("enable-ctrlc-debug" == current_arg)
                enable_ctrl_c_to_debug = atoi(argv[++command_idx]);
            else if ("enable-gc" == current_arg)
                enable_gc = atoi(argv[++command_idx]);
            else if ("enable-code-allign" == current_arg)
                wo::config::ENABLE_IR_CODE_ACTIVE_ALLIGN = atoi(argv[++command_idx]);
            else if ("enable-ansi-color" == current_arg)
                wo::config::ENABLE_OUTPUT_ANSI_COLOR_CTRL = atoi(argv[++command_idx]);
            else if ("coroutine-thread-count" == current_arg)
                coroutine_mgr_thread_count = atoi(argv[++command_idx]);
            else
                wo::wo_stderr << ANSI_HIR "Woolang: " << ANSI_RST << "unknown setting --" << current_arg << wo::wo_endl;
        }
    }

    wo::wo_init_locale(basic_env_local);

    if (enable_gc)
        wo::gc::gc_start(); // I dont know who will disable gc..

    wo::fvmscheduler::init(coroutine_mgr_thread_count);

    if (enable_std_package)
    {
        wo_virtual_source(wo_stdlib_src_path, wo_stdlib_src_data, false);
        wo_virtual_source(wo_stdlib_debug_src_path, wo_stdlib_debug_src_data, false);
        wo_virtual_source(wo_stdlib_vm_src_path, wo_stdlib_vm_src_data, false);
        wo_virtual_source(wo_stdlib_thread_src_path, wo_stdlib_thread_src_data, false);
        wo_virtual_source(wo_stdlib_roroutine_src_path, wo_stdlib_roroutine_src_data, false);
        wo_virtual_source(wo_stdlib_macro_src_path, wo_stdlib_macro_src_data, false);
    }

    if (enable_ctrl_c_to_debug)
        wo_handle_ctrl_c(nullptr);
}

wo_string_t  wo_compile_date(void)
{
    return __DATE__ " " __TIME__;
}
wo_string_t  wo_version(void)
{
    return version_str;
}
wo_integer_t wo_version_int(void)
{
    return version;
}

#define WO_ORIGIN_VAL(v) (reinterpret_cast<wo::value*>(v))
#define WO_VAL(v) (WO_ORIGIN_VAL(v)->get())
#define WO_VM(v) (reinterpret_cast<wo::vmbase*>(v))
#define CS_VAL(v) (reinterpret_cast<wo_value>(v))
#define CS_VM(v) (reinterpret_cast<wo_vm>(v))

wo_string_t wo_locale_name()
{
    return wo::wo_global_locale_name.c_str();
}

wo_string_t wo_exe_path()
{
    return wo::exe_path();
}

wo_ptr_t wo_safety_pointer_ignore_fail(wo::gchandle_t* gchandle)
{
    if (gchandle->has_been_closed)
    {
        return nullptr;
    }
    return gchandle->holding_handle;
}

wo_ptr_t wo_safety_pointer(wo::gchandle_t* gchandle)
{
    if (gchandle->has_been_closed)
    {
        wo_fail(WO_FAIL_ACCESS_NIL, "Reading a closed gchandle.");
        return nullptr;
    }
    return gchandle->holding_handle;
}

wo_type wo_valuetype(wo_value value)
{
    auto _rsvalue = WO_VAL(value);

    return (wo_type)_rsvalue->type;
}
wo_integer_t wo_int(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::integer_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not an integer.");
        return wo_cast_int(value);
    }
    return _rsvalue->integer;
}
wo_real_t wo_real(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::real_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not an real.");
        return wo_cast_real(value);
    }
    return _rsvalue->real;
}
float wo_float(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::real_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not an real.");
        return wo_cast_float(value);
    }
    return (float)_rsvalue->real;
}
wo_handle_t wo_handle(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::handle_type
        && _rsvalue->type != wo::value::valuetype::gchandle_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not a handle.");
        return wo_cast_handle(value);
    }
    return _rsvalue->type == wo::value::valuetype::handle_type ?
        (wo_handle_t)_rsvalue->handle
        :
        (wo_handle_t)wo_safety_pointer(_rsvalue->gchandle);
}
wo_ptr_t wo_pointer(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::handle_type
        && _rsvalue->type != wo::value::valuetype::gchandle_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not a handle.");
        return wo_cast_pointer(value);
    }
    return _rsvalue->type == wo::value::valuetype::handle_type ?
        (wo_ptr_t)_rsvalue->handle
        :
        (wo_ptr_t)wo_safety_pointer(_rsvalue->gchandle);
}
wo_string_t wo_string(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::string_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not a string.");
        return wo_cast_string(value);
    }
    wo::gcbase::gc_read_guard rg1(_rsvalue->string);
    return _rsvalue->string->c_str();
}
wo_bool_t wo_bool(const wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    return _rsvalue->handle != 0;
}
wo_value wo_value_of_gchandle(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->type != wo::value::valuetype::gchandle_type)
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "This value is not a gchandle.");
        return nullptr;
    }
    return CS_VAL(&_rsvalue->gchandle->holding_value);
}

wo_bool_t wo_is_ref(wo_value value)
{
    return WO_ORIGIN_VAL(value)->is_ref();
}

void wo_set_int(wo_value value, wo_integer_t val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_integer(val);
}
void wo_set_real(wo_value value, wo_real_t val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_real(val);
}
void wo_set_float(wo_value value, float val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_real((wo_real_t)val);
}
void wo_set_handle(wo_value value, wo_handle_t val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_handle(val);
}
void wo_set_pointer(wo_value value, wo_ptr_t val)
{
    if (val)
        WO_VAL(value)->set_handle((wo_handle_t)val);
    else
        wo_fail(WO_FAIL_ACCESS_NIL, "Cannot set a nullptr");
}
void wo_set_string(wo_value value, wo_string_t val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_string(val);
}
void wo_set_bool(wo_value value, wo_bool_t val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_integer(val ? 1 : 0);
}
void wo_set_gchandle(wo_value value, wo_ptr_t resource_ptr, wo_value holding_val, void(*destruct_func)(wo_ptr_t))
{
    WO_VAL(value)->set_gcunit_with_barrier(wo::value::valuetype::gchandle_type);
    auto handle_ptr = wo::gchandle_t::gc_new<wo::gcbase::gctype::eden>(WO_VAL(value)->gcunit);
    handle_ptr->holding_handle = resource_ptr;
    if (holding_val)
    {
        handle_ptr->holding_value.set_val(WO_VAL(holding_val));
        if (handle_ptr->holding_value.is_gcunit())
            handle_ptr->holding_value.gcunit->gc_type = wo::gcbase::gctype::no_gc;
    }
    handle_ptr->destructor = destruct_func;
}
void wo_set_val(wo_value value, wo_value val)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_val(WO_VAL(val));
}
void wo_set_ref(wo_value value, wo_value val)
{
    auto _rsvalue = WO_ORIGIN_VAL(value);
    auto _ref_val = WO_VAL(val);

    if (_rsvalue->is_ref())
        _rsvalue->set_ref(_rsvalue->ref->set_ref(_ref_val));
    else
        _rsvalue->set_ref(_ref_val);
}

void wo_set_struct(wo_value value, uint16_t structsz)
{
    auto _rsvalue = WO_VAL(value);
    _rsvalue->set_gcunit_with_barrier(wo::value::valuetype::struct_type);

    wo::struct_t::gc_new<wo::gcbase::gctype::eden>(_rsvalue->gcunit, structsz);
}

wo_integer_t wo_cast_int(wo_value value)
{
    auto _rsvalue = WO_VAL(value);

    switch (_rsvalue->type)
    {
    case wo::value::valuetype::integer_type:
        return _rsvalue->integer;
    case wo::value::valuetype::handle_type:
        return (wo_integer_t)_rsvalue->handle;
    case wo::value::valuetype::real_type:
        return (wo_integer_t)_rsvalue->real;
    case wo::value::valuetype::string_type:
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->string);
        return (wo_integer_t)atoll(_rsvalue->string->c_str());
    }
    default:
        wo_fail(WO_FAIL_TYPE_FAIL, "This value can not cast to integer.");
        return 0;
        break;
    }
}
wo_real_t wo_cast_real(wo_value value)
{
    auto _rsvalue = WO_VAL(value);

    switch (reinterpret_cast<wo::value*>(value)->type)
    {
    case wo::value::valuetype::integer_type:
        return (wo_real_t)_rsvalue->integer;
    case wo::value::valuetype::handle_type:
        return (wo_real_t)_rsvalue->handle;
    case wo::value::valuetype::real_type:
        return _rsvalue->real;
    case wo::value::valuetype::string_type:
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->string);
        return atof(_rsvalue->string->c_str());
    }
    default:
        wo_fail(WO_FAIL_TYPE_FAIL, "This value can not cast to real.");
        return 0;
        break;
    }
}

float wo_cast_float(wo_value value)
{
    return (float)wo_cast_real(value);
}

wo_handle_t wo_cast_handle(wo_value value)
{
    auto _rsvalue = WO_VAL(value);

    switch (reinterpret_cast<wo::value*>(value)->type)
    {
    case wo::value::valuetype::integer_type:
        return (wo_handle_t)_rsvalue->integer;
    case wo::value::valuetype::handle_type:
        return _rsvalue->handle;
    case wo::value::valuetype::gchandle_type:
        return (wo_handle_t)wo_safety_pointer(_rsvalue->gchandle);
    case wo::value::valuetype::real_type:
        return (wo_handle_t)_rsvalue->real;
    case wo::value::valuetype::string_type:
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->string);
        return (wo_handle_t)atoll(_rsvalue->string->c_str());
    }
    default:
        wo_fail(WO_FAIL_TYPE_FAIL, "This value can not cast to handle.");
        return 0;
        break;
    }
}
wo_ptr_t wo_cast_pointer(wo_value value)
{
    return (wo_ptr_t)wo_cast_handle(value);
}

std::string _enstring(const std::string& sstr, bool need_wrap)
{
    if (need_wrap)
    {
        const char* str = sstr.c_str();
        std::string result;
        while (*str)
        {
            unsigned char uch = *str;
            if (iscntrl(uch))
            {
                char encode[8] = {};
                sprintf(encode, "\\u00%02x", (unsigned int)uch);

                result += encode;
            }
            else
            {
                switch (uch)
                {
                case '"':
                    result += R"(\")"; break;
                case '\\':
                    result += R"(\\)"; break;
                default:
                    result += *str; break;
                }
            }
            ++str;
        }
        return "\"" + result + "\"";
    }
    else
        return sstr;
}
std::string _destring(const std::string& dstr)
{
    const char* str = dstr.c_str();
    std::string result;
    if (*str == '"')
        ++str;
    while (*str)
    {
        char uch = *str;
        if (uch == '\\')
        {
            // Escape character 
            char escape_ch = *++str;
            switch (escape_ch)
            {
            case '\'':
            case '"':
            case '?':
            case '\\':
                result += escape_ch; break;
            case 'a':
                result += '\a'; break;
            case 'b':
                result += '\b'; break;
            case 'f':
                result += '\f'; break;
            case 'n':
                result += '\n'; break;
            case 'r':
                result += '\r'; break;
            case 't':
                result += L'\t'; break;
            case 'v':
                result += '\v'; break;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
            {
                // oct 1byte 
                unsigned char oct_ascii = escape_ch - '0';
                for (int i = 0; i < 2; i++)
                {
                    unsigned char nextch = (unsigned char)*++str;
                    if (wo::lexer::lex_isodigit(nextch))
                    {
                        oct_ascii *= 8;
                        oct_ascii += wo::lexer::lex_hextonum(nextch);
                    }
                    else
                        break;
                }
                result += oct_ascii;
                break;
            }
            case 'X':
            case 'x':
            {
                // hex 1byte 
                unsigned char hex_ascii = 0;
                for (int i = 0; i < 2; i++)
                {
                    unsigned char nextch = (unsigned char)*++str;
                    if (wo::lexer::lex_isxdigit(nextch))
                    {
                        hex_ascii *= 16;
                        hex_ascii += wo::lexer::lex_hextonum(nextch);
                    }
                    else if (i == 0)
                        goto str_escape_sequences_fail;
                    else
                        break;
                }
                result += (char)hex_ascii;
                break;
            }
            case 'U':
            case 'u':
            {
                // hex 1byte 
                unsigned char hex_ascii = 0;
                for (int i = 0; i < 4; i++)
                {
                    unsigned char nextch = (unsigned char)*++str;
                    if (wo::lexer::lex_isxdigit(nextch))
                    {
                        hex_ascii *= 16;
                        hex_ascii += wo::lexer::lex_hextonum(nextch);
                    }
                    else if (i == 0)
                        goto str_escape_sequences_fail;
                    else
                        break;
                }
                result += (char)hex_ascii;
                break;
            }
            default:
            str_escape_sequences_fail:
                result += escape_ch;
                break;
            }
        }
        else if (uch == '"')
            break;
        else
            result += uch;
        ++str;
    }
    return result;
}
void _wo_cast_value(wo::value* value, wo::lexer* lex, wo::value::valuetype except_type);
void _wo_cast_array(wo::value* value, wo::lexer* lex)
{
    wo::array_t* rsarr;
    wo::array_t::gc_new<wo::gcbase::gctype::eden>(*std::launder((wo::gcbase**)&rsarr));

    while (true)
    {
        auto lex_type = lex->peek(nullptr);
        if (lex_type == +wo::lex_type::l_index_end)
        {
            lex->next(nullptr);
            break;
        }

        _wo_cast_value(value, lex, wo::value::valuetype::invalid); // key!
        rsarr->push_back(*value);

        if (lex->peek(nullptr) == +wo::lex_type::l_comma)
            lex->next(nullptr);
    }

    value->set_gcunit_with_barrier(wo::value::valuetype::array_type, rsarr);
}
void _wo_cast_map(wo::value* value, wo::lexer* lex)
{
    wo::mapping_t* rsmap;
    wo::mapping_t::gc_new<wo::gcbase::gctype::eden>(*std::launder((wo::gcbase**)&rsmap));

    while (true)
    {
        auto lex_type = lex->peek(nullptr);
        if (lex_type == +wo::lex_type::l_right_curly_braces)
        {
            // end
            lex->next(nullptr);
            break;
        }

        _wo_cast_value(value, lex, wo::value::valuetype::invalid); // key!
        auto& val_place = (*rsmap)[*value];

        lex_type = lex->next(nullptr);
        if (lex_type != +wo::lex_type::l_typecast)
            wo_fail(WO_FAIL_TYPE_FAIL, "Unexcept token while parsing map, here should be ':'.");

        _wo_cast_value(&val_place, lex, wo::value::valuetype::invalid); // value!

        if (lex->peek(nullptr) == +wo::lex_type::l_comma)
            lex->next(nullptr);
    }

    value->set_gcunit_with_barrier(wo::value::valuetype::mapping_type, rsmap);
}
void _wo_cast_value(wo::value* value, wo::lexer* lex, wo::value::valuetype except_type)
{
    std::wstring wstr;
    auto lex_type = lex->next(&wstr);
    if (lex_type == +wo::lex_type::l_left_curly_braces) // is map
        _wo_cast_map(value, lex);
    else if (lex_type == +wo::lex_type::l_index_begin) // is array
        _wo_cast_array(value, lex);
    else if (lex_type == +wo::lex_type::l_literal_string) // is string
        value->set_string(wo::wstr_to_str(wstr).c_str());
    else if (lex_type == +wo::lex_type::l_add
        || lex_type == +wo::lex_type::l_sub
        || lex_type == +wo::lex_type::l_literal_integer
        || lex_type == +wo::lex_type::l_literal_real) // is integer
    {
        bool positive = true;
        if (lex_type == +wo::lex_type::l_sub || lex_type == +wo::lex_type::l_add)
        {
            lex_type = lex->next(&wstr);
            if (lex_type == +wo::lex_type::l_sub)
                positive = false;

            if (lex_type != +wo::lex_type::l_literal_integer
                && lex_type != +wo::lex_type::l_literal_real)
                wo_fail(WO_FAIL_TYPE_FAIL, "Unknown token while parsing.");
        }

        if (lex_type == +wo::lex_type::l_literal_integer) // is real
            value->set_integer(positive
                ? std::stoll(wo::wstr_to_str(wstr).c_str())
                : -std::stoll(wo::wstr_to_str(wstr).c_str()));
        else if (lex_type == +wo::lex_type::l_literal_real) // is real
            value->set_real(positive
                ? std::stod(wo::wstr_to_str(wstr).c_str())
                : -std::stod(wo::wstr_to_str(wstr).c_str()));

    }
    else if (lex_type == +wo::lex_type::l_nil) // is nil
        value->set_nil();
    else if (wstr == L"true")
        value->set_integer(1);// true
    else if (wstr == L"false")
        value->set_integer(0);// false
    else if (wstr == L"null")
        value->set_nil();// null
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Unknown token while parsing.");

    if (except_type != wo::value::valuetype::invalid && except_type != value->type)
        wo_fail(WO_FAIL_TYPE_FAIL, "Unexcept value type after parsing.");

}
void wo_cast_value_from_str(wo_value value, wo_string_t str, wo_type except_type)
{
    wo::lexer lex(wo::str_to_wstr(str), "json");

    _wo_cast_value(WO_VAL(value), &lex, (wo::value::valuetype)except_type);
}

void _wo_cast_string(wo::value* value, std::map<wo::gcbase*, int>* traveled_gcunit, bool _fit_layout, std::string* out_str, int depth, bool force_to_be_str)
{
    auto _rsvalue = value->get();

    //if (value->type == wo::value::valuetype::is_ref)
    //    *out_str += "<is_ref>";

    switch (_rsvalue->type)
    {
    case wo::value::valuetype::integer_type:
        *out_str += _enstring(std::to_string(_rsvalue->integer), force_to_be_str);
        return;
    case wo::value::valuetype::handle_type:
        *out_str += _enstring(std::to_string(_rsvalue->handle), force_to_be_str);
        return;
    case wo::value::valuetype::real_type:
        *out_str += _enstring(std::to_string(_rsvalue->real), force_to_be_str);
        return;
    case wo::value::valuetype::gchandle_type:
        *out_str += _enstring(std::to_string((wo_handle_t)wo_safety_pointer(_rsvalue->gchandle)), force_to_be_str);
        return;
    case wo::value::valuetype::string_type:
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->string);
        *out_str += _enstring(*_rsvalue->string, true);
        return;
    }
    case wo::value::valuetype::mapping_type:
    {
        if (wo::mapping_t* map = _rsvalue->mapping)
        {
            wo::gcbase::gc_read_guard rg1(_rsvalue->mapping);
            if ((*traveled_gcunit)[map] >= 1)
            {
                _fit_layout = true;
                if ((*traveled_gcunit)[map] >= 2)
                {
                    *out_str += "{ ... }";
                    return;
                }
            }
            (*traveled_gcunit)[map]++;

            *out_str += _fit_layout ? "{" : "{\n";
            bool first_kv_pair = true;
            for (auto& [v_key, v_val] : *map)
            {
                if (!first_kv_pair)
                    *out_str += _fit_layout ? ", " : ",\n";
                first_kv_pair = false;

                for (int i = 0; !_fit_layout && i <= depth; i++)
                    *out_str += "    ";
                _wo_cast_string(const_cast<wo::value*>(&v_key), traveled_gcunit, _fit_layout, out_str, depth + 1, true);
                *out_str += _fit_layout ? ":" : " : ";
                _wo_cast_string(&v_val, traveled_gcunit, _fit_layout, out_str, depth + 1, false);

            }
            if (!_fit_layout)
                *out_str += "\n";
            for (int i = 0; !_fit_layout && i < depth; i++)
                *out_str += "    ";
            *out_str += "}";

            (*traveled_gcunit)[map]--;
        }
        else
            *out_str += "nil";
        return;
    }
    case wo::value::valuetype::array_type:
    {
        if (wo::array_t* arr = _rsvalue->array)
        {
            wo::gcbase::gc_read_guard rg1(_rsvalue->array);
            if ((*traveled_gcunit)[arr] >= 1)
            {
                _fit_layout = true;
                if ((*traveled_gcunit)[arr] >= 2)
                {
                    *out_str += "[ ... ]";
                    return;
                }
            }
            (*traveled_gcunit)[arr]++;

            *out_str += _fit_layout ? "[" : "[\n";
            bool first_value = true;
            for (auto& v_val : *arr)
            {
                if (!first_value)
                    *out_str += _fit_layout ? "," : ",\n";
                first_value = false;

                for (int i = 0; !_fit_layout && i <= depth; i++)
                    *out_str += "    ";
                _wo_cast_string(&v_val, traveled_gcunit, _fit_layout, out_str, depth + 1, false);
            }
            if (!_fit_layout)
                *out_str += "\n";
            for (int i = 0; !_fit_layout && i < depth; i++)
                *out_str += "    ";
            *out_str += "]";

            (*traveled_gcunit)[arr]--;
        }
        else
            *out_str += "nil";
        return;
    }
    case wo::value::valuetype::closure_type:
        *out_str += "<closure function>";
        return;
    case wo::value::valuetype::struct_type:
        *out_str += "<struct value>";
        return;
    case wo::value::valuetype::invalid:
        *out_str += "nil";
        return;
    default:
        wo_fail(WO_FAIL_TYPE_FAIL, "This value can not cast to string.");
        *out_str += "";
        break;
    }
}
wo_string_t wo_cast_string(const wo_value value)
{
    thread_local std::string _buf;
    _buf = "";

    auto _rsvalue = WO_VAL(value);
    switch (_rsvalue->type)
    {
    case wo::value::valuetype::integer_type:
        _buf = std::to_string(_rsvalue->integer);
        return _buf.c_str();
    case wo::value::valuetype::handle_type:
        _buf = std::to_string(_rsvalue->handle);
        return _buf.c_str();
    case wo::value::valuetype::gchandle_type:
        _buf = std::to_string((wo_handle_t)wo_safety_pointer(_rsvalue->gchandle));
        return _buf.c_str();
    case wo::value::valuetype::real_type:
        _buf = std::to_string(_rsvalue->real);
        return _buf.c_str();
    case wo::value::valuetype::string_type:
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->string);
        return _rsvalue->string->c_str();
    }
    case wo::value::valuetype::closure_type:
        return "<closure function>";
    case wo::value::valuetype::struct_type:
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->structs);
        std::string tmp_buf = "struct {\n";
        for (uint16_t i = 0; i < _rsvalue->structs->m_count; ++i)
        {
            // TODO: Struct may recursive, handle it..
            tmp_buf += "    +" + std::to_string(i) + " : " + wo_cast_string(CS_VAL(&_rsvalue->structs->m_values[i])) + ",\n";
        }
        tmp_buf += "}";
        _buf = tmp_buf;
        return _buf.c_str();
    }
    case wo::value::valuetype::invalid:
        return "nil";
    default:
        break;
    }

    std::map<wo::gcbase*, int> _tved_gcunit;
    _wo_cast_string(reinterpret_cast<wo::value*>(value), &_tved_gcunit, false, &_buf, 0, false);

    return _buf.c_str();
}

wo_string_t wo_type_name(const wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    switch (_rsvalue->type)
    {
    case wo::value::valuetype::integer_type:
        return "int";
    case wo::value::valuetype::handle_type:
        return "handle";
    case wo::value::valuetype::real_type:
        return "real";
    case wo::value::valuetype::string_type:
        return "string";
    case wo::value::valuetype::array_type:
        return "array";
    case wo::value::valuetype::mapping_type:
        return "map";
    case wo::value::valuetype::gchandle_type:
        return "gchandle";
    case wo::value::valuetype::closure_type:
        return "closure";
    case wo::value::valuetype::invalid:
        return "nil";
    default:
        return "unknown";
    }
}

wo_integer_t wo_argc(const wo_vm vm)
{
    return reinterpret_cast<const wo::vmbase*>(vm)->tc->integer;
}
wo_result_t wo_ret_bool(wo_vm vm, wo_bool_t result)
{
    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_integer(result ? 1 : 0));
}
wo_result_t wo_ret_int(wo_vm vm, wo_integer_t result)
{
    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_integer(result));
}
wo_result_t wo_ret_real(wo_vm vm, wo_real_t result)
{
    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_real(result));
}
wo_result_t wo_ret_float(wo_vm vm, float result)
{
    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_real((wo_real_t)result));
}
wo_result_t wo_ret_handle(wo_vm vm, wo_handle_t result)
{
    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_handle(result));
}
wo_result_t wo_ret_pointer(wo_vm vm, wo_ptr_t result)
{
    if (result)
        return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_handle((wo_handle_t)result));
    return wo_ret_panic(vm, "Cannot return a nullptr");
}
wo_result_t wo_ret_string(wo_vm vm, wo_string_t result)
{
    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr->set_string(result));
}
wo_result_t wo_ret_gchandle(wo_vm vm, wo_ptr_t resource_ptr, wo_value holding_val, void(*destruct_func)(wo_ptr_t))
{
    WO_VM(vm)->cr->set_gcunit_with_barrier(wo::value::valuetype::gchandle_type);
    auto handle_ptr = wo::gchandle_t::gc_new<wo::gcbase::gctype::eden>(WO_VM(vm)->cr->gcunit);
    handle_ptr->holding_handle = resource_ptr;
    if (holding_val)
    {
        handle_ptr->holding_value.set_val(WO_VAL(holding_val));
        if (handle_ptr->holding_value.is_gcunit())
            handle_ptr->holding_value.gcunit->gc_type = wo::gcbase::gctype::no_gc;
    }
    handle_ptr->destructor = destruct_func;

    return reinterpret_cast<wo_result_t>(WO_VM(vm)->cr);
}
wo_result_t wo_ret_val(wo_vm vm, wo_value result)
{
    wo_assert(result);
    return reinterpret_cast<wo_result_t>(
        WO_VM(vm)->cr->set_val(
            reinterpret_cast<wo::value*>(result)->get()
        ));
}
wo_result_t  wo_ret_ref(wo_vm vm, wo_value result)
{
    wo_assert(result);
    return reinterpret_cast<wo_result_t>(
        WO_VM(vm)->cr->set_ref(
            reinterpret_cast<wo::value*>(result)->get()
        ));
}

wo_result_t wo_ret_dup(wo_vm vm, wo_value result)
{
    auto* val = WO_VAL(result);
    WO_VM(vm)->cr->set_dup(val);
    return 0;
}

wo_result_t wo_ret_throw(wo_vm vm, wo_string_t reason)
{
    WO_VM(vm)->er->set_string(reason);
    wo::exception_recovery::rollback(WO_VM(vm), false);

    return 0;
}

wo_result_t wo_ret_halt(wo_vm vm, wo_string_t reason)
{
    WO_VM(vm)->er->set_string(reason);
    wo::exception_recovery::rollback(WO_VM(vm), true);

    return 0;
}

wo_result_t wo_ret_panic(wo_vm vm, wo_string_t reason)
{
    wo_fail(WO_FAIL_DEADLY, reason);

    return 0;
}

wo_result_t wo_ret_option_int(wo_vm vm, wo_integer_t result)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_integer(result);

    return 0;
}
wo_result_t wo_ret_option_real(wo_vm vm, wo_real_t result)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_real(result);

    return 0;
}
wo_result_t wo_ret_option_float(wo_vm vm, float result)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_real((wo_real_t)result);

    return 0;
}
wo_result_t  wo_ret_option_handle(wo_vm vm, wo_handle_t result)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_handle(result);

    return 0;
}
wo_result_t  wo_ret_option_string(wo_vm vm, wo_string_t result)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_string(result);

    return 0;
}

wo_result_t wo_ret_option_none(wo_vm vm)
{
    auto* wovm = WO_VM(vm);
    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(2);
    return 0;
}

wo_result_t wo_ret_option_ptr(wo_vm vm, wo_ptr_t result)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    if (result)
    {
        structptr->m_values[0].set_integer(1);
        structptr->m_values[1].set_handle((wo_handle_t)result);
    }
    else
        structptr->m_values[0].set_integer(2);

    return 0;
}

wo_result_t wo_ret_option_val(wo_vm vm, wo_value val)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_val(WO_VAL(val));

    return 0;
}

wo_result_t wo_ret_option_ref(wo_vm vm, wo_value val)
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_ref(WO_VAL(val));

    return 0;
}

wo_result_t wo_ret_option_gchandle(wo_vm vm, wo_ptr_t resource_ptr, wo_value holding_val, void(*destruct_func)(wo_ptr_t))
{
    auto* wovm = WO_VM(vm);

    wovm->cr->set_gcunit_with_barrier(wo::value::valuetype::struct_type);
    auto* structptr = wo::struct_t::gc_new<wo::gcbase::gctype::eden>(wovm->cr->gcunit, 2);
    wo::gcbase::gc_write_guard gwg1(structptr);

    structptr->m_values[0].set_integer(1);
    structptr->m_values[1].set_gcunit_with_barrier(wo::value::valuetype::gchandle_type);

    auto handle_ptr = wo::gchandle_t::gc_new<wo::gcbase::gctype::eden>(structptr->m_values[1].gcunit);
    handle_ptr->holding_handle = resource_ptr;
    if (holding_val)
    {
        handle_ptr->holding_value.set_val(WO_VAL(holding_val));
        if (handle_ptr->holding_value.is_gcunit())
            handle_ptr->holding_value.gcunit->gc_type = wo::gcbase::gctype::no_gc;
    }
    handle_ptr->destructor = destruct_func;

    return 0;
}

void wo_coroutine_pauseall()
{
    wo::fvmscheduler::pause_all();
}
void wo_coroutine_resumeall()
{
    wo::fvmscheduler::resume_all();
}

void wo_coroutine_stopall()
{
    wo::fvmscheduler::stop_all();
}

void _wo_check_atexit()
{
    std::shared_lock g1(wo::vmbase::_alive_vm_list_mx);

    do
    {
    waitting_vm_leave:
        for (auto& vm : wo::vmbase::_alive_vm_list)
            if (!(vm->vm_interrupt & wo::vmbase::LEAVE_INTERRUPT))
                goto waitting_vm_leave;
    } while (0);

    // STOP GC
}

void wo_abort_all_vm_to_exit()
{
    // wo_stop used for stop all vm and exit..

    // 1. ABORT ALL VM
    std::shared_lock g1(wo::vmbase::_alive_vm_list_mx);

    for (auto& vm : wo::vmbase::_alive_vm_list)
        vm->interrupt(wo::vmbase::ABORT_INTERRUPT);

    std::atexit(_wo_check_atexit);
}

wo_integer_t wo_lengthof(wo_value value)
{
    auto _rsvalue = WO_VAL(value);
    if (_rsvalue->is_nil())
        return 0;
    if (_rsvalue->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->array);
        return _rsvalue->array->size();
    }
    else if (_rsvalue->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->mapping);
        return _rsvalue->mapping->size();
    }
    else if (_rsvalue->type == wo::value::valuetype::string_type)
    {
        wo::gcbase::gc_read_guard rg1(_rsvalue->string);
        return wo::u8strlen(_rsvalue->string->c_str());
    }
    else if (_rsvalue->type == wo::value::valuetype::struct_type)
    {
        // no need lock for struct's count
        return _rsvalue->structs->m_count;
    }
    else
    {
        wo_fail(WO_FAIL_TYPE_FAIL, "Only 'string','array', 'struct' or 'map' can get length.");
        return 0;
    }
}

wo_bool_t wo_virtual_source(wo_string_t filepath, wo_string_t data, wo_bool_t enable_modify)
{
    return wo::create_virtual_source(wo::str_to_wstr(data), wo::str_to_wstr(filepath), enable_modify);
}

wo_vm wo_create_vm()
{
    return (wo_vm)new wo::vm;
}

wo_vm wo_sub_vm(wo_vm vm, size_t stacksz)
{
    return CS_VM(WO_VM(vm)->make_machine(stacksz));
}

wo_vm wo_gc_vm(wo_vm vm)
{
    return CS_VM(WO_VM(vm)->get_or_alloc_gcvm());
}

void wo_close_vm(wo_vm vm)
{
    delete (wo::vmbase*)vm;
}

void wo_co_yield()
{
    wo::fthread::yield();
}

void wo_co_sleep(double time)
{
    wo::fvmscheduler::wait(time);
}

struct wo_custom_waitter : public wo::fvmscheduler_fwaitable_base
{
    void* m_custom_data;

    bool be_pending()override
    {
        return true;
    }
};

wo_waitter_t wo_co_create_waitter()
{
    wo::shared_pointer<wo_custom_waitter>* cwaitter
        = new wo::shared_pointer<wo_custom_waitter>(new wo_custom_waitter);
    return cwaitter;
}

void wo_co_awake_waitter(wo_waitter_t waitter, void* val)
{
    (*(wo::shared_pointer<wo_custom_waitter>*)waitter)->m_custom_data = val;
    (*(wo::shared_pointer<wo_custom_waitter>*)waitter)->awake();
}

void* wo_co_wait_for(wo_waitter_t waitter)
{
    wo::fthread::wait(*(wo::shared_pointer<wo_custom_waitter>*)waitter);

    auto result = (*(wo::shared_pointer<wo_custom_waitter>*)waitter)->m_custom_data;
    delete (wo::shared_pointer<wo_custom_waitter>*)waitter;

    return result;
}


wo_bool_t _wo_load_source(wo_vm vm, wo_string_t virtual_src_path, wo_string_t src, size_t stacksz)
{
    // 1. Prepare lexer..
    wo::lexer* lex = nullptr;
    if (src)
        lex = new wo::lexer(wo::str_to_wstr(src), virtual_src_path);
    else
        lex = new wo::lexer(virtual_src_path);

    lex->has_been_imported(wo::str_to_wstr(lex->source_file));

    std::forward_list<wo::grammar::ast_base*> m_last_context;
    bool need_exchange_back = wo::grammar::ast_base::exchange_this_thread_ast(m_last_context);
    if (!lex->has_error())
    {
        // 2. Lexer will create ast_tree;
        auto result = wo::get_wo_grammar()->gen(*lex);
        if (result)
        {
            // 3. Create lang, most anything store here..
            wo::lang lang(*lex);

            lang.analyze_pass1(result);
            if (!lang.has_compile_error())
                lang.analyze_pass2(result);

            //result->display();
            if (!lang.has_compile_error())
            {
                wo::ir_compiler compiler;
                lang.analyze_finalize(result, &compiler);

                if (!lang.has_compile_error())
                {
                    compiler.end();
                    ((wo::vm*)vm)->set_runtime(compiler);

                    // OK
                }
            }
        }
    }

    wo::grammar::ast_base::clean_this_thread_ast();

    if (need_exchange_back)
        wo::grammar::ast_base::exchange_this_thread_ast(m_last_context);

    bool compile_has_err = lex->has_error();
    if (compile_has_err)
        WO_VM(vm)->compile_info = lex;
    else
        delete lex;

    return !compile_has_err;
}

wo_bool_t wo_has_compile_error(wo_vm vm)
{
    if (vm && WO_VM(vm)->compile_info && WO_VM(vm)->compile_info->has_error())
        return true;
    return false;
}

wo_string_t wo_get_compile_error(wo_vm vm, _wo_inform_style style)
{
    if (style == WO_DEFAULT)
        style = wo::config::ENABLE_OUTPUT_ANSI_COLOR_CTRL ? WO_NEED_COLOR : WO_NOTHING;

    thread_local std::string _vm_compile_errors;
    _vm_compile_errors = "";
    if (vm && WO_VM(vm)->compile_info)
    {
        auto& lex = *WO_VM(vm)->compile_info;


        std::string src_file_path = "";
        for (auto& err_info : lex.lex_error_list)
        {
            if (src_file_path != err_info.filename)
            {
                if (style == WO_NEED_COLOR)
                    _vm_compile_errors += ANSI_HIR "In file: '" ANSI_RST + (src_file_path = err_info.filename) + ANSI_HIR "'" ANSI_RST "\n";
                else
                    _vm_compile_errors += "In file: '" + (src_file_path = err_info.filename) + "'\n";
            }
            _vm_compile_errors += wo::wstr_to_str(err_info.to_wstring(style & WO_NEED_COLOR)) + "\n";
        }
        /*src_file_path = "";
        for (auto& war_info : lex.lex_warn_list)
        {
            if (src_file_path != war_info.filename)
                wo::wo_stderr << ANSI_HIY "In file: '" ANSI_RST << (src_file_path = war_info.filename) << ANSI_HIY "'" ANSI_RST << wo::wo_endl;
            wo_wstderr << war_info.to_wstring() << wo::wo_endl;
        }*/
    }
    return _vm_compile_errors.c_str();
}

wo_string_t wo_get_runtime_error(wo_vm vm)
{
    return wo_cast_string(CS_VAL(WO_VM(vm)->er));
}

wo_bool_t wo_abort_vm(wo_vm vm)
{
    std::shared_lock gs(wo::vmbase::_alive_vm_list_mx);

    if (wo::vmbase::_alive_vm_list.find(WO_VM(vm)) != wo::vmbase::_alive_vm_list.end())
    {
        return WO_VM(vm)->interrupt(wo::vmbase::vm_interrupt_type::ABORT_INTERRUPT);
    }
    return false;
}

wo_value wo_push_int(wo_vm vm, wo_int_t val)
{
    return CS_VAL((WO_VM(vm)->sp--)->set_integer(val));
}
wo_value wo_push_real(wo_vm vm, wo_real_t val)
{
    return CS_VAL((WO_VM(vm)->sp--)->set_real(val));
}
wo_value wo_push_handle(wo_vm vm, wo_handle_t val)
{
    return CS_VAL((WO_VM(vm)->sp--)->set_handle(val));
}
wo_value wo_push_pointer(wo_vm vm, wo_ptr_t val)
{
    return CS_VAL((WO_VM(vm)->sp--)->set_handle((wo_handle_t)val));
}
wo_value wo_push_gchandle(wo_vm vm, wo_ptr_t resource_ptr, wo_value holding_val, void(*destruct_func)(wo_ptr_t))
{
    auto* csp = WO_VM(vm)->sp--;

    csp->set_gcunit_with_barrier(wo::value::valuetype::gchandle_type);
    auto handle_ptr = wo::gchandle_t::gc_new<wo::gcbase::gctype::eden>(csp->gcunit);
    handle_ptr->holding_handle = resource_ptr;
    if (holding_val)
    {
        handle_ptr->holding_value.set_val(WO_VAL(holding_val));
        if (handle_ptr->holding_value.is_gcunit())
            handle_ptr->holding_value.gcunit->gc_type = wo::gcbase::gctype::no_gc;
    }
    handle_ptr->destructor = destruct_func;

    return CS_VAL(csp);
}
wo_value wo_push_string(wo_vm vm, wo_string_t val)
{
    return CS_VAL((WO_VM(vm)->sp--)->set_string(val));
}
wo_value wo_push_empty(wo_vm vm)
{
    return CS_VAL((WO_VM(vm)->sp--)->set_nil());
}
wo_value wo_push_val(wo_vm vm, wo_value val)
{
    if (val)
        return CS_VAL((WO_VM(vm)->sp--)->set_val(WO_VAL(val)));
    return CS_VAL((WO_VM(vm)->sp--)->set_nil());
}
wo_value wo_push_ref(wo_vm vm, wo_value val)
{
    if (val)
        return CS_VAL((WO_VM(vm)->sp--)->set_ref(WO_VAL(val)));
    return CS_VAL((WO_VM(vm)->sp--)->set_nil());
}
wo_value wo_push_valref(wo_vm vm, wo_value val)
{
    if (val)
        return CS_VAL((WO_VM(vm)->sp--)->set_trans(WO_ORIGIN_VAL(val)));
    return CS_VAL((WO_VM(vm)->sp--)->set_nil());
}


wo_value wo_top_stack(wo_vm vm)
{
    return CS_VAL((WO_VM(vm)->sp - 1));
}
void wo_pop_stack(wo_vm vm)
{
    ++WO_VM(vm)->sp;
}
wo_value wo_invoke_rsfunc(wo_vm vm, wo_int_t vmfunc, wo_int_t argc)
{
    return CS_VAL(WO_VM(vm)->invoke(vmfunc, argc));
}
wo_value wo_invoke_exfunc(wo_vm vm, wo_handle_t exfunc, wo_int_t argc)
{
    return CS_VAL(WO_VM(vm)->invoke(exfunc, argc));
}
wo_value wo_invoke_value(wo_vm vm, wo_value vmfunc, wo_int_t argc)
{
    wo::value* valfunc = WO_VAL(vmfunc);

    if (!vmfunc)
        wo_fail(WO_FAIL_CALL_FAIL, "Cannot call a 'nil' function.");
    else if (valfunc->type == wo::value::valuetype::integer_type)
        return CS_VAL(WO_VM(vm)->invoke(valfunc->integer, argc));
    else if (valfunc->type == wo::value::valuetype::handle_type)
        return CS_VAL(WO_VM(vm)->invoke(valfunc->handle, argc));
    else if (valfunc->type == wo::value::valuetype::closure_type)
        return CS_VAL(WO_VM(vm)->invoke(valfunc->closure, argc));
    else
        wo_fail(WO_FAIL_CALL_FAIL, "Not callable type.");
    return nullptr;
}

wo_value wo_dispatch_rsfunc(wo_vm vm, wo_int_t vmfunc, wo_int_t argc)
{
    auto* vmm = WO_VM(vm);
    vmm->set_br_yieldable(true);
    return CS_VAL(vmm->co_pre_invoke(vmfunc, argc));
}

wo_value wo_dispatch_value(wo_vm vm, wo_value vmfunc, wo_int_t argc)
{
    auto* vmm = WO_VM(vm);
    vmm->set_br_yieldable(true);
    return CS_VAL(vmm->co_pre_invoke(WO_VAL(vmfunc)->closure, argc));
}

wo_value wo_dispatch(wo_vm vm)
{
    if (WO_VM(vm)->env)
    {
        WO_VM(vm)->run();

        if (WO_VM(vm)->veh)
        {
            if (WO_VM(vm)->get_and_clear_br_yield_flag())
                return WO_CONTINUE;

            return reinterpret_cast<wo_value>(WO_VM(vm)->cr);
        }
        else
            return nullptr;
    }
    return nullptr;
}

void wo_break_yield(wo_vm vm)
{
    WO_VM(vm)->interrupt(wo::vmbase::BR_YIELD_INTERRUPT);
}

wo_bool_t wo_load_source_with_stacksz(wo_vm vm, wo_string_t virtual_src_path, wo_string_t src, size_t stacksz)
{
    if (!virtual_src_path)
        virtual_src_path = "__runtime_script__";

    wo_virtual_source(virtual_src_path, src, true);

    return _wo_load_source(vm, virtual_src_path, src, stacksz);
}

wo_bool_t wo_load_file_with_stacksz(wo_vm vm, wo_string_t virtual_src_path, size_t stacksz)
{
    return _wo_load_source(vm, virtual_src_path, nullptr, stacksz);
}

wo_bool_t wo_load_source(wo_vm vm, wo_string_t virtual_src_path, wo_string_t src)
{
    return wo_load_source_with_stacksz(vm, virtual_src_path, src, 0);
}

wo_bool_t wo_load_file(wo_vm vm, wo_string_t virtual_src_path)
{
    return wo_load_file_with_stacksz(vm, virtual_src_path, 0);
}

wo_value wo_run(wo_vm vm)
{
    if (WO_VM(vm)->env)
    {
        WO_VM(vm)->ip = WO_VM(vm)->env->rt_codes;
        WO_VM(vm)->run();
        if (WO_VM(vm)->veh)
            return reinterpret_cast<wo_value>(WO_VM(vm)->cr);
        else
            return nullptr;
    }
    return nullptr;
}

// CONTAINER OPERATE

wo_value wo_struct_get(wo_value value, uint16_t offset)
{
    auto _struct = WO_VAL(value);

    if (_struct->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_struct->type == wo::value::valuetype::struct_type)
    {
        wo::struct_t* struct_impl = _struct->structs;
        wo::gcbase::gc_read_guard gwg1(struct_impl);
        if (offset < struct_impl->m_count)
        {
            auto* result = &struct_impl->m_values[offset];
            if (wo::gc::gc_is_marking())
                struct_impl->add_memo(result);

            return CS_VAL(result);
        }
        else
            wo_fail(WO_FAIL_INDEX_FAIL, "Index out of range.");
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a struct.");
    return nullptr;
}

void wo_arr_resize(wo_value arr, wo_int_t newsz, wo_value init_val)
{
    auto _arr = WO_VAL(arr);

    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_write_guard g1(_arr->array);
        size_t arrsz = _arr->array->size();
        if ((size_t)newsz < arrsz && wo::gc::gc_is_marking())
        {
            for (size_t i = newsz; i < arrsz; ++i)
                _arr->array->add_memo(&(*_arr->array)[i]);
        }
        _arr->array->resize((size_t)newsz, *WO_VAL(init_val));
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");
}

wo_value wo_arr_insert(wo_value arr, wo_int_t place, wo_value val)
{
    auto _arr = WO_VAL(arr);

    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_write_guard g1(_arr->array);

        if ((size_t)place < _arr->array->size())
        {
            auto index = _arr->array->insert(_arr->array->begin() + place, wo::value());
            if (val)
                index->set_val(WO_VAL(val));
            else
                index->set_nil();

            wo_assert(!index->is_ref());
            return CS_VAL(&*index);
        }
        else
            wo_fail(WO_FAIL_INDEX_FAIL, "Index out of range.");
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");

    return nullptr;
}

wo_value wo_arr_add(wo_value arr, wo_value elem)
{
    auto _arr = WO_VAL(arr);

    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_write_guard g1(_arr->array);

        if (elem)
            _arr->array->push_back(*WO_VAL(elem));
        else
            _arr->array->emplace_back(wo::value());

        return reinterpret_cast<wo_value>(&_arr->array->back());
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");

    return nullptr;
}

wo_value wo_arr_get(wo_value arr, wo_int_t index)
{
    auto _arr = WO_VAL(arr);
    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_read_guard g1(_arr->array);

        if ((size_t)index < _arr->array->size())
            return CS_VAL(&(*_arr->array)[index]);
        else
            wo_fail(WO_FAIL_INDEX_FAIL, "Index out of range.");

    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");

    return nullptr;
}
wo_int_t wo_arr_find(wo_value arr, wo_value elem)
{
    auto _arr = WO_VAL(arr);
    auto _aim = WO_VAL(elem);
    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_read_guard g1(_arr->array);

        auto fnd = std::find_if(_arr->array->begin(), _arr->array->end(),
            [&](const wo::value& _elem)->bool
            {
                return _elem.type == _aim->type
                    && _elem.handle == _aim->handle;
            });
        if (fnd != _arr->array->end())
            return fnd - _arr->array->begin();
        return -1;

    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");

    return -1;
}
void wo_arr_remove(wo_value arr, wo_int_t index)
{
    auto _arr = WO_VAL(arr);
    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_write_guard g1(_arr->array);

        if (index != -1)
        {
            if ((size_t)index < _arr->array->size())
            {
                if (wo::gc::gc_is_marking())
                    _arr->array->add_memo(&(*_arr->array)[index]);
                _arr->array->erase(_arr->array->begin() + index);
            }
            else
                wo_fail(WO_FAIL_INDEX_FAIL, "Index out of range.");
        }
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");
}
void wo_arr_clear(wo_value arr)
{
    auto _arr = WO_VAL(arr);
    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_write_guard g1(_arr->array);
        if (wo::gc::gc_is_marking())
            for (auto& val : *_arr->array)
                _arr->array->add_memo(&val);
        _arr->array->clear();
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");
}

wo_bool_t wo_arr_is_empty(wo_value arr)
{
    auto _arr = WO_VAL(arr);
    if (_arr->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_arr->type == wo::value::valuetype::array_type)
    {
        wo::gcbase::gc_write_guard g1(_arr->array);
        return _arr->array->empty();
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not an array.");
    return true;
}

wo_bool_t wo_map_find(wo_value map, wo_value index)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_read_guard g1(_map->mapping);
        if (index)
            return _map->mapping->find(*WO_VAL(index)) != _map->mapping->end();
        return  _map->mapping->find(wo::value()) != _map->mapping->end();
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");

    return false;
}

wo_value wo_map_get_by_default(wo_value map, wo_value index, wo_value default_value)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::value* result = nullptr;
        wo::gcbase::gc_write_guard g1(_map->mapping);
        do
        {
            auto fnd = _map->mapping->find(*WO_VAL(index));
            if (fnd != _map->mapping->end())
                result = &fnd->second;
        } while (false);
        if (!result)
        {
            if (default_value)
                result = &((*_map->mapping)[*WO_VAL(index)] = *WO_VAL(default_value));
            else
            {
                result = &((*_map->mapping)[*WO_VAL(index)]);
                result->set_nil();
            }
        }
        if (wo::gc::gc_is_marking())
            _map->mapping->add_memo(result);

        return CS_VAL(result);
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");

    return nullptr;
}

wo_value wo_map_get_or_default(wo_value map, wo_value index, wo_value default_value)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::value* result = nullptr;
        wo::gcbase::gc_write_guard g1(_map->mapping);
        do
        {
            auto fnd = _map->mapping->find(*WO_VAL(index));
            if (fnd != _map->mapping->end())
                result = &fnd->second;
        } while (false);

        if (!result)
            return CS_VAL(default_value);

        if (wo::gc::gc_is_marking())
            _map->mapping->add_memo(result);

        return CS_VAL(result);
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");

    return nullptr;
}

wo_value wo_map_get(wo_value map, wo_value index)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_read_guard g1(_map->mapping);
        auto fnd = _map->mapping->find(*WO_VAL(index));
        if (fnd != _map->mapping->end())
        {
            if (wo::gc::gc_is_marking())
                _map->mapping->add_memo(&fnd->second);
            return CS_VAL(&fnd->second);
        }
        return nullptr;
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");

    return nullptr;
}

wo_value wo_map_set(wo_value map, wo_value index, wo_value val)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_write_guard g1(_map->mapping);
        wo::value* result;
        if (val)
            result = (*_map->mapping)[*WO_VAL(index)].set_val(WO_VAL(val));
        else
            result = (*_map->mapping)[*WO_VAL(index)].set_nil();

        return CS_VAL(result);
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");

    return nullptr;
}

wo_bool_t wo_map_remove(wo_value map, wo_value index)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_write_guard g1(_map->mapping);
        if (wo::gc::gc_is_marking())
        {
            auto fnd = _map->mapping->find(*WO_VAL(index));
            if (fnd != _map->mapping->end())
            {
                _map->mapping->add_memo(&fnd->first);
                _map->mapping->add_memo(&fnd->second);
            }
        }
        return 0 != _map->mapping->erase(*WO_VAL(index));
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");

    return false;
}
void wo_map_clear(wo_value map)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_write_guard g1(_map->mapping);
        if (wo::gc::gc_is_marking())
        {
            for (auto& kvpair : *_map->mapping)
            {
                _map->mapping->add_memo(&kvpair.first);
                _map->mapping->add_memo(&kvpair.second);
            }
        }
        _map->mapping->clear();
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");
}

wo_bool_t wo_map_is_empty(wo_value map)
{
    auto _map = WO_VAL(map);
    if (_map->is_nil())
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is 'nil'.");
    else if (_map->type == wo::value::valuetype::mapping_type)
    {
        wo::gcbase::gc_write_guard g1(_map->mapping);
        return _map->mapping->empty();
    }
    else
        wo_fail(WO_FAIL_TYPE_FAIL, "Value is not a map.");
    return true;
}

wo_bool_t wo_gchandle_close(wo_value gchandle)
{
    if (WO_VAL(gchandle)->gchandle)
        return WO_VAL(gchandle)->gchandle->close();
    return false;
}

// DEBUGGEE TOOLS
void wo_attach_default_debuggee(wo_vm vm)
{
    wo::default_debuggee* dgb = new wo::default_debuggee;
    if (auto* old_debuggee = WO_VM(vm)->attach_debuggee(dgb))
        delete old_debuggee;
}

wo_bool_t wo_has_attached_debuggee(wo_vm vm)
{
    if (WO_VM(vm)->current_debuggee())
        return true;
    return false;
}

void wo_disattach_debuggee(wo_vm vm)
{
    WO_VM(vm)->attach_debuggee(nullptr);
}

void wo_disattach_and_free_debuggee(wo_vm vm)
{
    if (auto* dbg = WO_VM(vm)->attach_debuggee(nullptr))
        delete dbg;
}

void wo_break_immediately(wo_vm vm)
{
    if (auto* debuggee = dynamic_cast<wo::default_debuggee*>(WO_VM(vm)->current_debuggee()))
        debuggee->breakdown_immediately();
    else
        wo_fail(WO_FAIL_DEBUGGEE_FAIL, "'wo_break_immediately' can only break the vm attached default debuggee.");

}

wo_integer_t wo_extern_symb(wo_vm vm, wo_string_t fullname)
{
    const auto& extern_table = WO_VM(vm)->env->program_debug_info->extern_function_map;
    auto fnd = extern_table.find(fullname);
    if (fnd != extern_table.end())
        return fnd->second;
    return 0;
}

wo_string_t wo_debug_trace_callstack(wo_vm vm, size_t layer)
{
    std::stringstream sstream;
    WO_VM(vm)->dump_call_stack(layer, false, sstream);

    wo_set_string(CS_VAL(WO_VM(vm)->er), "");
    wo_assert(WO_VM(vm)->er->type == wo::value::valuetype::string_type);

    *(WO_VM(vm)->er->string) = sstream.str();
    return WO_VM(vm)->er->string->c_str();
}