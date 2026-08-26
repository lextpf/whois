// Unit tests for Graffito's runtime-independent projection math.
//
// This suite tests the production code directly: GraffitoMath.hpp and
// GraffitoShaderContract.hpp carry no game or ImGui dependency, so they are
// included and exercised as-is. There is nothing mirrored here.

#include "../src/GraffitoMath.hpp"
#include "../src/GraffitoShaderContract.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace Graffito::Math;

TEST(GraffitoBasis, YawZeroFacesPositiveYWithoutMirroring)
{
    const auto basis = BuildUprightBasis(0.0);
    EXPECT_NEAR(basis.forward.x, 0.0, 1e-12);
    EXPECT_NEAR(basis.forward.y, 1.0, 1e-12);
    EXPECT_NEAR(basis.right.x, -1.0, 1e-12);
    EXPECT_NEAR(basis.right.y, 0.0, 1e-12);
    EXPECT_NEAR(Dot(Cross(basis.right, basis.up), basis.forward), 1.0, 1e-12);
}

TEST(GraffitoBasis, QuarterTurnUsesSkyrimYawConvention)
{
    const auto basis = BuildUprightBasis(PI * .5);
    EXPECT_NEAR(basis.forward.x, 1.0, 1e-12);
    EXPECT_NEAR(basis.forward.y, 0.0, 1e-12);
    EXPECT_NEAR(basis.right.x, 0.0, 1e-12);
    EXPECT_NEAR(basis.right.y, 1.0, 1e-12);
}

TEST(GraffitoBasis, CameraFacingYawPointsReadableNormalAtTarget)
{
    const Vec3 anchor{10.0, 20.0, 90.0};
    for (const Vec3 target : {Vec3{10.0, 40.0, 130.0},
                              Vec3{35.0, 20.0, 70.0},
                              Vec3{-15.0, 20.0, 95.0},
                              Vec3{10.0, -5.0, 100.0}})
    {
        const auto basis = BuildUprightBasis(YawFacingPoint(anchor, target, .37));
        Vec3 horizontalView{target.x - anchor.x, target.y - anchor.y, 0.0};
        ASSERT_TRUE(Normalize(horizontalView, horizontalView));
        EXPECT_NEAR(Dot(basis.forward, horizontalView), 1.0, 1e-12);
    }

    EXPECT_DOUBLE_EQ(YawFacingPoint(anchor, anchor, .37), .37);
}

TEST(GraffitoSmoothing, CrossesAngleWrapByShortestRoute)
{
    const double current = 179.0 * PI / 180.0;
    const double target = -179.0 * PI / 180.0;
    const double smoothed = SmoothAngle(current, target, .1, 1.0);
    EXPECT_GT(ShortestAngleDelta(current, smoothed), 0.0);
    EXPECT_LT(std::abs(ShortestAngleDelta(smoothed, target)),
              std::abs(ShortestAngleDelta(current, target)));
}

