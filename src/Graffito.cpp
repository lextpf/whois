#include "PCH.hpp"

#include "Graffito.hpp"
#include "GraffitoMath.hpp"
#include "GraffitoShaderContract.hpp"

#include <SKSE/SKSE.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace Graffito
{
namespace
{
// ImDrawVert's ordinary POSITION/TEXCOORD/COLOR input remains unchanged, so the
// ImGui input layout and the bound pixel shader keep working. Two things differ
// from ImGui's stock orthographic VS: the position becomes a real projective
// transform, and the color receives the ink material. Homography rows produce
// (clipX, clipY, clipW). The normalized-screen affine depth plane is converted
// back to clipZ so the GPU receives a real homogeneous position and
// perspective-correctly interpolates UV/color.
//
// kPlaneVS and GraffitoShaderContract.hpp are hand-maintained mirrors of each
// other. Nothing in the build cross-checks them, so edit both together.
constexpr const char* kPlaneVS = R"(
cbuffer GraffitoCB : register(b0)
{
    float4 sourceAnchor;
    float4 segmentParams;
    float4 materialParams;
    float4 fisheyeParams;
    float4 segments[48];
};

struct VS_INPUT
{
    float2 pos : POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;

    // Give every typography row its own lens profile. The cubic horizontal
    // offset pins both endpoints, expands the middle interval, and compresses
    // the wings. Height falls from 1.18x at the midpoint to .74x at the ends,
    // making the fisheye visible even in a head-on view.
    float invHalfWidth = max(fisheyeParams.z, 0.0);
    float fisheyeStrength = invHalfWidth > 0.0 ? saturate(fisheyeParams.w) : 0.0;
    float halfWidth = invHalfWidth > 0.0 ? rcp(invHalfWidth) : 0.0;
    float t = clamp((input.pos.x - fisheyeParams.x) * invHalfWidth, -1.0, 1.0);
    float edge = abs(t);
    float edgeWeight = smoothstep(0.0, 1.0, edge);
    float verticalScale = 1.0 + fisheyeStrength * lerp(0.18, -0.26, edgeWeight);
    float2 warpedPosition = input.pos;
    warpedPosition.x += halfWidth * 0.24 * fisheyeStrength * t * (1.0 - t * t);
    warpedPosition.y =
        fisheyeParams.y + (input.pos.y - fisheyeParams.y) * verticalScale;

    // A vertical cylinder is a ruled surface. Adjacent chord planes share an
    // entire vertical ruling, so hard source-X selection is continuous and
    // preserves each strip's exact projective mapping.
    float slot = floor((warpedPosition.x - segmentParams.x) * segmentParams.y);
    int base = ((int)clamp(slot, 0.0, segmentParams.z)) << 2;

    float3 source = float3(warpedPosition - sourceAnchor.xy, 1.0);
    float3 clipXYW = float3(dot(segments[base + 0].xyz, source),
                            dot(segments[base + 1].xyz, source),
                            dot(segments[base + 2].xyz, source));

    // Convert homogeneous NDC XY to homogeneous normalized-screen XY.
    // Screen Y is top-down, unlike NDC Y.
    float2 screenNumerator =
        0.5 * float2(clipXYW.x + clipXYW.z, clipXYW.z - clipXYW.y);
    float depthNumerator =
        dot(segments[base + 3].xyz, float3(screenNumerator, clipXYW.z));
    output.pos = float4(clipXYW.xy, depthNumerator, clipXYW.z);

    float wing = saturate(abs(input.pos.x - sourceAnchor.z) * max(sourceAnchor.w, 0.0));
    wing = smoothstep(0.0, 1.0, wing);
    float luminance = dot(input.col.rgb, float3(0.2126, 0.7152, 0.0722));
    output.col.rgb = lerp(input.col.rgb,
                          luminance.xxx,
                          saturate(materialParams.x)) * max(materialParams.y, 0.0);
    output.col.rgb = lerp(output.col.rgb,
                          float3(1.0, 1.0, 1.0),
                          saturate(materialParams.w) * wing);
    output.col.a = input.col.a * saturate(materialParams.z);
    output.uv = input.uv;
    return output;
}
)";

using ShaderConstants = ShaderContract::Constants;

struct CallbackParams
{
    Projection projection{};
    InkMaterial material{};
};

struct SavedVertexState
{
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11Buffer> constantBuffer;
    bool active = false;
};

// One camera-projected control point. ndc spans -1 to 1 with y upward, screen01
// spans 0 to 1 with y downward (the DepthClip convention), and depth is the
// viewport depth WorldPtToScreenPt3 returns, in the 0 to 1 range.
struct ProjectedPoint
{
    Math::Vec2 ndc{};
    Math::Vec2 screen01{};
    double depth = 1.0;
};

ComPtr<ID3D11Device> s_Device;
ComPtr<ID3D11DeviceContext> s_Context;
ComPtr<ID3D11VertexShader> s_VertexShader;
ComPtr<ID3D11Buffer> s_ConstantBuffer;
std::deque<CallbackParams> s_ParamArena;
std::vector<SavedVertexState> s_StateStack;
bool s_Initialized = false;

// Preflight budget for the CPU mirror of kPlaneVS against the camera. The
// position budget is in NDC units, which span -1 to 1, so it is about a fifth of
// a pixel on a 1920-wide viewport. The depth budget is in viewport-depth units,
// which span 0 to 1. Both are tight enough to catch a wrong solve and loose
// enough to pass float rounding.
constexpr double kProjectionTolerance = 2e-4;
constexpr double kDepthTolerance = 5e-4;

Math::Vec3 ToMath(const RE::NiPoint3& point)
{
    return {
        static_cast<double>(point.x), static_cast<double>(point.y), static_cast<double>(point.z)};
}

RE::NiPoint3 ToEngine(const Math::Vec3& point)
{
    return {static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z)};
}

