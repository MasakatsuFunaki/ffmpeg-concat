#pragma once

// ---------------------------------------------------------------------------
// core/scoped_handle.h – zero-overhead C++20 RAII wrappers for C/FFmpeg/OpenSSL
// handles, C-API out/inout pointer adaptors, and scope guards.
// ---------------------------------------------------------------------------

#include "core/concepts.h"

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

namespace core {

// Stateless functor deleter parameterized by a C free function pointer.
// Dispatches automatically between T** free functions (av_frame_free,
// av_packet_free, avcodec_free_context, avformat_close_input, ...) and T*
// free functions (sws_freeContext, avformat_free_context, EVP_MD_CTX_free).
template <auto FreeFn>
struct FnDeleter {
    template <typename T>
        requires std::invocable<decltype(FreeFn), T**> ||
                 std::invocable<decltype(FreeFn), T*>
    void operator()(T* ptr) const noexcept {
        if (!ptr) {
            return;
        }
        if constexpr (std::invocable<decltype(FreeFn), T**>) {
            FreeFn(&ptr);
        } else {
            FreeFn(ptr);
        }
    }
};

// Zero-overhead unique ownership handle for any C resource with a free function.
template <typename T, auto FreeFn>
using UniqueHandle = std::unique_ptr<T, FnDeleter<FreeFn>>;

// C++20 adaptor matching C++23 std::out_ptr: resets target, hands a T** to a
// C output parameter, and transfers ownership back on scope exit.
template <ResettablePointer Ptr>
class OutPtrAdaptor {
public:
    using pointer = typename Ptr::pointer;

    explicit OutPtrAdaptor(Ptr& target) noexcept : target_(target) {
        target_.reset();
    }

    ~OutPtrAdaptor() noexcept {
        if (raw_) {
            target_.reset(raw_);
        }
    }

    OutPtrAdaptor(const OutPtrAdaptor&) = delete;
    OutPtrAdaptor& operator=(const OutPtrAdaptor&) = delete;
    OutPtrAdaptor(OutPtrAdaptor&&) = delete;
    OutPtrAdaptor& operator=(OutPtrAdaptor&&) = delete;

    operator pointer*() noexcept { return &raw_; }

private:
    Ptr& target_;
    pointer raw_ = nullptr;
};

// C++20 adaptor matching C++23 std::inout_ptr: releases target into a raw
// pointer for C APIs that mutate a T** in place (e.g. av_dict_set,
// avcodec_open2 options) and re-acquires the resulting pointer on scope exit.
template <ResettablePointer Ptr>
class InOutPtrAdaptor {
public:
    using pointer = typename Ptr::pointer;

    explicit InOutPtrAdaptor(Ptr& target) noexcept
        : target_(target), raw_(target.release()) {}

    ~InOutPtrAdaptor() noexcept {
        target_.reset(raw_);
    }

    InOutPtrAdaptor(const InOutPtrAdaptor&) = delete;
    InOutPtrAdaptor& operator=(const InOutPtrAdaptor&) = delete;
    InOutPtrAdaptor(InOutPtrAdaptor&&) = delete;
    InOutPtrAdaptor& operator=(InOutPtrAdaptor&&) = delete;

    operator pointer*() noexcept { return &raw_; }

private:
    Ptr& target_;
    pointer raw_ = nullptr;
};

template <ResettablePointer Ptr>
[[nodiscard]] auto out_ptr(Ptr& target) noexcept {
    return OutPtrAdaptor<Ptr>{target};
}

template <ResettablePointer Ptr>
[[nodiscard]] auto inout_ptr(Ptr& target) noexcept {
    return InOutPtrAdaptor<Ptr>{target};
}

// Generic RAII scope-exit action; call dismiss() to disarm on commit.
template <std::invocable Fn>
    requires std::move_constructible<Fn>
class ScopeGuard {
public:
    explicit ScopeGuard(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(fn)) {}

    ScopeGuard(ScopeGuard&& other) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(other.fn_)), active_(std::exchange(other.active_, false)) {}

    ~ScopeGuard() noexcept {
        if (active_) {
            fn_();
        }
    }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    void dismiss() noexcept { active_ = false; }

private:
    Fn fn_;
    bool active_ = true;
};

template <std::invocable Fn>
[[nodiscard]] auto make_scope_guard(Fn&& fn) {
    return ScopeGuard<std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

}  // namespace core