TEST(GraffitoMotion, VelocityBlendTracksSampleDelta)
{
    const auto velocity =
        BlendMotionVelocity({20.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {10.0, 0.0, 0.0}, .1, .5);
    EXPECT_NEAR(velocity.x, 60.0, 1e-12);
    EXPECT_DOUBLE_EQ(velocity.y, 0.0);
    EXPECT_DOUBLE_EQ(velocity.z, 0.0);
}

TEST(GraffitoMotion, PredictionBridgesOnlyTheObservedSampleWindow)
{
    const Vec3 sample{10.0, 20.0, 30.0};
    const Vec3 velocity{100.0, -50.0, 25.0};

    const auto fresh = PredictMotionPosition(sample, velocity, .012, .016);
    EXPECT_NEAR(fresh.x, 11.2, 1e-12);
    EXPECT_NEAR(fresh.y, 19.4, 1e-12);
    EXPECT_NEAR(fresh.z, 30.3, 1e-12);

    // A stale sample stops after 1.5 intervals rather than drifting forever.
    const auto stale = PredictMotionPosition(sample, velocity, 1.0, .016);
    EXPECT_NEAR(stale.x, 12.4, 1e-12);
    EXPECT_NEAR(stale.y, 18.8, 1e-12);
    EXPECT_NEAR(stale.z, 30.6, 1e-12);
}

TEST(GraffitoMotion, PredictionCapsImplausibleDisplacement)
{
    const Vec3 sample{1.0, 2.0, 3.0};
    const auto predicted = PredictMotionPosition(sample, {10000.0, 0.0, 0.0}, .05, .05);
    EXPECT_NEAR(predicted.x, 33.0, 1e-12);
    EXPECT_DOUBLE_EQ(predicted.y, sample.y);
    EXPECT_DOUBLE_EQ(predicted.z, sample.z);
}

TEST(GraffitoMotion, InvalidInputFallsBackWithoutANan)
{
    const Vec3 sample{1.0, 2.0, 3.0};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto predicted = PredictMotionPosition(sample, {nan, 0.0, 0.0}, .01, .016);
    EXPECT_DOUBLE_EQ(predicted.x, sample.x);
    EXPECT_DOUBLE_EQ(predicted.y, sample.y);
    EXPECT_DOUBLE_EQ(predicted.z, sample.z);
}

TEST(GraffitoRaycast, CameraForwardRayHitsNearestSphereSurface)
{
    EXPECT_DOUBLE_EQ(RaySphereHitDistance({0.0, 0.0, 0.0}, {0.0, 2.0, 0.0}, {0.0, 10.0, 0.0}, 2.0),
                     8.0);
    EXPECT_DOUBLE_EQ(RaySphereHitDistance({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 0.0}, 2.0),
                     0.0);
}

TEST(GraffitoRaycast, RejectsMissesBehindCameraAndPastRange)
{
    EXPECT_TRUE(
        std::isinf(RaySphereHitDistance({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {3.0, 10.0, 0.0}, 2.0)));
    EXPECT_TRUE(
        std::isinf(RaySphereHitDistance({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, -10.0, 0.0}, 2.0)));
    EXPECT_TRUE(std::isinf(
        RaySphereHitDistance({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 10.0, 0.0}, 2.0, 7.9)));
}

TEST(GraffitoRaycast, InvalidGeometryFailsWithoutProducingANearHit)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_TRUE(std::isinf(RaySphereHitDistance({0.0, 0.0, 0.0}, {}, {0.0, 10.0, 0.0}, 2.0)));
    EXPECT_TRUE(
        std::isinf(RaySphereHitDistance({0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 10.0, 0.0}, nan)));
}

TEST(GraffitoFacing, FrontUsesIdentityInk)
{
    const auto material = EvaluateFacingMaterial(1.0, 15.0, .12, .22);
    EXPECT_DOUBLE_EQ(material.opacity, 1.0);
    EXPECT_DOUBLE_EQ(material.desaturation, 0.0);
    EXPECT_DOUBLE_EQ(material.brightness, 1.0);
}

TEST(GraffitoFacing, BackUsesConfiguredMirroredBleed)
{
    const auto material = EvaluateFacingMaterial(-1.0, 15.0, .12, .22);
    EXPECT_DOUBLE_EQ(material.opacity, .12);
    EXPECT_DOUBLE_EQ(material.desaturation, BACK_BLEED_DESATURATION);
    EXPECT_DOUBLE_EQ(material.brightness, BACK_BLEED_BRIGHTNESS);
}

TEST(GraffitoFacing, EdgeUsesDesaturatedInkSeam)
{
    const auto material = EvaluateFacingMaterial(0.0, 15.0, .12, .22);
    EXPECT_DOUBLE_EQ(material.opacity, .22);
    EXPECT_DOUBLE_EQ(material.desaturation, EDGE_SEAM_DESATURATION);
    EXPECT_DOUBLE_EQ(material.brightness, EDGE_SEAM_BRIGHTNESS);
}

TEST(GraffitoFacing, TransitionIsContinuousAcrossEdge)
{
    const auto justFront = EvaluateFacingMaterial(1e-7, 15.0, .12, .22);
    const auto justBack = EvaluateFacingMaterial(-1e-7, 15.0, .12, .22);
    EXPECT_NEAR(justFront.opacity, .22, 1e-10);
    EXPECT_NEAR(justBack.opacity, .22, 1e-10);
    EXPECT_NEAR(justFront.desaturation, justBack.desaturation, 1e-10);
    EXPECT_NEAR(justFront.brightness, justBack.brightness, 1e-10);

    const double bandEdge = std::sin(15.0 * PI / 180.0);
    EXPECT_NEAR(EvaluateFacingMaterial(bandEdge, 15.0, .12, .22).opacity, 1.0, 1e-12);
    EXPECT_NEAR(EvaluateFacingMaterial(-bandEdge, 15.0, .12, .22).opacity, .12, 1e-12);
}

TEST(GraffitoFacing, ZeroReverseAlphasRestoreOneSidedRead)
{
    EXPECT_DOUBLE_EQ(EvaluateFacingMaterial(-1.0, 15.0, 0.0, 0.0).opacity, 0.0);
    EXPECT_DOUBLE_EQ(EvaluateFacingMaterial(0.0, 15.0, 0.0, 0.0).opacity, 0.0);
    EXPECT_DOUBLE_EQ(EvaluateFacingMaterial(1.0, 15.0, 0.0, 0.0).opacity, 1.0);
}

TEST(GraffitoFacing, NonFiniteDirectionFailsInvisible)
{
    const auto material =
        EvaluateFacingMaterial(std::numeric_limits<double>::quiet_NaN(), 15.0, .12, .22);
    EXPECT_DOUBLE_EQ(material.opacity, 0.0);
}

TEST(GraffitoFolio, CanonicalAnglesCrossfadeWithoutADeadZone)
{
    const auto atDegrees = [](double degrees)
    {
        const double radians = degrees * PI / 180.0;
        return EvaluateFolioWeights(std::cos(radians), std::sin(radians));
    };

    const auto front = atDegrees(0.0);
    EXPECT_DOUBLE_EQ(front.front, 1.0);
    EXPECT_DOUBLE_EQ(front.spine, 0.0);
    EXPECT_DOUBLE_EQ(front.back, 0.0);

    const auto frontCrossover = atDegrees(50.0);
    EXPECT_NEAR(frontCrossover.front, .5, 1e-12);
    EXPECT_NEAR(frontCrossover.spine, .5, 1e-12);
    EXPECT_DOUBLE_EQ(frontCrossover.back, 0.0);

    const auto passing = atDegrees(60.0);
    EXPECT_DOUBLE_EQ(passing.front, 0.0);
    EXPECT_DOUBLE_EQ(passing.spine, 1.0);
    EXPECT_DOUBLE_EQ(passing.back, 0.0);

    const auto spine = atDegrees(90.0);
    EXPECT_DOUBLE_EQ(spine.front, 0.0);
    EXPECT_DOUBLE_EQ(spine.spine, 1.0);
    EXPECT_DOUBLE_EQ(spine.back, 0.0);

    const auto backCrossover = atDegrees(120.0);
    EXPECT_DOUBLE_EQ(backCrossover.front, 0.0);
    EXPECT_NEAR(backCrossover.spine, .5, 1e-12);
    EXPECT_NEAR(backCrossover.back, .5, 1e-12);

    const auto back = atDegrees(180.0);
    EXPECT_DOUBLE_EQ(back.front, 0.0);
    EXPECT_DOUBLE_EQ(back.spine, 0.0);
    EXPECT_DOUBLE_EQ(back.back, 1.0);
}

TEST(GraffitoFolio, DenseSweepIsContinuousNormalizedAndLeftRightSymmetric)
{
    for (int quarterDegrees = 0; quarterDegrees <= 720; ++quarterDegrees)
    {
        const double degrees = static_cast<double>(quarterDegrees) * .25;
        const double radians = degrees * PI / 180.0;
        const double frontDot = std::cos(radians);
        const double rightDot = std::sin(radians);
        const auto right = EvaluateFolioWeights(frontDot, rightDot);
        const auto left = EvaluateFolioWeights(frontDot, -rightDot);

        for (double weight : {right.front, right.spine, right.back})
        {
            EXPECT_TRUE(std::isfinite(weight));
            EXPECT_GE(weight, 0.0);
            EXPECT_LE(weight, 1.0);
        }
        EXPECT_NEAR(right.front + right.spine + right.back, 1.0, 1e-12);
        EXPECT_DOUBLE_EQ(right.front * right.back, 0.0);
        EXPECT_DOUBLE_EQ(left.front, right.front);
        EXPECT_DOUBLE_EQ(left.spine, right.spine);
        EXPECT_DOUBLE_EQ(left.back, right.back);

        if (degrees > 0.0 && degrees < 180.0)
        {
            EXPECT_EQ(right.sideSign, 1);
            EXPECT_EQ(left.sideSign, -1);
        }
    }
}

TEST(GraffitoFolio, DefaultFaceOpacitiesNeverLeaveAnEmptyRead)
{
    constexpr double reverseAlpha = .72;
    constexpr double spineAlpha = .88;
    for (int degrees = -180; degrees <= 180; ++degrees)
    {
        const double radians = static_cast<double>(degrees) * PI / 180.0;
        const auto weights = EvaluateFolioWeights(std::cos(radians), std::sin(radians));
        const double combinedOpacity =
            weights.front + weights.spine * spineAlpha + weights.back * reverseAlpha;
        EXPECT_GE(combinedOpacity, reverseAlpha - 1e-12);
    }
}

TEST(GraffitoFolio, CameraElevationDoesNotChangeAzimuthWeights)
{
    const double radians = 118.0 * PI / 180.0;
    const auto level = EvaluateFolioWeights(std::cos(radians), std::sin(radians));
    const auto elevated = EvaluateFolioWeights(std::cos(radians) * .08, std::sin(radians) * .08);

    EXPECT_NEAR(elevated.front, level.front, 1e-12);
    EXPECT_NEAR(elevated.spine, level.spine, 1e-12);
    EXPECT_NEAR(elevated.back, level.back, 1e-12);
    EXPECT_EQ(elevated.sideSign, level.sideSign);
}

TEST(GraffitoFolio, InvalidOrTopDownDirectionFailsInvisible)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (const auto weights : {EvaluateFolioWeights(0.0, 0.0),
                               EvaluateFolioWeights(1e-9, -1e-9),
                               EvaluateFolioWeights(nan, 0.0),
                               EvaluateFolioWeights(0.0, infinity)})
    {
        EXPECT_DOUBLE_EQ(weights.front, 0.0);
        EXPECT_DOUBLE_EQ(weights.spine, 0.0);
        EXPECT_DOUBLE_EQ(weights.back, 0.0);
    }
}

