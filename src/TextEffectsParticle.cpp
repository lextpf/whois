#include "TextEffectsInternal.hpp"

// Particle aura motion and drawing. DrawParticleAura is the only entry point; every other
// symbol here is file-local.
//
// Render thread only, and stateless: no game state is read and nothing survives the frame.
// Each particle attribute is a pure function of the scaled time, the particle index and the
// style slot, hashed through PTrait, so plates can appear, disappear or hot-reload with no
// spawn bookkeeping and with no flicker.
//
// Flow of one call:
//
//   DrawParticleAura(params)         one call per enabled style of one region
//     per-call setup     blend mode, halo scale, crowd alpha, radial band, air current
//     for each particle
//       PTrait salts  -> ParticleContext   radius, speed, direction, depth, size, color
//       dispatch      -> dedicated renderer (9 of the 10 original styles; Snow takes an
//                        archetype), or a StyleMotionSpec archetype for every other style
//       draw          -> DrawHaloThenSprite: halo, crisp sprite, rare glint
//                        or a procedural shape when the style loaded no sprite
//
// The F/M/C/G tags in the comments below name the tuning pass a line came from, not a code
// entity:
//   F = form and depth   F1 depth size/alpha, F2 amplitude parallax, F3 shared air current,
//                        F4 depth-biased fall rate, F6 the live depth knob
//   M = motion           M1 non-sinusoidal paths, M2 stratified placement
//   C = color            C1 depth temperature mix, C2 fallback core tint, C3 warm apex push,
//                        C4 the live warmth knob
//   G = glow             G1/G2 backlight halo, G3 specular glint, G5 the live glow knobs

