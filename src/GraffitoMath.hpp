#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

/**
 * @namespace Graffito::Math
 * @brief Pure geometry helpers for perspective-projected world text.
 * @author Alex (https://github.com/lextpf)
 *
 * This header has no ImGui, Direct3D, or Skyrim dependencies, so these routines
 * are directly unit-testable. The renderer-facing Graffito module converts
 * engine types at the edge.
 *
 * Every routine is stateless and touches no shared state, so the game thread and
 * the render thread may both call it. Unless a parameter states otherwise: world
 * positions and lengths are in game units, angles are in radians, times are in
 * seconds, and source offsets are in ImGui pixels with +Y downward.
 */
namespace Graffito::Math
{
inline constexpr double PI = 3.1415926535897932384626433832795;
inline constexpr double TWO_PI = PI * 2.0;

/**
 * @struct Vec2
 * @brief Plain two-dimensional math value.
 *
 * The value can represent a
 * source-space offset, a screen point, or a solver sample.
 */
struct Vec2
{
    double x = 0.0;
    double y = 0.0;
};

/**
 * @struct Vec3
 * @brief Plain three-dimensional math value.
 *
 * The value can represent a
 * world position, a world direction, or one `(x, y, value)` sample
 * for the affine plane
 * solvers.
 */
struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator+(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

inline Vec3 operator-(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

inline Vec3 operator*(const Vec3& value, double scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

inline double Dot(const Vec3& lhs, const Vec3& rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

inline Vec3 Cross(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

inline double LengthSquared(const Vec3& value)
{
    return Dot(value, value);
}

/**
 * @brief Scale a vector to unit length.
 *
 * @param value   Vector to normalize.
 * @param out     Receives the unit vector. It is only meaningful when the
 *                function returns true, and it is set to zero when the length
 *                test fails.
 * @param epsilon Smallest accepted length. The test compares the squared length
 *                against epsilon squared.
 * @return true when the result is a finite unit vector; false for a non-finite
 *         input or for a length at or below epsilon.
 */
inline bool Normalize(const Vec3& value, Vec3& out, double epsilon = 1e-12)
{
    const double lenSq = LengthSquared(value);
    if (!std::isfinite(lenSq) || lenSq <= epsilon * epsilon)
    {
        out = {};
        return false;
    }

    const double invLen = 1.0 / std::sqrt(lenSq);
    out = value * invLen;
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

inline bool IsFinite(const Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/**
 * @brief Blend a newly measured world-space velocity into the previous estimate.
 *
 * An invalid sample keeps the prior finite estimate instead of injecting a
 * one-frame spike.
 *
 * @param previousVelocity  Prior estimate. A non-finite value is treated as zero.
 * @param previousSample    Earlier world position.
 * @param currentSample     Later world position.
 * @param sampleDeltaTime   Seconds between the two samples. A non-finite value,
 *                          or one at or below 1e-6, returns the prior estimate.
 * @param response          Blend weight for the new measurement, clamped to [0, 1].
 * @return The blended velocity, in world units per second.
 */
inline Vec3 BlendMotionVelocity(const Vec3& previousVelocity,
                                const Vec3& previousSample,
                                const Vec3& currentSample,
                                double sampleDeltaTime,
                                double response)
{
    if (!IsFinite(previousSample) || !IsFinite(currentSample) || !std::isfinite(sampleDeltaTime) ||
        sampleDeltaTime <= 1e-6)
    {
        return IsFinite(previousVelocity) ? previousVelocity : Vec3{};
    }

    const Vec3 measured = (currentSample - previousSample) * (1.0 / sampleDeltaTime);
    if (!IsFinite(measured))
    {
        return IsFinite(previousVelocity) ? previousVelocity : Vec3{};
    }

    const Vec3 finitePrevious = IsFinite(previousVelocity) ? previousVelocity : Vec3{};
    response = std::clamp(response, 0.0, 1.0);
    return finitePrevious * (1.0 - response) + measured * response;
}

/**
 * @brief Predict a sampled world position just far enough to bridge render frames.
 *
 * The horizon is also capped at 1.5 observed sample intervals, so a stalled
 * snapshot freezes near the last credible pose instead of running away.
 *
 * @param sample          Last known world position.
 * @param velocity        Current velocity estimate, in world units per second.
 * @param sampleAge       Seconds since @p sample was taken. It is raised to 0.
 * @param sampleInterval  Observed seconds between samples, clamped to
 * [0.001, `maxHorizon`].
 *
 * @param maxHorizon      Upper bound on the extrapolation time, in seconds. The default is 50 ms.
 * It must be 0.001 or more; a smaller positive bound inverts the limits of the sample interval
 * clamp below, which is undefined behavior.
 * @param maxDisplacement Upper bound on the extrapolated offset length, in world
 *                        units. The default is 32.
 * @return The extrapolated position. Any non-finite input, or a non-positive
 *         bound, returns `sample` unchanged; a non-finite `sample` returns the
 *         zero vector.
 */
inline Vec3 PredictMotionPosition(const Vec3& sample,
                                  const Vec3& velocity,
                                  double sampleAge,
                                  double sampleInterval,
                                  double maxHorizon = .050,
                                  double maxDisplacement = 32.0)
{
    if (!IsFinite(sample) || !IsFinite(velocity) || !std::isfinite(sampleAge) ||
        !std::isfinite(sampleInterval) || !std::isfinite(maxHorizon) ||
        !std::isfinite(maxDisplacement) || maxHorizon <= 0.0 || maxDisplacement <= 0.0)
    {
        return IsFinite(sample) ? sample : Vec3{};
    }

    sampleAge = std::max(0.0, sampleAge);
    sampleInterval = std::clamp(sampleInterval, .001, maxHorizon);
    const double horizon = std::min(sampleAge, std::min(maxHorizon, sampleInterval * 1.5));
    Vec3 offset = velocity * horizon;
    const double offsetLengthSquared = LengthSquared(offset);
    const double maxLengthSquared = maxDisplacement * maxDisplacement;
    if (!std::isfinite(offsetLengthSquared))
    {
        return sample;
    }
    if (offsetLengthSquared > maxLengthSquared)
    {
        offset = offset * (maxDisplacement / std::sqrt(offsetLengthSquared));
    }
    return sample + offset;
}

/**
 * @brief Return the first non-negative distance at which a ray enters a sphere.
 *
 * The direction is normalized internally, so callers can pass the camera basis
 * verbatim. An origin already inside the sphere returns zero.
 *
 * @param rayOrigin    Ray start point. A non-finite value returns infinity.
 * @param rayDirection Ray direction, of any length. A zero-length or non-finite
 *                     direction returns infinity.
 * @param sphereCenter Sphere center. A non-finite value returns infinity.
 * @param sphereRadius Sphere radius. A value at or below zero returns infinity;
 *                     it does not degenerate to a point test.
 * @param maxDistance  Largest accepted entry distance, in world units. A
 *                     negative or NaN value returns infinity. The default
 *                     accepts any distance.
 * @return Entry distance along the normalized direction, or infinity for no hit.
 */
inline double RaySphereHitDistance(const Vec3& rayOrigin,
                                   const Vec3& rayDirection,
                                   const Vec3& sphereCenter,
                                   double sphereRadius,
                                   double maxDistance = std::numeric_limits<double>::infinity())
{
    constexpr double NO_HIT = std::numeric_limits<double>::infinity();
    if (!std::isfinite(rayOrigin.x) || !std::isfinite(rayOrigin.y) || !std::isfinite(rayOrigin.z) ||
        !std::isfinite(sphereCenter.x) || !std::isfinite(sphereCenter.y) ||
        !std::isfinite(sphereCenter.z) || !std::isfinite(sphereRadius) || sphereRadius <= 0.0 ||
        std::isnan(maxDistance) || maxDistance < 0.0)
    {
        return NO_HIT;
    }

    Vec3 direction{};
    if (!Normalize(rayDirection, direction))
    {
        return NO_HIT;
    }

    const Vec3 toCenter = sphereCenter - rayOrigin;
    const double centerDistance = Dot(toCenter, direction);
    const double toCenterSquared = LengthSquared(toCenter);
    if (!std::isfinite(centerDistance) || !std::isfinite(toCenterSquared))
    {
        return NO_HIT;
    }

    const double radiusSquared = sphereRadius * sphereRadius;
    const double perpendicularSquared =
        std::max(0.0, toCenterSquared - centerDistance * centerDistance);
    if (!std::isfinite(radiusSquared) || perpendicularSquared > radiusSquared)
    {
        return NO_HIT;
    }

    const double halfChord = std::sqrt(std::max(0.0, radiusSquared - perpendicularSquared));
    if (centerDistance + halfChord < 0.0)
    {
        return NO_HIT;
    }

    const double entryDistance = std::max(0.0, centerDistance - halfChord);
    return entryDistance <= maxDistance ? entryDistance : NO_HIT;
}

/**
 * @struct UprightBasis
 * @brief Orthonormal right-handed page axes for upright actor text.
 */
struct UprightBasis
{
    Vec3 forward{};  ///< Front normal: the side from which the text is readable.
    Vec3 right{};    ///< Page-right when viewed from the actor's front.
    Vec3 up{};       ///< World up, always (0, 0, 1).
};

/**
 * @brief Build the stable yaw-only basis used by actor-bound Graffito.
 *
 * Skyrim's yaw-zero forward is +Y, hence `(sin(yaw), cos(yaw), 0)`. Page-right
 * is `worldUp x forward`; the actor's anatomical right would mirror the
 * lettering for a camera standing in front of the actor.
 *
 * The result obeys the PlanePose convention, `forward == Cross(right, up)`, so
 * it can be used as a plane basis directly.
 *
 * @param yawRadians Actor yaw, in radians. It is not scrubbed: a non-finite yaw
 *                   gives a non-finite basis.
 * @return The orthonormal right-handed page basis.
 */
inline UprightBasis BuildUprightBasis(double yawRadians)
{
    const Vec3 worldUp{0.0, 0.0, 1.0};
    const Vec3 forward{std::sin(yawRadians), std::cos(yawRadians), 0.0};
    return {forward, Cross(worldUp, forward), worldUp};
}

/**
 * @brief Resolve Skyrim yaw whose readable normal points from an anchor toward a target.
 *
 * Only the horizontal separation is used, because a yaw has no vertical part.
 *
 * @param anchor       Plane position.
 * @param target       Point the readable normal must face.
 * @param fallbackYaw  Returned when the horizontal separation is non-finite or
 *                     at or below 1e-5 world units. A non-finite fallback
 *                     itself yields 0.
 * @return The yaw, in radians. A resolved yaw is from -pi through pi. A
 *         returned fallback is passed through as given and is not wrapped.
 */
inline double YawFacingPoint(const Vec3& anchor, const Vec3& target, double fallbackYaw)
{
    const double dx = target.x - anchor.x;
    const double dy = target.y - anchor.y;
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) <= 1e-5)
    {
        return std::isfinite(fallbackYaw) ? fallbackYaw : 0.0;
    }
    return std::atan2(dx, dy);
}

/**
 * @brief Wrap an angle to the turn centred on zero.
 *
 * The result is in radians, from -pi
 * through pi. A non-finite angle returns 0.
 *
 * @param angle  Angle to wrap, in radians.
 *
 * @return       The wrapped angle, from -pi through pi.
 */
inline double WrapAngle(double angle)
{
    if (!std::isfinite(angle))
    {
        return 0.0;
    }
    return std::remainder(angle, TWO_PI);
}

/**
 * @brief Signed shortest turn between two angles.
 *
 * @param fromRadians Start angle, in radians.
 * @param toRadians   End angle, in radians.
 * @return The turn from the start angle to the end angle, in radians, from -pi
 *         through pi. A non-finite input returns 0.
 */
inline double ShortestAngleDelta(double fromRadians, double toRadians)
{
    return WrapAngle(toRadians - fromRadians);
}

/**
 * @brief Frame-rate-independent exponential approach factor.
 *
 * With frame time $\Delta t$, settle time $T$ and residual error fraction
 * $\epsilon$, the factor is
 *
 * $$ \alpha = 1 - \epsilon^{\,\Delta t / T} $$
 *
 * so compounding it over a total time T leaves exactly epsilon of the original
 * error whatever the frame pacing. This matches the renderer's smoothing
 * convention.
 *
 * @param deltaTime  Frame time, in seconds. A non-finite or non-positive value
 *                   returns 0, so nothing moves this frame.
 * @param settleTime Time T, in seconds. A non-finite or non-positive value
 *                   returns 1, so the value snaps to its target.
 * @param epsilon    Error fraction still left at the settle time, clamped to
 *                   [1e-9, 1 - 1e-9]. The default leaves 1 percent.
 * @return The blend factor, from 0 through 1.
 */
inline double ExponentialAlpha(double deltaTime, double settleTime, double epsilon = 0.01)
{
    if (!std::isfinite(deltaTime) || deltaTime <= 0.0)
    {
        return 0.0;
    }
    if (!std::isfinite(settleTime) || settleTime <= 0.0)
    {
        return 1.0;
    }
    epsilon = std::clamp(epsilon, 1e-9, 1.0 - 1e-9);
    return std::clamp(1.0 - std::pow(epsilon, deltaTime / settleTime), 0.0, 1.0);
}

/**
 * @brief Smooth toward a target angle along the shortest turn.
 *
 * @param currentRadians Present angle. A non-finite value adopts the target.
 * @param targetRadians  Wanted angle. A non-finite value returns the wrapped
 *                       present angle without smoothing.
 * @param deltaTime      Frame time, in seconds.
 * @param settleTime     Settle time, in seconds; see ExponentialAlpha.
 * @param epsilon        Error fraction still left at the settle time.
 * @return The new angle, in radians, from -pi through pi.
 */
inline double SmoothAngle(double currentRadians,
                          double targetRadians,
                          double deltaTime,
                          double settleTime,
                          double epsilon = 0.01)
{
    if (!std::isfinite(currentRadians))
    {
        currentRadians = targetRadians;
    }
    if (!std::isfinite(targetRadians))
    {
        return WrapAngle(currentRadians);
    }
    const double alpha = ExponentialAlpha(deltaTime, settleTime, epsilon);
    return WrapAngle(currentRadians + ShortestAngleDelta(currentRadians, targetRadians) * alpha);
}

/**
 * @brief Cubic ease with zero slope at both ends.
 *
 * The input is clamped to the unit range first, so the result runs from 0
 * through 1. A
 * non-finite input is not scrubbed: an infinite value saturates to
 * the nearer end of that range,
 * while NaN passes the clamp unchanged and gives
 * NaN. No caller in this header can reach it with
 * a non-finite value.
 *
 * @param value  Input value.
 * @return       The eased value from zero
 * through one, or NaN when `value` is NaN.
 */
inline double SmoothStep(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

/**
 * @struct FacingMaterial
 * @brief View-dependent ink treatment for a two-sided inscription.
 *
 * The caller decides where opacity is applied: it is either folded into the
 * plate's other fades on the CPU or handed to the shader as the material
 * opacity, never both. Graffito's vertex shader consumes the desaturation and
 * brightness fields for every vertex, so fill, outline, and shadow all receive
 * the same reverse-side ink bleed.
 */
struct FacingMaterial
{
    double opacity = 0.0;       ///< Alpha multiplier for all projected ink, from 0 through 1.
    double desaturation = 0.0;  ///< Blend of RGB toward luminance, from 0 through 1.
    double brightness = 1.0;    ///< RGB multiplier applied after the desaturation.
};

// Fixed ink treatment of the two non-front cases. The reverse side keeps a trace
// of hue, while the edge seam is fully desaturated and darker still.
inline constexpr double BACK_BLEED_DESATURATION = 0.82;
inline constexpr double BACK_BLEED_BRIGHTNESS = 0.72;
inline constexpr double EDGE_SEAM_DESATURATION = 1.0;
inline constexpr double EDGE_SEAM_BRIGHTNESS = 0.55;

/**
 * @brief Resolve front ink, an edge-on seam, and mirrored backside bleed.
 *
 * Material colors are blended weighted by opacity, so there is no hue pop as the
 * front face gives way to the seam and the reverse ink.
 *
 * @param frontDot       Dot product of the plane normal with the unit direction
 *                       from the plane toward the viewer. A positive value
 *                       selects the readable side, a negative value the reverse
 *                       side, and exactly zero forces the pure edge seam. A
 *                       non-finite value returns a fully transparent material.
 * @param fadeDegrees    Angular transition band on either side of edge-on, in
 *                       degrees, clamped to at most 90. A non-finite or
 *                       non-positive value removes the band, but a frontDot of
 *                       exactly zero still gives the pure seam.
 * @param backBleedAlpha Opacity of the reverse-side ink, clamped to [0, 1].
 * @param edgeSeamAlpha  Opacity at the pure edge seam, clamped to [0, 1].
 * @return The blended opacity, desaturation, and brightness. When the blended
 *         opacity is at or below 1e-12 the blend has no weight, so the two
 *         treatment fields keep their defaults of 0 desaturation and 1
 *         brightness.
 */
inline FacingMaterial EvaluateFacingMaterial(double frontDot,
                                             double fadeDegrees,
                                             double backBleedAlpha,
                                             double edgeSeamAlpha)
{
    if (!std::isfinite(frontDot))
    {
        return {};
    }

    frontDot = std::clamp(frontDot, -1.0, 1.0);
    backBleedAlpha = std::isfinite(backBleedAlpha) ? std::clamp(backBleedAlpha, 0.0, 1.0) : 0.0;
    edgeSeamAlpha = std::isfinite(edgeSeamAlpha) ? std::clamp(edgeSeamAlpha, 0.0, 1.0) : 0.0;

    double sideProgress = 1.0;
    if (std::isfinite(fadeDegrees) && fadeDegrees > 0.0)
    {
        const double fadeRadians = std::clamp(fadeDegrees, 0.0, 90.0) * PI / 180.0;
        const double angleFromEdge = std::asin(std::abs(frontDot));
        sideProgress = fadeRadians > 1e-12 ? SmoothStep(angleFromEdge / fadeRadians) : 1.0;
    }

    if (frontDot == 0.0)
    {
        sideProgress = 0.0;
    }

    const FacingMaterial side =
        frontDot > 0.0
            ? FacingMaterial{1.0, 0.0, 1.0}
            : FacingMaterial{backBleedAlpha, BACK_BLEED_DESATURATION, BACK_BLEED_BRIGHTNESS};
    const FacingMaterial edge{edgeSeamAlpha, EDGE_SEAM_DESATURATION, EDGE_SEAM_BRIGHTNESS};
    const double sideContribution = side.opacity * sideProgress;
    const double edgeContribution = edge.opacity * (1.0 - sideProgress);

    FacingMaterial out{};
    out.opacity = sideContribution + edgeContribution;
    if (out.opacity <= 1e-12)
    {
        return out;
    }
    out.desaturation =
        (side.desaturation * sideContribution + edge.desaturation * edgeContribution) / out.opacity;
    out.brightness =
        (side.brightness * sideContribution + edge.brightness * edgeContribution) / out.opacity;
    return out;
}

/**
 * @struct PlanePose
 * @brief Position and orthonormal page basis for an arbitrary text plane.
 *
 * The three axes must be orthonormal and right-handed, with
 * `normal == Cross(right, up)`, and +normal is the readable side. Nothing checks
 * the invariant. CylinderPointFromSourceOffset ignores the stored normal and
 * recomputes `Cross(right, up)`, so a pose whose normal disagrees bends the
 * cylinder the wrong way with no diagnostic.
 */
struct PlanePose
{
    Vec3 origin{};  ///< World point that source offset (0, 0) maps to.
    Vec3 normal{};  ///< Front normal: the side from which the text is readable.
    Vec3 right{};   ///< Page-right axis, toward increasing source X.
    Vec3 up{};      ///< Page-up axis, toward decreasing source Y.
};

/**
 * @brief Translate a plane along its own normal without changing its basis.
 *
 * A positive distance moves toward the readable side. A non-finite distance
 * becomes zero, so a
 * bad depth setting cannot poison the projection.
 *
 * @param pose            Source plane pose.

 * * @param normalDistance  Signed distance along the plane normal.
 * @return                The
 * translated pose.
 */
inline PlanePose OffsetPlanePose(const PlanePose& pose, double normalDistance)
{
    const double distance = std::isfinite(normalDistance) ? normalDistance : 0.0;
    PlanePose out = pose;
    out.origin = pose.origin + pose.normal * distance;
    return out;
}

/**
 * @struct FolioWeights
 * @brief Continuous view weights for the full front and the compact marker.
 *
 * For any valid direction the three weights are a partition:
 * `front + spine + back == 1`, and `front * back == 0`, so the front and rear
 * treatments are never both active. A non-finite direction, or one whose front
 * and right components have a planar length at or below 1e-6 (a straight-down
 * view), returns all three weights as zero instead of a partition, so nothing
 * is drawn.
 */
struct FolioWeights
{
    double front = 0.0;  ///< Weight of the full front inscription.
    double spine = 0.0;  ///< Weight of the compact side facet.
    double back = 0.0;   ///< Weight of the rear facet.
    /// Side the spine and facet poses must use: +1 is page-right, -1 is
    /// page-left. It stays at +1 when all three weights are zero.
    int sideSign = 1;
};

/**
 * @brief Depth multipliers of the configured relief spacing, far to near.
 *
 * The caller offsets each relief plane by minus this multiple of the spacing
 * along the plane normal, so index 0 is the farthest layer and adjacent layers
 * are exactly one spacing apart.
 */
inline constexpr std::array<double, 3> FOLIO_RELIEF_STEPS{3.0, 2.0, 1.0};

/**
 * @struct FolioFacetMetrics
 * @brief Source-space dimensions and head clearance for the
 * compact marker.
 */
struct FolioFacetMetrics
{
    double reliefSpacing = 0.0;  ///< Gap between adjacent relief planes, in source pixels.
    double sideWidth = 48.0;     ///< Width of the side facet, in source pixels.
    double height = 56.0;        ///< Height of the marker, in source pixels.
    double rearWidth = 66.0;     ///< Width of the rear facet, in source pixels.
    double markerLift = 9.0;     ///< Clearance above the head anchor, in source pixels.

    /// @brief Distance from the front face to the farthest relief plane, in source pixels.
    double TotalReliefDepth() const { return reliefSpacing * FOLIO_RELIEF_STEPS.front(); }

    /**
     * @brief Rank glyph size for one facet, in source pixels.
     *
     * The size is 0.82 of the smaller of the facet width and the marker height,
     * then clamped to 42 through 96, so even a small facet carries a legible
     * rank.
     *
     * @param width Facet width, in source pixels.
     * @return The rank glyph size, in source pixels.
     */
    double RankSize(double width) const
    {
        return std::clamp(std::min(width, height) * .82, 42.0, 96.0);
    }
};

/**
 * @brief Derive the compact marker's metrics from the name typography.
 *
 * Relief spacing stays in source pixels, so changing typography cannot enlarge
 * the gap between adjacent planes. The reverse coupling does exist: the side
 * width is at least 1.5 times the total relief depth, so a deeper relief widens
 * that one field until its clamp stops it. The visible triangle is sized
 * independently at about half a name row high, so it remains legible when it
 * replaces the complete inscription at side and rear angles.
 *
 * @param nameFontSize        Name row font size, in source pixels. A non-finite
 *                            or negative value is treated as 0.
 * @param reliefSpacingPixels Wanted gap between adjacent relief planes, in
 *                            source pixels, clamped to [0, 28]. A non-finite
 *                            value is treated as 0, which disables the relief.
 * @return Metrics in source pixels. Height is clamped to [56, 120], side width
 *         to [48, 108], rear width to [66, 140], and marker lift to [8, 20], so
 *         every field stays positive even for a zero font size.
 */
inline FolioFacetMetrics ComputeFolioFacetMetrics(double nameFontSize, double reliefSpacingPixels)
{
    const double fontSize = std::isfinite(nameFontSize) ? std::max(0.0, nameFontSize) : 0.0;
    const double spacing =
        std::isfinite(reliefSpacingPixels) ? std::max(0.0, reliefSpacingPixels) : 0.0;

    FolioFacetMetrics out{};
    out.reliefSpacing = std::clamp(spacing, 0.0, 28.0);
    out.height = std::clamp(fontSize * .55, 56.0, 120.0);
    out.sideWidth = std::clamp(std::max(out.TotalReliefDepth() * 1.5, fontSize * .42), 48.0, 108.0);
    out.rearWidth = std::clamp(out.height * 1.18, 66.0, 140.0);
    out.markerLift = std::clamp(out.height * .16, 8.0, 20.0);
    return out;
}

// Crossfade band edges, as an azimuth in degrees from the readable front normal,
// measured in the page plane. The front inscription yields to the compact side
// facet across the first band, and the side facet yields to the rear facet
// across the second. The second band must start at or after the first band ends,
// or the FolioWeights partition no longer sums to 1.
inline constexpr double FOLIO_FRONT_TO_SPINE_START_DEGREES = 45.0;
inline constexpr double FOLIO_FRONT_TO_SPINE_END_DEGREES = 55.0;
inline constexpr double FOLIO_SPINE_TO_BACK_START_DEGREES = 110.0;
inline constexpr double FOLIO_SPINE_TO_BACK_END_DEGREES = 130.0;

/**
 * @brief Blend the full front, the compact side facet, and the rear facet.
 *
 * The pair is re-normalized to unit length in the page plane, which removes
 * camera elevation from the azimuth decision. The side facet only becomes
 * active at 45 degrees or more off front, where its selected-side sign is well
 * conditioned, so no hysteresis state is needed and there is no dead angle.
 *
 * @verbatim
 * azimuth from the readable front normal, in degrees
 *
 * 0          45        55             110       130        180
 * |--front----|~~fade~~~|----spine-----|~~fade~~~|---back----|
 * @endverbatim
 *
 * @param frontDot Component along the plane normal of the direction from the
 *                 plane toward the viewer. It need not be normalized.
 * @param rightDot Component of that same direction along the page-right axis.
 * @return The three weights and the selected side.
 */
inline FolioWeights EvaluateFolioWeights(double frontDot, double rightDot)
{
    if (!std::isfinite(frontDot) || !std::isfinite(rightDot))
    {
        return {};
    }

    const double planarLength = std::hypot(frontDot, rightDot);
    if (!std::isfinite(planarLength) || planarLength <= 1e-6)
    {
        return {};
    }

    const double normalizedFront = frontDot / planarLength;
    const double normalizedRight = rightDot / planarLength;
    const double angleDegrees = std::abs(std::atan2(normalizedRight, normalizedFront)) * 180.0 / PI;
    const double frontToSpine =
        SmoothStep((angleDegrees - FOLIO_FRONT_TO_SPINE_START_DEGREES) /
                   (FOLIO_FRONT_TO_SPINE_END_DEGREES - FOLIO_FRONT_TO_SPINE_START_DEGREES));
    const double spineToBack =
        SmoothStep((angleDegrees - FOLIO_SPINE_TO_BACK_START_DEGREES) /
                   (FOLIO_SPINE_TO_BACK_END_DEGREES - FOLIO_SPINE_TO_BACK_START_DEGREES));

    return {1.0 - frontToSpine,
            frontToSpine * (1.0 - spineToBack),
            spineToBack,
            normalizedRight < 0.0 ? -1 : 1};
}

/**
 * @brief Map a source-space offset onto an arbitrary plane pose.
 *
 * Source +Y is downward, matching ImGui, so it maps toward the negative pose up
 * axis. A
 * non-finite offset component or scale is treated as 0, so the result
 * still lies on the plane.

 * *
 * @param pose                Plane pose.
 * @param sourceOffset        Source-space offset, in
 * pixels.
 * @param worldUnitsPerPixel  Plane scale in world units per source pixel.
 * @return The
 * mapped world point.
 */
inline Vec3 WorldPointFromSourceOffset(const PlanePose& pose,
                                       const Vec2& sourceOffset,
                                       double worldUnitsPerPixel)
{
    const double x = std::isfinite(sourceOffset.x) ? sourceOffset.x : 0.0;
    const double y = std::isfinite(sourceOffset.y) ? sourceOffset.y : 0.0;
    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;
    return pose.origin + pose.right * (x * scale) - pose.up * (y * scale);
}

/**
 * @brief Map a source-space offset onto a cylinder wrapped about the pose up axis.
 *
 * @p apexOffsetXPixels locates the arc apex relative to the pose's source
 * anchor, and @p radiansPerPixel is the constant curvature; a zero curvature, or
 * a zero radius, reproduces WorldPointFromSourceOffset exactly. Every scalar
 * argument is treated as 0 when it is not finite, so a non-finite curvature or
 * radius takes that same flat path. The apex column is the fixed point of the
 * wrap: at the apex the cylinder and the flat plane give the same world point,
 * for every source Y.
 *
 * @p surfaceRadius is this surface's own radius, so concentric layers can share
 * a single axis. The angle always comes from the caller's curvature and is never
 * re-derived from arc length at this radius; preserving arc length instead would
 * slide a layer sideways against the front it belongs to.
 *
 * With origin `O`, axes `R`, `U` and `N = R x U`, scale `s`
 * (`worldUnitsPerPixel`), apex `a` (`apexOffsetXPixels`), curvature `k`
 * (`radiansPerPixel`) and radius `r` (`surfaceRadius`):
 *
 * $$
 * P(x,y) = O + R\,\bigl(a\,s + r\sin\theta\bigr)
 *            + N\,r\,\bigl(\cos\theta - 1\bigr) - U\,(y\,s),
 * \qquad \theta = (x - a)\,k
 * $$
 *
 * The axis is the line through `O + R(a s) - N r` along `U`, one radius behind
 * the apex, so the apex is the point closest to the readable side and the wings
 * recede along `-N`.
 *
 * @verbatim
 * Cross-section viewed along U (theta > 0):
 *
 *                         +N
 *                          ^
 *                          A    P
 *                          |   /
 *                        r |  / r
 *                          | /
 *                          +----------------> +R
 *                          C
 *
 * A: apex, x = a, theta = 0
 * C: cylinder axis, O + R(a s) - N r
 * P: wrapped point, theta = (x - a) k
 *
 * Both A and P are r units from C. Positive theta turns toward +R.
 * @endverbatim
 *
 * Because the horizontal term is `r sin(theta)` while the angle comes from `k`
 * alone, `r` also
 * sets the physical width: the small-angle limit `r*theta`
 * matches WorldPointFromSourceOffset
 * only when `r == s / k`. Any other radius
 * rescales the inscription by `r*k/s` instead of only
 * curving it.
 *
 * @param pose                Plane pose.
 * @param sourceOffset Source-space
 * offset, in pixels.
 * @param worldUnitsPerPixel  Plane scale in world units per source pixel.
 *
 * @param apexOffsetXPixels   Horizontal arc-apex offset from the source anchor.
 * @param
 * radiansPerPixel     Cylindrical curvature.
 * @param surfaceRadius       Radius of this
 * concentric surface, in world units.
 * @return                    The mapped world point.
 */
inline Vec3 CylinderPointFromSourceOffset(const PlanePose& pose,
                                          const Vec2& sourceOffset,
                                          double worldUnitsPerPixel,
                                          double apexOffsetXPixels,
                                          double radiansPerPixel,
                                          double surfaceRadius)
{
    const double kappa = std::isfinite(radiansPerPixel) ? radiansPerPixel : 0.0;
    const double radius = std::isfinite(surfaceRadius) ? surfaceRadius : 0.0;
    if (kappa == 0.0 || radius == 0.0)
    {
        return WorldPointFromSourceOffset(pose, sourceOffset, worldUnitsPerPixel);
    }

    const double x = std::isfinite(sourceOffset.x) ? sourceOffset.x : 0.0;
    const double y = std::isfinite(sourceOffset.y) ? sourceOffset.y : 0.0;
    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;
    const double apex = std::isfinite(apexOffsetXPixels) ? apexOffsetXPixels : 0.0;
    const double theta = (x - apex) * kappa;
    const Vec3 normal = Cross(pose.right, pose.up);
    return pose.origin + pose.right * (apex * scale + radius * std::sin(theta)) +
           normal * (radius * (std::cos(theta) - 1.0)) - pose.up * (y * scale);
}

/**
 * @brief Build a rear-facing pose that preserves the front plate's footprint.
 *
 * @p sourcePivotXPixels is the reflection axis relative to the front pose's
 * source anchor. For every source point `(x,y)`, the returned pose maps it to
 * the same world point as the front pose maps `(2*pivot-x,y)`.
 *
 * @param frontPose          Front plate pose. Its basis invariant is preserved.
 * @param sourcePivotXPixels Reflection axis, in source pixels relative to the
 *                           front source anchor. A non-finite value becomes 0.
 * @param worldUnitsPerPixel Physical scale of one source pixel, in world units.
 *                           A non-finite value becomes 0.
 * @return The rear-facing pose, sharing the front pose's up axis.
 */
inline PlanePose BuildFolioBackPose(const PlanePose& frontPose,
                                    double sourcePivotXPixels,
                                    double worldUnitsPerPixel)
{
    const double pivot = std::isfinite(sourcePivotXPixels) ? sourcePivotXPixels : 0.0;
    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;

    PlanePose out{};
    out.origin = frontPose.origin + frontPose.right * (2.0 * pivot * scale);
    out.normal = frontPose.normal * -1.0;
    out.right = frontPose.right * -1.0;
    out.up = frontPose.up;
    return out;
}

/**
 * @brief Build the compact side-indicator plane at a selected edge of the front.
 *
 * @p frontEdgeOffsetPixels is the edge point relative to the front source
 * anchor, normally the selected horizontal bound and the plate's vertical
 * center. The returned origin is the spine layout's own source anchor.
 *
 * The returned plane stands perpendicular to the front plate, and its readable
 * normal points away from the plate along the selected side: page-right for a
 * non-negative sideSign, page-left for a negative one. Source +X runs from the
 * front face toward the rear on the page-right side, and from the rear toward
 * the front face on the page-left side, so the inscription reads correctly from
 * whichever side is selected.
 *
 * @param frontPose             Front plate pose.
 * @param sideSign              Selected side. A negative value is page-left; 0
 *                              or positive is page-right.
 * @param frontEdgeOffsetPixels Edge point, in source pixels relative to the
 *                              front source anchor.
 * @param worldUnitsPerPixel    Physical scale of one source pixel, in world
 *                              units. A non-finite value becomes 0.
 * @return The side-indicator pose, pinned to the edge point.
 */
inline PlanePose BuildFolioSpinePose(const PlanePose& frontPose,
                                     int sideSign,
                                     const Vec2& frontEdgeOffsetPixels,
                                     double worldUnitsPerPixel)
{
    const double side = sideSign < 0 ? -1.0 : 1.0;

    PlanePose out{};
    out.origin = WorldPointFromSourceOffset(frontPose, frontEdgeOffsetPixels, worldUnitsPerPixel);
    out.normal = frontPose.right * side;
    out.right = frontPose.normal * -side;
    out.up = frontPose.up;
    return out;
}

/**
 * @brief Center a side-facet plane across the depth behind the front face.
 *
 * The returned pose keeps the handed side basis from BuildFolioSpinePose, but
 * its source anchor sits halfway between the inscription and its rear relief
 * layer. A triangle drawn symmetrically around that anchor therefore occupies
 * the real front-to-back span instead of lying across the front plane.
 *
 * @param frontPose             Front plate pose.
 * @param sideSign              Selected side; see BuildFolioSpinePose.
 * @param frontEdgeOffsetPixels Edge point, in source pixels relative to the
 *                              front source anchor.
 * @param worldUnitsPerPixel    Physical scale of one source pixel, in world
 *                              units. A non-finite value becomes 0.
 * @param depthWorldUnits       Front-to-back span to straddle, in world units.
 *                              A non-finite or negative value becomes 0, which
 *                              reproduces BuildFolioSpinePose exactly.
 * @return The centered side-facet pose.
 */
inline PlanePose BuildFolioFacetPose(const PlanePose& frontPose,
                                     int sideSign,
                                     const Vec2& frontEdgeOffsetPixels,
                                     double worldUnitsPerPixel,
                                     double depthWorldUnits)
{
    PlanePose out =
        BuildFolioSpinePose(frontPose, sideSign, frontEdgeOffsetPixels, worldUnitsPerPixel);
    const double depth = std::isfinite(depthWorldUnits) ? std::max(0.0, depthWorldUnits) : 0.0;
    out.origin = out.origin - frontPose.normal * (depth * 0.5);
    return out;
}

/**
 * @brief Hinge upright text onto the ground while pinning one source-space point.
 *
 * Source +Y is downward, matching ImGui and BuildProjection. At progress zero
 * the exact upright origin and basis are returned. At one, the page normal is
 * world-up, its top points back toward the actor, and @p sourceHinge lands on
 * @p groundHinge without sliding.
 *
 * @p progress is clamped to [0, 1] and smoothstep-eased before it drives both
 * the 0 to 90 degree hinge angle and the hinge translation, so mid-range values
 * are not linear in angle: 0.25 gives about 14.1 degrees, not 22.5. A
 * non-finite value is treated as 0.
 *
 * @param yawRadians         Actor yaw, in radians; see BuildUprightBasis. It is
 *                           not scrubbed.
 * @param progress           Hinge progress, from 0 upright through 1 landed.
 * @param uprightOrigin      World point that source offset (0, 0) maps to at
 *                           progress 0. It is not scrubbed.
 * @param groundHinge        World point the source hinge lands on at progress
 *                           1. It is not scrubbed.
 * @param sourceHinge        Pinned point, in source pixels relative to the
 *                           source anchor. It is not scrubbed.
 * @param worldUnitsPerPixel Physical scale of one source pixel, in world units.
 *                           A non-finite value becomes 0.
 * @return The hinged pose. For a finite yaw its basis stays orthonormal and
 *         right-handed, so it obeys the PlanePose convention at every progress.
 */
inline PlanePose BuildFallenEpitaphPose(double yawRadians,
                                        double progress,
                                        const Vec3& uprightOrigin,
                                        const Vec3& groundHinge,
                                        const Vec2& sourceHinge,
                                        double worldUnitsPerPixel)
{
    const UprightBasis upright = BuildUprightBasis(yawRadians);
    const double eased = SmoothStep(std::isfinite(progress) ? progress : 0.0);
    const double angle = eased * PI * 0.5;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);

    PlanePose out{};
    out.normal = upright.forward * cosine + upright.up * sine;
    out.right = upright.right;
    out.up = upright.up * cosine - upright.forward * sine;

    const double scale = std::isfinite(worldUnitsPerPixel) ? worldUnitsPerPixel : 0.0;
    const Vec3 uprightHinge =
        uprightOrigin + out.right * (sourceHinge.x * scale) - upright.up * (sourceHinge.y * scale);
    const Vec3 movingHinge = uprightHinge * (1.0 - eased) + groundHinge * eased;
    out.origin =
        movingHinge - out.right * (sourceHinge.x * scale) + out.up * (sourceHinge.y * scale);
    return out;
}

/**
 * @brief Smooth distance fade between a near and a far bound.
 *
 * The result is 1 at or below the near bound, 0 at or beyond the far bound, and
 * a smoothstep in between. A non-finite distance returns 0, so a bad
 * measurement hides the plate instead of drawing it at full strength.
 *
 * @param distance  Distance to the plate, in world units. It is raised to 0.
 * @param fadeStart Near bound, in world units. It is raised to 0. A non-finite
 *                  value becomes 0.
 * @param fadeEnd   Far bound, in world units. It is raised to 0. A non-finite
 *                  value adopts the near bound.
 * @return The fade factor, from 0 through 1. When the far bound is not above
 *         the near bound the band collapses to a hard cut at the far bound: 1
 *         at or below it, 0 above it.
 */
inline double RangeFade(double distance, double fadeStart, double fadeEnd)
{
    if (!std::isfinite(distance))
    {
        return 0.0;
    }
    distance = std::max(0.0, distance);
    fadeStart = std::max(0.0, std::isfinite(fadeStart) ? fadeStart : 0.0);
    fadeEnd = std::max(0.0, std::isfinite(fadeEnd) ? fadeEnd : fadeStart);
    if (fadeEnd <= fadeStart)
    {
        return distance <= fadeEnd ? 1.0 : 0.0;
    }
    return 1.0 - SmoothStep((distance - fadeStart) / (fadeEnd - fadeStart));
}

/**
 * @struct Homography
 * @brief Row-major 3-by-3 projective map.
 *
 * For a source point
 * $(x,y)$, the homogeneous map and perspective divide are
 *
 * $$
 * \begin{bmatrix} X \\ Y \\ W
 * \end{bmatrix}
 * = H \begin{bmatrix} x \\ y \\ 1 \end{bmatrix},
 * \qquad (u,v) =
 * \left(\frac{X}{W},\frac{Y}{W}\right),
 * \qquad |W| > \epsilon.
 * $$
 *
 * Transform accepts
 * either sign of $W$. BuildProjection applies the stronger invariant
 * $W > 0$ at its control
 * points, so adjacent chord maps keep one orientation.
 */
struct Homography
{
    /// Nine entries in row order: the two numerator rows, then the denominator
    /// row. The default is the identity map.
    std::array<double, 9> m{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

    /**
     * @brief Map a point and apply the perspective divide.
     *
     * @param point   Source point.
     * @param out     Receives the mapped point. It is set to zero when the
     *                denominator test fails, and holds a non-finite value when
     *                the divide itself overflows.
     * @param epsilon Smallest accepted absolute denominator.
     * @return true when the mapped point is finite; false for a non-finite or
     *         near-zero denominator, or for a non-finite quotient.
     */
    bool Transform(const Vec2& point, Vec2& out, double epsilon = 1e-12) const
    {
        const double w = m[6] * point.x + m[7] * point.y + m[8];
        if (!std::isfinite(w) || std::abs(w) <= epsilon)
        {
            out = {};
            return false;
        }
        out.x = (m[0] * point.x + m[1] * point.y + m[2]) / w;
        out.y = (m[3] * point.x + m[4] * point.y + m[5]) / w;
        return std::isfinite(out.x) && std::isfinite(out.y);
    }

    /**
     * @brief Sample the denominator row at a point, without the divide.
     *
     * BuildProjection uses it to reject a chord whose denominator is not
     * positive at every control point, because a sign change there means the
     * map folds.
     *
     * @param point Source point.
     * @return The denominator, which is the homogeneous W of the mapped point.
     */
    double Denominator(const Vec2& point) const { return m[6] * point.x + m[7] * point.y + m[8]; }
};

/**
 * @struct AffinePlane
 * @brief Affine scalar field over two-dimensional coordinates.
 *
 * The
 * field is `value = xSlope*x + ySlope*y + constant`.
 */
struct AffinePlane
{
    double xSlope = 0.0;
    double ySlope = 0.0;
    double constant = 0.0;

    double Sample(double x, double y) const { return xSlope * x + ySlope * y + constant; }
};

namespace detail
{
using Matrix3 = std::array<double, 9>;

/**
 * @brief Multiply two row-major 3-by-3 matrices.
 *
 * @param lhs  Left matrix.
 * @param rhs
 * Right matrix.
 * @return     The product `lhs * rhs`.
 */
inline Matrix3 Multiply(const Matrix3& lhs, const Matrix3& rhs)
{
    Matrix3 out{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t col = 0; col < 3; ++col)
        {
            for (std::size_t k = 0; k < 3; ++k)
            {
                out[row * 3 + col] += lhs[row * 3 + k] * rhs[k * 3 + col];
            }
        }
    }
    return out;
}

/**
 * @brief Solve a square system by Gauss-Jordan elimination with scaled pivoting.
 *
 * Each row is scored by its own largest absolute coefficient, so a system that
 * mixes pixel-sized and unit-sized rows still picks sane pivots.
 *
 * @tparam N        Number of unknowns.
 * @param augmented The N by N+1 augmented matrix. It is overwritten in place,
 *                  including on a failed solve.
 * @param out       Receives the N solution values. It is only meaningful when
 *                  the function returns true.
 * @param epsilon   Degeneracy threshold. A row scale, a scaled pivot score, or
 *                  a pivot relative to its row scale at or below it fails.
 * @return true when every unknown resolved to a finite value.
 */
template <std::size_t N>
inline bool SolveLinear(double (&augmented)[N][N + 1], std::array<double, N>& out, double epsilon)
{
    std::array<double, N> rowScale{};
    for (std::size_t row = 0; row < N; ++row)
    {
        for (std::size_t col = 0; col < N; ++col)
        {
            rowScale[row] = std::max(rowScale[row], std::abs(augmented[row][col]));
        }
        if (!std::isfinite(rowScale[row]) || rowScale[row] <= epsilon)
        {
            return false;
        }
    }

    for (std::size_t col = 0; col < N; ++col)
    {
        std::size_t pivot = col;
        double pivotScore = 0.0;
        for (std::size_t row = col; row < N; ++row)
        {
            const double score = std::abs(augmented[row][col]) / rowScale[row];
            if (score > pivotScore)
            {
                pivotScore = score;
                pivot = row;
            }
        }
        if (!std::isfinite(pivotScore) || pivotScore <= epsilon)
        {
            return false;
        }
        if (pivot != col)
        {
            for (std::size_t k = 0; k <= N; ++k)
            {
                std::swap(augmented[col][k], augmented[pivot][k]);
            }
            std::swap(rowScale[col], rowScale[pivot]);
        }

        const double divisor = augmented[col][col];
        if (!std::isfinite(divisor) || std::abs(divisor) <= epsilon * rowScale[col])
        {
            return false;
        }
        for (std::size_t k = col; k <= N; ++k)
        {
            augmented[col][k] /= divisor;
        }

        for (std::size_t row = 0; row < N; ++row)
        {
            if (row == col)
            {
                continue;
            }
            const double factor = augmented[row][col];
            if (factor == 0.0)
            {
                continue;
            }
            for (std::size_t k = col; k <= N; ++k)
            {
                augmented[row][k] -= factor * augmented[col][k];
            }
        }
    }

    for (std::size_t row = 0; row < N; ++row)
    {
        out[row] = augmented[row][N];
        if (!std::isfinite(out[row]))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Hartley normalization of four points: centre them, then scale them.
 *
 * The scale puts the mean distance from the centroid at the square root of 2,
 * which is what keeps the eight-equation homography solve conditioned for both
 * pixel coordinates and tiny normalized-screen quads.
 *
 * @param points     Four input points.
 * @param normalized Receives the centred and scaled points.
 * @param transform  Receives the map from input space to normalized space.
 * @param inverse    Receives the exact inverse of that map.
 * @param epsilon    Smallest accepted mean distance from the centroid.
 * @return true on success; false for a non-finite point or a point set that is
 *         effectively one point. All three out parameters are then left
 *         unchanged.
 */
inline bool Normalization(const std::array<Vec2, 4>& points,
                          std::array<Vec2, 4>& normalized,
                          Matrix3& transform,
                          Matrix3& inverse,
                          double epsilon)
{
    Vec2 center{};
    for (const auto& point : points)
    {
        if (!std::isfinite(point.x) || !std::isfinite(point.y))
        {
            return false;
        }
        center.x += point.x;
        center.y += point.y;
    }
    center.x *= 0.25;
    center.y *= 0.25;

    double meanDistance = 0.0;
    for (const auto& point : points)
    {
        meanDistance += std::hypot(point.x - center.x, point.y - center.y);
    }
    meanDistance *= 0.25;
    if (!std::isfinite(meanDistance) || meanDistance <= epsilon)
    {
        return false;
    }

    const double scale = std::sqrt(2.0) / meanDistance;
    transform = {scale, 0.0, -scale * center.x, 0.0, scale, -scale * center.y, 0.0, 0.0, 1.0};
    inverse = {1.0 / scale, 0.0, center.x, 0.0, 1.0 / scale, center.y, 0.0, 0.0, 1.0};
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        normalized[i] = {(points[i].x - center.x) * scale, (points[i].y - center.y) * scale};
    }
    return true;
}
}  // namespace detail

/**
 * @brief Solve the unique projective map between two non-degenerate four-point sets.
 *
 * Hartley normalization plus scaled partial-pivot elimination keeps the solve
 * stable for both pixel coordinates and tiny normalized-screen quads.
 *
 * @param source      Four source points, in correspondence order.
 * @param destination Four destination points, in the same order.
 * @param out         Receives the solved map. It is cleared first.
 * @param epsilon     Degeneracy threshold for normalization, elimination, and
 *                    the denominator test.
 * @return true when the solve succeeded and every correspondence reprojects to
 *         within 1e-7 times the largest absolute destination coordinate, where
 *         that coordinate is treated as at least 1.
 *
 * @post The returned matrix is scaled so its largest absolute entry is exactly
 *       1, and signed so the denominator at the source-set centroid is
 *       positive. BuildProjection depends on both halves of that gauge: it
 *       rejects any chord whose denominator is not positive at its own control
 *       points, rescales every later chord to match its neighbour's denominator
 *       at a shared point, then requires the two to still agree at two shared
 *       corners within a tolerance that never tightens below 1e-4 absolute. A
 *       change to either convention makes the wrapped solve reject its chords
 *       and fall back to the flat single-quad plate, without failing this
 *       function's own validation.
 */
inline bool SolveHomography(const std::array<Vec2, 4>& source,
                            const std::array<Vec2, 4>& destination,
                            Homography& out,
                            double epsilon = 1e-10)
{
    out = {};
    std::array<Vec2, 4> src{};
    std::array<Vec2, 4> dst{};
    detail::Matrix3 srcTransform{};
    detail::Matrix3 srcInverse{};
    detail::Matrix3 dstTransform{};
    detail::Matrix3 dstInverse{};
    if (!detail::Normalization(source, src, srcTransform, srcInverse, epsilon) ||
        !detail::Normalization(destination, dst, dstTransform, dstInverse, epsilon))
    {
        return false;
    }

    double equations[8][9]{};
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        const double x = src[i].x;
        const double y = src[i].y;
        const double u = dst[i].x;
        const double v = dst[i].y;
        const std::size_t row = i * 2;
        equations[row][0] = x;
        equations[row][1] = y;
        equations[row][2] = 1.0;
        equations[row][6] = -u * x;
        equations[row][7] = -u * y;
        equations[row][8] = u;

        equations[row + 1][3] = x;
        equations[row + 1][4] = y;
        equations[row + 1][5] = 1.0;
        equations[row + 1][6] = -v * x;
        equations[row + 1][7] = -v * y;
        equations[row + 1][8] = v;
    }

    std::array<double, 8> solution{};
    if (!detail::SolveLinear(equations, solution, epsilon))
    {
        return false;
    }

    const detail::Matrix3 normalized{solution[0],
                                     solution[1],
                                     solution[2],
                                     solution[3],
                                     solution[4],
                                     solution[5],
                                     solution[6],
                                     solution[7],
                                     1.0};
    out.m = detail::Multiply(dstInverse, detail::Multiply(normalized, srcTransform));

    double largest = 0.0;
    for (double value : out.m)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        largest = std::max(largest, std::abs(value));
    }
    if (largest <= epsilon)
    {
        return false;
    }
    for (double& value : out.m)
    {
        value /= largest;
    }

    Vec2 sourceCenter{};
    for (const auto& point : source)
    {
        sourceCenter.x += point.x * 0.25;
        sourceCenter.y += point.y * 0.25;
    }
    double centerDenominator = out.Denominator(sourceCenter);
    if (!std::isfinite(centerDenominator) || std::abs(centerDenominator) <= epsilon)
    {
        return false;
    }
    if (centerDenominator < 0.0)
    {
        for (double& value : out.m)
        {
            value = -value;
        }
        centerDenominator = -centerDenominator;
    }

    double destinationScale = 1.0;
    for (const auto& point : destination)
    {
        destinationScale = std::max({destinationScale, std::abs(point.x), std::abs(point.y)});
    }
    const double tolerance = 1e-7 * destinationScale;
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        Vec2 projected{};
        if (!out.Transform(source[i], projected, epsilon) ||
            std::hypot(projected.x - destination[i].x, projected.y - destination[i].y) > tolerance)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Fit an affine scalar plane through four samples, or reject the set.
 *
 * This is not a least-squares fit. The three samples forming the largest
 * triangle in normalized space are solved exactly, then all four residuals are
 * checked and the whole fit is rejected when any of them is too large.
 * BuildProjection relies on that rejection to drop a non-planar strip.
 *
 * @param samples           Four samples as `(x, y, value)`.
 * @param out               Receives the fitted plane. It is cleared first.
 * @param epsilon           Degeneracy threshold for normalization, triangle
 *                          area, and elimination.
 * @param residualTolerance Relative tolerance. The absolute limit is
 *                          `residualTolerance * max(1, largest |value|)`.
 * @return true when the triangle is well conditioned and every residual is
 *         within the limit; false otherwise.
 */
inline bool SolveAffinePlane(const std::array<Vec3, 4>& samples,
                             AffinePlane& out,
                             double epsilon = 1e-10,
                             double residualTolerance = 1e-4)
{
    out = {};
    std::array<Vec2, 4> points{};
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (!std::isfinite(samples[i].x) || !std::isfinite(samples[i].y) ||
            !std::isfinite(samples[i].z))
        {
            return false;
        }
        points[i] = {samples[i].x, samples[i].y};
    }

    std::array<Vec2, 4> normalized{};
    detail::Matrix3 transform{};
    detail::Matrix3 inverse{};
    if (!detail::Normalization(points, normalized, transform, inverse, epsilon))
    {
        return false;
    }

    // Pick the largest normalized-screen triangle. It gives the best
    // conditioned exact plane; the remaining sample is used for validation.
    std::array<std::size_t, 3> best{0, 1, 2};
    double bestArea = 0.0;
    for (std::size_t a = 0; a < 2; ++a)
    {
        for (std::size_t b = a + 1; b < 3; ++b)
        {
            for (std::size_t c = b + 1; c < 4; ++c)
            {
                const double area = std::abs(
                    (normalized[b].x - normalized[a].x) * (normalized[c].y - normalized[a].y) -
                    (normalized[b].y - normalized[a].y) * (normalized[c].x - normalized[a].x));
                if (area > bestArea)
                {
                    bestArea = area;
                    best = {a, b, c};
                }
            }
        }
    }
    if (!std::isfinite(bestArea) || bestArea <= epsilon)
    {
        return false;
    }

    double equations[3][4]{};
    for (std::size_t row = 0; row < best.size(); ++row)
    {
        const std::size_t i = best[row];
        equations[row][0] = normalized[i].x;
        equations[row][1] = normalized[i].y;
        equations[row][2] = 1.0;
        equations[row][3] = samples[i].z;
    }
    std::array<double, 3> normalizedPlane{};
    if (!detail::SolveLinear(equations, normalizedPlane, epsilon))
    {
        return false;
    }

    const double scale = transform[0];
    const double centerX = -transform[2] / scale;
    const double centerY = -transform[5] / scale;
    out.xSlope = normalizedPlane[0] * scale;
    out.ySlope = normalizedPlane[1] * scale;
    out.constant = normalizedPlane[2] - out.xSlope * centerX - out.ySlope * centerY;

    double valueScale = 1.0;
    for (const auto& sample : samples)
    {
        valueScale = std::max(valueScale, std::abs(sample.z));
    }
    for (const auto& sample : samples)
    {
        const double residual = std::abs(out.Sample(sample.x, sample.y) - sample.z);
        if (!std::isfinite(residual) || residual > residualTolerance * valueScale)
        {
            return false;
        }
    }
    return std::isfinite(out.xSlope) && std::isfinite(out.ySlope) && std::isfinite(out.constant);
}

/**
 * @brief Best-fit affine depth field over an arbitrary number of samples.
 *
 * A curved plate has no exact depth plane, so the whole-plate value passed to
 * the depth-clip stage is an approximation. Unlike SolveAffinePlane, which
 * solves the best-conditioned three samples exactly and rejects the fit on any
 * disagreeing residual, this minimizes squared error over every sample and
 * reports the worst residual, leaving the accept or reject decision to the
 * caller. BuildProjection discards that residual, so its wrapped-plate depth
 * carries no accuracy guarantee.
 *
 * @param samples     Pointer to @p count samples as `(x, y, value)`.
 * @param count       Number of samples. Fewer than 3 returns false.
 * @param out         Receives the fitted plane. It is cleared first.
 * @param maxResidual Receives the largest absolute residual over the samples. It
 *                    is set to 0 before the solve.
 * @param epsilon     Degeneracy threshold for the normal-equation elimination.
 * @return true when the solve succeeded; false on a null pointer, too few
 *         samples, a non-finite sample, a singular system, or a non-finite
 *         residual.
 */
inline bool SolveAffinePlaneLeastSquares(const Vec3* samples,
                                         std::size_t count,
                                         AffinePlane& out,
                                         double& maxResidual,
                                         double epsilon = 1e-10)
{
    out = {};
    maxResidual = 0.0;
    if (samples == nullptr || count < 3)
    {
        return false;
    }

    double sumX = 0.0, sumY = 0.0, sumZ = 0.0, sumXX = 0.0, sumXY = 0.0, sumYY = 0.0, sumXZ = 0.0,
           sumYZ = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const Vec3& sample = samples[i];
        if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z))
        {
            return false;
        }
        sumX += sample.x;
        sumY += sample.y;
        sumZ += sample.z;
        sumXX += sample.x * sample.x;
        sumXY += sample.x * sample.y;
        sumYY += sample.y * sample.y;
        sumXZ += sample.x * sample.z;
        sumYZ += sample.y * sample.z;
    }

    double equations[3][4] = {{sumXX, sumXY, sumX, sumXZ},
                              {sumXY, sumYY, sumY, sumYZ},
                              {sumX, sumY, static_cast<double>(count), sumZ}};
    std::array<double, 3> solution{};
    if (!detail::SolveLinear(equations, solution, epsilon))
    {
        return false;
    }
    if (!std::isfinite(solution[0]) || !std::isfinite(solution[1]) || !std::isfinite(solution[2]))
    {
        return false;
    }

    out.xSlope = solution[0];
    out.ySlope = solution[1];
    out.constant = solution[2];
    for (std::size_t i = 0; i < count; ++i)
    {
        const double residual = std::abs(out.Sample(samples[i].x, samples[i].y) - samples[i].z);
        if (!std::isfinite(residual))
        {
            return false;
        }
        maxResidual = std::max(maxResidual, residual);
    }
    return true;
}
}  // namespace Graffito::Math