TEST(GraffitoFolio, FacetMetricsStayReadableAtProductionFontSizes)
{
    const auto defaults = ComputeFolioFacetMetrics(122.0, 2.0);
    EXPECT_DOUBLE_EQ(defaults.reliefSpacing, 2.0);
    EXPECT_DOUBLE_EQ(defaults.TotalReliefDepth(), 6.0);
    EXPECT_GT(defaults.sideWidth, 50.0);
    EXPECT_GT(defaults.height, 65.0);
    EXPECT_GT(defaults.rearWidth, 75.0);
    EXPECT_NEAR(defaults.markerLift, defaults.height * .16, 1e-12);
    EXPECT_GT(defaults.RankSize(defaults.rearWidth), 50.0);

    const auto configured = ComputeFolioFacetMetrics(205.0, 2.0);
    EXPECT_DOUBLE_EQ(configured.reliefSpacing, 2.0);
    EXPECT_DOUBLE_EQ(configured.TotalReliefDepth(), 6.0);
    EXPECT_GT(configured.sideWidth, defaults.sideWidth);
    EXPECT_GT(configured.height, 110.0);
    EXPECT_GT(configured.rearWidth, 130.0);
    EXPECT_GT(configured.markerLift, defaults.markerLift);
    EXPECT_GT(configured.RankSize(configured.rearWidth), 90.0);

    // The rank deliberately breaks above the triangle's base instead of being
    // fitted into its narrowing interior.
    const double rankSize = configured.RankSize(configured.rearWidth);
    const double rankTop = configured.height * .34 - rankSize * .5;
    EXPECT_LT(rankTop, 0.0);
    EXPECT_LE(rankSize, 96.0);
}

TEST(GraffitoFolio, ReliefSpacingAllowsZeroAndPreservesSourcePixels)
{
    const auto disabled = ComputeFolioFacetMetrics(205.0, 0.0);
    EXPECT_DOUBLE_EQ(disabled.reliefSpacing, 0.0);
    EXPECT_DOUBLE_EQ(disabled.TotalReliefDepth(), 0.0);
    EXPECT_GT(disabled.sideWidth, 0.0);
    EXPECT_GT(disabled.height, 0.0);

    const auto shallow = ComputeFolioFacetMetrics(205.0, .5);
    EXPECT_DOUBLE_EQ(shallow.reliefSpacing, .5);
    EXPECT_DOUBLE_EQ(shallow.TotalReliefDepth(), 1.5);

    const auto bounded = ComputeFolioFacetMetrics(205.0, 100.0);
    EXPECT_DOUBLE_EQ(bounded.reliefSpacing, 28.0);
    EXPECT_DOUBLE_EQ(bounded.TotalReliefDepth(), 84.0);
}

TEST(GraffitoFolio, ReliefPlanesKeepTheConfiguredAdjacentSpacing)
{
    constexpr double spacing = 2.0;
    const std::array<double, 3> offsets{FOLIO_RELIEF_STEPS[0] * spacing,
                                        FOLIO_RELIEF_STEPS[1] * spacing,
                                        FOLIO_RELIEF_STEPS[2] * spacing};

    EXPECT_DOUBLE_EQ(offsets[0], 6.0);
    EXPECT_DOUBLE_EQ(offsets[1], 4.0);
    EXPECT_DOUBLE_EQ(offsets[2], 2.0);
    EXPECT_DOUBLE_EQ(offsets[0] - offsets[1], spacing);
    EXPECT_DOUBLE_EQ(offsets[1] - offsets[2], spacing);
    EXPECT_DOUBLE_EQ(offsets[2], spacing);
}

TEST(GraffitoFolio, InvalidFacetInputsRemainFiniteAndBounded)
{
    const auto metrics = ComputeFolioFacetMetrics(std::numeric_limits<double>::quiet_NaN(),
                                                  std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isfinite(metrics.reliefSpacing));
    EXPECT_GE(metrics.reliefSpacing, 0.0);
    for (double value : {metrics.sideWidth,
                         metrics.height,
                         metrics.rearWidth,
                         metrics.markerLift,
                         metrics.RankSize(metrics.sideWidth)})
    {
        EXPECT_TRUE(std::isfinite(value));
        EXPECT_GT(value, 0.0);
    }
}

TEST(GraffitoRange, SmoothlyFadesOnlyInsideConfiguredBand)
{
    EXPECT_DOUBLE_EQ(RangeFade(100.0, 800.0, 1200.0), 1.0);
    EXPECT_DOUBLE_EQ(RangeFade(1200.0, 800.0, 1200.0), 0.0);
    EXPECT_NEAR(RangeFade(1000.0, 800.0, 1200.0), .5, 1e-12);
}

TEST(GraffitoHomography, RecoversKnownProjectiveMap)
{
    const std::array<Vec2, 4> source{{{-2.0, -1.0}, {3.0, -1.0}, {3.0, 2.0}, {-2.0, 2.0}}};
    const Homography expected{{1.2, .15, -.3, -.2, .9, .4, .06, -.04, 1.0}};
    std::array<Vec2, 4> destination{};
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        ASSERT_TRUE(expected.Transform(source[i], destination[i]));
    }

    Homography solved{};
    ASSERT_TRUE(SolveHomography(source, destination, solved));
    const std::array<Vec2, 3> probes{{{0.0, 0.0}, {1.25, -.4}, {-1.1, 1.4}}};
    for (const auto& probe : probes)
    {
        Vec2 want{};
        Vec2 got{};
        ASSERT_TRUE(expected.Transform(probe, want));
        ASSERT_TRUE(solved.Transform(probe, got));
        EXPECT_NEAR(got.x, want.x, 1e-9);
        EXPECT_NEAR(got.y, want.y, 1e-9);
    }
}