namespace TextEffects
{

// Procedural 4-pointed star: glow disc, star polygon, then a bright core. No call site
// today: the untextured fallback path draws orbs, wisps and sparks only.
static void DrawStar4(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      ImU32 color,
                      ImU32 glowColor,
                      float rotation = .0f)
{
    const float innerRatio = .35f;  // Inner radius as fraction of outer
    const float outerR = size;
    const float innerR = size * innerRatio;

    // 4 points = 8 vertices alternating outer/inner
    ImVec2 points[8];
    for (int i = 0; i < 8; ++i)
    {
        float angle = rotation + (float)i * .785398f;  // 45 degrees = pi/4
        float radius = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
    }

    list->AddCircleFilled(pos, size * 1.8f, glowColor, 16);

    list->AddConvexPolyFilled(points, 8, color);

    int coreAlpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    list->AddCircleFilled(pos, size * .3f, IM_COL32(255, 255, 255, coreAlpha / 2), 8);
}

// Procedural 6-pointed star: two glow discs, star polygon, then a bright core. No call site
// today, as for DrawStar4.
static void DrawStar6(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      ImU32 color,
                      ImU32 glowColor,
                      float rotation = .0f)
{
    const float innerRatio = .45f;
    const float outerR = size;
    const float innerR = size * innerRatio;

    // 6 points = 12 vertices
    ImVec2 points[12];
    for (int i = 0; i < 12; ++i)
    {
        float angle = rotation + (float)i * .5235988f;  // 30 degrees = pi/6
        float radius = (i % 2 == 0) ? outerR : innerR;
        points[i] = ImVec2(pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius);
    }

    list->AddCircleFilled(pos, size * 2.2f, glowColor, 16);
    list->AddCircleFilled(pos, size * 1.5f, glowColor, 16);

    list->AddConvexPolyFilled(points, 12, color);

    int coreAlpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
    list->AddCircleFilled(pos, size * .3f, IM_COL32(255, 255, 255, coreAlpha / 2), 8);
}

// Procedural orb used when no sprite is loaded. The outer layers carry the
// primary color; the core is the primary/secondary midpoint blended half way
// to white rather than pure white, matching the warm-core/cool-edge look of
// the textured path.
// (C2 - this path is non-textured by definition and never touches the sprite.)
static void DrawSoftOrb(ImDrawList* list,
                        const ImVec2& pos,
                        float size,
                        int r,
                        int g,
                        int b,
                        int r2,
                        int g2,
                        int b2,
                        int baseAlpha)
{
    // Layered discs approximate a smooth gradient falloff
    const int layers = 6;
    for (int i = layers - 1; i >= 0; --i)
    {
        float t = (float)i / (float)(layers - 1);
        float radius = size * (.38f + .82f * t);
        int layerAlpha = (int)(baseAlpha * (1.0f - t * .62f) * (1.0f - t * .22f));
        layerAlpha = std::clamp(layerAlpha, 0, 255);
        list->AddCircleFilled(pos, radius, IM_COL32(r, g, b, layerAlpha), 16);
    }
    // Tinted-luminous core (~midway primary/secondary, then toward white).
    int cr = (r + r2) / 2;
    int cg = (g + g2) / 2;
    int cb = (b + b2) / 2;
    list->AddCircleFilled(pos,
                          size * .25f,
                          IM_COL32((cr + 255) / 2, (cg + 255) / 2, (cb + 255) / 2, baseAlpha / 2),
                          12);
}

// Procedural wisp: trail segments opposite the travel angle, then the body.
static void DrawWisp(ImDrawList* list,
                     const ImVec2& pos,
                     float size,
                     float angle,
                     int r,
                     int g,
                     int b,
                     int baseAlpha,
                     float trailLength)
{
    float trailAngle = angle + PI;
    float dx = std::cos(trailAngle);
    float dy = std::sin(trailAngle);

    // Trail segments, fading with distance
    const int trailSegments = 6;
    for (int i = trailSegments - 1; i >= 0; --i)
    {
        float t = (float)i / (float)trailSegments;
        float segX = pos.x + dx * size * trailLength * t;
        float segY = pos.y + dy * size * trailLength * t;
        float segSize = size * (.94f - t * .48f);
        int segAlpha = (int)(baseAlpha * (.58f - t * .34f));
        segAlpha = std::clamp(segAlpha, 0, 255);
        list->AddCircleFilled(ImVec2(segX, segY), segSize, IM_COL32(r, g, b, segAlpha), 12);
    }

    // Body: three nested discs plus a white core
    list->AddCircleFilled(pos, size * 1.8f, IM_COL32(r, g, b, baseAlpha / 10), 16);
    list->AddCircleFilled(pos, size * 1.22f, IM_COL32(r, g, b, baseAlpha / 6), 14);
    list->AddCircleFilled(pos, size * .88f, IM_COL32(r, g, b, (int)(baseAlpha * .72f)), 12);
    list->AddCircleFilled(pos, size * .28f, IM_COL32(255, 255, 255, baseAlpha / 4), 8);
}

// Procedural rune glyph; runeType % 4 selects the shape. No call site today, as for
// DrawStar4: the Runes style reaches the sprite path or DrawSoftOrb instead.
static void DrawRune(ImDrawList* list,
                     const ImVec2& pos,
                     float size,
                     int r,
                     int g,
                     int b,
                     int baseAlpha,
                     int runeType)
{
    ImU32 glowCol = IM_COL32(r, g, b, baseAlpha / 3);
    ImU32 mainCol = IM_COL32(r, g, b, baseAlpha);
    ImU32 brightCol =
        IM_COL32(std::min(255, r + 50), std::min(255, g + 50), std::min(255, b + 50), baseAlpha);
    float thickness = size * .15f;

    list->AddCircleFilled(pos, size * 1.8f, IM_COL32(r, g, b, baseAlpha / 5), 16);

    switch (runeType % 4)
    {
        case 0:
        {
            // Diamond with an inner cross and two corner dots
            float s = size;
            list->AddQuad(ImVec2(pos.x, pos.y - s),
                          ImVec2(pos.x + s * .7f, pos.y),
                          ImVec2(pos.x, pos.y + s),
                          ImVec2(pos.x - s * .7f, pos.y),
                          mainCol,
                          thickness);
            list->AddLine(ImVec2(pos.x, pos.y - s * .5f),
                          ImVec2(pos.x, pos.y + s * .5f),
                          brightCol,
                          thickness * .8f);
            list->AddLine(ImVec2(pos.x - s * .35f, pos.y),
                          ImVec2(pos.x + s * .35f, pos.y),
                          brightCol,
                          thickness * .8f);
            list->AddCircleFilled(ImVec2(pos.x, pos.y - s), size * .12f, brightCol, 8);
            list->AddCircleFilled(ImVec2(pos.x, pos.y + s), size * .12f, brightCol, 8);
            break;
        }
        case 1:
        {
            // Triangle with a ringed dot inside
            float s = size * .9f;
            ImVec2 p1(pos.x, pos.y - s);
            ImVec2 p2(pos.x - s * .866f, pos.y + s * .5f);
            ImVec2 p3(pos.x + s * .866f, pos.y + s * .5f);
            list->AddTriangle(p1, p2, p3, mainCol, thickness);
            list->AddCircle(pos, size * .35f, brightCol, 12, thickness * .7f);
            list->AddCircleFilled(pos, size * .15f, brightCol, 8);
            break;
        }
        case 2:
        {
            // Six spokes 30 degrees apart, so they fan over a half turn
            float s = size;
            for (int i = 0; i < 6; ++i)
            {
                float angle = (float)i * .5236f;  // 30 degrees
                ImVec2 outer(pos.x + std::cos(angle) * s, pos.y + std::sin(angle) * s);
                list->AddLine(pos, outer, mainCol, thickness);
            }
            list->AddCircleFilled(pos, size * .2f, brightCol, 10);
            break;
        }
        case 3:
        {
            // Concentric circles with four cardinal dots
            list->AddCircle(pos, size * .9f, mainCol, 14, thickness * .7f);
            list->AddCircle(pos, size * .5f, mainCol, 12, thickness * .6f);
            list->AddCircleFilled(pos, size * .2f, brightCol, 8);
            for (int i = 0; i < 4; ++i)
            {
                float angle = (float)i * 1.5708f;  // 90 degrees
                ImVec2 dotPos(pos.x + std::cos(angle) * size * .7f,
                              pos.y + std::sin(angle) * size * .7f);
                list->AddCircleFilled(dotPos, size * .1f, brightCol, 6);
            }
            break;
        }
    }
}

// Procedural spark: trail segments opposite the travel angle, then the body.
static void DrawSpark(ImDrawList* list,
                      const ImVec2& pos,
                      float size,
                      float angle,
                      int r,
                      int g,
                      int b,
                      int baseAlpha,
                      float life)
{
    // Lower life shifts the color toward orange/yellow
    float heat = 1.0f - life;
    int sr = std::min(255, r + (int)(100 * heat));
    int sg = std::min(255, g + (int)(50 * heat));
    int sb = std::max(0, b - (int)(30 * heat));

    float trailAngle = angle + PI;
    const int trailSegs = 4;
    for (int i = trailSegs - 1; i >= 0; --i)
    {
        float t = (float)(i + 1) / (float)(trailSegs + 1);
        float trailX = pos.x + std::cos(trailAngle) * size * 3.0f * t;
        float trailY = pos.y + std::sin(trailAngle) * size * 3.0f * t;
        float segSize = size * (1.0f - t * .7f);
        int segAlpha = (int)(baseAlpha * (1.0f - t) * .6f);
        list->AddCircleFilled(ImVec2(trailX, trailY), segSize, IM_COL32(sr, sg, sb, segAlpha), 8);
    }

    list->AddCircleFilled(pos, size * 2.0f, IM_COL32(sr, sg, sb, baseAlpha / 4), 12);
    list->AddCircleFilled(pos, size, IM_COL32(sr, sg, sb, baseAlpha), 10);
    list->AddCircleFilled(pos, size * .4f, IM_COL32(255, 255, 220, baseAlpha), 8);
}

// Independent per-particle trait streams. Each enumerator is a hash salt, and
// distinct salts keep the traits uncorrelated with each other; a linear
// (i * prime) pattern correlates across both particles and traits.
enum class Trait : uint32_t
{
    Radial = 1,       // Radial home position within the style's band
    Speed = 2,        // Orbital/cycle speed multiplier
    Direction = 3,    // Orbit direction selector
    Depth = 4,        // Pseudo-depth (size + alpha modulation)
    Size = 5,         // Base size multiplier
    JitterAngle = 6,  // Static positional jitter direction
    JitterDist = 7,   // Static positional jitter distance; also the fall/rise rate spread
    Phase = 8,        // Phase jitter on the golden angle; also the fall/rise/zap loop offset
    Bob = 9,          // Bobbing phase; also the Aurora/Flow elevation band slot
    HomeX = 10,       // Stratified horizontal home jitter for fall, rise and flow types
    Hue = 11,         // Decorrelated micro temperature jitter for color
    Elevation = 12,   // Per-particle vertical band offset for the tilted orbit
    Flip = 13,        // Flipbook animation phase (desyncs strip frames)
    Fill = 14         // Interior-filler selector: breaks a few orbiters off the ring
};

// The crisp pixel-art quad of a textured particle is
// finalSize * kSpriteSizeMul * coreSizeScale, where coreSizeScale is the
// per-style art compensation from ParticleTextures. The additive halo scales
// the same finalSize * kSpriteSizeMul base by glowSize and ignores
// coreSizeScale (see DrawHaloThenSprite). 3.4 keeps the shipped 16px PNGs at or
// above their native pixel size at default ParticleSize; 2.5 minifies them to
// ~12px quads and eats texels. Point sampling magnifies by repeating pixels,
// so the art stays crisp.
constexpr float kSpriteSizeMul = 3.4f;

// kOrbitTilt squashes the vertical axis so a flat ring reads as a disc seen at
// an angle. kOrbitHeightSpread is the full width of the per-particle vertical
// band, so an offset lands in [-kOrbitHeightSpread/2, +kOrbitHeightSpread/2] of
// radiusY; it spreads orbiters over different elevations instead of one flat
// halo. Both amplitudes are deliberately small.
constexpr float kOrbitTilt = 0.45f;
constexpr float kOrbitHeightSpread = 0.5f;

// The tilted orbit alone is a hollow oval outline. This fraction of orbiters
// moves to an interior radius so the aura fills in as a disc; particles render
// behind the text, so the fillers occupy the empty middle without hurting
// readability. kInteriorFloor keeps the very center, where the text is densest,
// clear.
//
// Radial home of one style, as a fraction of the aura radius:
//
//   0           kInteriorFloor           bandFloor                    1
//   |--- kept clear ---|--- interior fillers ---|--- outer ring -------|
//                        30% of particles          70% of particles
//                        area-uniform              area-uniform
//
// bandFloor is SoloBandFloor(style) for a lone style, else the shared
// per-slot floor computed in DrawParticleAura.
constexpr float kFillFraction = 0.30f;   // Share of orbiters that fill the interior
constexpr float kInteriorFloor = 0.12f;  // Smallest filler radius (fraction of aura)

// One sample of the shared tilted elliptical orbit. `ox`/`oy` are offsets as a
// fraction of radiusX/radiusY. `depthAng` is the front(1)/back(0) term derived
// from the orbital angle; it drives size and alpha so front-of-ring particles
// read as nearer. `tangent` is the tangent of the untilted circle, so on the
// squashed ellipse it only approximates the direction of travel.
struct OrbitSample
{
    float ox, oy;     // base offset as fraction of radiusX / radiusY
    float depthAng;   // 0 = back of ring (screen-top), 1 = front (screen-bottom)
    float sizeMul;    // 0.78 .. 1.18 from depthAng
    float alphaMul;   // 0.62 .. 1.0  from depthAng
    float tangent;    // orbital tangent angle (for streaks / spin)
    bool behindText;  // depthAng < 0.5 -> back arc; no reader today
};

static OrbitSample SampleOrbit(float orbitAngle, float radialAnchor, float heightBand, float tilt)
{
    OrbitSample s;
    const float c = std::cos(orbitAngle);
    const float sn = std::sin(orbitAngle);
    s.ox = c * radialAnchor;
    s.oy = sn * radialAnchor * tilt + heightBand;
    s.depthAng = .5f + .5f * sn;
    s.sizeMul = .78f + .40f * s.depthAng;
    s.alphaMul = .62f + .38f * s.depthAng;
    s.tangent = orbitAngle + 1.5707963f;
    s.behindText = s.depthAng < .5f;
    return s;
}

// 32-bit finalizer (three xorshifts, two multiplies) - decorrelates
// consecutive integers into uniformly distributed hash values.
static uint32_t PMixU32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

// Stateless per-particle trait in [0,1). Stable across frames (no flicker)
// because it depends only on particle index, style slot, and salt.
static float PTrait(int particleIndex, int styleIndex, Trait salt)
{
    uint32_t h =
        PMixU32(static_cast<uint32_t>(particleIndex) +
                PMixU32(static_cast<uint32_t>(styleIndex) + PMixU32(static_cast<uint32_t>(salt))));
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Area-uniform radius in the annulus [bandFloor, 1]: equal particle density
// per unit area instead of crowding the inner edge.
static float AnnulusRadius(float bandFloor, float u)
{
    float f2 = bandFloor * bandFloor;
    return std::sqrt(f2 + (1.0f - f2) * u);
}

// Cheap stable hash in [0,1) from a float seed. Used where the seed is a
// continuous value instead of an index: the second sway octave in FallLayout
// (seeded from the golden-angle value) and the per-cycle re-seat position of
// the Zap archetype (seeded from golden plus the cycle number). Adds no state.
// The stratified horizontal home of the fall/rise/flow types comes from PTrait
// with the HomeX salt, not from here.
static float PHash01(float seed)
{
    float s = std::sin(seed) * 43758.5453f;
    return s - std::floor(s);
}

// Stateless flipbook clock. Returns the frame the particle shows right now.
// The frame count comes from the loaded sprite (the shipped strips hold 4
// frames); a single-frame sprite, or fps <= 0, returns 0, so this is free to
// call unconditionally. The per-particle Flip phase desyncs
// siblings and speedVar spreads the cadence. `fps` is frames per timeScaled
// unit, so the global ParticleSpeed knob scales animation rate as well as
// motion.
static int FlipFrame(
    int texStyleId, int particleIndex, int styleIndex, float timeScaled, float speedVar, float fps)
{
    const int frames = ParticleTextures::GetFrameCountForIndex(texStyleId, particleIndex);
    if (frames <= 1 || fps <= .0f)
    {
        return 0;
    }
    const float phase = PTrait(particleIndex, styleIndex, Trait::Flip) * static_cast<float>(frames);
    const float t = timeScaled * fps * speedVar + phase;
    return static_cast<int>(t) % frames;
}

// Inner band edge when a style renders alone and may fill the nameplate
// interior. With several styles enabled the shared band formula separates them
// radially instead. Only the paths that read radialAnchor or minRadius consult
// this value: the dedicated orbiters (Firefly/Dust/Mote/Wisp/Spark/Leaf/
// CherryBlossom) and the Orbit, Twinkle and Zap archetypes. Smoke, Aurora and
// every Rise/Fall/Flow recipe span the full aura and ignore it.
static float SoloBandFloor(Settings::ParticleStyle style)
{
    switch (style)
    {
        case Settings::ParticleStyle::Spark:
            return .42f;  // orbiting embers fill a mid annulus
        case Settings::ParticleStyle::Wisp:
            return .40f;
        case Settings::ParticleStyle::Firefly:
            return .34f;
        case Settings::ParticleStyle::Leaf:
        case Settings::ParticleStyle::CherryBlossom:
        case Settings::ParticleStyle::Dust:
        case Settings::ParticleStyle::Mote:
            return .28f;  // orbiters fill the whole region
        // ---- sprite-expansion orbiters ----
        case Settings::ParticleStyle::Moon:
            return .60f;  // a single accent on the outer ring
        case Settings::ParticleStyle::Planet:
            return .55f;  // deep ring, clear of the text
        case Settings::ParticleStyle::Constellation:
            return .45f;
        case Settings::ParticleStyle::Bat:
            return .42f;  // the circling path stays off the letters
        case Settings::ParticleStyle::Gem:
        case Settings::ParticleStyle::Curse:
        case Settings::ParticleStyle::Void:
            return .40f;
        case Settings::ParticleStyle::Arcane:
        case Settings::ParticleStyle::Hex:
        case Settings::ParticleStyle::Vortex:
            return .38f;
        case Settings::ParticleStyle::Enchant:
        case Settings::ParticleStyle::Runes:
            return .36f;
        case Settings::ParticleStyle::Butterfly:
        case Settings::ParticleStyle::Fairy:
            return .34f;
        case Settings::ParticleStyle::Glitter:
            return .28f;  // anchored sparkle field fills the region
        default:
            // Unlisted orbiters (Snow, Coin, Ink, Pollen, Pixiedust, Ash, Zap)
            // use this floor; the fall, rise and flow types ignore it.
            return .30f;
    }
}

// Fraction of particles travelling in the reverse direction, so an aura does
// not move rigidly one way. Only the orbiting and flow types use it.
static float CounterRotateChance(Settings::ParticleStyle style)
{
    switch (style)
    {
        case Settings::ParticleStyle::Firefly:
        case Settings::ParticleStyle::Dust:
        case Settings::ParticleStyle::Mote:
        case Settings::ParticleStyle::Leaf:
        case Settings::ParticleStyle::CherryBlossom:
        case Settings::ParticleStyle::Aurora:
        // sprite-expansion orbiters that drift independently
        case Settings::ParticleStyle::Arcane:
        case Settings::ParticleStyle::Enchant:
        case Settings::ParticleStyle::Gem:
        case Settings::ParticleStyle::Hex:
        case Settings::ParticleStyle::Curse:
        case Settings::ParticleStyle::Constellation:
        case Settings::ParticleStyle::Butterfly:
        case Settings::ParticleStyle::Fairy:
        case Settings::ParticleStyle::Runes:
            return .5f;
        case Settings::ParticleStyle::Wisp:
        case Settings::ParticleStyle::Spark:
        case Settings::ParticleStyle::Bat:  // mostly circles one way
            return .3f;
        // Everything else returns 0. Vortex/Void swirl coherently and the Wind,
        // Fog and Sand currents gust one way, so mixed directions would break
        // the single current. Moon/Planet are solitary accents where direction
        // variety means nothing, and the remaining orbiters (Snow, Coin, Ink,
        // Pollen, Pixiedust, Ash, Zap) circle as one body. The rise, fall and
        // twinkle archetypes never read dirSign, so for them the value is inert.
        default:
            return .0f;
    }
}

// Shared per-particle state passed to each style's render function.
struct ParticleContext
{
    ImDrawList* list;
    ImVec2 center;
    float radiusX, radiusY;
    float alpha, particleSize, timeScaled;
    int texStyleId;
    bool hasTextures;
    int particleIndex;
    int particleCount;
    int styleIndex;  // Index among enabled styles; needed by PTrait salts
    // golden: golden-angle placement seed, also reused as a per-particle phase
    // constant. phase: orbit start angle, golden plus a hashed jitter.
    float golden, phase;
    float minRadius;     // Inner edge of this style's radial band [0,1]
    float radialAnchor;  // Area-uniform home radius; fillers sit below minRadius
    float jitterAngle, jitterDist;
    float alphaVariation;
    float speedVar;    // Per-particle speed multiplier [.72, 1.28]
    float dirSign;     // Orbit direction: +1 or -1
    float depth;       // Pseudo-depth [0,1]: 0 = far (small/dim), 1 = near
    float sizeVar;     // Per-particle base size multiplier [.85, 1.15]
    float bobPhase;    // Independent phase for bobbing/secondary motion
    float heightBand;  // Per-particle vertical band offset for the tilted orbit
    int r, g, b;
    int r2, g2, b2;  // Secondary gradient color
    // Whether params.colorSecondary was set. No reader today: r2/g2/b2 already
    // fall back to the primary color when it was not.
    bool hasSecondaryColor;
    ParticleTextures::BlendMode texBlendMode;

    // Live tuning knobs (F6/C4/G5) and the air current shared by this call (F3).
    float depthStrength;   // Scales the whole 3D read (F1/F2); 0 = flat sheet
    float warmth;          // Scales color temperature mix + apex pulse (C1/C3)
    float glowStrength;    // Halo alpha multiplier (G1); 0 also drops the glint
    float glowSize;        // Halo radius as a multiple of the crisp sprite (G1)
    float shineThreshold;  // Sine threshold for the rare specular glint (G3)
    float glintScale;      // Scales the G3 glint alpha (haloScale for matte art)
    float coreSizeScale;   // Art-aware crisp-core compensation; halo excluded
    float airX, airY;      // Air current shared by this call, depth-weighted (F3)

    // F1: widen depth into a real near/far range, scaled live by depthStrength.
    // Far ~0.62x/0.55x, near ~1.28x/1.10x at strength 1; strength 0 -> 1.0.
    float DepthSizeScale() const
    {
        float raw = .62f + .66f * depth;
        return 1.0f + (raw - 1.0f) * depthStrength;
    }
    float DepthAlphaScale() const
    {
        float raw = .55f + .55f * depth;
        return 1.0f + (raw - 1.0f) * depthStrength;
    }
    // F2: depth parallax on motion AMPLITUDE only (never the sin time argument).
    // Far ~0.55x amplitude, near ~1.25x.
    float DepthParallaxScale() const { return 1.0f + (.55f + .70f * depth - 1.0f) * depthStrength; }

    // Current flipbook frame for this particle's sprite at the given cadence.
    // Free for static sprites (frame count 1 -> always 0).
    int Flip(float fps) const
    {
        return FlipFrame(texStyleId, particleIndex, styleIndex, timeScaled, speedVar, fps);
    }
};

// G1/G2/G3: additive backlight halo in the secondary color behind the crisp
// pixel sprite, then the sprite, then a rare eased specular glint on top. Halo
// and glint both use the featureless soft disc (ParticleTextures::DrawSoftGlow);
// scaling the sprite art itself to 2x instead reads as a transparent duplicate
// of the particle, not a glow. The crisp 16px core stays dominant. Without a
// texture this routes to DrawSoftOrb. DrawSoftGlow is a no-op until
// ParticleTextures has generated the shared disc, so a missing disc drops the
// halo and the glint and leaves the crisp sprite drawing on its own. `a` is the
// final particle alpha and already carries the depth term.
static void DrawHaloThenSprite(const ParticleContext& ctx,
                               const ImVec2& pos,
                               float finalSize,
                               int a,
                               float rotation,
                               int frame = 0)
{
    if (!ctx.hasTextures)
    {
        DrawSoftOrb(ctx.list,
                    pos,
                    finalSize * ctx.coreSizeScale,
                    ctx.r,
                    ctx.g,
                    ctx.b,
                    ctx.r2,
                    ctx.g2,
                    ctx.b2,
                    a);
        return;
    }

    const float haloBaseSize = finalSize * kSpriteSizeMul;
    const float coreSize = haloBaseSize * ctx.coreSizeScale;

    // G1/G2: additive backlight, slightly wider near-camera. `a` already carries
    // the depth-alpha (DepthAlphaScale, applied once in the caller), so the halo
    // inherits it without re-applying it. The alpha gate is low because dim
    // particles need the halo most; only near-invisible ones are skipped.
    if (ctx.glowStrength > .0f && a > 12)
    {
        int ha = std::clamp((int)(a * ctx.glowStrength), 0, 255);
        if (ha > 2)
        {
            // 1.6x: the Gaussian disc carries visible alpha only over its
            // inner third, so the quad is oversized to reach the intended
            // footprint.
            float haloSize = haloBaseSize * ctx.glowSize * 1.6f * (1.0f + .15f * ctx.depth);
            ParticleTextures::DrawSoftGlow(
                ctx.list, pos, haloSize, IM_COL32(ctx.r2, ctx.g2, ctx.b2, ha));
        }
    }

    // The crisp pixel sprite, point-sampled, in the chosen INI blend mode.
    ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                          pos,
                                          coreSize,
                                          ctx.texStyleId,
                                          ctx.particleIndex,
                                          IM_COL32(ctx.r, ctx.g, ctx.b, a),
                                          ctx.texBlendMode,
                                          rotation,
                                          frame);

    // G3: rare eased specular twinkle on the light layer only, never on the
    // crisp quad. glintScale mutes it in step with the halo for matte or dark
    // art; styles with no motion recipe keep 1.0. The soft disc is drawn
    // at just over core size, so it swells over the sprite rather than
    // duplicating it.
    if (ctx.glowStrength > .0f && a > 24)
    {
        float tw = std::sin(ctx.timeScaled * .9f * ctx.speedVar + ctx.golden * 3.7f);
        float shine = SmoothStep((tw - ctx.shineThreshold) * 7.0f);
        if (shine > .02f)
        {
            int sa = std::clamp((int)(a * shine * .5f * ctx.glintScale), 0, 255);
            if (sa > 2)
            {
                ParticleTextures::DrawSoftGlow(
                    ctx.list,
                    pos,
                    coreSize * 1.1f,
                    IM_COL32((ctx.r2 + 255) / 2, (ctx.g2 + 255) / 2, (ctx.b2 + 255) / 2, sa));
            }
        }
    }
}

// Slow differential orbit plus an incommensurate-sine wander, with a blink on
// alpha and size. No rotation: the sprite stays upright so pixels stay crisp.
static void RenderFireflyParticle(const ParticleContext& ctx)
{
    float omega = .12f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);