// Admission check for a projection, not only a finiteness test: it also enforces
// the ranges the shader assumes. A multi-chord plate needs a positive invStride,
// because slot selection multiplies the source offset by it: a zero value
// collapses every vertex onto chord 0, and a negative value reverses the chord
// order. A single-chord plate may leave invStride at 0. Fisheye strength must
// already lie in 0 to 1 here: the shader and the CPU mirror both saturate it, so
// an out-of-range value would be corrected silently. Dropping the plate instead
// keeps a caller bug visible.
bool IsFinite(const Projection& projection)
{
    if (!projection.valid || !std::isfinite(projection.sourceAnchor.x) ||
        !std::isfinite(projection.sourceAnchor.y) || projection.segmentCount < 1 ||
        projection.segmentCount > static_cast<int>(MAX_SEGMENTS) ||
        !std::isfinite(projection.firstBoundaryX) || !std::isfinite(projection.invStride) ||
        (projection.segmentCount > 1 && projection.invStride <= .0f) ||
        (projection.segmentCount == 1 && projection.invStride < .0f) ||
        !std::isfinite(projection.wingCenterX) || !std::isfinite(projection.wingInvHalfWidth) ||
        projection.wingInvHalfWidth < .0f || !std::isfinite(projection.fisheye.center.x) ||
        !std::isfinite(projection.fisheye.center.y) ||
        !std::isfinite(projection.fisheye.invHalfWidth) ||
        !std::isfinite(projection.fisheye.strength) || projection.fisheye.invHalfWidth < .0f ||
        projection.fisheye.strength < .0f || projection.fisheye.strength > 1.0f ||
        !std::isfinite(projection.plateDepth.xSlope) ||
        !std::isfinite(projection.plateDepth.ySlope) ||
        !std::isfinite(projection.plateDepth.constant))
    {
        return false;
    }
    const auto finite = [](float value) { return std::isfinite(value); };
    for (int i = 0; i < projection.segmentCount; ++i)
    {
        const auto& segment = projection.segments[static_cast<std::size_t>(i)];
        if (!std::ranges::all_of(segment.homography, finite) ||
            !std::isfinite(segment.depth.xSlope) || !std::isfinite(segment.depth.ySlope) ||
            !std::isfinite(segment.depth.constant))
        {
            return false;
        }
    }
    return true;
}