TEST(GraffitoHomography, RejectsCollinearControlPoints)
{
    const std::array<Vec2, 4> source{{{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}, {3.0, 0.0}}};
    const std::array<Vec2, 4> destination{{{0.0, 0.0}, {1.0, 1.0}, {2.0, 1.0}, {3.0, 0.0}}};
    Homography result{};
    EXPECT_FALSE(SolveHomography(source, destination, result));
}

TEST(GraffitoDepth, FitsNormalizedScreenAffinePlane)
{
    constexpr double xSlope = .17;
    constexpr double ySlope = -.08;
    constexpr double constant = .42;
    const auto z = [](double x, double y) { return xSlope * x + ySlope * y + constant; };
    const std::array<Vec3, 4> samples{
        {{.1, .2, z(.1, .2)}, {.8, .15, z(.8, .15)}, {.9, .9, z(.9, .9)}, {.2, .75, z(.2, .75)}}};

    AffinePlane result{};
    ASSERT_TRUE(SolveAffinePlane(samples, result));
    EXPECT_NEAR(result.xSlope, xSlope, 1e-10);
    EXPECT_NEAR(result.ySlope, ySlope, 1e-10);
    EXPECT_NEAR(result.constant, constant, 1e-10);
}

TEST(GraffitoCylinder, ZeroCurvatureIsExactlyFlat)
{
    const auto upright = BuildUprightBasis(.37);
    const PlanePose pose{{12.0, -8.0, 90.0}, upright.forward, upright.right, upright.up};
    constexpr double scale = .1;
    for (const Vec2 source : {Vec2{-320.0, -40.0}, Vec2{13.0, 7.0}, Vec2{470.0, 65.0}})
    {
        const Vec3 flat = WorldPointFromSourceOffset(pose, source, scale);
        const Vec3 cylinder = CylinderPointFromSourceOffset(pose, source, scale, 37.0, .0, 120.0);
        EXPECT_DOUBLE_EQ(cylinder.x, flat.x);
        EXPECT_DOUBLE_EQ(cylinder.y, flat.y);
        EXPECT_DOUBLE_EQ(cylinder.z, flat.z);
    }
}

TEST(GraffitoCylinder, StripCornersAreCoplanar)
{
    const auto upright = BuildUprightBasis(-.41);
    const PlanePose pose{{2.0, 3.0, 100.0}, upright.forward, upright.right, upright.up};
    constexpr double scale = .1;
    constexpr double apex = 21.0;
    constexpr double kappa = .0008;
    constexpr double radius = scale / kappa;
    constexpr double x0 = 180.0;
    constexpr double x1 = 310.0;
    constexpr double y0 = -70.0;
    constexpr double y1 = 85.0;

    const Vec3 p00 = CylinderPointFromSourceOffset(pose, {x0, y0}, scale, apex, kappa, radius);
    const Vec3 p10 = CylinderPointFromSourceOffset(pose, {x1, y0}, scale, apex, kappa, radius);
    const Vec3 p11 = CylinderPointFromSourceOffset(pose, {x1, y1}, scale, apex, kappa, radius);
    const Vec3 p01 = CylinderPointFromSourceOffset(pose, {x0, y1}, scale, apex, kappa, radius);
    Vec3 stripNormal{};
    ASSERT_TRUE(Normalize(Cross(p10 - p00, p01 - p00), stripNormal));
    EXPECT_NEAR(Dot(p11 - p00, stripNormal), 0.0, 1e-12);
}

TEST(GraffitoCylinder, ConcentricLayersShareOneAxis)
{
    const auto upright = BuildUprightBasis(.23);
    const PlanePose front{{2.0, 3.0, 100.0}, upright.forward, upright.right, upright.up};
    constexpr double scale = .1;
    constexpr double apex = -17.0;
    constexpr double kappa = .0009;
    constexpr double frontRadius = scale / kappa;
    const Vec3 normal = Cross(front.right, front.up);
    Vec3 referenceAxis{};
    bool haveReference = false;

    for (double offset : {-5.0, 0.0, 7.0})
    {
        const PlanePose layer = OffsetPlanePose(front, offset);
        const double radius = frontRadius + offset;
        const Vec3 apexPoint =
            CylinderPointFromSourceOffset(layer, {apex, 0.0}, scale, apex, kappa, radius);
        const Vec3 axis = apexPoint - normal * radius;
        if (!haveReference)
        {
            referenceAxis = axis;
            haveReference = true;
        }
        EXPECT_NEAR(axis.x, referenceAxis.x, 1e-12);
        EXPECT_NEAR(axis.y, referenceAxis.y, 1e-12);
        EXPECT_NEAR(axis.z, referenceAxis.z, 1e-12);
    }
}

TEST(GraffitoCylinder, WingsCompressHorizontallyWithoutCompressingHeight)
{
    const auto upright = BuildUprightBasis(.0);
    const PlanePose pose{{0.0, 0.0, 0.0}, upright.forward, upright.right, upright.up};
    constexpr double scale = .1;
    constexpr double halfWidth = 750.0;
    constexpr double totalArc = 70.0 * PI / 180.0;
    constexpr double kappa = totalArc / (2.0 * halfWidth);
    constexpr double radius = scale / kappa;
    constexpr double dx = 10.0;

    const auto horizontalSpan = [&](double centerX)
    {
        const Vec3 a =
            CylinderPointFromSourceOffset(pose, {centerX - dx * .5, 0.0}, scale, .0, kappa, radius);
        const Vec3 b =
            CylinderPointFromSourceOffset(pose, {centerX + dx * .5, 0.0}, scale, .0, kappa, radius);
        return std::abs(Dot(b - a, pose.right));
    };
    const double centerSpan = horizontalSpan(.0);
    const double wingSpan = horizontalSpan(halfWidth - dx * .5);
    const Vec3 top =
        CylinderPointFromSourceOffset(pose, {halfWidth, -50.0}, scale, .0, kappa, radius);
    const Vec3 bottom =
        CylinderPointFromSourceOffset(pose, {halfWidth, 50.0}, scale, .0, kappa, radius);

    const double expectedWingRatio =
        (std::sin(totalArc * .5) - std::sin(totalArc * .5 - kappa * dx)) /
        (2.0 * std::sin(kappa * dx * .5));
    EXPECT_NEAR(wingSpan / centerSpan, expectedWingRatio, 1e-11);
    EXPECT_NEAR(expectedWingRatio, std::cos(totalArc * .5), 3e-3);
    EXPECT_NEAR(std::sqrt(LengthSquared(bottom - top)), 100.0 * scale, 1e-12);
}

