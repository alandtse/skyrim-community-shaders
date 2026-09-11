#include "ActorUtils.h"
#include <algorithm>
#include <cmath>

namespace
{
	RE::NiPoint3 TransformHavokPoint(const RE::hkTransform& transform, const RE::hkVector4& point)
	{
		alignas(16) float localPoint[4];
		_mm_store_ps(localPoint, point.quad);

		__m128 worldPoint = transform.translation.quad;
		worldPoint = _mm_add_ps(worldPoint, _mm_mul_ps(transform.rotation.col0.quad, _mm_set1_ps(localPoint[0])));
		worldPoint = _mm_add_ps(worldPoint, _mm_mul_ps(transform.rotation.col1.quad, _mm_set1_ps(localPoint[1])));
		worldPoint = _mm_add_ps(worldPoint, _mm_mul_ps(transform.rotation.col2.quad, _mm_set1_ps(localPoint[2])));

		alignas(16) float transformedPoint[4];
		_mm_store_ps(transformedPoint, worldPoint);
		return RE::NiPoint3(transformedPoint[0], transformedPoint[1], transformedPoint[2]) *
		       RE::bhkWorld::GetWorldScaleInverse();
	}
}

namespace Util
{
	bool GetShapeCollisionCapsule(RE::bhkNiCollisionObject* collisionObj, ShapeCollisionCapsule& capsule)
	{
		if (!collisionObj)
			return false;

		RE::bhkRigidBody* bhkRigid = collisionObj->body.get() ? collisionObj->body.get()->AsBhkRigidBody() : nullptr;
		RE::hkpRigidBody* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		if (bhkRigid && hkpRigid && !skyrim_cast<RE::hkpListShape*>(hkpRigid)) {  // Ignore hkpListShape, unsupported
			const auto* shape = hkpRigid->collidable.GetShape();
			if (!shape)
				return false;

			if (shape->type == RE::hkpShapeType::kCapsule) {
				const auto* capsuleShape = static_cast<const RE::hkpCapsuleShape*>(shape);
				RE::hkTransform transform;
				bhkRigid->GetTransform(transform);

				capsule.pointA = TransformHavokPoint(transform, capsuleShape->vertexA);
				capsule.pointB = TransformHavokPoint(transform, capsuleShape->vertexB);
				capsule.radius = capsuleShape->radius * RE::bhkWorld::GetWorldScaleInverse();
				if (capsule.pointB.z < capsule.pointA.z)
					std::swap(capsule.pointA, capsule.pointB);
				return std::isfinite(capsule.radius) && capsule.radius > 0.0f;
			}

			RE::hkVector4 massCenter;
			bhkRigid->GetCenterOfMassWorld(massCenter);
			float massTrans[4];
			// Use unaligned store to avoid UB from potential stack misalignment
			_mm_storeu_ps(massTrans, massCenter.quad);
			capsule.pointA = RE::NiPoint3(massTrans[0], massTrans[1], massTrans[2]) * RE::bhkWorld::GetWorldScaleInverse();
			capsule.pointB = capsule.pointA;
			return Util::ExtractShapeBound(shape, capsule.radius);
		}
		return false;
	}

	bool GetShapeBound(RE::bhkNiCollisionObject* collisionObj, RE::NiPoint3& centerPos, float& radius)
	{
		ShapeCollisionCapsule capsule;
		if (!GetShapeCollisionCapsule(collisionObj, capsule))
			return false;

		centerPos = (capsule.pointA + capsule.pointB) * 0.5f;
		radius = capsule.radius + capsule.pointA.GetDistance(capsule.pointB) * 0.5f;
		return true;
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
