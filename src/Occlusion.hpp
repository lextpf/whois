#pragma once

#include <SKSE/SKSE.h>

/**
 * @namespace Occlusion
 * @brief Actor visibility and occlusion culling.
 * @author Alex (https://github.com/lextpf)
 * @ingroup Occlusion
 *
 * Hides a nameplate when the game's built-in line-of-sight query fails or when the
 * actor sits behind the camera. A failed query resolves to visible, so a plate is
 * never hidden by a check that could not run.
 *
 * ## :material-arrow-decision: Thread Affinity
 *
 * The function names carry no `_GameThread` / `RT` suffix, so the affinity is
 * stated here:
 *
 * - `HasLineOfSightToActor` and `IsActorOccluded` dereference `RE::Actor` and
 *   `RE::PlayerCharacter`. The game thread must call them, and it publishes the
 *   result into the snapshot for the render thread.
 * - `GetCameraInfo` and `IsBehindCamera` read camera data only and are safe from
 *   either thread. The render thread calls `GetCameraInfo` every frame for focus
 *   selection and Graffito targeting, where a torn read is a benign one-frame glitch.
 *
 * ## :material-eye-off-outline: Occlusion Pipeline
 *
 * ```mermaid
 * ---
 * config:
 *   theme: dark
 *   look: handDrawn
 * ---
 * flowchart LR
 *     classDef check fill:#1e3a5f,stroke:#3b82f6,color:#e2e8f0
 *     classDef visible fill:#1a3a2a,stroke:#10b981,color:#e2e8f0
 *     classDef occluded fill:#3a1a1a,stroke:#ef4444,color:#e2e8f0
 *
 *     C[Visible]:::visible
 *     G0{Occlusion enabled and actor and player valid?}:::check
 *     G1{Camera data available?}:::check
 *
 *     G0 -->|No| C
 *     G0 -->|Yes| G1
 *     G1 -->|No| C
 *     G1 -->|Yes| A[Actor Position]:::check
 *     A --> B{Close to Camera?}
 *     B -->|Yes| C
 *     B -->|No| D{Behind Camera?}
 *     D -->|Yes| E[Occluded]:::occluded
 *     D -->|No| F{Line of Sight?}
 *     F -->|Yes| C
 *     F -->|No| E
 * ```
 *
 * ## :material-angle-acute: Behind-Camera Test
 *
 * **Step 1 - Direction vector.** From the camera world position $p_{cam}$ to the
 * actor world position $p_{actor}$:
 *
 * $$\vec{v} = p_{actor} - p_{cam}$$
 *
 * **Step 2 - Normalize**, so only orientation matters:
 *
 * $$\hat{d} = \frac{\vec{v}}{\|\vec{v}\|} = \frac{p_{actor} - p_{cam}}{\sqrt{v_x^2 + v_y^2 +
 * v_z^2}}$$
 *
 * **Step 3 - Dot product** against the camera unit forward vector $\hat{f}$, which is
 * the camera forward axis and not the player's facing:
 *
 * $$\hat{f} \cdot \hat{d} = f_x d_x + f_y d_y + f_z d_z = \cos\alpha$$
 *
 * where $\alpha$ is the angle between the camera's forward axis and the direction to
 * the actor. Key values:
 *
 * | $\cos\alpha$ | $\alpha$ | Meaning              |
 * |:------------:|:--------:|----------------------|
 * | $+1$         | $0$      | Directly in front    |
 * | $0$          | $90$     | Perpendicular (edge) |
 * | $-1$         | $180$    | Directly behind      |
 *
 * **Step 4 - Threshold test.** The actor counts as behind the camera when the dot
 * product falls below a threshold:
 *
 * $$\hat{f} \cdot \hat{d} < \theta_{behind}$$
 *
 * The threshold is $\theta_{behind} = -0.2$. Solving for the angle:
 *
 * $$\alpha = \arccos(\theta_{behind}) = \arccos(-0.2) \approx 101.5 deg$$
 *
 * A threshold of exactly $0$ would cut at $90 deg$, the geometric edge of the forward
 * hemisphere. The $-0.2$ extends the visible zone ${\approx}11.5 deg$ past
 * perpendicular, so actors slightly behind the camera stay visible. This keeps
 * nameplates from popping in and out at the screen border when the player turns.
 *
 * ## :material-ruler: Distance Units
 *
 * All distances are in Skyrim game units where $\approx 70$ units $= 1$ meter.
 *
 * |                      Constant | Value | Description                                        |
 * |-------------------------------|-------|----------------------------------------------------|
 * |    `CLOSE_DISTANCE_THRESHOLD` | 100.0 | Visible when the camera-to-anchor distance $< 100$ |
 * | `BEHIND_CAMERA_DOT_THRESHOLD` |  -0.2 | Behind camera past ~101.5 deg                      |
 */