TEST(GraffitoFolioPose, BackFaceIsReadableAndExactlyReflectsTheFrontFootprint)
{
    constexpr double yaw = .37;
    constexpr double pivotX = 13.5;
    constexpr double scale = .1;
    const auto upright = BuildUprightBasis(yaw);
    const PlanePose front{{10.0, 20.0, 120.0}, upright.forward, upright.right, upright.up};
    const PlanePose back = BuildFolioBackPose(front, pivotX, scale);

    EXPECT_NEAR(Dot(Cross(back.right, back.up), back.normal), 1.0, 1e-12);
    EXPECT_NEAR(Dot(back.normal, front.normal), -1.0, 1e-12);
    EXPECT_NEAR(Dot(back.right, front.right), -1.0, 1e-12);
    EXPECT_NEAR(Dot(back.up, front.up), 1.0, 1e-12);

    for (const Vec2 source : {Vec2{-40.0, -20.0}, Vec2{7.0, 11.0}, Vec2{65.0, 38.0}})
    {
        const Vec3 backWorld = WorldPointFromSourceOffset(back, source, scale);
        const Vec3 reflectedFrontWorld =
            WorldPointFromSourceOffset(front, {2.0 * pivotX - source.x, source.y}, scale);
        EXPECT_NEAR(backWorld.x, reflectedFrontWorld.x, 1e-12);
        EXPECT_NEAR(backWorld.y, reflectedFrontWorld.y, 1e-12);
        EXPECT_NEAR(backWorld.z, reflectedFrontWorld.z, 1e-12);
    }
}

TEST(GraffitoFolioPose, SpineFacesTheSelectedSideAndPinsItsAnchorToTheEdge)
{
    constexpr double yaw = -.63;
    constexpr double scale = .1;
    const auto upright = BuildUprightBasis(yaw);
    const PlanePose front{{2.0, 3.0, 100.0}, upright.forward, upright.right, upright.up};
    const Vec2 rightEdge{42.0, -8.0};
    const Vec2 leftEdge{-35.0, -8.0};
    const PlanePose rightSpine = BuildFolioSpinePose(front, 1, rightEdge, scale);
    const PlanePose leftSpine = BuildFolioSpinePose(front, -1, leftEdge, scale);

    const auto expectPinned = [&](const PlanePose& spine, const Vec2& edge)
    {
        const Vec3 expected = WorldPointFromSourceOffset(front, edge, scale);
        EXPECT_NEAR(spine.origin.x, expected.x, 1e-12);
        EXPECT_NEAR(spine.origin.y, expected.y, 1e-12);
        EXPECT_NEAR(spine.origin.z, expected.z, 1e-12);
        EXPECT_NEAR(LengthSquared(spine.normal), 1.0, 1e-12);
        EXPECT_NEAR(LengthSquared(spine.right), 1.0, 1e-12);
        EXPECT_NEAR(LengthSquared(spine.up), 1.0, 1e-12);
        EXPECT_NEAR(Dot(Cross(spine.right, spine.up), spine.normal), 1.0, 1e-12);
    };
    expectPinned(rightSpine, rightEdge);
    expectPinned(leftSpine, leftEdge);

    EXPECT_NEAR(Dot(rightSpine.normal, front.right), 1.0, 1e-12);
    EXPECT_NEAR(Dot(rightSpine.right, front.normal), -1.0, 1e-12);
    EXPECT_NEAR(Dot(leftSpine.normal, front.right), -1.0, 1e-12);
    EXPECT_NEAR(Dot(leftSpine.right, front.normal), 1.0, 1e-12);
}

TEST(GraffitoFolioPose, ReliefOffsetsAlongThePlaneNormal)
{
    const auto upright = BuildUprightBasis(.41);
    const PlanePose front{{2.0, 3.0, 100.0}, upright.forward, upright.right, upright.up};
    const PlanePose relief = OffsetPlanePose(front, -4.25);

    const Vec3 delta = relief.origin - front.origin;
    EXPECT_NEAR(Dot(delta, front.normal), -4.25, 1e-12);
    EXPECT_NEAR(Dot(delta, front.right), 0.0, 1e-12);
    EXPECT_NEAR(Dot(delta, front.up), 0.0, 1e-12);
    EXPECT_NEAR(Dot(relief.normal, front.normal), 1.0, 1e-12);
    EXPECT_NEAR(Dot(relief.right, front.right), 1.0, 1e-12);
    EXPECT_NEAR(Dot(relief.up, front.up), 1.0, 1e-12);
}

TEST(GraffitoFolioPose, SideFacetCentersAcrossTheReliefDepth)
{
    constexpr double scale = .1;
    constexpr double depth = 4.8;
    const auto upright = BuildUprightBasis(-.29);
    const PlanePose front{{2.0, 3.0, 100.0}, upright.forward, upright.right, upright.up};
    const Vec2 edge{42.0, -8.0};

    for (int sideSign : {-1, 1})
    {
        const PlanePose facet = BuildFolioFacetPose(front, sideSign, edge, scale, depth);
        const Vec3 edgeWorld = WorldPointFromSourceOffset(front, edge, scale);
        const Vec3 centerDelta = facet.origin - edgeWorld;
        EXPECT_NEAR(Dot(centerDelta, front.normal), -depth * .5, 1e-12);
        EXPECT_NEAR(Dot(centerDelta, front.right), 0.0, 1e-12);
        EXPECT_NEAR(Dot(centerDelta, front.up), 0.0, 1e-12);
        EXPECT_NEAR(Dot(Cross(facet.right, facet.up), facet.normal), 1.0, 1e-12);

        const double halfDepthPixels = depth / (2.0 * scale);
        const double frontX = sideSign > 0 ? -halfDepthPixels : halfDepthPixels;
        const Vec3 facetFront = WorldPointFromSourceOffset(facet, {frontX, 0.0}, scale);
        const Vec3 facetRear = WorldPointFromSourceOffset(facet, {-frontX, 0.0}, scale);
        const Vec3 expectedRear = edgeWorld - front.normal * depth;
        const auto expectPoint = [](const Vec3& actual, const Vec3& expected)
        {
            EXPECT_NEAR(actual.x, expected.x, 1e-12);
            EXPECT_NEAR(actual.y, expected.y, 1e-12);
            EXPECT_NEAR(actual.z, expected.z, 1e-12);
        };
        expectPoint(facetFront, edgeWorld);
        expectPoint(facetRear, expectedRear);
    }
}

