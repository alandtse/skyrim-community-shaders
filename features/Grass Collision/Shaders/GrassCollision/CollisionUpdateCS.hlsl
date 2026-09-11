cbuffer PerFrameCB : register(b0)
{
	float2 PosOffset;
	uint2 ArrayOrigin;
	int2 ValidMargin;
	float TimeDelta;
	uint BoundingBoxCount;
	float GrassInteractionRadius;
	float CollisionStrength;
	float SpringStrength;
	float Damping;
	float MaximumBend;
	float MaximumCompression;
	float CompressionRecovery;
}

struct BoundingBoxPacked
{
	float2 MinExtent;
	float2 MaxExtent;
	uint IndexStart;
	uint IndexEnd;
	float2 pad0;
};

StructuredBuffer<BoundingBoxPacked> CollisionBoundingBoxes : register(t0);

struct CollisionShapePacked
{
	float4 CurrentPointAAndRadius;
	float4 CurrentPointB;
	float4 PreviousPointA;
	float4 PreviousPointB;
};

StructuredBuffer<CollisionShapePacked> CollisionInstances : register(t1);
Texture2D<float4> PreviousDeformation : register(t2);
Texture2D<float2> PreviousVelocity : register(t3);
RWTexture2D<float4> Deformation : register(u0);
RWTexture2D<float2> Velocity : register(u1);

groupshared BoundingBoxPacked SharedBoundingBoxes[64];

float2 ClosestPointOnSegment(float2 position, float2 pointA, float2 pointB)
{
	float2 segment = pointB - pointA;
	float segmentLengthSquared = dot(segment, segment);
	float segmentPosition = segmentLengthSquared > 1e-5 ?
	                            saturate(dot(position - pointA, segment) / segmentLengthSquared) :
	                            0.0;
	return pointA + segment * segmentPosition;
}

[numthreads(8, 8, 1)] void main(
	uint3 groupId : SV_GroupID, uint3 dispatchThreadId : SV_DispatchThreadID,
	uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	if (groupIndex < BoundingBoxCount)
		SharedBoundingBoxes[groupIndex] = CollisionBoundingBoxes[groupIndex];
	GroupMemoryBarrierWithGroupSync();

	const uint TEXTURE_SIZE = 1024;
	const float WORLD_SIZE = 8192.0;
	const float COLLISION_ACCELERATION = 50.0;
	const int2 textureSize = int2(TEXTURE_SIZE, TEXTURE_SIZE);

	int2 cellID = int2(dispatchThreadId.xy) - int2(ArrayOrigin);
	cellID %= textureSize;
	cellID += int2(cellID < 0) * textureSize;
	float2 cellCentreMS = float2(cellID) + 0.5 - TEXTURE_SIZE * 0.5;
	cellCentreMS = cellCentreMS / TEXTURE_SIZE * WORLD_SIZE + PosOffset;

	int2 validMin = max(int2(0, 0), ValidMargin);
	int2 validMax = int2(TEXTURE_SIZE - 1, TEXTURE_SIZE - 1) + min(int2(0, 0), ValidMargin);
	bool isValid = all(cellID >= validMin) && all(cellID <= validMax);

	float2 bend = 0.0;
	float2 velocity = 0.0;
	float compression = 0.0;
	if (isValid) {
		float4 previousState = PreviousDeformation.Load(int3(dispatchThreadId.xy, 0));
		bend = previousState.xy;
		compression = previousState.z;
		velocity = PreviousVelocity.Load(int3(dispatchThreadId.xy, 0));
	}

	float deltaTime = clamp(TimeDelta, 0.0, 1.0 / 15.0);
	float2 collisionAcceleration = 0.0;
	float collisionCompression = 0.0;
	for (uint i = 0; i < BoundingBoxCount; ++i) {
		BoundingBoxPacked boundingBox = SharedBoundingBoxes[i];
		if (all(cellCentreMS >= boundingBox.MinExtent && cellCentreMS <= boundingBox.MaxExtent)) {
			for (uint j = boundingBox.IndexStart; j < boundingBox.IndexEnd; ++j) {
				CollisionShapePacked collider = CollisionInstances[j];
				float2 currentA = collider.CurrentPointAAndRadius.xy;
				float2 currentB = collider.CurrentPointB.xy;
				float2 previousA = collider.PreviousPointA.xy;
				float2 previousB = collider.PreviousPointB.xy;
				float2 currentCentre = (currentA + currentB) * 0.5;
				float2 previousCentre = (previousA + previousB) * 0.5;
				float projectedHalfLength = 0.5 * max(length(currentB - currentA), length(previousB - previousA));
				float radius = collider.CurrentPointAAndRadius.w + projectedHalfLength + max(GrassInteractionRadius, 0.0);
				float2 closestPoint = ClosestPointOnSegment(cellCentreMS, previousCentre, currentCentre);
				float2 delta = cellCentreMS - closestPoint;
				float distanceToSweep = length(delta);
				float penetration = radius - distanceToSweep;
				if (penetration > 0.0) {
					float penetrationRatio = saturate(penetration / max(radius, 1e-4));
					float2 movement = currentCentre - previousCentre;
					float2 fallbackDirection = dot(movement, movement) > 1e-5 ?
					                               normalize(float2(-movement.y, movement.x)) :
					                               float2(1.0, 0.0);
					float2 outwardDirection = distanceToSweep > 1e-4 ? delta / distanceToSweep : fallbackDirection;
					collisionAcceleration += outwardDirection * penetrationRatio;
					collisionCompression = max(collisionCompression, penetrationRatio);
				}
			}
		}
	}

	collisionAcceleration *= max(CollisionStrength, 0.0) * COLLISION_ACCELERATION;
	float springStrength = max(SpringStrength, 0.0);
	float springDenominator = 1.0 + max(Damping, 0.0) * deltaTime + springStrength * deltaTime * deltaTime;
	float2 updatedVelocity = (velocity + collisionAcceleration * deltaTime - bend * springStrength * deltaTime) / springDenominator;
	float2 updatedBend = bend + updatedVelocity * deltaTime;
	velocity = updatedVelocity;
	bend = updatedBend;

	float maximumBend = max(MaximumBend, 0.0);
	float bendMagnitude = length(bend);
	if (bendMagnitude > maximumBend && bendMagnitude > 1e-5) {
		float2 bendDirection = bend / bendMagnitude;
		bend = bendDirection * maximumBend;
		velocity -= bendDirection * max(dot(velocity, bendDirection), 0.0);
	}

	float updatedCompression = compression * exp(-max(CompressionRecovery, 0.0) * deltaTime);
	updatedCompression = max(updatedCompression, collisionCompression * max(MaximumCompression, 0.0));
	compression = updatedCompression;
	compression = clamp(compression, 0.0, max(MaximumCompression, 0.0));
	Deformation[dispatchThreadId.xy] = float4(bend, compression, 0.0);
	Velocity[dispatchThreadId.xy] = velocity;
}