namespace Occlusion
{
/**
 * @namespace Occlusion::Constants
 * @brief Constants for occlusion calculations.
 */
namespace Constants
{
inline constexpr float CLOSE_DISTANCE_THRESHOLD =
    100.0f;  ///< Visible when $\|p_{actor} - p_{cam}\| < 100$ game units
inline constexpr float BEHIND_CAMERA_DOT_THRESHOLD =
    -.2f;  ///< Behind camera when $\hat{f} \cdot \hat{d} < -0.2$ (~101.5 deg)
}  // namespace Constants

/**
 * @brief Check whether the player has line of sight to an actor.
 *
 * Wraps the game's `Actor::HasLineOfSight`, which traces against world geometry.
 *
 * @note Game-thread only. It dereferences `RE::Actor` and `RE::PlayerCharacter`.
 *
 * @param actor The actor to check visibility for.
 *
 * @return `true` if the player can see the actor, `false` if blocked. Also `true`
 *         when @p actor is null, when the player singleton is unavailable, or when
 *         the engine query fails. This is fail-open, to avoid false occlusion.
 */
bool HasLineOfSightToActor(RE::Actor* actor);

/**
 * @brief Check if an actor should be considered occluded.
 *
 * Combines the global occlusion setting, distance from the camera, the behind-camera
 * test, and line of sight through world geometry.
 *
 * @note Game-thread only. It dereferences `RE::Actor` and `RE::PlayerCharacter`.
 *       The game thread publishes the result into the snapshot for the render thread.
 * @note Fail-open. It returns `false` without evaluating any factor when occlusion
 *       is disabled, when @p actor or @p player is null, or when camera data is
 *       unavailable. `false` always means "do not hide", never "verified visible".
 *
 * @param actor The actor to check.
 * @param player The player actor. It is used only as a liveness guard: the
 *               line-of-sight query always starts from
 *               `RE::PlayerCharacter::GetSingleton()`, so a different actor here
 *               changes nothing.
 * @param actorWorldPos The world position to test. This is the nameplate anchor,
 *                      which can differ from the actor's root position. All
 *                      distances and angles are measured from the camera, not
 *                      from `player`.
 * @param occlusionEnabled Whether occlusion culling is enabled (from Settings).
 *
 * @return `true` if the actor is occluded and nameplate should be hidden.
 */
bool IsActorOccluded(RE::Actor* actor,
                     RE::Actor* player,
                     const RE::NiPoint3& actorWorldPos,
                     bool occlusionEnabled);

/**
 * @brief Get camera position and forward direction.
 *
 * @note Safe from either thread. The render thread calls it every frame for focus
 *       selection and Graffito targeting, where a torn read is a benign one-frame
 *       glitch.
 * @post On `false`, neither out-parameter is written. Callers must not use them.
 *
 * @param[out] outPos Camera world position.
 * @param[out] outForward Unit forward axis of the camera root node in world space
 *                        (column 1 of its rotation matrix). It is already
 *                        normalized. In third person it is the camera direction,
 *                        not the player's facing.
 *
 * @return `true` if camera data was retrieved successfully.
 */
bool GetCameraInfo(RE::NiPoint3& outPos, RE::NiPoint3& outForward);

/**
 * @brief Check if a world position is behind the camera.
 *
 * The cone is widened ~11.5 deg past perpendicular (dot < -0.2, that is ~101.5 deg),
 * so actors just outside the frustum edge stay visible. Only positions beyond
 * ~101.5 deg count as behind.
 *
 * @pre `cameraForward` is a unit vector. The function normalizes only the
 *      camera-to-position vector, so a non-unit forward axis scales the dot product
 *      and moves the threshold angle. `GetCameraInfo` supplies a normalized axis.
 *
 * @param worldPos The position to check.
 * @param cameraPos Camera world position.
 * @param cameraForward Camera forward unit axis, as returned by `GetCameraInfo`.
 *
 * @return `true` if the position is behind the camera. A position closer than
 *         0.001 units to the camera has no usable direction, so it returns `false`.
 */
bool IsBehindCamera(const RE::NiPoint3& worldPos,
                    const RE::NiPoint3& cameraPos,
                    const RE::NiPoint3& cameraForward);

}  // namespace Occlusion
