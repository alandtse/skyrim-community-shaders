#include "ActorUtils.h"

#include "Globals.h"

#include <algorithm>
#include <cmath>

namespace Util
{
	// Actor query pattern adapted from po3 under MIT.
	// https://github.com/powerof3/PapyrusExtenderSSE/blob/7a73b47bc87331bec4e16f5f42f2dbc98b66c3a7/include/Papyrus/Functions/Faction.h#L24C7-L46
	void ForEachLoadedActor(const std::function<void(RE::Actor*)>& a_callback)
	{
		if (!a_callback)
			return;

		auto* player = globals::game::player;
		if (player)
			a_callback(player);

		if (const auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (const auto& actorHandle : processLists->highActorHandles) {
				if (const auto actor = actorHandle.get(); actor && actor.get() != player)
					a_callback(actor.get());
			}
		}
	}

	void ForEachGeometry(RE::NiAVObject* a_root, const std::function<void(RE::BSGeometry*)>& a_callback)
	{
		if (!a_root || !a_callback)
			return;

		RE::BSVisit::TraverseScenegraphGeometries(a_root, [&a_callback](RE::BSGeometry* a_geometry) {
			a_callback(a_geometry);
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	void ForEachActorGeometry(RE::Actor* a_actor, const std::function<void(RE::BSGeometry*)>& a_callback)
	{
		if (!a_actor || !a_actor->Is3DLoaded() || !a_callback)
			return;

		auto* thirdPersonRoot = a_actor->Get3D(false);
		auto* firstPersonRoot = a_actor->Get3D(true);
		ForEachGeometry(thirdPersonRoot, a_callback);
		if (firstPersonRoot != thirdPersonRoot)
			ForEachGeometry(firstPersonRoot, a_callback);
	}

	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius)
	{
		if (!collisionObj)
			return false;

		RE::bhkRigidBody* bhkRigid = collisionObj->body.get() ? collisionObj->body.get()->AsBhkRigidBody() : nullptr;
		RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		if (bhkRigid && hkpRigid && !skyrim_cast<RE::hkpListShape*>(hkpRigid)) {  // Ignore hkpListShape, unsupported
			RE::hkVector4 massCenter;
			bhkRigid->GetCenterOfMassWorld(massCenter);
			float massTrans[4];
			// Use unaligned store to avoid UB from potential stack misalignment
			_mm_storeu_ps(massTrans, massCenter.quad);
			centerPos = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();
			return Util::ExtractShapeBound(hkpRigid->collidable.GetShape(), radius);
		}
		return false;
	}

	bool ExtractShapeBound(const RE::hkpShape* shape, float& radius)
	{
		using ShapeType = RE::hkpShapeType;
		if (!shape)
			return false;

		// Helpers to avoid repeating projection math and ensure offset-invariant half-extents
		auto project = [shape](float x, float y, float z) {
			return shape->GetMaximumProjection(RE::hkVector4{ x, y, z, 0.0f }) * RE::bhkWorld::GetWorldScaleInverse();
		};
		auto symmetricHalfExtents = [&project](float& hx, float& hy, float& hz) {
			float x_pos = project(1.0f, 0.0f, 0.0f);
			float x_neg = project(-1.0f, 0.0f, 0.0f);
			float y_pos = project(0.0f, 1.0f, 0.0f);
			float y_neg = project(0.0f, -1.0f, 0.0f);
			float z_pos = project(0.0f, 0.0f, 1.0f);
			float z_neg = project(0.0f, 0.0f, -1.0f);
			hx = 0.5f * (x_pos - x_neg);
			hy = 0.5f * (y_pos - y_neg);
			hz = 0.5f * (z_pos - z_neg);
		};
		auto halfDiagonal = [](float hx, float hy, float hz) {
			return sqrtf(hx * hx + hy * hy + hz * hz);
		};
		if (shape->type == ShapeType::kCapsule) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			// For capsules, use the maximum half-extent (typically hz for vertical orientation)
			// as the farthest point lies along the capsule's main axis, not at the diagonal
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else if (shape->type == ShapeType::kSphere) {
			// For spheres, any axis should yield the same half-extent; use symmetric X
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = hx;
			return true;
		} else if (shape->type == ShapeType::kBox) {
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = halfDiagonal(hx, hy, hz);
			return true;
		} else if (shape->type == ShapeType::kCylinder) {
			// Use symmetric half-extents; cylinder radius is max of X/Y half-extents
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			float hr = std::max(hx, hy);
			radius = sqrtf(hr * hr + hz * hz);
			return true;
		} else if (shape->type == ShapeType::kConvexVertices || shape->type == ShapeType::kTriangle) {
			// Offset-invariant estimate: take symmetric half-extents per axis and use the max
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		} else {
			// Fallback: mirror the convex/triangle approach for consistency
			float hx, hy, hz;
			symmetricHalfExtents(hx, hy, hz);
			radius = std::max(hx, std::max(hy, hz));
			return true;
		}
	}
}
