#pragma once

#include <cdr/calendar/date.h>
#include <cdr/types/integers.h>
#include <cdr/types/percent.h>
#include <cdr/calendar/freq.h>
#include <cdr/calendar/holiday_storage.h>
#include <cdr/base/check.h>
#include <cdr/types/expect.h>
#include <cdr/types/errors.h>
#include <cdr/base/numeric.h>

#include <functional>
#include <map>
#include <tuple>

namespace cdr {

class Curve;
using DateType = std::chrono::year_month_day;

template <typename T>
concept Contract = requires(T obj, Curve* curve) {
    { std::as_const(obj).SettlementDate() } -> std::same_as<DateType>;
    { obj.ApplyCurve(curve) } -> std::same_as<void>;
    { std::as_const(obj).NPV(curve) } -> std::same_as<std::optional<f64>>;
};

class [[nodiscard]] Curve final {
public:
    using PointsContainer = std::map<DateType, Percent>;
public:
    Curve() = default;

    template <std::input_iterator It>
    Curve(It begin, It end) {
        auto it = points_.begin();
        for (; begin != end; ++begin) {
            const auto& [date, value] = *begin;
            it = points_.emplace_hint(it, date, value);
        }
    }

    struct CurveEasyInit {
        CurveEasyInit& operator()(DateType date, Percent value) {
            parent_->Insert(date, value);
            return *this;
        }

        CurveEasyInit& SetToday(DateType date) {
            CDR_CHECK(date.ok()) << "invalid date " << date;
            parent_->today_ = date;
            return *this;
        }

        CurveEasyInit& SetCalendar(HolidayStorage* hs) {
            CDR_CHECK(hs != nullptr) << "calendar should be defined";
            parent_->calendar_ = hs;
            return *this;
        }

        CurveEasyInit& SetJurisdiction(const std::string& jur) {
            CDR_CHECK(!jur.empty()) << "jurisdiction should be non-empty";
            parent_->jurisdiction_ = jur;
            return *this;
        }

        Curve* parent_;
    };

    CurveEasyInit StaticInit();

    void Clear();

    template <typename Interpolation, typename... Args>
    [[nodiscard]] Percent Interpolated(DateType date, Args&&... args) {
        if constexpr (Interpolation::kStatefulImplementation) {
            auto args_tuple = std::forward_as_tuple(std::forward<Args>(args)...);

            static_assert(sizeof...(Args) >= 1, "Statefull interpolations must provide state as argument after date");
            auto&& state = std::get<0>(args_tuple);

            if constexpr (sizeof...(Args) > 1) {
                return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                    return state.Interpolate(points_, date,
                                             std::get<Is + 1>(std::forward_as_tuple(std::forward<Args>(args)...))...);
                }(std::make_index_sequence<sizeof...(Args) - 1>{});
            } else {
                return state.Interpolate(points_, date);
            }
        } else {
            return Interpolation::Interpolate(points_, date, std::forward<Args>(args)...);
        }
    }


    template <typename Interpolation, typename Transformation, typename... Args>
    [[nodiscard]] inline decltype(auto) InterpolatedTransformed(DateType dt, Transformation&& transform, Args&&... args) {
        Percent interpolated_value = Interpolated<Interpolation>(dt, std::forward<Args>(args)...);
        return std::invoke(std::forward<Transformation>(transform), interpolated_value);
    }

    template <Contract T>
    void AdaptToContract(T *contract) {
        static constexpr f64 kPrecisionNPV = 0.001;
        static constexpr f64 kNewtonRaphsonStep = 1e-6;
        static constexpr int kNewtonRaphsonMaxIters = 20;
        static constexpr f64 kNewtonRaphsonDerThreashold = 1e-12;
        
        DateType settlement = contract->SettlementDate();
        Period period{Today(), settlement};

        Percent left_df = Percent::FromFraction(0.);
        Percent right_df = Percent::FromFraction(1.);
        // TODO: ensure target function grows from 0 to 1

        PointsContainer::iterator node;
        if (auto iter = points_.lower_bound(settlement);
            iter == points_.end() || iter->first != settlement) [[likely]] {
            node = points_.emplace_hint(iter, settlement, Percent::Zero());
        } else {
            node = iter;
        }
       
        auto compute_npv = [&](Percent df) -> std::optional<f64> {
            node->second = Curve::DiscountToZeroRates(period, df);
            contract->ApplyCurve(this);
            return contract->NPV(this);
        };

        Percent df = MidPoint(left_df, right_df);

        for (int iter = 0; iter < kNewtonRaphsonMaxIters; ++iter) {
            auto npv_opt = compute_npv(df);
            
            if (!npv_opt.has_value()) [[unlikely]] {
                break;
            }

            f64 npv = npv_opt.value();

            if (std::abs(npv) <= kPrecisionNPV) {
                return;
            }

            Percent df_plus = df + Percent::FromFraction(kNewtonRaphsonStep);
            if (df_plus.Fraction() > 1.0) df_plus = Percent::FromFraction(1.0);

            auto npv_plus_opt = compute_npv(df_plus);
            if (!npv_plus_opt.has_value()) {
                break;
            }

            f64 deriv = (npv_plus_opt.value() - npv) / kNewtonRaphsonStep;
            if (std::abs(deriv) < kNewtonRaphsonDerThreashold) {
                break; // fallback to bissection
            }

            auto [df_new, clamped] = Clamp(df - Percent::FromFraction(npv / deriv), left_df, right_df);

            if (clamped) {
                break; // fallback to bissection
            }

            df = df_new;
        }

        std::optional<f64> npv;
        do {
            Percent mid_df = (left_df + right_df) / 2.;
            node->second = Curve::DiscountToZeroRates(period, mid_df);
            contract->ApplyCurve(this);
            npv = contract->NPV(this);
            CDR_CHECK(npv.has_value()) << "must have value";

            if (*npv < 0) {
                left_df = mid_df;
            } else {
                right_df = mid_df;
            }
        } while (std::abs(*npv) > kPrecisionNPV);
    }

    // Advance current date and all pillars by one buisness day
    void RollForward() noexcept;

    [[nodiscard]] DateType Today() const noexcept {
        return today_;
    }

    [[nodiscard]] HolidayStorage* Calendar() const noexcept {
        return calendar_;
    }

    [[nodiscard]] const PointsContainer& Pillars() const noexcept {
        return points_;
    }

    [[nodiscard]] static Percent ZeroRatesToDiscount(const Period& period, Percent p);
    [[nodiscard]] static Percent DiscountToZeroRates(const Period& period, Percent p);

private:

    void Insert(DateType when, Percent value);

private:
    PointsContainer points_;
    std::string jurisdiction_;
    HolidayStorage* calendar_;
    DateType today_;
};

}  // namespace cdr
