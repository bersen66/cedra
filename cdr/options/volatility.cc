#include <cdr/base/hardware_interference_size.h>
#include <cdr/calendar/date.h>
#include <cdr/options/volatility.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <cdr/types/errors.h>
#include <cdr/types/expect.h>

#include <cdr/base/aligned_alloc.h>

namespace stdr = std::ranges;

namespace cdr {

VolatilitySurface::VolatilitySurface(void* incremented_base) {
    CDR_CHECK(incremented_base != nullptr) << "VolatilitySurface cannot be nullptr.";
    header_ptr_ = static_cast<SurfaceHeader*>(incremented_base); auto* base_ptr = static_cast<std::byte*>(incremented_base);
    strikes_ptr_ = reinterpret_cast<f64*>(base_ptr + header_ptr_->strikes_byte_offset);
    dates_ptr_ = reinterpret_cast<f64*>(base_ptr + header_ptr_->dates_byte_offset);
    spline_coefficients_ptr_ = reinterpret_cast<SplineCoefficents*>(base_ptr + header_ptr_->volatility_byte_offset);
}

VolatilitySurface::VolatilitySurface(const VolatilitySurface& other) noexcept {
    other.Header().reference_count.fetch_add(1, std::memory_order_acq_rel);

    header_ptr_ = other.header_ptr_;
    strikes_ptr_ = other.strikes_ptr_;
    dates_ptr_ = other.dates_ptr_;
    spline_coefficients_ptr_ = other.spline_coefficients_ptr_;
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
        spline_coefficients_ptr_ = other.spline_coefficients_ptr_;
    }

    return *this;
}

VolatilitySurface::VolatilitySurface(VolatilitySurface&& other) noexcept
    : header_ptr_(std::exchange(other.header_ptr_, nullptr)),
      strikes_ptr_(std::exchange(other.strikes_ptr_, nullptr)),
      dates_ptr_(std::exchange(other.dates_ptr_, nullptr)),
      spline_coefficients_ptr_(std::exchange(other.spline_coefficients_ptr_, nullptr)) {
}

VolatilitySurface& VolatilitySurface::operator=(VolatilitySurface&& other) noexcept {
    if (&other != this) [[likely]] {
        this->Reclaim();
        header_ptr_ = std::exchange(other.header_ptr_, nullptr);
        strikes_ptr_ = std::exchange(other.strikes_ptr_, nullptr);
        dates_ptr_ = std::exchange(other.dates_ptr_, nullptr);
        spline_coefficients_ptr_ = std::exchange(other.spline_coefficients_ptr_, nullptr);
    }
    return *this;
}

VolatilitySurface::~VolatilitySurface() {
    if (header_ptr_) {
        this->Reclaim();
}
}

inline f64 Lerp(const f64 x, const f64 x1, const f64 y1, const f64 x2, const f64 y2) noexcept {
    if (std::abs(x2 - x1) < 1e-9)  {
        return y1;
    }
    const f64 t = (x - x1) / (x2 - x1);
    return y1 + t * (y2 - y1);
}

namespace internal {

template<typename Iter>
constexpr u64 IndexFromIterator(Iter begin, u64 size, Iter it) {
    u64 idx = (it == begin) ? 0 : std::distance(begin, it);
    if (idx >= (size-1)) {
        idx = size - 2;
    }

    return idx;
}

} // namespace internal