bool IsFinite(const InkMaterial& material)
{
    return std::isfinite(material.desaturation) && std::isfinite(material.brightness) &&
           std::isfinite(material.opacity) && std::isfinite(material.edgeSheen) &&
           material.desaturation >= .0f && material.desaturation <= 1.0f &&
           material.brightness >= .0f && material.opacity >= .0f && material.opacity <= 1.0f &&
           material.edgeSheen >= .0f && material.edgeSheen <= 1.0f;
}

bool ProjectPoint(RE::NiCamera* camera, const Math::Vec3& world, ProjectedPoint& out)
{
    if (!camera)
    {
        return false;
    }

    const auto& runtime = camera->GetRuntimeData();
    const auto& runtime2 = camera->GetRuntimeData2();
    float x = .0f;
    float y = .0f;
    float z = .0f;
    if (!RE::NiCamera::WorldPtToScreenPt3(
            runtime.worldToCam, runtime2.port, ToEngine(world), x, y, z, 1e-5f) ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
    {
        return false;
    }

    // WorldPtToScreenPt3's XY are normalized with Y increasing upward.
    // NDC uses the same Y direction; DepthClip samples normalized top-down Y.
    out.ndc = {static_cast<double>(x) * 2.0 - 1.0, static_cast<double>(y) * 2.0 - 1.0};
    out.screen01 = {static_cast<double>(x), 1.0 - static_cast<double>(y)};
    out.depth = static_cast<double>(z);

    // A control point outside the viewport is harmless, but one outside the
    // camera depth interval indicates a near/far-plane crossing. Cull the
    // whole plate rather than feed a sign-changing W to the rasterizer.
    constexpr double kDepthSlop = 1e-4;
    return out.depth >= -kDepthSlop && out.depth <= 1.0 + kDepthSlop;
}

// Scale-free thinness measure of a projected quad: twice its shoelace area
// divided by the squared bounding-box diagonal. The ratio approaches 0 as the
// quad degenerates toward a line, whatever its size on screen, so one fixed
// threshold works at every viewing distance.
double QuadAreaQuality(const std::array<Math::Vec2, 4>& points)
{
    double twiceArea = 0.0;
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        const auto& current = points[i];
        const auto& next = points[(i + 1) % points.size()];
        twiceArea += current.x * next.y - current.y * next.x;
        minX = std::min(minX, current.x);
        minY = std::min(minY, current.y);
        maxX = std::max(maxX, current.x);
        maxY = std::max(maxY, current.y);
    }
    const double diagonalSq = (maxX - minX) * (maxX - minX) + (maxY - minY) * (maxY - minY);
    return diagonalSq > 0.0 ? std::abs(twiceArea) / diagonalSq : 0.0;
}

ShaderConstants MakeConstants(const Projection& projection, const InkMaterial& material)
{
    static_assert(MAX_SEGMENTS == ShaderContract::MAX_SEGMENTS);
    std::array<ShaderContract::SegmentData, ShaderContract::MAX_SEGMENTS> segments{};
    for (int i = 0; i < projection.segmentCount; ++i)
    {
        const auto& source = projection.segments[static_cast<std::size_t>(i)];
        segments[static_cast<std::size_t>(i)] = {
            source.homography, {source.depth.xSlope, source.depth.ySlope, source.depth.constant}};
    }
    return ShaderContract::PackSegments(
        segments,
        static_cast<std::size_t>(projection.segmentCount),
        projection.firstBoundaryX,
        projection.invStride,
        {projection.sourceAnchor.x, projection.sourceAnchor.y},
        {projection.wingCenterX, projection.wingInvHalfWidth},
        {material.desaturation, material.brightness, material.opacity, material.edgeSheen},
        {projection.fisheye.center.x,
         projection.fisheye.center.y,
         projection.fisheye.invHalfWidth,
         projection.fisheye.strength});
}
}  // namespace

bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (s_Initialized)
    {
        return true;
    }
    if (!device || !context)
    {
        return false;
    }

    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errors;
    const HRESULT compileResult = D3DCompile(kPlaneVS,
                                             std::strlen(kPlaneVS),
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             "main",
                                             "vs_5_0",
                                             D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                             0,
                                             shaderBlob.GetAddressOf(),
                                             errors.GetAddressOf());
    if (FAILED(compileResult))
    {
        if (errors)
        {
            logger::warn("Graffito: vertex shader compile failed: {}",
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }

    if (FAILED(device->CreateVertexShader(shaderBlob->GetBufferPointer(),
                                          shaderBlob->GetBufferSize(),
                                          nullptr,
                                          s_VertexShader.GetAddressOf())))
    {
        logger::warn("Graffito: failed to create plane vertex shader");
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(ShaderConstants);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, s_ConstantBuffer.GetAddressOf())))
    {
        logger::warn("Graffito: failed to create plane constant buffer");
        s_VertexShader.Reset();
        return false;
    }

    s_Device = device;
    s_Context = context;
    s_Initialized = true;
    logger::info("Graffito: initialized (perspective-correct world text available)");
    return true;
}

void Shutdown()
{
    s_StateStack.clear();
    s_ParamArena.clear();
    s_ConstantBuffer.Reset();
    s_VertexShader.Reset();
    s_Context.Reset();
    s_Device.Reset();
    s_Initialized = false;
}

bool IsInitialized()
{
    return s_Initialized && s_Context && s_VertexShader && s_ConstantBuffer;
}

void BeginFrame()
{
    // Rendering of the previous ImDrawData is synchronous on the immediate
    // context, so none of its callback pointers are still live here.
    s_ParamArena.clear();
    s_StateStack.clear();
}

