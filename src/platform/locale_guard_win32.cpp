#include "platform/locale_guard.h"

#include <clocale>
#include <locale.h>
#include <memory>
#include <string>

namespace platform {

namespace {
constexpr int kConfigThreadLocaleError = -1;
}  // namespace

struct ScopedCNumericLocale::Impl {
    int prev_mode = kConfigThreadLocaleError;
    std::string saved;

    Impl() : prev_mode(_configthreadlocale(_ENABLE_PER_THREAD_LOCALE)) {
        if (prev_mode != kConfigThreadLocaleError) {
            if (const char* current = std::setlocale(LC_NUMERIC, nullptr)) {
                saved = current;
            }
            std::setlocale(LC_NUMERIC, "C");
        }
    }

    ~Impl() noexcept {
        if (!saved.empty()) {
            std::setlocale(LC_NUMERIC, saved.c_str());
        }
        if (prev_mode != kConfigThreadLocaleError) {
            _configthreadlocale(prev_mode);
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
