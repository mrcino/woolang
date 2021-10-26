#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <set>


#include "rs_assert.hpp"
#include "rs_meta.hpp"
#include "rs_gc.hpp"
#include "rs_instruct.hpp"
#include "rs_basic_type.hpp"
#include "rs_ir_compiler.hpp"
#include "rs_vm.hpp"

void example(rs::vmbase* vm)
{
    printf("what the hell!\n");

    vm->cr->integer = 9926;
    vm->cr->type = rs::value::valuetype::integer_type;
}

void veh_exception_test(rs::vmbase* vm)
{
    rs_fail("veh_exception_test.");
}

#define RS_IMPL
#include "rs.h"

int main()
{
    using namespace rs;
    using namespace rs::opnum;

    std::cout << "RestorableScene ver." << rs_version() << " " << rs_compile_date() << std::endl;
    std::cout << rs_compile_date() << std::endl;


    vm vmm;
    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c9;                         // 
    c9.jmp(tag("demo_main"));               //      jmp     demo_main;
    c9.tag("demo_func_01");                 //  :demo_func_01
    c9.mov(reg(reg::t0), imm(666));         //      mov     t0,     666;
    c9.addi(reg(reg::t0), imm(233));        //      addi    t0,     233; 
    c9.ret();                               //      ret
    c9.tag("demo_main");                    //  :demo_main
    c9.call(tag("demo_func_01"));           //      call    demo_func_01;
    c9.end();                               //      end

    vmm.set_runtime(c9.finalize());
    vmm.run();

    rs_test(vmm.stackbuttom == vmm.env.stack_begin);
    rs_test(vmm.stacktop == vmm.env.stack_begin);
    rs_test(vmm.cr->type == value::valuetype::is_ref && vmm.cr->get()->integer == 233+666);
    ///////////////////////////////////////////////////////////////////////////////////////

    ///////////////////////////////////////////////////////////////////////////////////////
    /*
    ir_compiler c8;                                     // 
    c8.call(&veh_exception_test);
    c8.end();                                          //      end

    vmm.set_runtime(c8.finalize());
    vmm.run();
    */
    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c7;                                     // 
    c7.call(&example);
    c7.end();                                          //      end

    vmm.set_runtime(c7.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::integer_type && vmm.cr->integer == 9926);
    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c6;                                     // 
    c6.land(imm(1), imm(1));
    c6.lor(imm(0), reg(reg::cr));
    c6.lnot(reg(reg::cr));
    c6.end();                                          //      end

    vmm.set_runtime(c6.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::integer_type && vmm.cr->integer == 0);

    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c5;                                     // 
    c5.psh(imm("Helloworld"));
    c5.psh(imm(125.56));
    c5.psh(imm(2133));
    c5.lds(reg(reg::cr), imm(0));
    c5.end();                                           //      end

    vmm.set_runtime(c5.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::string_type && *vmm.cr->string == "Helloworld");

    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c4;                                     // 
    c4.psh(imm(1));
    c4.psh(imm(2));
    c4.psh(imm(3));
    c4.psh(imm(4));
    c4.pop(2);
    c4.pop(reg(reg::cr));
    c4.end();                                           //      end

    vmm.set_runtime(c4.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::integer_type && vmm.cr->integer == 2);

    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c3;                                     // 
    c3.mov(reg(reg::cr), imm("hello,"));                //      mov     cr,   "hello,";
    c3.adds(reg(reg::cr), imm("world!"));               //      adds    cr,   "world!";
    c3.end();                                           //      end

    vmm.set_runtime(c3.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::string_type && *vmm.cr->string == "hello,world!");
    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c2;                                     // fast stack addressing:    
    c2.pshr(imm(2333));                                 //      psh     2333;
    c2.mov(reg(reg::cr), reg(reg::bp_offset(0)));       //      mov     cr,   [bp+0]
    c2.end();                                           //      end

    vmm.set_runtime(c2.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::integer_type && vmm.cr->integer == 2333);
    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c0;                                      // fast stack addressing:    
    c0.pshr(imm(2333.0456));                             //      pshr   2333.0456;
    c0.movr2i(reg(reg::cr), reg(reg::bp_offset(0)));     //      movr2i cr,   [bp+0]
    c0.end();                                            //      end

    vmm.set_runtime(c0.finalize());
    vmm.run();

    rs_test(vmm.cr->type == value::valuetype::integer_type && vmm.cr->integer == 2333);
    ///////////////////////////////////////////////////////////////////////////////////////

    ir_compiler c1;
    c1.psh(imm(0));                                     //      psh     0
    c1.set(reg(reg::bp_offset(0)), imm(0));             //      set     [bp+0],  0              int  i=0
    c1.tag("loop_begin");                               //  :loop_begin
    c1.lti(reg(reg::bp_offset(0)), imm(100000000));     //      lti     [bp+0],   100000000     while i < 100000000
    c1.jf(tag("loop_end"));                             //      jf      loop_end                {
    c1.addi(reg(reg::bp_offset(0)), imm(1));            //      addi    [bp+0],  1                  i+=1;
    c1.jmp(tag("loop_begin"));                          //      jmp     loop_begin              }
    c1.tag("loop_end");                                 //  :loop_end
    c1.pop(1);                                     //      pop     1
    c1.end();                                           //      end;


    while (true)
    {
        vmm.set_runtime(c1.finalize());

        auto beg = clock();
        vmm.run();
        auto end = clock();

        std::cout << (end - beg) << std::endl;
    }

    ///////////////////////////////////////////////////////////////////////////////////////

}