bool BuildProjection(const WorldPlane& plane,
                     const ImVec2& sourceAnchor,
                     const SourceBounds& sourceBounds,
                     Projection& out)
{
    out = {};
    if (!std::isfinite(plane.worldUnitsPerPixel) || plane.worldUnitsPerPixel <= .0f ||
        !std::isfinite(plane.wrap.sourceCenterX) || !std::isfinite(plane.wrap.radiansPerPixel) ||
        plane.wrap.radiansPerPixel < .0f || !std::isfinite(plane.wrap.surfaceRadius) ||
        plane.wrap.surfaceRadius < .0f || plane.wrap.segmentCount < 1 ||
        !std::isfinite(sourceAnchor.x) || !std::isfinite(sourceAnchor.y) ||
        !std::isfinite(sourceBounds.min.x) || !std::isfinite(sourceBounds.min.y) ||
        !std::isfinite(sourceBounds.max.x) || !std::isfinite(sourceBounds.max.y) ||
        sourceBounds.max.x <= sourceBounds.min.x || sourceBounds.max.y <= sourceBounds.min.y)
    {
        return false;
    }

    Math::Vec3 right{};
    if (!Math::Normalize(ToMath(plane.right), right))
    {
        return false;
    }
    Math::Vec3 up = ToMath(plane.up);
    up = up - right * Math::Dot(up, right);  // Re-orthogonalize up against right.
    if (!Math::Normalize(up, up))
    {
        return false;
    }
    const Math::Vec3 normal = Math::Cross(right, up);
    const Math::Vec3 origin = ToMath(plane.origin);
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z))
    {
        return false;
    }

    auto* camera = RE::Main::WorldRootCamera();
    if (!camera)
    {
        return false;
    }

    const double minX = static_cast<double>(sourceBounds.min.x - sourceAnchor.x);
    const double minY = static_cast<double>(sourceBounds.min.y - sourceAnchor.y);
    const double maxX = static_cast<double>(sourceBounds.max.x - sourceAnchor.x);
    const double maxY = static_cast<double>(sourceBounds.max.y - sourceAnchor.y);
    const double scale = static_cast<double>(plane.worldUnitsPerPixel);
    const Math::PlanePose pose{origin, normal, right, up};

    const auto signedTwiceArea = [](const std::array<Math::Vec2, 4>& points)
    {
        double area = .0;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const auto& current = points[i];
            const auto& next = points[(i + 1) % points.size()];
            area += current.x * next.y - current.y * next.x;
        }
        return area;
    };

    const auto solveSegment = [&](const std::array<Math::Vec2, 4>& source,
                                  const std::array<ProjectedPoint, 4>& projected,
                                  Projection::Segment& segment,
                                  double& signedArea,
                                  const Projection::Segment* scaleReference,
                                  const Math::Vec2& sharedScalePoint)
    {
        std::array<Math::Vec2, 4> destinationNdc{};
        std::array<Math::Vec3, 4> depthSamples{};
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            destinationNdc[i] = projected[i].ndc;
            depthSamples[i] = {
                projected[i].screen01.x, projected[i].screen01.y, projected[i].depth};
        }
        const double areaQuality = QuadAreaQuality(destinationNdc);
        signedArea = signedTwiceArea(destinationNdc);
        if (!std::isfinite(areaQuality) || areaQuality <= 1e-6 || !std::isfinite(signedArea) ||
            std::abs(signedArea) <= 1e-12)
        {
            return false;
        }

        Math::Homography homography{};
        if (!Math::SolveHomography(source, destinationNdc, homography))
        {
            return false;
        }
        // A homography is defined only up to scale, so a uniform rescale leaves
        // X/W, Y/W and Z/W untouched. Match this chord's W to the previous
        // chord's W at the shared ruling, so a glyph quad that straddles the
        // seam interpolates UV and color between two consistent W values.
        if (scaleReference)
        {
            const auto& reference = scaleReference->homography;
            const double referenceW = static_cast<double>(reference[6]) * sharedScalePoint.x +
                                      static_cast<double>(reference[7]) * sharedScalePoint.y +
                                      static_cast<double>(reference[8]);
            const double currentW = homography.Denominator(sharedScalePoint);
            if (!std::isfinite(referenceW) || !std::isfinite(currentW) || referenceW <= 1e-7 ||
                currentW <= 1e-7)
            {
                return false;
            }
            const double commonScale = referenceW / currentW;
            for (double& value : homography.m)
            {
                value *= commonScale;
            }
        }
        for (const auto& control : source)
        {
            if (homography.Denominator(control) <= 1e-7)
            {
                return false;
            }
        }

        Math::AffinePlane depthPlane{};
        if (!Math::SolveAffinePlane(depthSamples, depthPlane))
        {
            return false;
        }
        for (std::size_t i = 0; i < segment.homography.size(); ++i)
        {
            segment.homography[i] = static_cast<float>(homography.m[i]);
        }
        segment.depth.xSlope = static_cast<float>(depthPlane.xSlope);
        segment.depth.ySlope = static_cast<float>(depthPlane.ySlope);
        segment.depth.constant = static_cast<float>(depthPlane.constant);
        // Re-check W agreement at both ends of the shared ruling, this time on
        // the float values the shader will use. The rescale above was fitted at
        // the ruling midpoint only, so a relative disagreement here means the
        // two chords do not meet after the narrowing to float.
        if (scaleReference)
        {
            const auto denominator = [](const std::array<float, 9>& values, const Math::Vec2& point)
            {
                return static_cast<double>(values[6]) * point.x +
                       static_cast<double>(values[7]) * point.y + static_cast<double>(values[8]);
            };
            for (const std::size_t corner : {std::size_t{0}, std::size_t{3}})
            {
                const double referenceW = denominator(scaleReference->homography, source[corner]);
                const double currentW = denominator(segment.homography, source[corner]);
                const double magnitude = std::max({1.0, std::abs(referenceW), std::abs(currentW)});
                if (!std::isfinite(referenceW) || !std::isfinite(currentW) ||
                    std::abs(referenceW - currentW) > 1e-4 * magnitude)
                {
                    return false;
                }
            }
        }
        return true;
    };

    // Accuracy gate. Run the CPU mirror of kPlaneVS over a candidate and compare
    // it against the camera's own projection of the same world point. A solve
    // can be finite and well-conditioned and still be wrong, so no candidate is
    // published without passing this at every probe.
    const auto preflight = [&](const Projection& candidate,
                               const std::array<float, 2>& absolutePosition,
                               const Math::Vec3& world)
    {
        ProjectedPoint expected{};
        if (!ProjectPoint(camera, world, expected))
        {
            return false;
        }
        const ShaderConstants constants = MakeConstants(candidate, {});
        const auto clip = ShaderContract::EvaluateVertex(constants, absolutePosition);
        return std::isfinite(clip.x) && std::isfinite(clip.y) && std::isfinite(clip.z) &&
               std::isfinite(clip.w) && clip.w > 1e-7f &&
               std::hypot(clip.x / clip.w - expected.ndc.x, clip.y / clip.w - expected.ndc.y) <=
                   kProjectionTolerance &&
               std::abs(clip.z / clip.w - expected.depth) <= kDepthTolerance;
    };

    const auto solveFlat = [&]()
    {
        Projection candidate{};
        const std::array<Math::Vec2, 4> source{
            {{minX, minY}, {maxX, minY}, {maxX, maxY}, {minX, maxY}}};
        std::array<ProjectedPoint, 4> projected{};
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            const Math::Vec3 world = Math::WorldPointFromSourceOffset(pose, source[i], scale);
            if (!ProjectPoint(camera, world, projected[i]))
            {
                return false;
            }
        }

        double signedArea = .0;
        if (!solveSegment(source, projected, candidate.segments[0], signedArea, nullptr, {}))
        {
            return false;
        }
        candidate.sourceAnchor = sourceAnchor;
        candidate.segmentCount = 1;
        candidate.firstBoundaryX = sourceBounds.min.x;
        candidate.invStride = .0f;
        candidate.wingCenterX = plane.wrap.sourceCenterX;
        candidate.wingInvHalfWidth = .0f;
        candidate.plateDepth = candidate.segments[0].depth;
        candidate.valid = true;
        if (!IsFinite(candidate) || !preflight(candidate, {sourceAnchor.x, sourceAnchor.y}, origin))
        {
            return false;
        }
        out = candidate;
        return true;
    };

    // Cut the bounds into segmentCount vertical strips of equal source width.
    // Ruling i is shared by chord i-1 and chord i, so both chords project the
    // same two world points and the seam carries no gap:
    //
    //   source X   minX       x1        x2        x3       maxX
    //   ruling      R0        R1        R2        R3        R4
    //               |---------|---------|---------|---------|
    //   chord            C0        C1        C2        C3
    //
    // Each chord gets its own exact homography and depth plane. The shader picks
    // the chord with floor((x - firstBoundaryX) * invStride), so equal source
    // width is what makes a single reciprocal stride sufficient.
    const auto solveWrapped = [&](int requestedSegments)
    {
        const int segmentCount = std::clamp(requestedSegments, 2, static_cast<int>(MAX_SEGMENTS));
        Projection candidate{};
        std::array<std::array<ProjectedPoint, 2>, MAX_SEGMENTS + 1> rulings{};
        std::array<Math::Vec3, 2 * (MAX_SEGMENTS + 1)> plateDepthSamples{};
        const double stride = (maxX - minX) / static_cast<double>(segmentCount);
        const double apexOffset = static_cast<double>(plane.wrap.sourceCenterX - sourceAnchor.x);

        for (int i = 0; i <= segmentCount; ++i)
        {
            const double x = minX + stride * static_cast<double>(i);
            const std::array<double, 2> yValues{minY, maxY};
            for (std::size_t endpoint = 0; endpoint < yValues.size(); ++endpoint)
            {
                const Math::Vec2 source{x, yValues[endpoint]};
                const Math::Vec3 world =
                    Math::CylinderPointFromSourceOffset(pose,
                                                        source,
                                                        scale,
                                                        apexOffset,
                                                        plane.wrap.radiansPerPixel,
                                                        plane.wrap.surfaceRadius);
                if (!ProjectPoint(camera, world, rulings[static_cast<std::size_t>(i)][endpoint]))
                {
                    return false;
                }
                const auto& point = rulings[static_cast<std::size_t>(i)][endpoint];
                plateDepthSamples[static_cast<std::size_t>(i) * 2 + endpoint] = {
                    point.screen01.x, point.screen01.y, point.depth};
            }
        }

        // Every chord must project with the same winding. A sign flip means the
        // surface folds over inside the bounds, which no per-chord homography
        // can represent, so reject the whole wrap and let the ladder fall back.
        double windingSign = .0;
        for (int i = 0; i < segmentCount; ++i)
        {
            const double x0 = minX + stride * static_cast<double>(i);
            const double x1 = x0 + stride;
            const std::array<Math::Vec2, 4> source{
                {{x0, minY}, {x1, minY}, {x1, maxY}, {x0, maxY}}};
            const auto& left = rulings[static_cast<std::size_t>(i)];
            const auto& rightRuling = rulings[static_cast<std::size_t>(i + 1)];
            const std::array<ProjectedPoint, 4> projected{
                {left[0], rightRuling[0], rightRuling[1], left[1]}};
            double signedArea = .0;
            const Projection::Segment* scaleReference =
                i > 0 ? &candidate.segments[static_cast<std::size_t>(i - 1)] : nullptr;
            const Math::Vec2 sharedScalePoint{x0, (minY + maxY) * .5};
            if (!solveSegment(source,
                              projected,
                              candidate.segments[static_cast<std::size_t>(i)],
                              signedArea,
                              scaleReference,
                              sharedScalePoint))
            {
                return false;
            }
            const double sign = std::copysign(1.0, signedArea);
            if (i == 0)
            {
                windingSign = sign;
            }
            else if (sign != windingSign)
            {
                return false;
            }
        }

        Math::AffinePlane wholeDepth{};
        double maxResidual = .0;
        const std::size_t sampleCount = static_cast<std::size_t>(2 * (segmentCount + 1));
        if (!Math::SolveAffinePlaneLeastSquares(
                plateDepthSamples.data(), sampleCount, wholeDepth, maxResidual))
        {
            return false;
        }

        candidate.sourceAnchor = sourceAnchor;
        candidate.segmentCount = segmentCount;
        candidate.firstBoundaryX = sourceBounds.min.x;
        candidate.invStride =
            static_cast<float>(static_cast<double>(segmentCount) /
                               static_cast<double>(sourceBounds.max.x - sourceBounds.min.x));
        candidate.wingCenterX = plane.wrap.sourceCenterX;
        // A half-width at or below 1e-3 source pixels cannot carry a sheen ramp.
        // Disable the sheen instead of dividing by it.
        const float wingHalfWidth = std::max(std::abs(sourceBounds.min.x - candidate.wingCenterX),
                                             std::abs(sourceBounds.max.x - candidate.wingCenterX));
        candidate.wingInvHalfWidth = wingHalfWidth > 1e-3f ? 1.0f / wingHalfWidth : .0f;
        candidate.plateDepth.xSlope = static_cast<float>(wholeDepth.xSlope);
        candidate.plateDepth.ySlope = static_cast<float>(wholeDepth.ySlope);
        candidate.plateDepth.constant = static_cast<float>(wholeDepth.constant);
        candidate.valid = true;
        if (!IsFinite(candidate))
        {
            return false;
        }

        // Probe the first chord, the chord that owns the source anchor, and the
        // last chord. probeY clamps the anchor row into the bounds, so a plate
        // whose anchor sits outside its own bounds still probes a real row. The
        // world reference is the midpoint of the straight chord between the two
        // rulings, not the arc midpoint, because each strip's homography maps
        // that chord plane and not the cylinder surface.
        const int anchorSegment =
            std::clamp(static_cast<int>(
                           std::floor((sourceAnchor.x - sourceBounds.min.x) * candidate.invStride)),
                       0,
                       segmentCount - 1);
        const std::array<int, 3> probes{0, anchorSegment, segmentCount - 1};
        const double probeY = std::clamp(.0, minY, maxY);
        for (int segmentIndex : probes)
        {
            const double x0 = minX + stride * static_cast<double>(segmentIndex);
            const double x1 = x0 + stride;
            const double probeX = (x0 + x1) * .5;
            const Math::Vec3 left = Math::CylinderPointFromSourceOffset(pose,
                                                                        {x0, probeY},
                                                                        scale,
                                                                        apexOffset,
                                                                        plane.wrap.radiansPerPixel,
                                                                        plane.wrap.surfaceRadius);
            const Math::Vec3 rightPoint =
                Math::CylinderPointFromSourceOffset(pose,
                                                    {x1, probeY},
                                                    scale,
                                                    apexOffset,
                                                    plane.wrap.radiansPerPixel,
                                                    plane.wrap.surfaceRadius);
            const Math::Vec3 chordPoint = (left + rightPoint) * .5;
            if (!preflight(candidate,
                           {static_cast<float>(sourceAnchor.x + probeX),
                            static_cast<float>(sourceAnchor.y + probeY)},
                           chordPoint))
            {
                return false;
            }
        }

        out = candidate;
        return true;
    };

    constexpr float kMinimumCurvature = 1e-9f;
    const int requestedSegments =
        std::clamp(plane.wrap.segmentCount, 1, static_cast<int>(MAX_SEGMENTS));
    const bool wantsWrap = plane.wrap.radiansPerPixel > kMinimumCurvature &&
                           plane.wrap.surfaceRadius > .0f && requestedSegments > 1;
    if (wantsWrap)
    {
        if (solveWrapped(requestedSegments))
        {
            return true;
        }
        const int reducedSegments = requestedSegments / 2;
        if (reducedSegments > 1 && solveWrapped(reducedSegments))
        {
            return true;
        }
    }
    return solveFlat();
}