cdr::Expect<f64, Error> VolatilitySurface::Volatility(const DateType& date, f64 strike) const noexcept {
    if (header_ptr_ == nullptr) [[unlikely]] {
        return ErrorNoData();
    }

    // QoL constants
    const f64 target_time = Period{Today(), date}.Act365();
    const auto strikes_span = Strikes();
    const auto dates_span = Dates();
    const u32 strikes_size = header_ptr_->strikes_size;

    // Validate that date can be interpolated
    if (target_time < dates_span.front() || target_time > dates_span.back()) [[unlikely]] {
        return ErrorTimeExtrapolationNotAllowed();
    }

    // Validate that strike can be interpolated
    if (strike < strikes_span.front() || strike > strikes_span.back()) [[unlikely]] {
        return ErrorStrikeExtrapolationNotAllowed();
    }

    // Obtain date on surface
    auto date_it = std::ranges::lower_bound(dates_span, target_time);
    u64 date_idx = internal::IndexFromIterator(dates_span.begin(), dates_span.size(), date_it);

    // Obtain strike
    auto strike_it = std::ranges::lower_bound(strikes_span, strike);
    u64 strike_idx = internal::IndexFromIterator(strikes_span.begin(), strikes_span.size(), strike_it);

    // Compute interpolated volatility
    auto EvaluateSpline = [&](u64 date_idx) -> f64 {
        const auto& coeffs = spline_coefficients_ptr_[date_idx * strikes_size + strike_idx];
        const f64 dx = strike - strikes_span[strike_idx];
        return (coeffs.smile * dx + coeffs.skew) * dx + coeffs.base_level;
    };

    const f64 volaility_t1 = EvaluateSpline(date_idx);

    if (dates_span[date_idx] == target_time || dates_span.size() == 1) {
        return Ok(std::max(volaility_t1, 0.0));
    }

    const f64 volatility_t2 = EvaluateSpline(date_idx+1);
    const f64 result = Lerp(target_time, dates_span[date_idx], volaility_t1, dates_span[date_idx+1], volatility_t2);

    return Ok(result);
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
    SplineCoefficents* coeff_matrix_ptr = reinterpret_cast<SplineCoefficents*>(buffer_ptr + matrix_offset);

    // Copy strikes and dates
    std::memcpy(strikes_ptr, strikes_.data(), strikes_size_bytes);
    std::memcpy(dates_ptr, dates_.data(), dates_size_bytes);

    // Fill volatility matrix (unchanged logic)
    const u64 strikes_size = strikes_.size();
    const u64 dates_size = dates_.size();
    auto pillars_iter = pillars_.cbegin();

    std::vector<f64> row_raw_vols(strikes_size);

    for (u64 date_idx = 0; date_idx < dates_size; ++date_idx, ++pillars_iter) {
        // --- Grid Projection ---
        // Market quotes (pillars) may not exist for every strike in our global grid.
        // We project available market data onto the global grid using linear interpolation.
        const auto& date_strikes = pillars_iter->second;
        for (u64 strike_idx = 0; strike_idx < strikes_size; ++strike_idx) {
            auto pillar_iter = date_strikes.lower_bound(strikes_[strike_idx]);

            f64 output_value = 0.0;
            if (pillar_iter == date_strikes.end()) {
                // Strike is beyond the last known market pillar: apply flat right-extrapolation
                output_value = std::prev(pillar_iter)->second;
            } else if (pillar_iter->first == strikes_[strike_idx] || pillar_iter == date_strikes.begin()) {
                // Exact match or strike is before the first market pillar: apply flat left-extrapolation
                output_value = pillar_iter->second;
            } else {
                // Strike is between two market pillars: linearly interpolate to find the anchor point
                auto [prev_strike, prev_vol] = *std::prev(pillar_iter);
                auto [curr_strike, curr_vol] = *pillar_iter;
                output_value = Lerp(strikes_[strike_idx], prev_strike, prev_vol, curr_strike, curr_vol);
            }
            // Store the intermediate value in the temporary row buffer
            row_raw_vols[strike_idx] = output_value;
        }

        // --- Quadratic Spline Construction (C1 Continuity) ---
        // Convert raw points into a cacheable analytical form: f(dx) = smile*dx^2 + skew*dx + base_level
        SplineCoefficents* c_row = &coeff_matrix_ptr[date_idx * strikes_size];

        // Guard: A spline cannot be constructed with a single strike point
        if (strikes_size == 1) {
            c_row[0] = {0.0, 0.0, row_raw_vols[0]};
            continue;
        }

        // Initialize the boundary condition: the derivative (slope) at the first node.
        // We use the slope of the first chord as a reasonable starting approximation.
        f64 h0 = strikes_[1] - strikes_[0];
        f64 current_slope = (row_raw_vols[1] - row_raw_vols[0]) / h0;

        // Iterate through each interval [s, s+1] to "stitch" parabolas together
        for (u64 s = 0; s < strikes_size - 1; ++s) {
            const f64 x0 = strikes_[s];
            const f64 x1 = strikes_[s + 1];
            const f64 y0 = row_raw_vols[s];
            const f64 y1 = row_raw_vols[s + 1];
            const f64 h = x1 - x0;

            // base_level: The function value at the left node of the interval
            c_row[s].base_level = y0;

            // skew: The incoming slope (derivative) for this interval.
            // Passing this value from the previous segment ensures C1 continuity (no kinks).
            c_row[s].skew = current_slope;

            // smile: The curvature of the parabola. Derived algebraically to ensure
            // the parabola intersects the right node (y1) exactly.
            c_row[s].smile = (y1 - y0 - current_slope * h) / (h * h);

            // "Slope Handoff": Calculate the derivative at the end of the current interval (at x1).
            // This becomes the 'skew' for the next segment. Formula: f'(dx) = 2*a*dx + b
            current_slope = 2.0 * c_row[s].smile * h + c_row[s].skew;
        }

        // Finalize the last node.
        // While extrapolation is blocked in Volatility(), we populate the last node
        // for memory consistency, essentially continuing the last slope as a line.
        c_row[strikes_size - 1] = {0.0, current_slope, row_raw_vols[strikes_size - 1]};
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
