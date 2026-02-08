#pragma once

#include <SKSE/SKSE.h>

/**
 * @namespace Occlusion
 * @brief Actor visibility and occlusion culling.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Rendering
 *
 * Provides occlusion culling for nameplates using the game's built-in line-of-sight
 * system combined with camera frustum checks.
 *
 * ## :material-eye-off-outline: Occlusion Pipeline
 *
 * ```mermaid
 * %%{init: {'theme':'dark', 'look':'handDrawn'}}%%
 * flowchart TD
 *     classDef check fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef visible fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *     classDef occluded fill:#3a1a1a,stroke:#ef4444,color:#e2e8f0
 *
 *     A[Actor Position]:::check --> B{Close to Camera?}
 *     B -->|Yes| C[Visible]:::visible
 *     B -->|No| D{Behind Camera?}
 *     D -->|Yes| E[Occluded]:::occluded
 *     D -->|No| F{Line of Sight?}
 *     F -->|Yes| C
 *     F -->|No| E
 * ```
 *
 * ## :material-angle-acute: Behind-Camera Test
 *
 * **Step 1 — Direction vector.** Given the camera world position
 * $p_{cam}$ and the actor world position $p_{actor}$, compute the
 * displacement vector pointing from the camera toward the actor:
 *
 * $$\vec{v} = p_{actor} - p_{cam}$$
 *
 * **Step 2 — Normalize.** Divide by the vector's length to get a
 * unit direction (length $= 1$), so only orientation matters:
 *
 * $$\hat{d} = \frac{\vec{v}}{\|\vec{v}\|} = \frac{p_{actor} - p_{cam}}{\sqrt{v_x^2 + v_y^2 + v_z^2}}$$
 *
 * **Step 3 — Dot product.** The camera also provides a unit forward
 * vector $\hat{f}$ (the direction the player is looking). The dot
 * product of two unit vectors equals the cosine of the angle between
 * them:
 *
 * $$\hat{f} \cdot \hat{d} = f_x d_x + f_y d_y + f_z d_z = \cos\alpha$$
 *
 * where $\alpha$ is the angle between the camera's forward axis and
 * the direction to the actor. Key values:
 *
 * | $\cos\alpha$ | $\alpha$ | Meaning              |
 * |:------------:|:--------:|----------------------|
 * | $+1$         | $0$      | Directly in front    |
 * | $0$          | $90$     | Perpendicular (edge) |
 * | $-1$         | $180$    | Directly behind      |
 *
 * **Step 4 — Threshold test.** An actor is classified as behind the
 * camera when the dot product falls below a threshold:
 *
 * $$\hat{f} \cdot \hat{d} < \theta_{behind}$$
 *
 * We use $\theta_{behind} = -0.2$. Solving for the angle:
 *
 * $$\alpha = \arccos(\theta_{behind}) = \arccos(-0.2) \approx 101.5 deg$$
 *
 * A threshold of exactly $0$ would mean $90 deg$ the geometric edge of
 * the forward hemisphere. By choosing $-0.2$ we extend the visible
 * zone by ${\approx}11.5 deg$ past perpendicular, so actors slightly
 * behind the camera are still considered visible. This prevents
 * nameplates from popping in and out at the screen border when the
 * player turns.
 *
 * ## :material-ruler: Distance Units
 *
 * All distances are in Skyrim game units where $\approx 70$ units $= 1$ meter.
 *
 * |                    Constant | Value  | Description                                                       |
 * |-----------------------------|--------|-------------------------------------------------------------------|
 * |   `kCloseDistanceThreshold` | 100.0  | Always visible when $\\|p_{actor} - p_{cam}\\| < 100$ units       |
 * | `kBehindCameraDotThreshold` | -0.2   | Behind camera when $\hat{f} \cdot \hat{d} < -0.2$ ($\approx 101$) |
 * |     `kHeadHeightMultiplier` | 0.9    | Head position: $y_{head} = y_{base} + 0.9h$                       |
 */
namespace Occlusion
{
    /**
     * Constants for occlusion calculations.
     */
    namespace Constants
    {
        constexpr float kCloseDistanceThreshold = 100.0f;    ///< Visible when $\|p_{actor} - p_{cam}\| < 100$ game units
        constexpr float kBehindCameraDotThreshold = -0.2f;   ///< Behind camera when $\hat{f} \cdot \hat{d} < -0.2$ ($\approx 101$)
        constexpr float kHeadHeightMultiplier = 0.9f;        ///< Head position: $y_{head} = y_{base} + 0.9h$
    }

    /**
     * Check if player has line of sight to the specified actor.
     *
     * Uses the game's built-in `Actor::HasLineOfSight` function for accurate
     * collision detection against world geometry.
     *
     * @param actor The actor to check visibility for.
     *
     * @return `true` if player can see the actor, `false` if blocked.
     */
    bool HasLineOfSightToActor(RE::Actor* actor);

    /**
     * Check if an actor should be considered occluded.
     *
     * Considers multiple factors:
     * - Global occlusion setting
     * - Distance from camera
     * - Behind-camera check
     * - Line of sight through world geometry
     *
     * @param actor The actor to check.
     * @param player The player actor.
     * @param actorWorldPos The world position to check from.
     *
     * @return `true` if the actor is occluded and nameplate should be hidden.
     */
    bool IsActorOccluded(RE::Actor* actor, RE::Actor* player, const RE::NiPoint3& actorWorldPos);

    /**
     * Check if an actor is occluded using head position.
     *
     * Convenience overload that calculates world position from actor's head.
     *
     * @param actor The actor to check.
     * @param player The player actor.
     *
     * @return `true` if the actor is occluded.
     */
    bool IsActorOccluded(RE::Actor* actor, RE::Actor* player);

    /**
     * Get camera position and forward direction.
     *
     * @param[out] outPos Camera world position.
     * @param[out] outForward Camera forward direction.
     *
     * @return `true` if camera data was retrieved successfully.
     */
    bool GetCameraInfo(RE::NiPoint3& outPos, RE::NiPoint3& outForward);

    /**
     * Check if a world position is behind the camera.
     *
     * Uses dot product between camera forward and direction to target.
     * Threshold is set to ~101 degrees to catch actors just past peripheral vision.
     *
     * @param worldPos The position to check.
     * @param cameraPos Camera world position.
     * @param cameraForward Camera forward direction.
     *
     * @return `true` if the position is behind the camera.
     */
    bool IsBehindCamera(const RE::NiPoint3& worldPos, const RE::NiPoint3& cameraPos, const RE::NiPoint3& cameraForward);

}