TEST(GraffitoFolioPose, InvalidSourceInputsDoNotPoisonThePose)
{
    const auto upright = BuildUprightBasis(.2);
    const PlanePose front{{2.0, 3.0, 100.0}, upright.forward, upright.right, upright.up};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const PlanePose back = BuildFolioBackPose(front, nan, infinity);
    const PlanePose spine = BuildFolioSpinePose(front, 0, {nan, infinity}, .1);
    const PlanePose relief = OffsetPlanePose(front, nan);
    const PlanePose facet = BuildFolioFacetPose(front, 1, {nan, infinity}, .1, nan);

    for (const Vec3 point : {back.origin,
                             back.normal,
                             back.right,
                             back.up,
                             spine.origin,
                             spine.normal,
                             spine.right,
                             spine.up,
                             relief.origin,
                             relief.normal,
                             relief.right,
                             relief.up,
                             facet.origin,
                             facet.normal,
                             facet.right,
                             facet.up})
    {
        EXPECT_TRUE(std::isfinite(point.x));
        EXPECT_TRUE(std::isfinite(point.y));
        EXPECT_TRUE(std::isfinite(point.z));
    }
    EXPECT_NEAR(LengthSquared(back.normal), 1.0, 1e-12);
    EXPECT_NEAR(LengthSquared(spine.normal), 1.0, 1e-12);
}

TEST(GraffitoEpitaph, StartsAtExactUprightPoseWithoutPop)
{
    const Vec3 uprightOrigin{10.0, 20.0, 120.0};
    const Vec3 groundHinge{12.0, 28.0, 2.0};
    const Vec2 sourceHinge{7.0, -42.0};
    const auto pose = BuildFallenEpitaphPose(.37, 0.0, uprightOrigin, groundHinge, sourceHinge, .1);
    const auto upright = BuildUprightBasis(.37);

    EXPECT_NEAR(pose.origin.x, uprightOrigin.x, 1e-12);
    EXPECT_NEAR(pose.origin.y, uprightOrigin.y, 1e-12);
    EXPECT_NEAR(pose.origin.z, uprightOrigin.z, 1e-12);
    EXPECT_NEAR(Dot(pose.normal, upright.forward), 1.0, 1e-12);
    EXPECT_NEAR(Dot(pose.right, upright.right), 1.0, 1e-12);
    EXPECT_NEAR(Dot(pose.up, upright.up), 1.0, 1e-12);
}

TEST(GraffitoEpitaph, LandsTopCenterOnGroundAndExtendsForward)
{
    constexpr double yaw = .37;
    constexpr double scale = .1;
    const Vec3 uprightOrigin{10.0, 20.0, 120.0};
    const Vec3 groundHinge{12.0, 28.0, 2.0};
    const Vec2 sourceHinge{7.0, -42.0};
    const auto pose =
        BuildFallenEpitaphPose(yaw, 1.0, uprightOrigin, groundHinge, sourceHinge, scale);
    const auto upright = BuildUprightBasis(yaw);
    const auto worldFromSource = [&](const Vec2& source)
    { return pose.origin + pose.right * (source.x * scale) - pose.up * (source.y * scale); };

    const Vec3 landedHinge = worldFromSource(sourceHinge);
    EXPECT_NEAR(landedHinge.x, groundHinge.x, 1e-12);
    EXPECT_NEAR(landedHinge.y, groundHinge.y, 1e-12);
    EXPECT_NEAR(landedHinge.z, groundHinge.z, 1e-12);
    EXPECT_NEAR(pose.normal.x, 0.0, 1e-12);
    EXPECT_NEAR(pose.normal.y, 0.0, 1e-12);
    EXPECT_NEAR(pose.normal.z, 1.0, 1e-12);

    const Vec3 lowerInk = worldFromSource({sourceHinge.x, sourceHinge.y + 20.0});
    const Vec3 advance = lowerInk - landedHinge;
    EXPECT_GT(Dot(advance, upright.forward), 0.0);
    EXPECT_NEAR(lowerInk.z, groundHinge.z, 1e-12);
}

TEST(GraffitoEpitaph, BasisRemainsOrthonormalAndRightHanded)
{
    for (double progress : {0.0, .2, .5, .8, 1.0})
    {
        const auto pose = BuildFallenEpitaphPose(
            -.63, progress, {2.0, 3.0, 100.0}, {5.0, 7.0, 2.0}, {4.0, -30.0}, .1);
        EXPECT_NEAR(LengthSquared(pose.normal), 1.0, 1e-12);
        EXPECT_NEAR(LengthSquared(pose.right), 1.0, 1e-12);
        EXPECT_NEAR(LengthSquared(pose.up), 1.0, 1e-12);
        EXPECT_NEAR(Dot(pose.normal, pose.right), 0.0, 1e-12);
        EXPECT_NEAR(Dot(pose.normal, pose.up), 0.0, 1e-12);
        EXPECT_NEAR(Dot(pose.right, pose.up), 0.0, 1e-12);
        EXPECT_NEAR(Dot(Cross(pose.right, pose.up), pose.normal), 1.0, 1e-12);
    }
}

TEST(GraffitoShaderContract, PacksCBufferRowsInHlslOrder)
{
    const std::array<float, 9> homography{1, 2, 3, 4, 5, 6, 7, 8, 9};
    const auto constants =
        Graffito::ShaderContract::Pack(homography, {10, 11, 12}, {13, 14}, {.25f, .75f, .5f});

    EXPECT_EQ(constants.sourceAnchor, (std::array<float, 4>{13, 14, 0, 0}));
    EXPECT_EQ(constants.segmentParams, (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(constants.materialParams, (std::array<float, 4>{.25f, .75f, .5f, 0}));
    EXPECT_EQ(constants.fisheyeParams, (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(constants.segments[0], (std::array<float, 4>{1, 2, 3, 0}));
    EXPECT_EQ(constants.segments[1], (std::array<float, 4>{4, 5, 6, 0}));
    EXPECT_EQ(constants.segments[2], (std::array<float, 4>{7, 8, 9, 0}));
    EXPECT_EQ(constants.segments[3], (std::array<float, 4>{10, 11, 12, 0}));
}

TEST(GraffitoShaderContract, FisheyePinsEndpointsAndMagnifiesTheMiddle)
{
    Graffito::ShaderContract::Constants constants{};
    constants.fisheyeParams = {100.0f, 50.0f, .01f, 1.0f};

    const auto center =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {100.0f, 60.0f});
    const auto rightMid =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {150.0f, 60.0f});
    const auto leftMid =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {50.0f, 60.0f});
    const auto edge = Graffito::ShaderContract::EvaluateFisheyePosition(constants, {200.0f, 60.0f});

    EXPECT_FLOAT_EQ(center[0], 100.0f);
    EXPECT_NEAR(center[1], 61.8f, 1e-5f);
    EXPECT_NEAR(rightMid[0], 159.0f, 1e-5f);
    EXPECT_NEAR(leftMid[0], 41.0f, 1e-5f);
    EXPECT_FLOAT_EQ(edge[0], 200.0f);
    EXPECT_NEAR(edge[1], 57.4f, 1e-5f);
    EXPECT_GT(center[1] - 50.0f, edge[1] - 50.0f);
}

TEST(GraffitoShaderContract, FisheyeExpandsCenterIntervalsAndCompressesWingIntervals)
{
    Graffito::ShaderContract::Constants constants{};
    constants.fisheyeParams = {100.0f, 50.0f, .01f, 1.0f};

    const auto centerLeft =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {99.0f, 50.0f});
    const auto centerRight =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {101.0f, 50.0f});
    const auto wingInner =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {190.0f, 50.0f});
    const auto wingEnd =
        Graffito::ShaderContract::EvaluateFisheyePosition(constants, {200.0f, 50.0f});

    EXPECT_GT(centerRight[0] - centerLeft[0], 2.0f);
    EXPECT_LT(wingEnd[0] - wingInner[0], 10.0f);
}

