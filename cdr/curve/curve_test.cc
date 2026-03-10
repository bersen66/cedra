#include <algorithm>

#include <gtest/gtest.h>

#include <cdr/curve/curve.h>
#include <cdr/calendar/date.h>
#include <cdr/types/percent.h>
#include <cdr/curve/interpolation/linear.h>
#include <cdr/calendar/holiday_storage.h>

using namespace std::chrono;
using cdr::Percent;
using cdr::Linear;

namespace {

struct TestInterpolation {
    static constexpr bool kStatefulImplementation = true;

    static Percent Interpolate(const cdr::Curve::PointsContainer& points,
                               const DateType& date,
                               const cdr::HolidayStorage& hs,
                               const std::string& jur)
    {
        return Percent::Zero();
    }

};

struct DummyContract {
    std::optional<f64> NPV(cdr::Curve *curve) const {
        if (!rate.has_value()) {
            return std::nullopt;
        }
        return (target_rate - *rate).Apply(notional);
    }

    void ApplyCurve(cdr::Curve* curve) {
        auto curve_rate = curve->Interpolated<Linear>(settlement_date, *curve->Calendar(), jur);
        rate = curve_rate;
    }

    cdr::DateType SettlementDate() const {
        return settlement_date;
    }

    std::string jur;
    cdr::DateType settlement_date;
    std::optional<cdr::Percent> rate;
    cdr::Percent target_rate;
    f64 notional;
};
static_assert(cdr::Contract<DummyContract>);

} // anonymous namespace

TEST(Curve, BasicOps) {
    cdr::HolidayStorage hs;
    hs.StaticInit()
        ("TEST", day(1)/February/year(2001))
    ;
    cdr::DateType today = day(31)/December/year(2020);

    cdr::Curve curve;
    curve.StaticInit()
        .SetJurisdiction("TEST")
        .SetToday(today)
        .SetCalendar(&hs)
        (day(1)/January/year(2021), Percent::FromFraction(21))
        // Jan 2 and 3 are Sat and Sun
    ;

    auto query = day(1)/January/year(2001);
    auto incremented = [] (Percent p) { return p + Percent::FromFraction(1); };

    Percent pnt = curve.InterpolatedTransformed<cdr::Linear>(query, incremented, hs, "TEST");
    ASSERT_EQ(pnt, Percent::FromFraction(22));

    TestInterpolation ti;
    Percent other = curve.InterpolatedTransformed<TestInterpolation>(query, incremented, ti,  hs, "TEST");
    ASSERT_EQ(other, Percent::FromFraction(1));
}

TEST(Curve, RollForward) {
    cdr::HolidayStorage hs;
    hs.StaticInit()
        ("USD", day(3)/June/year(2027))
        ("USD", day(5)/June/year(2027))
        ("USD", day(7)/June/year(2027))
        ("USD", day(9)/June/year(2027))
        ("USD", day(11)/June/year(2027))
        ("WRONG", day(12)/June/year(2027))
        ("WRONG", day(13)/June/year(2027))
        ("WRONG", day(14)/June/year(2027))
    ;
    DateType today = day(1)/June/year(2027);

    cdr::Curve curve;
    curve.StaticInit()
         .SetJurisdiction("USD")
         .SetCalendar(&hs)
         .SetToday(today)
         (day(2)/June/year(2027), Percent::FromPercentage(1))
         (day(4)/June/year(2027), Percent::FromPercentage(2))
         (day(8)/June/year(2027), Percent::FromPercentage(3))
         (day(10)/June/year(2027), Percent::FromPercentage(4))
    ;
    std::set<DateType> target {
        day(4)/June/year(2027),
        day(8)/June/year(2027),
        day(10)/June/year(2027),
        day(14)/June/year(2027),
    };

    curve.RollForward();
    ASSERT_EQ(curve.Today(), day(2)/June/year(2027));
    const auto& pillars = curve.Pillars();

    ASSERT_EQ(pillars.size(), target.size());
    ASSERT_TRUE(std::equal(pillars.begin(), pillars.end(), target.begin(),
                            [](const auto& point, const auto& date) {
                                return point.first == date;
                            }));
}

TEST(Curve, DummyContract) {
    cdr::HolidayStorage hs;
    hs.StaticInit()
        ("TEST", day(31)/December/year(2025))
        ("TEST", day(2)/January/year(2026))
    ;
    DateType today = day(1)/January/year(2026);

    cdr::Curve curve;
    curve.StaticInit()
         .SetToday(today)
         .SetCalendar(&hs)
         .SetJurisdiction("TEST")
    ;

    DummyContract contract {"TEST", day(3)/January/year(2026), std::nullopt, cdr::Percent::FromFraction(0.20), 100.};
    std::cerr << "before adapt\n";
    curve.AdaptToContract(&contract);

    ASSERT_NEAR(contract.rate.value().Fraction(), contract.target_rate.Fraction(), 0.001);
}

class CurveBootstrapTest : public ::testing::Test {
protected:
    void SetUp() override {
        hs.StaticInit()
            ("TEST", day(31)/December/year(2025))
            ("TEST", day(2)/January/year(2026));
        today = day(1)/January/year(2026);
    }

    cdr::HolidayStorage hs;
    cdr::DateType today;
};

TEST_F(CurveBootstrapTest, SimpleContract) {
    cdr::Curve curve;
    curve.StaticInit().SetToday(today).SetCalendar(&hs).SetJurisdiction("TEST");

    DummyContract contract{"TEST", day(5)/January/year(2026), std::nullopt, cdr::Percent::FromFraction(0.20), 100.};
    curve.AdaptToContract(&contract);

    EXPECT_NEAR(contract.rate.value().Fraction(), 0.20, 0.001);
}

TEST_F(CurveBootstrapTest, ExistingPillar) {
    cdr::Curve curve;
    curve.StaticInit().SetToday(today).SetCalendar(&hs).SetJurisdiction("TEST")
        (day(5)/January/year(2026), cdr::Percent::FromFraction(0.10));

    DummyContract contract{"TEST", day(5)/January/year(2026), std::nullopt, cdr::Percent::FromFraction(0.20), 100.};
    curve.AdaptToContract(&contract);

    auto it = curve.Pillars().find(day(5)/January/year(2026));
    ASSERT_NE(it, curve.Pillars().end());
    EXPECT_NEAR(it->second.Fraction(), 0.20, 0.001);
}