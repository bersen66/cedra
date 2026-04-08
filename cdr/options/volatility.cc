#include <cdr/base/hardware_interference_size.h>
#include <cdr/calendar/date.h>
#include <cdr/options/volatility.h>

#include <cstring>
#include <new>

#include <cdr/base/aligned_alloc.h>

namespace stdr = std::ranges;

namespace cdr {

VolatilitySurface::VolatilitySurface(void* incremented_base) {
    CDR_CHECK(incremented_base != nullptr) << "VolatilitySurface cannot be nullptr.";
    header_ptr_ = static_cast<SurfaceHeader*>(incremented_base);
    auto* base_ptr = static_cast<std::byte*>(incremented_base);
    strikes_ptr_ = reinterpret_cast<f64*>(base_ptr + header_ptr_->strikes_byte_offset);
    dates_ptr_ = reinterpret_cast<f64*>(base_ptr + header_ptr_->dates_byte_offset);
    volatility_ptr_ = reinterpret_cast<f64*>(base_ptr + header_ptr_->volatility_byte_offset);
}

VolatilitySurface::VolatilitySurface(const VolatilitySurface& other) noexcept {
    other.Header().reference_count.fetch_add(1, std::memory_order_acq_rel);

    header_ptr_ = other.header_ptr_;
    strikes_ptr_ = other.strikes_ptr_;
    dates_ptr_ = other.dates_ptr_;
    volatility_ptr_ = other.volatility_ptr_;
}

VolatilitySurface& VolatilitySurface::operator=(const VolatilitySurface& other) noexcept {
    if (&other != this) [[likely]] {
        if (other.header_ptr_) {
            other.Header().reference_count.fetch_add(1, std::memory_order_acq_rel);
        }
        this->Reclaim();

        header_ptr_ = other.header_ptr_;
        strikes_ptr_ = other.strikes_ptr_;
        dates_ptr_ = other.dates_ptr_;
        volatility_ptr_ = other.volatility_ptr_;
    }

    return *this;
}

VolatilitySurface::VolatilitySurface(VolatilitySurface&& other) noexcept
    : header_ptr_(std::exchange(other.header_ptr_, nullptr)),
      strikes_ptr_(std::exchange(other.strikes_ptr_, nullptr)),
      dates_ptr_(std::exchange(other.dates_ptr_, nullptr)),
      volatility_ptr_(std::exchange(other.volatility_ptr_, nullptr)) {
}

VolatilitySurface& VolatilitySurface::operator=(VolatilitySurface&& other) noexcept {
    if (&other != this) [[likely]] {
        this->Reclaim();
        header_ptr_ = std::exchange(other.header_ptr_, nullptr);
        strikes_ptr_ = std::exchange(other.strikes_ptr_, nullptr);
        dates_ptr_ = std::exchange(other.dates_ptr_, nullptr);
        volatility_ptr_ = std::exchange(other.volatility_ptr_, nullptr);
    }
    return *this;
}

VolatilitySurface::~VolatilitySurface() {
    if (header_ptr_) {
        this->Reclaim();
    }
}

inline f64 Lerp(const f64 x, const f64 x1, const f64 y1, const f64 x2, const f64 y2) noexcept {
    if (std::abs(x2 - x1) < 1e-9) return y1;
    const f64 t = (x - x1) / (x2 - x1);
    return y1 + t * (y2 - y1);
}

f64 VolatilitySurface::Volatility(const DateType& date, f64 strike) const noexcept {
    if (!header_ptr_) [[unlikely]] {
        return 0.0;
    }

    const f64 target_t = Period{header_ptr_->today, date}.Act365();

    const auto strikes = Strikes();
    const auto dates = Dates();
    const u64 n_strikes = header_ptr_->strikes_size;

    auto it_date = stdr::lower_bound(dates, target_t);
    u64 d1{};
    u64 d2{};

    if (it_date == dates.begin()) {
        d1 = d2 = 0;
    } else if (it_date == dates.end()) {
        d1 = d2 = dates.size() - 1;
    } else {
        d2 = std::distance(dates.begin(), it_date);
        d1 = d2 - 1;
    }

    auto it_strike = stdr::lower_bound(strikes, strike);
    u64 s1, s2;

    if (it_strike == strikes.begin()) {
        s1 = s2 = 0;
    } else if (it_strike == strikes.end()) {
        s1 = s2 = strikes.size() - 1;
    } else {
        s2 = std::distance(strikes.begin(), it_strike);
        s1 = s2 - 1;
    }

    const f64* row1 = volatility_ptr_ + (d1 * n_strikes);
    const f64* row2 = volatility_ptr_ + (d2 * n_strikes);

    f64 vol_at_d1{};
    f64 vol_at_d2{};

    if (s1 == s2) [[unlikely]] {
        vol_at_d1 = row1[s1];
    } else {
        vol_at_d1 = Lerp(strike, strikes[s1], row1[s1], strikes[s2], row1[s2]);
    }

    if (d1 == d2) [[unlikely]] {
        return vol_at_d1;
    }

    
    if (s1 == s2) [[unlikely]] {
        vol_at_d2 = row2[s1];
    } else {
        vol_at_d2 = Lerp(strike, strikes[s1], row2[s1], strikes[s2], row2[s2]);
    }

    return Lerp(target_t, dates[d1], vol_at_d1, dates[d2], vol_at_d2);
}

bool VolatilitySurface::Reclaim() noexcept {
    if (!header_ptr_) {
        return false;
    }

    if (const u64 remaining = header_ptr_->reference_count.fetch_sub(1, std::memory_order_acq_rel); remaining == 1) {
        cdr::AlignedFree(const_cast<SurfaceHeader*>(header_ptr_));
        header_ptr_ = nullptr;
        return true;
    }

    header_ptr_ = nullptr;
    return false;
}

Expect<void, Error> VolatilitySurfaceProvider::AddPillar(const DateType& date, const StrikeType strike,
                                                         const VolatilityType volatility) {
    if (date < today_) {
        return ErrorDateInAPast();
    }

    strikes_.push_back(strike);
    pillars_[date][strike] = volatility;
    dates_.push_back(Period{today_, date}.Act365());

    return Ok();
}

inline u64 AlignToCacheLine(const u64 size) noexcept {
    static constexpr u64 cache_line_size = kDestructiveInterferenceSize - 1;
    return (size + cache_line_size) & ~cache_line_size;
}

Expect<void, Error> VolatilitySurfaceProvider::UpdateSnapshot() noexcept {
    // Sort and remove duplicates in dates
    stdr::sort(dates_);
    dates_.erase(stdr::unique(dates_).begin(), dates_.end());

    // Sort and remove duplicates in strikes
    stdr::sort(strikes_);
    strikes_.erase(stdr::unique(strikes_).begin(), strikes_.end());

    // Compute sizes
    constexpr u64 header_size = sizeof(VolatilitySurface::SurfaceHeader);
    const u64 strikes_size_bytes = strikes_.size() * sizeof(f64);
    const u64 dates_size_bytes = dates_.size() * sizeof(f64);
    const u64 matrix_size_bytes = dates_.size() * strikes_.size() * sizeof(f64);

    // Compute aligned offsets
    const u64 strikes_offset = AlignToCacheLine(header_size);
    const u64 dates_offset = AlignToCacheLine(strikes_offset + strikes_size_bytes);
    const u64 matrix_offset = AlignToCacheLine(dates_offset + dates_size_bytes);
    const u64 total_size = AlignToCacheLine(matrix_offset + matrix_size_bytes);

    // Allocate buffer
    std::byte* buffer_ptr =
        static_cast<std::byte*>(cdr::AlignedAlloc(kDestructiveInterferenceSize, total_size));
    if (!buffer_ptr) [[unlikely]] {
        return ErrorNoMemory();
    }

    // Fill header
    VolatilitySurface::SurfaceHeader* header = new (buffer_ptr) VolatilitySurface::SurfaceHeader;
    header->magic_number = VolatilitySurface::kMagicNumber;
    header->today = today_;
    header->strikes_size = strikes_.size();
    header->dates_size = dates_.size();
    header->reference_count.store(1, std::memory_order_relaxed);

    header->strikes_byte_offset = strikes_offset;
    header->dates_byte_offset = dates_offset;
    header->volatility_byte_offset = matrix_offset;
    header->total_size_in_bytes = total_size;

    // Pointers to payload areas
    f64* strikes_ptr = reinterpret_cast<f64*>(buffer_ptr + strikes_offset);
    f64* dates_ptr = reinterpret_cast<f64*>(buffer_ptr + dates_offset);
    f64* volatility_matrix_ptr = reinterpret_cast<f64*>(buffer_ptr + matrix_offset);

    // Copy strikes and dates
    std::memcpy(strikes_ptr, strikes_.data(), strikes_size_bytes);
    std::memcpy(dates_ptr, dates_.data(), dates_size_bytes);

    // Fill volatility matrix (unchanged logic)
    const u64 strikes_size = strikes_.size();
    const u64 dates_size = dates_.size();
    auto pillars_iter = pillars_.cbegin();

    for (u64 date_idx = 0; date_idx < dates_size; ++date_idx, ++pillars_iter) {
        const auto& date_strikes = pillars_iter->second;
        for (u64 strike_idx = 0; strike_idx < strikes_size; ++strike_idx) {
            const u64 matrix_idx = strikes_size * date_idx + strike_idx;
            auto pillar_iter = date_strikes.lower_bound(strikes_[strike_idx]);

            f64 output_value = 0.0;
            if (pillar_iter == date_strikes.end()) {
                output_value = std::prev(pillar_iter)->second;
            } else if (pillar_iter->first == strikes_[strike_idx] || pillar_iter == date_strikes.begin()) {
                output_value = pillar_iter->second;
            } else {
                auto [prev_strike, prev_vol] = *std::prev(pillar_iter);
                auto [curr_strike, curr_vol] = *pillar_iter;
                output_value = Lerp(strikes_[strike_idx], prev_strike, prev_vol, curr_strike, curr_vol);
            }
            volatility_matrix_ptr[matrix_idx] = output_value;
        }
    }

    // Swap the new surface
    void* old_surface_ptr = surface_ptr_.exchange(buffer_ptr, std::memory_order_acq_rel);
    if (old_surface_ptr) {
        VolatilitySurface::SurfaceHeader* old_header = static_cast<VolatilitySurface::SurfaceHeader*>(old_surface_ptr);
        if (old_header->reference_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cdr::AlignedFree(old_surface_ptr);
        }
    }
    return Ok();
}

Expect<VolatilitySurface, Error> VolatilitySurfaceProvider::ProvideSnapshot() const noexcept {
    void* data_ptr = surface_ptr_.load(std::memory_order_acquire);
    if (!data_ptr) [[unlikely]] {
        return ErrorNoData();
    }
    static_cast<VolatilitySurface::SurfaceHeader*>(data_ptr)->reference_count.fetch_add(1, std::memory_order_acq_rel);

    return cdr::Ok<VolatilitySurface>(std::in_place, data_ptr);
}

VolatilitySurfaceProvider::~VolatilitySurfaceProvider() {
    void* surface_ptr = surface_ptr_.exchange(nullptr, std::memory_order_acq_rel);
    if (!surface_ptr) [[unlikely]] {
        return;
    }

    VolatilitySurface::SurfaceHeader* header = static_cast<VolatilitySurface::SurfaceHeader*>(surface_ptr);
    const u64 old_refs = header->reference_count.fetch_sub(1, std::memory_order_acq_rel);
    if (old_refs == 1) {
        cdr::AlignedFree(surface_ptr);
    }
}

}  // namespace cdr