    float par = ctx.DepthParallaxScale();  // F2: scales amplitude only
    float wanderX = (std::sin(ctx.timeScaled * .47f * ctx.speedVar + ctx.golden) +
                     .6f * std::sin(ctx.timeScaled * .83f + ctx.golden * 1.7f)) *
                    .07f * par;
    float wanderY = (std::sin(ctx.timeScaled * .53f * ctx.speedVar + ctx.golden * 2.1f) +
                     .6f * std::sin(ctx.timeScaled * .71f + ctx.bobPhase)) *
                    .07f * par;
    float breathe = .9f + .06f * par * std::sin(ctx.timeScaled * .3f + ctx.golden);
    float x = ctx.center.x + orb.ox * breathe * ctx.radiusX + wanderX * ctx.radiusX +
              ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + orb.oy * breathe * ctx.radiusY + wanderY * ctx.radiusY +
              ctx.airY * ctx.depth * ctx.radiusY;

    float blink = std::sin(ctx.timeScaled * 2.4f * ctx.speedVar + ctx.golden * 3.0f);
    float glow = .5f + .5f * (.5f + .5f * blink);
    float finalAlpha = ctx.alpha * glow * ctx.alphaVariation * ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (.92f + .12f * glow) * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    // C3: small warm push on the red channel at the glow apex (Firefly, Mote).
    ParticleContext warm = ctx;
    warm.r = std::min(255, ctx.r + (int)(14 * glow * ctx.warmth));
    DrawHaloThenSprite(warm, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(4.0f));
}

// Circles the plate on the tilted ring and periodically flares outward hot,
// cooling back onto the ring. The trail follows the orbital tangent.
static void RenderSparkParticle(const ParticleContext& ctx)
{
    float omega = .16f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);

    // Each particle flares on its own slow cycle: a sharp outward push that
    // settles back onto the ring. `flare` in [0,1], peaking at cycle start.
    float cyc = std::fmod(ctx.timeScaled * .5f * ctx.speedVar + ctx.golden, TWO_PI) * INV_TWO_PI;
    float flare = std::exp(-cyc * 6.0f);
    float rad = 1.0f + flare * .35f;
    float x = ctx.center.x + orb.ox * rad * ctx.radiusX +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist * .5f;
    float y = ctx.center.y + orb.oy * rad * ctx.radiusY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist * .5f;

    float flicker = .8f + .2f * std::sin(ctx.timeScaled * 15.0f + ctx.golden * 5.0f);
    float finalAlpha = ctx.alpha * (.65f + .35f * flare) * flicker * ctx.alphaVariation *
                       ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .05f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (.92f + .5f * flare) * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    // Whiter and more orange at the flare peak, back to the tier color after.
    int sr = std::clamp(ctx.r + (int)(90 * flare), 0, 255);
    int sg = std::clamp(ctx.g + (int)(50 * flare), 0, 255);
    int sb = std::clamp(ctx.b - (int)(20 * flare), 0, 255);

    // Travel direction for the trail and the sprite rotation. dirSign mirrors
    // the tangent (-t) rather than reversing it (t + PI), so a counter-rotating
    // particle trails along the mirrored tangent, not along its exact path.
    float moveAngle = orb.tangent * ctx.dirSign;

    if (ctx.hasTextures)
    {
        const int frame = ctx.Flip(6.0f);
        float trailDx = -std::cos(moveAngle);
        float trailDy = -std::sin(moveAngle);
        float trailSpacing = finalSize * 3.6f;
        for (int t = 2; t >= 1; --t)
        {
            float tf = static_cast<float>(t) / 3.0f;
            float tx = x + trailDx * trailSpacing * tf;
            float ty = y + trailDy * trailSpacing * tf;
            int trailA = std::clamp(static_cast<int>(a * (.3f - .1f * t)), 0, 255);
            ParticleTextures::DrawSpriteWithIndex(ctx.list,
                                                  ImVec2(tx, ty),
                                                  finalSize * kSpriteSizeMul * ctx.coreSizeScale,
                                                  ctx.texStyleId,
                                                  ctx.particleIndex,
                                                  IM_COL32(sr, sg, sb, trailA),
                                                  ctx.texBlendMode,
                                                  moveAngle,
                                                  frame);
        }
        DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize, a, moveAngle, frame);
    }
    else
    {
        DrawSpark(ctx.list, ImVec2(x, y), finalSize, moveAngle, sr, sg, sb, a, 1.0f - flare);
    }
}

