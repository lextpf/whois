#include "Occlusion.h"
#include "Settings.h"

#include <RE/A/Actor.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>

namespace Occlusion
{
    bool GetCameraInfo(RE::NiPoint3& outPos, RE::NiPoint3& outForward)
    {
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera || !camera->cameraRoot)
        {
            return false;
        }

        outPos = camera->cameraRoot->world.translate;

        // Column 1 of the camera rotation matrix points forward in world space
        const RE::NiMatrix3& rot = camera->cameraRoot->world.rotate;
        outForward.x = rot.entry[0][1];
        outForward.y = rot.entry[1][1];
        outForward.z = rot.entry[2][1];

        return true;
    }

    bool IsBehindCamera(const RE::NiPoint3& worldPos, const RE::NiPoint3& cameraPos, const RE::NiPoint3& cameraForward)
    {
        RE::NiPoint3 toTarget = worldPos - cameraPos;
        float distance = toTarget.Length();

        if (distance < 0.001f)
        {
            return false;  // At camera position, not behind
        }

        // Normalize direction to target
        toTarget.x /= distance;
        toTarget.y /= distance;
        toTarget.z /= distance;

        // Dot product: Negative means behind camera
        float dot = toTarget.x * cameraForward.x +
                    toTarget.y * cameraForward.y +
                    toTarget.z * cameraForward.z;

        return dot < Constants::kBehindCameraDotThreshold;
    }

    bool HasLineOfSightToActor(RE::Actor* actor)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !actor)
        {
            return true;  // Assume visible if we can't check
        }

        bool losResult = true;
        // HasLineOfSight returns success, losResult is set to true if LOS exists
        if (player->HasLineOfSight(actor, losResult))
        {
            return losResult;
        }

        return true;  // If check failed, assume visible
    }

    bool IsActorOccluded(RE::Actor* actor, RE::Actor* player, const RE::NiPoint3& actorWorldPos)
    {
        // Early out if occlusion is disabled
        if (!Settings::EnableOcclusionCulling || !actor || !player)
        {
            return false;
        }

        // Get camera info
        RE::NiPoint3 cameraPos, cameraForward;
        if (!GetCameraInfo(cameraPos, cameraForward))
        {
            return false;  // No camera, don't occlude
        }

        // Calculate distance to actor
        RE::NiPoint3 toActor = actorWorldPos - cameraPos;
        float distance = toActor.Length();

        // Very close actors are always visible
        if (distance < Constants::kCloseDistanceThreshold)
        {
            return false;
        }

        // Check if actor is behind camera
        if (IsBehindCamera(actorWorldPos, cameraPos, cameraForward))
        {
            return true;
        }

        // Use game's built-in line of sight check
        return !HasLineOfSightToActor(actor);
    }

}