TEST(GraffitoShaderContract, ZeroFisheyeIsExactlyIdentity)
{
    Graffito::ShaderContract::Constants constants{};
    constants.fisheyeParams = {100.0f, 50.0f, .01f, .0f};
    EXPECT_EQ(Graffito::ShaderContract::EvaluateFisheyePosition(constants, {137.0f, 63.0f}),
              (std::array<float, 2>{137.0f, 63.0f}));

    constants.fisheyeParams = {100.0f, 50.0f, .0f, 1.0f};
    EXPECT_EQ(Graffito::ShaderContract::EvaluateFisheyePosition(constants, {137.0f, 63.0f}),
              (std::array<float, 2>{137.0f, 63.0f}));
}

TEST(GraffitoShaderContract, GoldenVertexMatchesHlslComposition)
{
    const std::array<float, 9> homography{
        .08f, .01f, -.2f, -.005f, .07f, .15f, .002f, -.003f, 1.0f};
    const auto constants = Graffito::ShaderContract::Pack(
        homography, {.17f, -.08f, .42f}, {100.0f, 200.0f}, {0.0f, 1.0f, 1.0f});
    const auto clip = Graffito::ShaderContract::EvaluateVertex(constants, {104.0f, 197.0f});

    EXPECT_NEAR(clip.x, .09f, 1e-6f);
    EXPECT_NEAR(clip.y, -.08f, 1e-6f);
    EXPECT_NEAR(clip.z, .477355f, 1e-6f);
    EXPECT_NEAR(clip.w, 1.017f, 1e-6f);
}

TEST(GraffitoShaderContract, UsesTopDownScreenYForDepth)
{
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto constants =
        Graffito::ShaderContract::Pack(identity, {0, 1, 0}, {10.0f, 20.0f}, {0, 1, 1});
    const auto clip = Graffito::ShaderContract::EvaluateVertex(constants, {10.0f, 20.5f});

    EXPECT_FLOAT_EQ(clip.y, .5f);
    EXPECT_FLOAT_EQ(clip.w, 1.0f);
    EXPECT_FLOAT_EQ(clip.z, .25f);
}

TEST(GraffitoShaderContract, MatchesHomographyAndDepthPlaneAcrossProbes)
{
    const std::array<float, 9> homographyValues{
        .08f, .01f, -.2f, -.005f, .07f, .15f, .002f, -.003f, 1.0f};
    const std::array<float, 3> depthValues{.17f, -.08f, .42f};
    const std::array<float, 2> anchor{100.0f, 200.0f};
    const auto constants =
        Graffito::ShaderContract::Pack(homographyValues, depthValues, anchor, {0, 1, 1});
    const Homography homography{{.08, .01, -.2, -.005, .07, .15, .002, -.003, 1.0}};
    const AffinePlane depth{.17, -.08, .42};
    const std::array<Vec2, 4> probes{
        {{100.0, 200.0}, {104.0, 197.0}, {92.5, 211.0}, {130.0, 230.0}}};

    for (const auto& absolute : probes)
    {
        const Vec2 local{absolute.x - anchor[0], absolute.y - anchor[1]};
        Vec2 expectedNdc{};
        ASSERT_TRUE(homography.Transform(local, expectedNdc));

        const auto clip = Graffito::ShaderContract::EvaluateVertex(
            constants, {static_cast<float>(absolute.x), static_cast<float>(absolute.y)});
        ASSERT_GT(std::abs(clip.w), 1e-6f);
        EXPECT_NEAR(clip.w, homography.Denominator(local), 1e-6);
        EXPECT_NEAR(clip.x / clip.w, expectedNdc.x, 1e-6);
        EXPECT_NEAR(clip.y / clip.w, expectedNdc.y, 1e-6);

        const double screenX = (expectedNdc.x + 1.0) * .5;
        const double screenY = (1.0 - expectedNdc.y) * .5;
        EXPECT_NEAR(clip.z / clip.w, depth.Sample(screenX, screenY), 1e-6);
    }
}