// Orbits the tilted ring with a radial and vertical weave; the echo trail
// follows the orbital tangent.
static void RenderWispParticle(const ParticleContext& ctx)
{
    float omega = .14f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);

    // Weave: radial breathing plus vertical undulation riding the orbit.
    float weave = .12f * std::sin(ctx.timeScaled * .8f * ctx.speedVar + ctx.golden * 1.7f);
    float bob = .06f * std::sin(ctx.timeScaled * .8f + ctx.bobPhase);
    float x = ctx.center.x + orb.ox * (1.0f + weave) * ctx.radiusX +
              std::cos(ctx.jitterAngle) * ctx.radiusX * ctx.jitterDist;
    float y = ctx.center.y + (orb.oy + bob) * ctx.radiusY +
              std::sin(ctx.jitterAngle) * ctx.radiusY * ctx.jitterDist;

    float pulse = .64f + .26f * std::sin(ctx.timeScaled * .6f * ctx.speedVar + ctx.golden * 2.0f);
    float finalAlpha =
        ctx.alpha * pulse * ctx.alphaVariation * .95f * ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize = ctx.particleSize *
                      (.88f + .06f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f)) *
                      ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    int wr = std::min(255, ctx.r + 24);
    int wg = std::min(255, ctx.g + 30);
    int wb = std::min(255, ctx.b + 36);

    // The trail follows the orbital tangent; see the dirSign note in
    // RenderSparkParticle for the counter-rotating case.
    float moveAngle = orb.tangent * ctx.dirSign;
    float trailLength = 1.12f + .32f * std::sin(ctx.golden);

    if (ctx.hasTextures)
    {
        const int frame = ctx.Flip(5.0f);
        float echoDist = finalSize * 5.0f;
        float ex = x - std::cos(moveAngle) * echoDist;
        float ey = y - std::sin(moveAngle) * echoDist;
        int echoA = std::clamp(static_cast<int>(a * .24f), 0, 255);
        ParticleTextures::DrawSpriteWithIndex(
            ctx.list,
            ImVec2(ex, ey),
            finalSize * (kSpriteSizeMul * .8f) * ctx.coreSizeScale,
            ctx.texStyleId,
            ctx.particleIndex,
            IM_COL32(ctx.r, ctx.g, ctx.b, echoA),
            ctx.texBlendMode,
            moveAngle,
            frame);
        DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize * (5.8f / 6.0f), a, moveAngle, frame);
    }
    else
    {
        DrawWisp(ctx.list, ImVec2(x, y), finalSize, moveAngle, wr, wg, wb, a, trailLength);
    }
}

// Shared falling-particle layout. Returns screen position for a particle that
// loops top->bottom through the aura, distributed across its full width by a
// stable per-particle horizontal home. `fallRate` sets the descent speed and
// `swayAmp` the horizontal oscillation amplitude. `swayRate` is that
// oscillation's angular rate in radians per timeScaled unit, so the sway
// follows the clock, not the descent progress. `edge` is filled with a
// top/bottom fade factor and `fall` with the [0,1) vertical progress.
static ImVec2 FallLayout(const ParticleContext& ctx,
                         float fallRate,
                         float swayAmp,
                         float swayRate,
                         float& fall,
                         float& edge)
{
    // M2: stratified jittered cell -> one particle per equal-width column, full
    // width including the right edge. Independent integer-hash salts (HomeX,
    // Phase) keep home and fall phase decorrelated so columns don't lock-step.
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float homeX = (cell + jit) * 2.0f - 1.0f;
    float phaseOff = PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Phase);

    // F4: depth-biased terminal velocity, near particles falling slightly
    // faster, over a wide per-particle spread. M1 owns the eased descent below;
    // these only multiply the rate, so the two layer instead of fighting.
    float rateVar = .6f + .8f * PTrait(ctx.particleIndex, ctx.styleIndex, Trait::JitterDist);
    fall = std::fmod(
        ctx.timeScaled * fallRate * ctx.speedVar * rateVar * (.85f + .30f * ctx.depth) + phaseOff,
        1.0f);

    // M1: two incommensurate sway octaves + slow FBM cross-drift so the lateral
    // path bends instead of tracing one clean sine. F2 scales the sway AMPLITUDE
    // by depth (parallax) without touching any sin time argument.
    float par = ctx.DepthParallaxScale();
    float ph = PHash01(ctx.golden * 2.3f + 1.7f) * TWO_PI;
    float sway = swayAmp * par *
                 (.7f * std::sin(ctx.timeScaled * swayRate + ctx.golden * 3.0f) +
                  .3f * std::sin(ctx.timeScaled * swayRate * 1.93f + ph));
    float drift = (FBMNoise(ctx.golden * .7f, fall * 2.0f + ctx.timeScaled * .05f, 3) - .5f) * par;

    // M1: slight non-constant descent rate.
    float fy = Saturate(fall + .06f * std::sin(fall * TWO_PI + ctx.golden));

    edge = (std::min)(SmoothStep(fall * 6.0f), SmoothStep((1.0f - fall) * 6.0f));
    float x = ctx.center.x + (homeX + sway + drift * swayAmp * .6f) * ctx.radiusX;
    float y = ctx.center.y + (-1.0f + 2.0f * fy) * ctx.radiusY;
    return ImVec2(x, y);
}

// Alias for DrawHaloThenSprite, kept for readability at the weather-art call
// sites: the Fall and Flow archetypes, Snow, Smoke, Aurora, and the orbiters
// drawn with weather art (Leaf, CherryBlossom, Dust). The Rise archetype calls
// DrawHaloThenSprite directly. The behavior is identical.
static void DrawWeatherSprite(const ParticleContext& ctx,
                              const ImVec2& pos,
                              float finalSize,
                              int a,
                              float rotation,
                              int frame = 0)
{
    DrawHaloThenSprite(ctx, pos, finalSize, a, rotation, frame);
}

// Falls slowly with a lateral sway. Not reached today: GetNewStyleMotion gives
// Snow the Orbit archetype, and DrawParticleAura calls this renderer only when
// the style has no motion recipe.
static void RenderSnowParticle(const ParticleContext& ctx)
{
    float fall, edge;
    ImVec2 pos = FallLayout(ctx, .32f, .16f, .9f, fall, edge);

    float finalAlpha = ctx.alpha * edge * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize *
                      (.92f + .08f * std::sin(ctx.timeScaled * 1.1f + ctx.golden * 2.0f)) *
                      ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, pos, finalSize, a, .0f, ctx.Flip(3.0f));
}

// Rises from the bottom, expanding and fading out.
static void RenderSmokeParticle(const ParticleContext& ctx)
{
    // M2: stratified home (scaled to the narrower smoke column) + decorrelated
    // rise phase from an independent integer-hash salt.
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float homeX = ((cell + jit) * 2.0f - 1.0f) * .7f;
    float phaseOff = PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Phase);
    float rise = std::fmod(ctx.timeScaled * .26f * ctx.speedVar + phaseOff, 1.0f);
    float drift = .22f * std::sin(ctx.timeScaled * .5f + ctx.golden * 2.0f) * rise;
    float x = ctx.center.x + (homeX + drift) * ctx.radiusX;
    float y = ctx.center.y + (.85f - 1.85f * rise) * ctx.radiusY;  // bottom -> top

    float fade = SmoothStep(rise * 4.0f) * SmoothStep((1.0f - rise) * 3.0f);
    float finalAlpha = ctx.alpha * fade * .9f * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    // expands as it rises
    float finalSize = ctx.particleSize * (.7f + 1.1f * rise) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(3.5f));
}

// Orbits with an in/out radius drift and a rocking rotation.
static void RenderLeafParticle(const ParticleContext& ctx)
{
    float omega = .11f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);
    // Radius drifts in and out, so the path is not a rigid ring.
    float drift = 1.0f + .08f * std::sin(ctx.timeScaled * .5f + ctx.bobPhase);
    float x = ctx.center.x + orb.ox * drift * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + orb.oy * drift * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float finalAlpha = ctx.alpha * ctx.alphaVariation * ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    float rotation = .5f * std::sin(ctx.timeScaled * 1.3f * ctx.speedVar + ctx.golden);  // flutter
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, rotation, ctx.Flip(3.5f));
}

// Orbits with a vertical bob and a slow continuous spin.
static void RenderCherryBlossomParticle(const ParticleContext& ctx)
{
    float omega = .09f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);
    float bob = .05f * std::sin(ctx.timeScaled * .6f + ctx.bobPhase);
    float x = ctx.center.x + orb.ox * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (orb.oy + bob) * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float finalAlpha = ctx.alpha * ctx.alphaVariation * ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .04f)
    {
        return;
    }
    float finalSize = ctx.particleSize * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    float rotation = ctx.timeScaled * .5f * ctx.dirSign * ctx.speedVar + ctx.golden;  // slow spin
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, rotation, ctx.Flip(3.0f));
}

// Flows horizontally across the upper aura on a slow vertical wave, with an
// alpha shimmer. No rotation.
static void RenderAuroraParticle(const ParticleContext& ctx)
{
    // M2: stratified flow start-offset (one band slot per particle) + an
    // independent integer-hash salt for the vertical band offset.
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float startOff = cell + jit;
    // Frac, not fmod, keeps the loop in [0,1) for BOTH travel directions. fmod
    // goes negative once a counter-flowing particle's phase does, which zeroes
    // the edge fade and leaves half the band permanently invisible.
    float flow = Frac(ctx.timeScaled * .18f * ctx.speedVar * ctx.dirSign + startOff);
    // Spread over y in [-.45, +.10] of radiusY (negative is above the center)
    // rather than a thin top line: a band of .30 packs every strand onto one
    // elevation, so a dense tier clumps into a single orbit instead of layering.
    float bandY =
        -.45f + .55f * PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Bob);  // upper band
    float par = ctx.DepthParallaxScale();                                      // F2: amplitude only
    float wave = .12f * par * std::sin(flow * TWO_PI * 1.5f + ctx.timeScaled * .6f + ctx.golden);
    // F3: air current shared by this call, depth-weighted.
    float x =
        ctx.center.x + (-1.0f + 2.0f * flow) * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (bandY + wave) * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float edge = (std::min)(SmoothStep(flow * 6.0f), SmoothStep((1.0f - flow) * 6.0f));
    float shimmer = .5f + .5f * std::sin(ctx.timeScaled * 1.5f + ctx.golden * 2.0f);
    float finalAlpha =
        ctx.alpha * edge * (.6f + .4f * shimmer) * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (.95f + .12f * shimmer) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(4.0f));
}