void* MakeCallbackParams(const Projection& projection, const InkMaterial& material)
{
    if (!IsInitialized() || !IsFinite(projection) || !IsFinite(material))
    {
        return nullptr;
    }
    s_ParamArena.push_back({projection, material});
    return &s_ParamArena.back();
}

void ApplyCallback(const ImDrawList* /*drawList*/, const ImDrawCmd* command)
{
    if (!s_Context)
    {
        return;
    }

    // Push a marker even on a recoverable failure so RestoreCallback remains
    // balanced and can never pop a state belonging to an earlier bracket.
    SavedVertexState saved{};
    if (!IsInitialized() || !command || !command->UserCallbackData)
    {
        s_StateStack.push_back(std::move(saved));
        return;
    }
    const auto* params = static_cast<const CallbackParams*>(command->UserCallbackData);
    if (!IsFinite(params->projection) || !IsFinite(params->material))
    {
        s_StateStack.push_back(std::move(saved));
        return;
    }

    const ShaderConstants constants = MakeConstants(params->projection, params->material);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(s_Context->Map(s_ConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        s_StateStack.push_back(std::move(saved));
        return;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    s_Context->Unmap(s_ConstantBuffer.Get(), 0);

    s_Context->VSGetShader(saved.shader.GetAddressOf(), nullptr, nullptr);
    s_Context->VSGetConstantBuffers(0, 1, saved.constantBuffer.GetAddressOf());
    saved.active = true;
    s_StateStack.push_back(std::move(saved));

    s_Context->VSSetShader(s_VertexShader.Get(), nullptr, 0);
    ID3D11Buffer* buffer = s_ConstantBuffer.Get();
    s_Context->VSSetConstantBuffers(0, 1, &buffer);
}

void RestoreCallback(const ImDrawList* /*drawList*/, const ImDrawCmd* /*command*/)
{
    if (!s_Context || s_StateStack.empty())
    {
        return;
    }

    SavedVertexState saved = std::move(s_StateStack.back());
    s_StateStack.pop_back();
    if (!saved.active)
    {
        return;
    }

    s_Context->VSSetShader(saved.shader.Get(), nullptr, 0);
    ID3D11Buffer* buffer = saved.constantBuffer.Get();
    s_Context->VSSetConstantBuffers(0, 1, &buffer);
}
}  // namespace Graffito