TEST(GraffitoShaderContract, SegmentSelectionUsesEachChordAndIsContinuousAtBoundary)
{
    using Graffito::ShaderContract::MAX_SEGMENTS;
    using Graffito::ShaderContract::SegmentData;
    const std::array<float, 9> left{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::array<float, 9> right{2, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::array<float, 3> depth{0, 0, .5f};
    std::array<SegmentData, MAX_SEGMENTS> segments{};
    segments[0] = {left, depth};
    segments[1] = {right, depth};
    const auto constants = Graffito::ShaderContract::PackSegments(
        segments, 2, -10.0f, .1f, {0, 0}, {0, 0}, {0, 1, 1, 0});

    EXPECT_EQ(Graffito::ShaderContract::EvaluateSegmentSlot(constants, {-5, 3}), 0u);
    EXPECT_EQ(Graffito::ShaderContract::EvaluateSegmentSlot(constants, {0, 3}), 1u);
    EXPECT_EQ(Graffito::ShaderContract::EvaluateSegmentSlot(constants, {5, 3}), 1u);

    const auto boundary = Graffito::ShaderContract::EvaluateVertex(constants, {0, 3});
    const auto fromLeft = Graffito::ShaderContract::EvaluateVertex(
        Graffito::ShaderContract::Pack(left, depth, {0, 0}, {0, 1, 1}), {0, 3});
    const auto fromRight = Graffito::ShaderContract::EvaluateVertex(
        Graffito::ShaderContract::Pack(right, depth, {0, 0}, {0, 1, 1}), {0, 3});
    EXPECT_NEAR(boundary.x, fromLeft.x, 1e-6f);
    EXPECT_NEAR(boundary.x, fromRight.x, 1e-6f);
    EXPECT_NEAR(boundary.y, fromLeft.y, 1e-6f);
    EXPECT_NEAR(boundary.y, fromRight.y, 1e-6f);
    EXPECT_NEAR(boundary.z, fromLeft.z, 1e-6f);
    EXPECT_NEAR(boundary.z, fromRight.z, 1e-6f);
    EXPECT_NEAR(boundary.w, fromLeft.w, 1e-6f);
    EXPECT_NEAR(boundary.w, fromRight.w, 1e-6f);
}

TEST(GraffitoShaderContract, SegmentSlotSelectionCoversAllStrips)
{
    using Graffito::ShaderContract::MAX_SEGMENTS;
    using Graffito::ShaderContract::SegmentData;
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<SegmentData, MAX_SEGMENTS> segments{};
    for (auto& segment : segments)
    {
        segment = {identity, {0, 0, 1}};
    }
    constexpr std::size_t count = 8;
    const auto constants = Graffito::ShaderContract::PackSegments(
        segments, count, -40.0f, .1f, {0, 0}, {0, 0}, {0, 1, 1, 0});

    std::size_t previous = 0;
    for (int x = -60; x <= 60; ++x)
    {
        const std::size_t slot =
            Graffito::ShaderContract::EvaluateSegmentSlot(constants, {static_cast<float>(x), 0});
        EXPECT_LT(slot, count);
        EXPECT_GE(slot, previous);
        previous = slot;
    }
    EXPECT_EQ(Graffito::ShaderContract::EvaluateSegmentSlot(constants, {-1000, 0}), 0u);
    EXPECT_EQ(Graffito::ShaderContract::EvaluateSegmentSlot(constants, {1000, 0}), count - 1);
}

TEST(GraffitoShaderContract, WingWeightIsSmoothAndSymmetric)
{
    using Graffito::ShaderContract::MAX_SEGMENTS;
    using Graffito::ShaderContract::SegmentData;
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<SegmentData, MAX_SEGMENTS> segments{};
    segments[0] = {identity, {0, 0, 1}};
    const auto constants = Graffito::ShaderContract::PackSegments(
        segments, 1, 0, 0, {100, 200}, {100, .01f}, {0, 1, 1, 0});

    EXPECT_FLOAT_EQ(Graffito::ShaderContract::EvaluateWingWeight(constants, {100, 200}), 0.0f);
    EXPECT_FLOAT_EQ(Graffito::ShaderContract::EvaluateWingWeight(constants, {0, 200}), 1.0f);
    EXPECT_FLOAT_EQ(Graffito::ShaderContract::EvaluateWingWeight(constants, {200, 200}), 1.0f);
    EXPECT_FLOAT_EQ(Graffito::ShaderContract::EvaluateWingWeight(constants, {50, 200}), 0.5f);
    EXPECT_FLOAT_EQ(Graffito::ShaderContract::EvaluateWingWeight(constants, {150, 200}), 0.5f);
}

TEST(GraffitoShaderContract, FrontMaterialLeavesVertexColorUnchanged)
{
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto constants = Graffito::ShaderContract::Pack(identity, {0, 0, 1}, {0, 0}, {0, 1, 1});
    const Graffito::ShaderContract::VertexColor input{.8f, .3f, .1f, .42f};
    const auto output = Graffito::ShaderContract::EvaluateColor(constants, input);

    EXPECT_FLOAT_EQ(output.r, input.r);
    EXPECT_FLOAT_EQ(output.g, input.g);
    EXPECT_FLOAT_EQ(output.b, input.b);
    EXPECT_FLOAT_EQ(output.a, input.a);
}

TEST(GraffitoShaderContract, BackMaterialDesaturatesAndDimsWithoutChangingAlpha)
{
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto constants =
        Graffito::ShaderContract::Pack(identity, {0, 0, 1}, {0, 0}, {.82f, .72f, 1.0f});
    const Graffito::ShaderContract::VertexColor input{.8f, .3f, .1f, .42f};
    const auto output = Graffito::ShaderContract::EvaluateColor(constants, input);
    const float luminance = .8f * .2126f + .3f * .7152f + .1f * .0722f;

    EXPECT_NEAR(output.r, (.8f + (luminance - .8f) * .82f) * .72f, 1e-6f);
    EXPECT_NEAR(output.g, (.3f + (luminance - .3f) * .82f) * .72f, 1e-6f);
    EXPECT_NEAR(output.b, (.1f + (luminance - .1f) * .82f) * .72f, 1e-6f);
    EXPECT_FLOAT_EQ(output.a, input.a);
}

TEST(GraffitoShaderContract, MaterialOpacityMultipliesVertexAlpha)
{
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto constants =
        Graffito::ShaderContract::Pack(identity, {0, 0, 1}, {0, 0}, {0, 1, .35f});
    const Graffito::ShaderContract::VertexColor input{.8f, .3f, .1f, .42f};
    const auto output = Graffito::ShaderContract::EvaluateColor(constants, input);

    EXPECT_FLOAT_EQ(output.r, input.r);
    EXPECT_FLOAT_EQ(output.g, input.g);
    EXPECT_FLOAT_EQ(output.b, input.b);
    EXPECT_NEAR(output.a, input.a * .35f, 1e-7f);
}

TEST(GraffitoShaderContract, EdgeSheenAppearsOnlyOnWrappedWings)
{
    using Graffito::ShaderContract::MAX_SEGMENTS;
    using Graffito::ShaderContract::SegmentData;
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<SegmentData, MAX_SEGMENTS> segments{};
    segments[0] = {identity, {0, 0, 1}};
    const auto constants =
        Graffito::ShaderContract::PackSegments(segments, 1, 0, 0, {0, 0}, {0, 1}, {0, 1, 1, .2f});
    const Graffito::ShaderContract::VertexColor input{.8f, .3f, .1f, .42f};
    const auto center = Graffito::ShaderContract::EvaluateColor(constants, input, 0.0f);
    const auto wing = Graffito::ShaderContract::EvaluateColor(constants, input, 1.0f);

    EXPECT_FLOAT_EQ(center.r, input.r);
    EXPECT_FLOAT_EQ(center.g, input.g);
    EXPECT_FLOAT_EQ(center.b, input.b);
    EXPECT_NEAR(wing.r, .84f, 1e-6f);
    EXPECT_NEAR(wing.g, .44f, 1e-6f);
    EXPECT_NEAR(wing.b, .28f, 1e-6f);
    EXPECT_FLOAT_EQ(wing.a, input.a);
}
}  // namespace