// Slow circling orbit plus a small two-axis drift and an alpha twinkle. The
// omega stays at .09; near .04 the orbit reads as static. The rate carries no
// radius divisor, unlike the other orbiters, so the angular speed does not
// fall off with radius.
static void RenderDustParticle(const ParticleContext& ctx)
{
    float orbit = ctx.phase + ctx.timeScaled * .09f * ctx.speedVar * ctx.dirSign;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);
    float par = ctx.DepthParallaxScale();  // F2: amplitude only
    float driftX = .04f * par * std::sin(ctx.timeScaled * .20f + ctx.golden);
    float driftY = .04f * par * std::sin(ctx.timeScaled * .17f + ctx.bobPhase);
    float x = ctx.center.x + (orb.ox + driftX) * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (orb.oy + driftY) * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float twinkle = .55f + .45f * (.5f + .5f * std::sin(ctx.timeScaled * .7f + ctx.golden * 2.0f));
    float finalAlpha =
        ctx.alpha * twinkle * .85f * ctx.alphaVariation * ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize = ctx.particleSize * .85f * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(2.5f));
}

// Orbit plus a two-axis wander and a breathing radius, with a slow glow cycle
// on alpha and size.
static void RenderMoteParticle(const ParticleContext& ctx)
{
    float omega = .10f * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);
    float par = ctx.DepthParallaxScale();  // F2: amplitude only
    float wanderX = (std::sin(ctx.timeScaled * .43f * ctx.speedVar + ctx.golden) +
                     .6f * std::sin(ctx.timeScaled * .67f + ctx.golden * 1.7f)) *
                    .05f * par;
    float wanderY = (std::sin(ctx.timeScaled * .49f * ctx.speedVar + ctx.golden * 2.1f) +
                     .6f * std::sin(ctx.timeScaled * .59f + ctx.bobPhase)) *
                    .05f * par;
    float breathe = .94f + .06f * par * std::sin(ctx.timeScaled * .5f + ctx.golden);
    float x = ctx.center.x + orb.ox * breathe * ctx.radiusX + wanderX * ctx.radiusX +
              ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + orb.oy * breathe * ctx.radiusY + wanderY * ctx.radiusY +
              ctx.airY * ctx.depth * ctx.radiusY;

    float glow = .5f + .5f * std::sin(ctx.timeScaled * .7f * ctx.speedVar + ctx.golden * 2.0f);
    float finalAlpha = ctx.alpha * (.55f + .45f * glow) * ctx.alphaVariation *
                       ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (.92f + .14f * glow) * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    // C3: small warm push on the red channel at the glow apex (Firefly, Mote).
    ParticleContext warm = ctx;
    warm.r = std::min(255, ctx.r + (int)(14 * glow * ctx.warmth));
    DrawHaloThenSprite(warm, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(3.0f));
}

// ===========================================================================
// Sprite-expansion styles: motion archetypes
//
// These styles share parameterized archetypes instead of bespoke renderers:
// the orbit/rise/fall/flow code lives once, and each style is a named recipe
// in GetNewStyleMotion. The dedicated renderers above still serve nine of the
// ten original styles; Snow now takes the Orbit archetype, so its renderer is
// unreachable. Every amplitude is a fraction of the aura radius and
// deliberately small.
// ===========================================================================

// Motion recipe for one sprite-expansion style.
struct StyleMotionSpec
{
    enum class Arch : uint8_t
    {
        Orbit,    // Tilted elliptical ring (SampleOrbit) + wander/bob/swoop
        Rise,     // Stratified columns, bottom -> top, edge fades
        Fall,     // FallLayout: stratified columns, top -> bottom
        Flow,     // Horizontal band current (Aurora-class)
        Twinkle,  // Anchored scatter on the tilted disc, eased twinkle
        Zap       // Deterministic blink cycles re-seated by hash; no style maps here
    };

    Arch arch = Arch::Orbit;
    float rate = .12f;        // Orbit omega / fall,rise,flow progress / twinkle and zap rate
    float amp = .05f;         // Wander (Orbit), sway (Rise/Fall), wave (Flow), drift (Twinkle)
    float freq = .9f;         // Rise: sway cycles per ascent; Fall: sway rate; others ignore
    float bobAmp = .0f;       // Orbit: figure-eight vertical bob amplitude
    float swoopAmp = .0f;     // Orbit: enveloped vertical dive (bat passes)
    float breatheAmp = .0f;   // Orbit: slow in/out radius breathing (void/vortex)
    float flipFps = 4.0f;     // Flipbook cadence, frames per timeScaled unit (0 holds frame 0)
    float spinRate = .0f;     // Orbit only: spin in radians per timeScaled unit (0 = upright)
    float flutterAmp = .0f;   // Rocking rotation amplitude in radians (Orbit, Rise, Fall)
    float pulseAmp = .0f;     // Size pulse amplitude
    float shimmerAmp = .0f;   // Alpha modulation depth [0,1]
    float alphaMul = 1.0f;    // Base alpha multiplier
    float haloScale = 1.0f;   // Scales the additive backlight and the G3 glint (0 = matte art)
    int blend = -1;           // -1 = INI blend; else a ParticleTextures::BlendMode value
    bool topBand = false;     // Orbit: ride above the name; Flow: upper band
    bool bottomBand = false;  // Flow: hug the lower edge (fog)
    float driftX = .0f;       // Rise: lateral drift accumulating with ascent (zzz)
    bool grow = false;        // Rise: grow with ascent (bubble/steam)
    bool fadeAtTop = false;   // Rise: long dissolve near the top (soul)
    bool popAtEnd = false;    // Rise: end-of-life pop sprite (bubble)
};

