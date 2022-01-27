#include "rs_roroutine_fiber.hpp"
#include "rs_assert.hpp"
#include "rs_memory.hpp"

#ifdef RS_PLATRORM_OS_WINDOWS
#   include <Windows.h>
#else
#   include <ucontext.h>
#endif

constexpr size_t FIBER_DEFAULT_STACK_SZ = 16384;

namespace rs
{
#ifdef RS_PLATRORM_OS_WINDOWS
    fiber::fiber()
    {
        // Make a thread to fiber
        m_context = ConvertThreadToFiber(nullptr);
        if (!m_context)
            m_context = GetCurrentFiber();

        m_pure_fiber = false;
        rs_assert(m_context);
    }
    fiber::fiber(void(*fiber_entry)(void*), void* argn)
    {
        // Create a new fiber
        m_context = CreateFiber(FIBER_DEFAULT_STACK_SZ, fiber_entry, argn);
        m_pure_fiber = true;

        rs_assert(m_context);
    }
    fiber::~fiber()
    {
        if (m_pure_fiber)
            DeleteFiber(m_context);
    }
    bool fiber::switch_to(fiber* another_fiber)
    {
        if (another_fiber->m_context)
        {
            SwitchToFiber(another_fiber->m_context);
            return true;
        }
        return false;
    }
#else

    void fiber::_fiber_func_invoker(
        uint32_t aimf_lo32,
        uint32_t aimf_hi32,
        uint32_t argp_lo32,
        uint32_t argp_hi32)
    {
        auto aimf = (void(*)(void*))(((uint64_t)aimf_lo32) | (((uint64_t)aimf_hi32) << 32));
        auto argp = (void*)(((uint64_t)argp_lo32) | (((uint64_t)argp_hi32) << 32));

        aimf(argp);
    }

    fiber::fiber()
    {
        // Make a thread to fiber
        m_fiber_stack = nullptr;
        m_pure_fiber = false;
    }
    fiber::fiber(void(*fiber_entry)(void*), void* argn)
    {
        // Create a new fiber
        getcontext(&m_context);

        m_fiber_stack = alloc64(FIBER_DEFAULT_STACK_SZ);
        m_context.uc_stack.ss_sp = m_fiber_stack;
        m_context.uc_stack.ss_size = FIBER_DEFAULT_STACK_SZ;
        m_context.uc_link = nullptr;

        uint32_t fib_entry_lo32 = (uint64_t)fiber_entry;
        uint32_t fib_entry_hi32 = ((uint64_t)fiber_entry) >> 32;

        uint32_t arg_lo32 = (uint64_t)argn;
        uint32_t arg_hi32 = ((uint64_t)argn) >> 32;

        makecontext(&m_context, (void(*)(void))_fiber_func_invoker, 4,
            fib_entry_lo32, fib_entry_hi32,
            arg_lo32, arg_hi32);

        m_pure_fiber = true;

        rs_assert(m_fiber_stack);
    }
    fiber::~fiber()
    {
        if (m_pure_fiber)
        {
            free64(m_fiber_stack);
        }
    }
    bool fiber::switch_to(fiber* another_fiber)
    {
        swapcontext(&m_context, &another_fiber->m_context);
        return true;
    }

#endif

}