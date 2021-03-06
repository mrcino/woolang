#pragma once
#include "wo_memory.hpp"
#include "wo_assert.hpp"

#include <shared_mutex>
#include <atomic>


namespace wo
{
    namespace gc
    {
        void gc_start();
        bool gc_is_marking();
    }

    template<typename NodeT>
    struct atomic_list
    {
        std::atomic<NodeT*> last_node = nullptr;

        void add_one(NodeT* node)
        {
            NodeT* last_last_node = last_node;// .exchange(node);
            do
            {
                node->last = last_last_node;
            } while (!last_node.compare_exchange_weak(last_last_node, node));
        }

        NodeT* pick_all()
        {
            NodeT* result = nullptr;
            result = last_node.exchange(nullptr);

            return result;
        }
    };

    struct value;

    struct gcbase
    {
        inline static atomic_list<gcbase> eden_age_gcunit_list;
        inline static atomic_list<gcbase> young_age_gcunit_list;
        inline static atomic_list<gcbase> old_age_gcunit_list;

        // TODO : _shared_spin need to remake.
        struct _shared_spin
        {
            std::atomic_flag _sspin_write_flag = {};
            std::atomic_uint _sspin_read_flag = {};

            inline void lock() noexcept
            {
                while (_sspin_write_flag.test_and_set());
                while (_sspin_read_flag);
            }
            inline void unlock() noexcept
            {
                _sspin_write_flag.clear();
            }
            inline void lock_shared() noexcept
            {
                while (_sspin_write_flag.test_and_set());
                _sspin_read_flag.fetch_add(1);
                _sspin_write_flag.clear();
            }
            inline void unlock_shared() noexcept
            {
                _sspin_read_flag.fetch_sub(1);
            }
        };

        struct gc_read_guard
        {
            gcbase* _mx;
            inline gc_read_guard(gcbase* sp)
                :_mx(sp)
            {
                _mx->read();
            }
            inline ~gc_read_guard()
            {
                _mx->read_end();
            }
        };

        struct gc_write_guard
        {
            gcbase* _mx;
            inline gc_write_guard(gcbase* sp)
                :_mx(sp)
            {
                _mx->write();
            }
            inline ~gc_write_guard()
            {
                _mx->write_end();
            }
        };

        enum class gctype : uint8_t
        {
            no_gc,

            eden,
            young,
            old,
        };
        enum class gcmarkcolor : uint8_t
        {
            no_mark,
            self_mark,
            full_mark,
        };

        gctype gc_type = gctype::no_gc;
        gcmarkcolor gc_mark_color = gcmarkcolor::no_mark;
        uint16_t gc_mark_version = 0;
        uint16_t gc_mark_alive_count = 0;

        inline void gc_mark(uint16_t version, gcmarkcolor color)
        {
            if (gc_mark_version != version)
            {
                // first mark
                gc_mark_version = version;
                gc_mark_color = color;

                gc_mark_alive_count++;
            }
            else
            {
                gcmarkcolor aim_color = color;
                while (aim_color > gc_mark_color)
                {
                    static_assert(sizeof(std::atomic<gcmarkcolor>) == sizeof(gcmarkcolor));

                    gcmarkcolor old_color = ((std::atomic<gcmarkcolor>&)gc_mark_color).exchange(aim_color);
                    if (aim_color < old_color)
                    {
                        aim_color = old_color;
                    }
                }
            }
        }

        inline gcmarkcolor gc_marked(uint16_t version)
        {
            if (version == gc_mark_version)
            {
                return gc_mark_color;
            }
            return gc_mark_color = gcmarkcolor::no_mark;
        }

        using rw_lock = _shared_spin;
        rw_lock gc_read_write_mx;
        inline void write()
        {
            gc_read_write_mx.lock();
        }
        inline void write_end()
        {
            gc_read_write_mx.unlock();
        }
        inline void read()
        {
            gc_read_write_mx.lock_shared();
        }
        inline void read_end()
        {
            gc_read_write_mx.unlock_shared();
        }

        // used in linklist;
        gcbase* last = nullptr;

        struct memo_unit
        {
            gcbase* gcunit;
            memo_unit* last;
        };

        std::atomic<memo_unit*> m_memo = nullptr;

        memo_unit* pick_memo()
        {
            return m_memo.exchange(nullptr);
        }
        void add_memo(const value* val);

        virtual ~gcbase() 
        {
            auto memoptr = pick_memo();
            while (memoptr)
            {
                auto* curmemo = memoptr;
                memoptr = memoptr->last;

                delete curmemo;
            }
        };

        inline static std::atomic_uint32_t gc_new_count = 0;
    };

    template<typename T>
    struct gcunit : public gcbase, public T
    {
        template<gcbase::gctype AllocType, typename ... ArgTs >
        static gcunit<T>* gc_new(gcbase*& write_aim, ArgTs && ... args)
        {
            ++gc_new_count;

            auto* created_gcnuit = new (alloc64(sizeof(gcunit<T>)))gcunit<T>(args...);
            created_gcnuit->gc_type = AllocType;

            *reinterpret_cast<std::atomic<gcbase*>*>(&write_aim) = created_gcnuit;

            switch (AllocType)
            {
            case wo::gcbase::gctype::no_gc:
                /* DO NOTHING */
                break;
            case wo::gcbase::gctype::eden:
                eden_age_gcunit_list.add_one(created_gcnuit);
                break;
            case wo::gcbase::gctype::young:
                young_age_gcunit_list.add_one(created_gcnuit);
                break;
            case wo::gcbase::gctype::old:
                old_age_gcunit_list.add_one(created_gcnuit);
                break;
            default:
                // wo_error("Unknown gc type.");
                break;
            }

            return created_gcnuit;
        }

        template<typename ... ArgTs>
        gcunit(ArgTs && ... args) : T(args...)
        {

        }

        template<typename TT>
        inline gcunit& operator = (TT&& _val)
        {
            (*(T*)this) = _val;
            return *this;
        }
    };


}