// One static recipe per sprite-expansion style, plus any style above whose
// motion uses a shared archetype. The rest return nullptr and keep their
// dedicated renderers. Dark or opaque pixel art pins Alpha blend,
// because under the default additive blend black adds nothing and vanishes.
// haloScale mutes the backlight for matte art for the same reason.
static const StyleMotionSpec* GetNewStyleMotion(Settings::ParticleStyle style)
{
    using S = Settings::ParticleStyle;
    using A = StyleMotionSpec::Arch;
    static constexpr int kAlpha = static_cast<int>(ParticleTextures::BlendMode::Alpha);
    switch (style)
    {
        // ---- orbiters ----
        case S::Snow:
        {
            // Ring drift with a broad, slow bob.
            static const StyleMotionSpec s{
                .rate = .10f, .amp = .06f, .bobAmp = .05f, .flipFps = 3.0f, .shimmerAmp = .12f};
            return &s;
        }
        case S::Arcane:
        {
            // Slow revolution, slow spin, breathing alpha.
            static const StyleMotionSpec s{
                .rate = .08f, .amp = .04f, .spinRate = .35f, .pulseAmp = .08f, .shimmerAmp = .30f};
            return &s;
        }
        case S::Enchant:
        {
            // Ring with a wide wander and a soft alpha shimmer.
            static const StyleMotionSpec s{
                .rate = .11f, .amp = .06f, .flipFps = 4.5f, .shimmerAmp = .45f};
            return &s;
        }
        case S::Gem:
        {
            // Upright on a slow ring; the sprite strip carries the facet glint.
            static const StyleMotionSpec s{.rate = .09f,
                                           .amp = .03f,
                                           .bobAmp = .05f,
                                           .flipFps = 5.0f,
                                           .shimmerAmp = .15f,
                                           .haloScale = .7f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Hex:
        {
            // Slow ring; the sprite strip carries the flame flicker.
            static const StyleMotionSpec s{
                .rate = .07f, .amp = .04f, .flipFps = 6.0f, .shimmerAmp = .35f, .haloScale = .8f};
            return &s;
        }
        case S::Curse:
        {
            // Slow ring with deep alpha dips.
            static const StyleMotionSpec s{.rate = .06f,
                                           .amp = .05f,
                                           .flipFps = 5.0f,
                                           .shimmerAmp = .50f,
                                           .alphaMul = .9f,
                                           .haloScale = .35f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Planet:
        {
            // Deep ring, slow but visibly revolving; a rate near .035 reads as
            // a static sticker.
            static const StyleMotionSpec s{.rate = .10f,
                                           .amp = .02f,
                                           .flipFps = 3.0f,
                                           .pulseAmp = .04f,
                                           .haloScale = .5f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Constellation:
        {
            // Wheels over the name in the top band, twinkling via the strip; a
            // rate near .02 reads as static.
            static const StyleMotionSpec s{
                .rate = .06f, .amp = .03f, .flipFps = 3.5f, .shimmerAmp = .40f, .topBand = true};
            return &s;
        }
        case S::Moon:
        {
            // A single accent arcing across the top band; a rate near .025
            // reads as static.
            static const StyleMotionSpec s{.rate = .08f,
                                           .amp = .02f,
                                           .flipFps = .0f,
                                           .pulseAmp = .05f,
                                           .shimmerAmp = .15f,
                                           .haloScale = .8f,
                                           .blend = kAlpha,
                                           .topBand = true};
            return &s;
        }
        case S::Void:
        {
            // Ring with an inward breathing pull; the strip spins the swirl.
            static const StyleMotionSpec s{.rate = .07f,
                                           .amp = .04f,
                                           .breatheAmp = .10f,
                                           .flipFps = 5.0f,
                                           .shimmerAmp = .25f,
                                           .haloScale = .6f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Vortex:
        {
            // One coherent current (no counter-rotation) spiraling in and out.
            static const StyleMotionSpec s{
                .rate = .16f, .amp = .04f, .breatheAmp = .14f, .flipFps = 7.0f, .shimmerAmp = .25f};
            return &s;
        }
        case S::Bat:
        {
            // Circles with enveloped swooping dives; the strip flaps the wings.
            static const StyleMotionSpec s{.rate = .20f,
                                           .amp = .05f,
                                           .swoopAmp = .10f,
                                           .flipFps = 7.0f,
                                           .haloScale = .35f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Butterfly:
        {
            // Slow ring, figure-eight bob, slow flap.
            static const StyleMotionSpec s{.rate = .07f,
                                           .amp = .06f,
                                           .bobAmp = .06f,
                                           .flipFps = 5.0f,
                                           .haloScale = .5f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Fairy:
        {
            // Firefly-class wanderer with wider excursions and a stronger pulse.
            static const StyleMotionSpec s{.rate = .13f,
                                           .amp = .10f,
                                           .bobAmp = .04f,
                                           .flipFps = .0f,
                                           .pulseAmp = .12f,
                                           .shimmerAmp = .45f};
            return &s;
        }
        case S::Runes:
        {
            // Upright on a slow ring; the strip morphs between rune shapes at a
            // slow cadence.
            static const StyleMotionSpec s{
                .rate = .06f, .amp = .05f, .flipFps = 2.8f, .shimmerAmp = .35f};
            return &s;
        }
        case S::Pollen:
        {
            // Slow ring with a wide wander. It orbits rather than falls: the
            // drift is the identity, and the ring keeps it on screen instead of
            // despawning at the bottom.
            static const StyleMotionSpec s{
                .rate = .05f, .amp = .14f, .bobAmp = .05f, .flipFps = 3.5f, .shimmerAmp = .30f};
            return &s;
        }
        case S::Pixiedust:
        {
            // Pollen's wander with Glitter's hard strip twinkle. The rate stays
            // near .15: at .10 a particle completes less than one revolution per
            // pass and reads as floating, and a wander above ~.08 stops it
            // settling into an even ring.
            static const StyleMotionSpec s{.rate = .15f,
                                           .amp = .08f,
                                           .bobAmp = .06f,
                                           .flipFps = 4.5f,
                                           .pulseAmp = .08f,
                                           .shimmerAmp = .55f};
            return &s;
        }
        case S::Ash:
        {
            // Slow ring with a rocking flutter and an alpha shimmer.
            static const StyleMotionSpec s{.rate = .07f,
                                           .amp = .09f,
                                           .flipFps = .0f,
                                           .flutterAmp = .30f,
                                           .shimmerAmp = .35f,
                                           .haloScale = .9f};
            return &s;
        }
        case S::Zap:
        {
            // Brisk ring with hard shimmer dips, so bolts snap in and out
            // while circling. This is the Orbit archetype, not Arch::Zap.
            static const StyleMotionSpec s{
                .rate = .20f, .amp = .05f, .flipFps = 8.0f, .pulseAmp = .15f, .shimmerAmp = .55f};
            return &s;
        }
        // ---- risers ----
        case S::Bubble:
        {
            // Ascent with a wiggle keyed to progress; pops at the top.
            static const StyleMotionSpec s{.arch = A::Rise,
                                           .rate = .20f,
                                           .amp = .07f,
                                           .freq = 2.0f,
                                           .flipFps = 3.5f,
                                           .alphaMul = .9f,
                                           .haloScale = .5f,
                                           .blend = kAlpha,
                                           .grow = true,
                                           .popAtEnd = true};
            return &s;
        }
        case S::Heart:
        {
            // Ascent with a squared positive-half-sine size pulse.
            static const StyleMotionSpec s{.arch = A::Rise,
                                           .rate = .14f,
                                           .amp = .05f,
                                           .freq = 1.2f,
                                           .flipFps = 4.5f,
                                           .pulseAmp = .10f,
                                           .haloScale = .5f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Soul:
        {
            // Ascent that dissolves well before the top edge.
            static const StyleMotionSpec s{.arch = A::Rise,
                                           .rate = .10f,
                                           .amp = .08f,
                                           .freq = .8f,
                                           .flipFps = 3.5f,
                                           .alphaMul = .9f,
                                           .haloScale = .7f,
                                           .blend = kAlpha,
                                           .fadeAtTop = true};
            return &s;
        }
        case S::Steam:
        {
            // Brisk narrow column, expanding as it climbs.
            static const StyleMotionSpec s{.arch = A::Rise,
                                           .rate = .30f,
                                           .amp = .04f,
                                           .freq = 1.4f,
                                           .flipFps = 4.5f,
                                           .alphaMul = .85f,
                                           .haloScale = .3f,
                                           .blend = kAlpha,
                                           .grow = true};
            return &s;
        }
        case S::Zzz:
        {
            // Slow ascent with an accumulating drift to one side.
            static const StyleMotionSpec s{.arch = A::Rise,
                                           .rate = .09f,
                                           .amp = .03f,
                                           .freq = .7f,
                                           .flipFps = 3.0f,
                                           .alphaMul = .9f,
                                           .haloScale = .4f,
                                           .blend = kAlpha,
                                           .driftX = .22f};
            return &s;
        }
        case S::Ember:
        {
            // Ascent with an alpha shimmer, fading well before the top; the
            // strip carries the flame.
            static const StyleMotionSpec s{.arch = A::Rise,
                                           .rate = .16f,
                                           .amp = .06f,
                                           .freq = 1.0f,
                                           .flipFps = 6.0f,
                                           .shimmerAmp = .40f,
                                           .fadeAtTop = true};
            return &s;
        }
        // ---- fallers ----
        case S::Confetti:
        {
            // Fast fall with a rocking rotation; the strip flips the scrap.
            static const StyleMotionSpec s{.arch = A::Fall,
                                           .rate = .28f,
                                           .amp = .13f,
                                           .freq = 1.1f,
                                           .flipFps = 5.0f,
                                           .flutterAmp = .55f,
                                           .haloScale = .0f,
                                           .blend = kAlpha};
            return &s;
        }
        // ---- orbiters drawn with falling-weather art ----
        case S::Coin:
        {
            // Slow ring; the strip spins the faces.
            static const StyleMotionSpec s{.rate = .08f,
                                           .amp = .035f,
                                           .bobAmp = .025f,
                                           .flipFps = 7.0f,
                                           .haloScale = .5f,
                                           .blend = kAlpha};
            return &s;
        }
        case S::Ink:
        {
            // Low, slow ring; matte art, so no halo.
            static const StyleMotionSpec s{.rate = .06f,
                                           .amp = .055f,
                                           .bobAmp = .03f,
                                           .flipFps = .0f,
                                           .alphaMul = .9f,
                                           .haloScale = .0f,
                                           .blend = kAlpha};
            return &s;
        }
        // ---- flow ----
        case S::Wind:
        {
            // Full-width current in one direction, no counter-flow.
            static const StyleMotionSpec s{
                .arch = A::Flow, .rate = .16f, .amp = .04f, .flipFps = 4.0f, .haloScale = .4f};
            return &s;
        }
        case S::Fog:
        {
            // Slow lower-edge current with a size pulse and alpha shimmer.
            static const StyleMotionSpec s{.arch = A::Flow,
                                           .rate = .045f,
                                           .amp = .05f,
                                           .flipFps = 3.0f,
                                           .pulseAmp = .06f,
                                           .shimmerAmp = .35f,
                                           .alphaMul = .7f,
                                           .haloScale = .25f,
                                           .blend = kAlpha,
                                           .bottomBand = true};
            return &s;
        }
        case S::Sand:
        {
            // Lower-band current, faster than Fog and slower than Wind.
            static const StyleMotionSpec s{.arch = A::Flow,
                                           .rate = .10f,
                                           .amp = .05f,
                                           .flipFps = 3.0f,
                                           .shimmerAmp = .25f,
                                           .alphaMul = .9f,
                                           .haloScale = .3f,
                                           .blend = kAlpha,
                                           .bottomBand = true};
            return &s;
        }
        // ---- twinkle ----
        case S::Glitter:
        {
            // Anchored sparkle field with an eased twinkle.
            static const StyleMotionSpec s{.arch = A::Twinkle,
                                           .rate = .55f,
                                           .amp = .035f,
                                           .flipFps = 4.5f,
                                           .pulseAmp = .10f,
                                           .shimmerAmp = .70f};
            return &s;
        }
        default:
            return nullptr;  // styles with a dedicated renderer
    }
}

// Orbit archetype: the shared tilted ring plus per-style wander, figure-eight
// bob, enveloped swoops, radius breathing, and spin/flutter rotation.
static void RenderArchOrbitParticle(const ParticleContext& ctx, const StyleMotionSpec& spec)
{
    float omega = spec.rate * ctx.speedVar * ctx.dirSign / (.6f + .4f * ctx.radialAnchor);
    float orbit = ctx.phase + ctx.timeScaled * omega;
    // Top-banded styles (moon, constellation) ride above the name instead of
    // wrapping the plate's midline.
    float heightBand = spec.topBand ? (ctx.heightBand * .6f - .45f) : ctx.heightBand;
    OrbitSample orb = SampleOrbit(orbit, ctx.radialAnchor, heightBand, kOrbitTilt);

    float par = ctx.DepthParallaxScale();  // F2: amplitude only
    float wanderX = (std::sin(ctx.timeScaled * .47f * ctx.speedVar + ctx.golden) +
                     .6f * std::sin(ctx.timeScaled * .83f + ctx.golden * 1.7f)) *
                    spec.amp * par;
    float wanderY = (std::sin(ctx.timeScaled * .53f * ctx.speedVar + ctx.golden * 2.1f) +
                     .6f * std::sin(ctx.timeScaled * .71f + ctx.bobPhase)) *
                    spec.amp * par;
    // Figure-eight read: the vertical bob turns at 1.9 rad per timeScaled unit,
    // ~3.6x the .53 primary wander cadence.
    float bob = spec.bobAmp * par * std::sin(ctx.timeScaled * 1.9f * ctx.speedVar + ctx.bobPhase);
    // Enveloped swoop: dives arrive in passes, not as a constant oscillation.
    float swoop = .0f;
    if (spec.swoopAmp > .0f)
    {
        float envelope = .5f + .5f * std::sin(ctx.timeScaled * .23f + ctx.golden);
        swoop = spec.swoopAmp * par * envelope *
                std::sin(ctx.timeScaled * 1.7f * ctx.speedVar + ctx.bobPhase);
    }
    // Slow radius breathing (Void, Vortex).
    float breathe =
        1.0f + spec.breatheAmp * std::sin(ctx.timeScaled * .5f * ctx.speedVar + ctx.bobPhase);

    float x = ctx.center.x + orb.ox * breathe * ctx.radiusX + wanderX * ctx.radiusX +
              ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (orb.oy * breathe + bob + swoop) * ctx.radiusY +
              wanderY * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float shimmer =
        1.0f - spec.shimmerAmp *
                   (.5f + .5f * std::sin(ctx.timeScaled * .9f * ctx.speedVar + ctx.golden * 2.0f));
    float finalAlpha = ctx.alpha * spec.alphaMul * shimmer * ctx.alphaVariation *
                       ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .04f)
    {
        return;
    }
    float pulse = 1.0f + spec.pulseAmp * std::sin(ctx.timeScaled * .6f * ctx.speedVar + ctx.golden);
    float finalSize = ctx.particleSize * pulse * ctx.sizeVar * ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    float rotation = .0f;
    if (spec.spinRate != .0f)
    {
        rotation = ctx.timeScaled * spec.spinRate * ctx.dirSign * ctx.speedVar + ctx.golden;
    }
    else if (spec.flutterAmp != .0f)
    {
        rotation = spec.flutterAmp * std::sin(ctx.timeScaled * 1.3f * ctx.speedVar + ctx.golden);
    }
    DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize, a, rotation, ctx.Flip(spec.flipFps));
}

// Rise archetype: stratified columns climbing bottom -> top with sway keyed
// to the ascent, optional accumulating side-drift, growth, a long top
// dissolve, and the bubble's end-of-life pop.
static void RenderArchRiseParticle(const ParticleContext& ctx, const StyleMotionSpec& spec)
{
    // M2: stratified home + decorrelated phase (same recipe as FallLayout).
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float homeX = ((cell + jit) * 2.0f - 1.0f) * .85f;
    float phaseOff = PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Phase);
    float rateVar = .75f + .5f * PTrait(ctx.particleIndex, ctx.styleIndex, Trait::JitterDist);
    float rise = std::fmod(ctx.timeScaled * spec.rate * ctx.speedVar * rateVar + phaseOff, 1.0f);

    // Sway keyed to the ascent rather than the clock, so the wiggle traces the
    // path, plus lateral drift accumulating with progress (Zzz).
    float par = ctx.DepthParallaxScale();
    float sway = spec.amp * par * std::sin(rise * TWO_PI * spec.freq + ctx.golden * 3.0f);
    float x = ctx.center.x + (homeX + sway + spec.driftX * rise) * ctx.radiusX;
    float y = ctx.center.y + (.9f - 1.8f * rise) * ctx.radiusY;

    // Bubble pop: the last slice of the ascent swaps in the pop sprite,
    // blooming slightly while it cuts out; the column then respawns low.
    const bool popReady =
        spec.popAtEnd && ctx.hasTextures && ParticleTextures::HasPopSprite(ctx.texStyleId);
    if (popReady && rise > .92f)
    {
        float popT = (rise - .92f) / .08f;
        // Continuous handoff: keep the per-particle alpha variation and start
        // from the size the grown sprite reached at the seam, so only the art
        // changes; the pop then blooms outward. DrawPopSprite takes the final
        // quad edge, so kSpriteSizeMul and coreSizeScale are applied here
        // instead of inside DrawHaloThenSprite.
        int pa = std::clamp((int)(ctx.alpha * spec.alphaMul * ctx.alphaVariation * (1.0f - popT) *
                                  ctx.DepthAlphaScale() * 255.0f),
                            0,
                            255);
        if (pa > 2)
        {
            const float seamGrow = spec.grow ? (.72f + .55f * .92f) : 1.0f;
            float popSize = ctx.particleSize * seamGrow * (1.0f + .18f * popT) * ctx.sizeVar *
                            ctx.DepthSizeScale() * kSpriteSizeMul * ctx.coreSizeScale;
            // An animated pop strip plays its sequence ONCE across the pop
            // window, synced to popT rather than the clock, and holds the last
            // frame as it fades out.
            const int popFrames = ParticleTextures::GetPopFrameCount(ctx.texStyleId);
            const int popFrame =
                (std::min)(static_cast<int>(popT * static_cast<float>(popFrames)), popFrames - 1);
            ParticleTextures::DrawPopSprite(ctx.list,
                                            ImVec2(x, y),
                                            popSize,
                                            ctx.texStyleId,
                                            IM_COL32(ctx.r, ctx.g, ctx.b, pa),
                                            ctx.texBlendMode,
                                            .0f,
                                            popFrame);
        }
        return;
    }

    float fadeIn = SmoothStep(rise * 5.0f);
    // With a pop ready the sprite stays solid until the pop takes over;
    // otherwise it dissolves toward the top (long dissolve for souls).
    float fadeOut = popReady ? 1.0f : SmoothStep((1.0f - rise) * (spec.fadeAtTop ? 2.2f : 4.0f));
    float shimmer =
        1.0f - spec.shimmerAmp *
                   (.5f + .5f * std::sin(ctx.timeScaled * .9f * ctx.speedVar + ctx.golden * 2.0f));
    float finalAlpha = ctx.alpha * spec.alphaMul * fadeIn * fadeOut * shimmer * ctx.alphaVariation *
                       ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }

    float growMul = spec.grow ? (.72f + .55f * rise) : 1.0f;
    // Squared positive half-sine: one pulse per cycle, then a rest.
    float beat = std::sin(ctx.timeScaled * 2.2f * ctx.speedVar + ctx.golden);
    float pulse = 1.0f + spec.pulseAmp * (std::max)(.0f, beat) * (std::max)(.0f, beat);
    float finalSize = ctx.particleSize * growMul * pulse * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    float rotation =
        (spec.flutterAmp != .0f)
            ? spec.flutterAmp * std::sin(ctx.timeScaled * 1.3f * ctx.speedVar + ctx.golden)
            : .0f;
    DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize, a, rotation, ctx.Flip(spec.flipFps));
}

// Fall archetype: the shared stratified fall with per-style rate/sway plus
// optional rocking flutter (Confetti, the only faller today) and an alpha
// shimmer.
static void RenderArchFallParticle(const ParticleContext& ctx, const StyleMotionSpec& spec)
{
    float fall, edge;
    ImVec2 pos = FallLayout(ctx, spec.rate, spec.amp, spec.freq, fall, edge);

    float shimmer =
        1.0f - spec.shimmerAmp *
                   (.5f + .5f * std::sin(ctx.timeScaled * 1.1f * ctx.speedVar + ctx.golden * 2.0f));
    float finalAlpha =
        ctx.alpha * spec.alphaMul * edge * shimmer * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .04f)
    {
        return;
    }
    float pulse = 1.0f + spec.pulseAmp * std::sin(ctx.timeScaled * .8f * ctx.speedVar + ctx.golden);
    float finalSize = ctx.particleSize * pulse * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    float rotation =
        (spec.flutterAmp != .0f)
            ? spec.flutterAmp * std::sin(ctx.timeScaled * 1.4f * ctx.speedVar + ctx.golden)
            : .0f;
    DrawWeatherSprite(ctx, pos, finalSize, a, rotation, ctx.Flip(spec.flipFps));
}

// Flow archetype: a horizontal current across the plate. Wind fills the
// middle band at speed; fog hugs the lower edge and breathes.
static void RenderArchFlowParticle(const ParticleContext& ctx, const StyleMotionSpec& spec)
{
    // M2: stratified flow start-offset. Frac() keeps the loop in [0,1) for
    // BOTH travel directions (fmod goes negative for dirSign = -1).
    float cell = ((float)ctx.particleIndex + .5f) / (float)ctx.particleCount;
    float jit =
        (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::HomeX) - .5f) / (float)ctx.particleCount;
    float startOff = cell + jit;
    float flow = Frac(ctx.timeScaled * spec.rate * ctx.speedVar * ctx.dirSign + startOff);

    float bandCenter = spec.topBand ? -.40f : (spec.bottomBand ? .52f : .0f);
    float bandSpread = (spec.topBand || spec.bottomBand) ? .28f : .80f;
    float bandY =
        bandCenter + (PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Bob) - .5f) * bandSpread;
    float par = ctx.DepthParallaxScale();  // F2: amplitude only
    float wave =
        spec.amp * par * std::sin(flow * TWO_PI * 1.5f + ctx.timeScaled * .6f + ctx.golden);
    float x =
        ctx.center.x + (-1.0f + 2.0f * flow) * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (bandY + wave) * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float edge = (std::min)(SmoothStep(flow * 6.0f), SmoothStep((1.0f - flow) * 6.0f));
    float shimmer =
        1.0f - spec.shimmerAmp * (.5f + .5f * std::sin(ctx.timeScaled * 1.2f + ctx.golden * 2.0f));
    float finalAlpha =
        ctx.alpha * spec.alphaMul * edge * shimmer * ctx.alphaVariation * ctx.DepthAlphaScale();
    if (finalAlpha < .03f)
    {
        return;
    }
    float pulse = 1.0f + spec.pulseAmp * std::sin(ctx.timeScaled * .7f + ctx.golden);
    float finalSize = ctx.particleSize * pulse * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawWeatherSprite(ctx, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(spec.flipFps));
}

// Twinkle archetype: parked on the same tilted disc the orbiters use, with the
// home angle never advancing, plus a small two-axis drift and an eased,
// desynced twinkle.
static void RenderArchTwinkleParticle(const ParticleContext& ctx, const StyleMotionSpec& spec)
{
    OrbitSample orb = SampleOrbit(ctx.phase, ctx.radialAnchor, ctx.heightBand, kOrbitTilt);
    float par = ctx.DepthParallaxScale();
    float driftX = spec.amp * par * std::sin(ctx.timeScaled * .21f + ctx.golden);
    float driftY = spec.amp * par * std::sin(ctx.timeScaled * .17f + ctx.bobPhase);
    float x = ctx.center.x + (orb.ox + driftX) * ctx.radiusX + ctx.airX * ctx.depth * ctx.radiusX;
    float y = ctx.center.y + (orb.oy + driftY) * ctx.radiusY + ctx.airY * ctx.depth * ctx.radiusY;

    float tw = .5f + .5f * std::sin(ctx.timeScaled * spec.rate * TWO_PI * .5f * ctx.speedVar +
                                    ctx.golden * 2.7f);
    float bright = SmoothStep(tw);
    float finalAlpha = ctx.alpha * spec.alphaMul *
                       ((1.0f - spec.shimmerAmp) + spec.shimmerAmp * bright) * ctx.alphaVariation *
                       ctx.DepthAlphaScale() * orb.alphaMul;
    if (finalAlpha < .03f)
    {
        return;
    }
    float finalSize = ctx.particleSize * (1.0f + spec.pulseAmp * bright) * ctx.sizeVar *
                      ctx.DepthSizeScale() * orb.sizeMul;
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);
    DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize, a, .0f, ctx.Flip(spec.flipFps));
}

// Zap archetype: deterministic blink cycles. Each cycle re-seats the sprite at
// a hashed position on the disc; the envelope reaches zero before the jump, so
// the reposition is never visible. Fully stateless, like everything else here.
// Unreachable today: no recipe sets Arch::Zap, and the Zap style itself uses
// the Orbit archetype.
static void RenderArchZapParticle(const ParticleContext& ctx, const StyleMotionSpec& spec)
{
    float cyc = ctx.timeScaled * spec.rate * ctx.speedVar +
                PTrait(ctx.particleIndex, ctx.styleIndex, Trait::Phase) * 8.0f;
    float cycIdx = std::floor(cyc);
    float frac = cyc - cycIdx;

    float h1 = PHash01(ctx.golden + cycIdx * 17.23f);
    float h2 = PHash01(ctx.golden * 1.7f + cycIdx * 9.71f);
    float ang = h1 * TWO_PI;
    float rad = AnnulusRadius(ctx.minRadius, h2);
    float x = ctx.center.x + std::cos(ang) * rad * ctx.radiusX;
    float y = ctx.center.y + (std::sin(ang) * rad * kOrbitTilt + ctx.heightBand) * ctx.radiusY;

    // Sharp attack, fast decay; dark for the whole back half of the cycle.
    float env = (frac < .10f) ? SmoothStep(frac / .10f) : std::exp(-(frac - .10f) * 7.0f);
    float finalAlpha = ctx.alpha * spec.alphaMul * env * ctx.DepthAlphaScale();
    if (finalAlpha < .05f)
    {
        return;
    }
    float finalSize =
        ctx.particleSize * (1.0f + spec.pulseAmp * env) * ctx.sizeVar * ctx.DepthSizeScale();
    int a = std::clamp((int)(finalAlpha * 255.0f), 0, 255);

    // Frames advance within the strike, not on the wall clock, so the shape
    // changes only while the sprite is lit.
    int frames = ParticleTextures::GetFrameCountForIndex(ctx.texStyleId, ctx.particleIndex);
    int frame = (frames > 1) ? static_cast<int>(frac * spec.flipFps) % frames : 0;
    DrawHaloThenSprite(ctx, ImVec2(x, y), finalSize, a, .0f, frame);
}

// Entry point: draw one style's particles around one aura region a
// nameplate from RendererEffects, or a card from Deck. Render thread only, and
// it reads no game state. Draws nothing when the draw list is missing, when the
// configured alpha is at or below .05, or when the count is not positive. One
// call covers one style slot of one region; styleIndex keeps the slots apart in
// both the radial band and every trait hash.
void DrawParticleAura(const ParticleAuraParams& params)
{
    if (!params.list || params.alpha <= .05f || params.particleCount <= 0)
    {
        return;
    }

    int texStyleId = static_cast<int>(params.style);
    bool useTextures = params.useParticleTextures && ParticleTextures::IsInitialized();
    int texCount = useTextures ? ParticleTextures::GetTextureCount(texStyleId) : 0;
    bool hasTextures = (texCount > 0);

    // Map settings int (0=Additive, 1=Screen, 2=Alpha) to ParticleTextures::BlendMode
    ParticleTextures::BlendMode texBlend = ParticleTextures::BlendMode::Additive;
    if (params.blendMode == 1)
    {
        texBlend = ParticleTextures::BlendMode::Screen;
    }
    else if (params.blendMode == 2)
    {
        texBlend = ParticleTextures::BlendMode::Alpha;
    }

    // Sprite-expansion styles can pin their blend mode and mute the backlight
    // halo: matte or dark pixel art must not render additively, because black
    // adds nothing and vanishes.
    const StyleMotionSpec* newSpec = GetNewStyleMotion(params.style);
    const auto& visibilityTuning = ParticleTextures::GetStyleVisibilityTuning(texStyleId);
    float glowStrength = params.glowStrength * visibilityTuning.haloAlphaScale;
    if (newSpec)
    {
        if (newSpec->blend >= 0)
        {
            texBlend = static_cast<ParticleTextures::BlendMode>(newSpec->blend);
        }
        glowStrength *= newSpec->haloScale;
    }

    // Crowd normalizer. Shipped tiers enable up to 6 types; a .10 step would
    // cut those to 2/3 alpha before any per-particle math, so the step is .05.
    float alpha = params.alpha;
    if (params.enabledStyleCount > 1)
    {
        alpha /= (1.0f + .05f * (params.enabledStyleCount - 1));
    }

    int baseR = (params.color >> IM_COL32_R_SHIFT) & 0xFF;
    int baseG = (params.color >> IM_COL32_G_SHIFT) & 0xFF;
    int baseB = (params.color >> IM_COL32_B_SHIFT) & 0xFF;

    bool hasSecondaryColor = (params.colorSecondary != 0);
    int baseR2 = hasSecondaryColor ? ((params.colorSecondary >> IM_COL32_R_SHIFT) & 0xFF) : baseR;
    int baseG2 = hasSecondaryColor ? ((params.colorSecondary >> IM_COL32_G_SHIFT) & 0xFF) : baseG;
    int baseB2 = hasSecondaryColor ? ((params.colorSecondary >> IM_COL32_B_SHIFT) & 0xFF) : baseB;

    float timeScaled = params.time * params.speed;

    // F3: air current computed ONCE per call (2 sins, amortized across every
    // particle of this style slot). It depends only on the call's scaled time,
    // so two styles share one current exactly when their scaled speeds match:
    // the deck path gives every style the same speed, the nameplate path scales
    // the speed per style. Only the orbit, flow and twinkle paths read
    // airX/airY. Spark and Wisp, which carry their own trails, and the fall,
    // rise and zap paths ignore it.
    float airX = .055f * std::sin(timeScaled * .13f);
    float airY = .045f * std::sin(timeScaled * .11f + 1.7f);

    // Radial band for this style: with several styles enabled each slot starts
    // its ring at a different floor, so the styles settle at different mean
    // radii instead of piling onto one. The bands share the outer edge.
    float styleBandT = (params.enabledStyleCount > 0)
                           ? (static_cast<float>(params.styleIndex) + .5f) /
                                 static_cast<float>(params.enabledStyleCount)
                           : .5f;
    float bandFloor = std::clamp(.58f + .20f * styleBandT, .58f, .88f);
    if (params.enabledStyleCount <= 1)
    {
        bandFloor = SoloBandFloor(params.style);
    }
    float counterChance = CounterRotateChance(params.style);

    for (int i = 0; i < params.particleCount; ++i)
    {
        // Quasi-random angular placement via the golden angle (~137.508 deg),
        // minimal clustering. Constants:
        //   2.399963f  = golden angle in radians (~ pi(3-sqrt(5)))
        //   97         = prime per-style offset, prevents pattern repetition
        float golden = (float)(i + params.styleIndex * 97) * 2.399963f;
        float phase = golden + PTrait(i, params.styleIndex, Trait::Phase) * 1.2f;

        // Most orbiters ride the outer band; a fraction (Trait::Fill) drop onto
        // an interior radius so the tilted oval fills in as a disc rather than a
        // hollow ring. Fillers render behind the text, so readability is intact.
        float rU = PTrait(i, params.styleIndex, Trait::Radial);
        float radialAnchor;
        if (PTrait(i, params.styleIndex, Trait::Fill) < kFillFraction)
        {
            const float a2 = kInteriorFloor * kInteriorFloor;
            const float b2 = bandFloor * bandFloor;
            radialAnchor =
                std::sqrt(a2 + (b2 - a2) * rU);  // area-uniform [kInteriorFloor, bandFloor]
        }
        else
        {
            radialAnchor = AnnulusRadius(bandFloor, rU);
        }
        float speedVar = .72f + .56f * PTrait(i, params.styleIndex, Trait::Speed);
        float dirSign =
            (PTrait(i, params.styleIndex, Trait::Direction) < counterChance) ? -1.0f : 1.0f;
        float depth = PTrait(i, params.styleIndex, Trait::Depth);
        float sizeVar = .85f + .30f * PTrait(i, params.styleIndex, Trait::Size);
        float jitterAngle = PTrait(i, params.styleIndex, Trait::JitterAngle) * TWO_PI;
        float jitterDist = PTrait(i, params.styleIndex, Trait::JitterDist) * .22f;
        float bobPhase = PTrait(i, params.styleIndex, Trait::Bob) * TWO_PI;
        float heightBand =
            (PTrait(i, params.styleIndex, Trait::Elevation) - .5f) * kOrbitHeightSpread;

        // Floor at .75: a floor of .6 averages a permanent ~20% alpha cut on
        // every particle. The [.75, 1] range keeps the drift without dimming the
        // whole aura.
        float alphaVariation =
            .75f + .25f * (.5f + .5f * std::sin(golden * 1.7f + timeScaled * .3f));

        // C1: depth-keyed temperature lerp from the warm primary to the cool
        // secondary, plus a small decorrelated jitter. Far particles sit in the
        // deeper primary and near ones lean to the luminous highlight, which
        // keeps them inside the tier palette rather than rotating away from it.
        // `warmth` (C4) scales the whole mix; 0 = monochrome tier.
        float tMix = (.20f + .55f * depth) * params.colorWarmth;
        int r = std::clamp(baseR + (int)((baseR2 - baseR) * tMix), 0, 255);
        int g = std::clamp(baseG + (int)((baseG2 - baseG) * tMix), 0, 255);
        int b = std::clamp(baseB + (int)((baseB2 - baseB) * tMix), 0, 255);
        // Decorrelated jitter from an independent salt: push R warm, pull B
        // cool, never a full hue spin.
        float hj = (PTrait(i, params.styleIndex, Trait::Hue) - .5f) * 2.0f;
        r = std::clamp(r + (int)(8 * hj * params.colorWarmth), 0, 255);
        b = std::clamp(b - (int)(8 * hj * params.colorWarmth), 0, 255);

        ParticleContext ctx{.list = params.list,
                            .center = params.center,
                            .radiusX = params.radiusX,
                            .radiusY = params.radiusY,
                            .alpha = alpha,
                            .particleSize = params.particleSize,
                            .timeScaled = timeScaled,
                            .texStyleId = texStyleId,
                            .hasTextures = hasTextures,
                            .particleIndex = i,
                            .particleCount = params.particleCount,
                            .styleIndex = params.styleIndex,
                            .golden = golden,
                            .phase = phase,
                            .minRadius = bandFloor,
                            .radialAnchor = radialAnchor,
                            .jitterAngle = jitterAngle,
                            .jitterDist = jitterDist,
                            .alphaVariation = alphaVariation,
                            .speedVar = speedVar,
                            .dirSign = dirSign,
                            .depth = depth,
                            .sizeVar = sizeVar,
                            .bobPhase = bobPhase,
                            .heightBand = heightBand,
                            .r = r,
                            .g = g,
                            .b = b,
                            .r2 = baseR2,
                            .g2 = baseG2,
                            .b2 = baseB2,
                            .hasSecondaryColor = hasSecondaryColor,
                            .texBlendMode = texBlend,
                            .depthStrength = params.depthStrength,
                            .warmth = params.colorWarmth,
                            .glowStrength = glowStrength,
                            .glowSize = params.glowSize,
                            .shineThreshold = params.shineThreshold,
                            .glintScale = newSpec ? newSpec->haloScale : 1.0f,
                            .coreSizeScale = visibilityTuning.coreSizeScale,
                            .airX = airX,
                            .airY = airY};

        switch (params.style)
        {
            case Settings::ParticleStyle::Firefly:
                RenderFireflyParticle(ctx);
                break;
            case Settings::ParticleStyle::Snow:
                if (newSpec)
                {
                    RenderArchOrbitParticle(ctx, *newSpec);
                }
                else
                {
                    RenderSnowParticle(ctx);
                }
                break;
            case Settings::ParticleStyle::Smoke:
                RenderSmokeParticle(ctx);
                break;
            case Settings::ParticleStyle::Spark:
                RenderSparkParticle(ctx);
                break;
            case Settings::ParticleStyle::Wisp:
                RenderWispParticle(ctx);
                break;
            case Settings::ParticleStyle::Leaf:
                RenderLeafParticle(ctx);
                break;
            case Settings::ParticleStyle::Aurora:
                RenderAuroraParticle(ctx);
                break;
            case Settings::ParticleStyle::CherryBlossom:
                RenderCherryBlossomParticle(ctx);
                break;
            case Settings::ParticleStyle::Dust:
                RenderDustParticle(ctx);
                break;
            case Settings::ParticleStyle::Mote:
                RenderMoteParticle(ctx);
                break;
            default:
                // Sprite-expansion styles route through their archetype; a
                // style with no recipe falls back to the firefly wanderer.
                if (newSpec)
                {
                    switch (newSpec->arch)
                    {
                        case StyleMotionSpec::Arch::Orbit:
                            RenderArchOrbitParticle(ctx, *newSpec);
                            break;
                        case StyleMotionSpec::Arch::Rise:
                            RenderArchRiseParticle(ctx, *newSpec);
                            break;
                        case StyleMotionSpec::Arch::Fall:
                            RenderArchFallParticle(ctx, *newSpec);
                            break;
                        case StyleMotionSpec::Arch::Flow:
                            RenderArchFlowParticle(ctx, *newSpec);
                            break;
                        case StyleMotionSpec::Arch::Twinkle:
                            RenderArchTwinkleParticle(ctx, *newSpec);
                            break;
                        case StyleMotionSpec::Arch::Zap:
                            RenderArchZapParticle(ctx, *newSpec);
                            break;
                    }
                }
                else
                {
                    RenderFireflyParticle(ctx);
                }
                break;
        }
    }
}

}  // namespace TextEffects
