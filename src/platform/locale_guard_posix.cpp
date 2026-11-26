#include "platform/locale_guard.h"

#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

#include <memory>

namespace platform {

struct ScopedCNumericLocale::Impl {
    static inline const locale_t kNullLocale = static_cast<locale_t>(0);

    locale_t c_loc = kNullLocale;
    locale_t prev = kNullLocale;

    Impl() noexcept : c_loc(::newlocale(LC_NUMERIC_MASK, "C", kNullLocale)) {
        if (c_loc != kNullLocale) {
            prev = ::uselocale(c_loc);
        }
    }

    ~Impl() noexcept {
        if (prev != kNullLocale) {
            ::uselocale(prev);
        }
        if (c_loc != kNullLocale) {
            ::freelocale(c_loc);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

ScopedCNumericLocale::ScopedCNumericLocale() : impl_(std::make_unique<Impl>()) {}

ScopedCNumericLocale::~ScopedCNumericLocale() noexcept = default;

ScopedCNumericLocale::ScopedCNumericLocale(ScopedCNumericLocale&&) noexcept = default;

ScopedCNumericLocale& ScopedCNumericLocale::operator=(ScopedCNumericLocale&&) noexcept = default;

}  // namespace platform
