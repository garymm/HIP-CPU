/* -----------------------------------------------------------------------------
 * Copyright (c) 2020 Advanced Micro Devices, Inc. All Rights Reserved.
 * See 'LICENSE' in the project root for license information.
 * -------------------------------------------------------------------------- */
#pragma once

#if !defined(__HIP_CPU_RT__)
    #error Private HIP-CPU RT implementation headers must not be included directly.
#endif

#include "event.hpp"
#include "helpers.hpp"
#include "stream.hpp"
#include "task.hpp"
#include "../../../../include/hip/hip_defines.h"
#include "../../../../include/hip/hip_enums.h"

#include <algorithm>
#include <execution>
#include <forward_list>
#include <future>
#include <iterator>
#include <limits>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable:4251) // TODO: temporary.
#endif

namespace hip
{
    namespace detail
    {
        // BEGIN CLASS RUNTIME
        class __HIP_API__ Runtime final {
            // NESTED TYPES
            struct Cleaner_ {
                // CREATORS
                Cleaner_() noexcept;
                ~Cleaner_();
            };

            // DATA - STATICS
            inline static Stream internal_stream_{};
            inline static std::forward_list<Stream> streams_{};
            inline static const Cleaner_ cleaner{};

            // IMPLEMENTATION - STATICS
            static
            hipError_t& last_error_() noexcept;
            static
            std::thread& processor_();
            static
            void wait_all_streams_();
        public:
            // STATICS
            static
            std::future<void> destroy_stream_async(Stream* s);
            static
            hipError_t last_error() noexcept;
            static
            std::future<Stream*> make_stream_async();
            static
            Stream* null_stream();
            static
            void push_task(Event* p, Stream* s);
            static
            hipError_t set_last_error(hipError_t e) noexcept;
            static
            void synchronize();
        };

        // NESTED TYPES
        // BEGIN CLASS RUNTIME::CLEANER_
        inline
        Runtime::Cleaner_::Cleaner_() noexcept
        {
            last_error_();
        }

        inline
        Runtime::Cleaner_::~Cleaner_()
        {
            Task poison{[](auto&& p) { p = true; }};
            auto fut{poison.get_future()};

            internal_stream_.apply([&poison](auto&& x) {
                x.push_back(std::move(poison));
            });

            if (processor_().joinable()) processor_().join();

            fut.wait();
        }
        // END CLASS RUNTIME::CLEANER_

        // IMPLEMENTATION - STATICS
        inline
        hipError_t& Runtime::last_error_() noexcept
        {
            static thread_local hipError_t r{hipSuccess};

            return r;
        }

        inline
        std::thread& Runtime::processor_()
        {
            static std::thread r{[]() {
                std::vector<Task> t;
                do {
                    using T = typename Stream::value_type;

                    T t{};
                    internal_stream_.apply([&t](auto&& ts) {
                        t = std::move(ts);
                    });

                    bool maybe_poison{};
                    for (auto&& x : t) {
                        x(maybe_poison);

                        if (maybe_poison) return wait_all_streams_();
                    }

                    bool backoff{true};
                    null_stream()->apply([&backoff](auto&& ts) {
                        backoff = std::empty(ts);
                    });

                    if (backoff) {
                        backoff = std::none_of(
                            std::begin(streams_),
                            std::end(streams_),
                            [](auto&& x) {
                            bool r{};
                            x.apply([&r](auto&& ts) { r = !std::empty(ts); });

                            return r;
                        });
                    }

                    if (!backoff) wait_all_streams_();
                    else {
                        static std::minstd_rand g{std::random_device{}()};
                        static std::uniform_int_distribution<std::uint32_t> d{
                            3, 1031};

                        for (auto i = 0u, n = d(g); i != n; ++i) {
                            pause_or_yield();
                        }
                    }
                } while (true);
            }};

            return r;
        }

        inline
        void Runtime::wait_all_streams_()
        {
            if (processor_().joinable()) processor_().detach();

            using T = typename Stream::value_type;

            auto f{std::async(std::launch::async, []() {
                T t{};

                null_stream()->apply([&t](auto&& ts) { t = std::move(ts); });

                for (auto&& x : t) {
                    bool nop{};
                    x(nop);
                };
            })};

            std::for_each(
                std::execution::par,
                std::begin(streams_),
                std::end(streams_),
                [](auto&& x) {
                T t{};

                x.apply([&t](auto&& ts) { t = std::move(ts); });

                for (auto&& y : t) { bool nop{}; y(nop); }
            });

            return f.wait();
        }

        // STATICS
        inline
        std::future<void> Runtime::destroy_stream_async(Stream* s)
        {
            Task r{[=](auto&&) {
                streams_.remove_if([=](auto&& x) { return &x == s; });
            }};
            auto fut{r.get_future()};

            internal_stream_.apply([&r](auto&& ts) {
                ts.push_back(std::move(r));
            });

            return fut;
        }

        inline
        hipError_t Runtime::last_error() noexcept
        {
            return last_error_();
        }

        inline
        std::future<Stream*> Runtime::make_stream_async()
        {   // TODO: use smart pointer.
            auto p{new std::promise<Stream*>};
            auto fut{p->get_future()};

            internal_stream_.apply([=](auto&& ts) {
                ts.emplace_back(Task{[=](auto&&) mutable {
                    p->set_value(&streams_.emplace_front());
                }});
            });

            if (processor_().joinable()) processor_().detach();

            return fut;
        }

        inline
        Stream* Runtime::null_stream()
        {
            static auto& r{streams_.emplace_front()};

            if (processor_().joinable()) processor_().detach();

            return &r;
        }

        inline
        void Runtime::push_task(Event* p, Stream* s)
        {
            if (!s) {
                s = null_stream();
                mark_as_all_synchronising(*p);
            }

            Task r{[=](auto&&) { update_timestamp(*p); }};
            add_done_signal(*p, r.get_future());

            s->apply([&r](auto&& ts) { ts.push_back(std::move(r)); });
        }

        inline
        hipError_t Runtime::set_last_error(hipError_t e) noexcept
        {
            return std::exchange(last_error_(), e);
        }

        inline
        void Runtime::synchronize()
        {   // TODO: redo, this induces ordering requirements on the processor.
            Task r{[](auto&&) { wait_all_streams_(); }};
            auto f{r.get_future()};

            internal_stream_.apply([&r](auto&& ts) {
                ts.push_back(std::move(r));
            });

            if (processor_().joinable()) processor_().detach();

            return f.wait();
        }
        // END CLASS RUNTIME
    } // Namespace hip::detail.
} // Namespace hip.

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif