#include "ProjectileHookDispatcher.h"

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

namespace
{
	bool HasEvent(ProjectileHookDispatcher::Event a_events, ProjectileHookDispatcher::Event a_event)
	{
		return (std::to_underlying(a_events) & std::to_underlying(a_event)) != 0;
	}
}

template <class T>
struct ProjectileHookDispatcher::ProjectileAddImpactHook
{
	static void thunk(T* a_projectile, RE::TESObjectREFR* a_reference,
		const RE::NiPoint3& a_targetLocation, const RE::NiPoint3& a_velocity,
		RE::hkpCollidable* a_collidable, std::int32_t a_arg6, std::uint32_t a_arg7)
	{
		GetSingleton().DispatchImpact(*a_projectile, a_targetLocation, a_velocity);
		func(a_projectile, a_reference, a_targetLocation, a_velocity, a_collidable, a_arg6, a_arg7);
	}

	static inline std::size_t size = REL::Relocate(0xBD, 0xBD, 0xBE);
	static inline REL::Relocation<decltype(thunk)> func;
};

template <class T>
struct ProjectileHookDispatcher::ProjectileUpdateHook
{
	static void thunk(T* a_projectile, float a_deltaTime)
	{
		GetSingleton().DispatchMotion(*a_projectile, a_deltaTime);
		func(a_projectile, a_deltaTime);
	}

	static inline std::size_t size = REL::Relocate(0xAB, 0xAB, 0xAC);
	static inline REL::Relocation<decltype(thunk)> func;
};

ProjectileHookDispatcher& ProjectileHookDispatcher::GetSingleton()
{
	static ProjectileHookDispatcher singleton;
	return singleton;
}

void ProjectileHookDispatcher::AddImpactObserver(void* a_owner, ImpactObserver a_observer)
{
	if (!a_owner || !a_observer)
		return;
	std::unique_lock lock(observersMutex);
	const auto end = impactObservers.begin() + impactObserverCount;
	if (std::ranges::find_if(impactObservers.begin(), end,
			[a_owner](const auto& a_registration) { return a_registration.owner == a_owner; }) != end)
		return;
	if (impactObserverCount >= impactObservers.size()) {
		logger::error("Projectile impact observer capacity exhausted");
		return;
	}
	impactObservers[impactObserverCount++] = { a_owner, a_observer };
}

void ProjectileHookDispatcher::AddMotionObserver(void* a_owner, MotionObserver a_observer)
{
	if (!a_owner || !a_observer)
		return;
	std::unique_lock lock(observersMutex);
	const auto end = motionObservers.begin() + motionObserverCount;
	if (std::ranges::find_if(motionObservers.begin(), end,
			[a_owner](const auto& a_registration) { return a_registration.owner == a_owner; }) != end)
		return;
	if (motionObserverCount >= motionObservers.size()) {
		logger::error("Projectile motion observer capacity exhausted");
		return;
	}
	motionObservers[motionObserverCount++] = { a_owner, a_observer };
}

void ProjectileHookDispatcher::RemoveObservers(void* a_owner)
{
	std::unique_lock lock(observersMutex);
	const auto removeOwner = [a_owner](auto& a_observers, std::size_t& a_count) {
		const auto first = a_observers.begin();
		const auto last = first + a_count;
		const auto newEnd = std::remove_if(first, last,
			[a_owner](const auto& a_registration) { return a_registration.owner == a_owner; });
		a_count = static_cast<std::size_t>(std::distance(first, newEnd));
		using Registration = typename std::remove_reference_t<decltype(a_observers)>::value_type;
		std::fill(first + a_count, a_observers.end(), Registration{});
	};
	removeOwner(impactObservers, impactObserverCount);
	removeOwner(motionObservers, motionObserverCount);
}

template <class T>
void ProjectileHookDispatcher::InstallProjectileHooks(Event a_events, RE::BGSProjectileData::Type a_type)
{
	const auto bit = static_cast<std::uint16_t>(a_type);
	if (HasEvent(a_events, Event::Impact) && (installedImpactTypes & bit) == 0) {
		stl::write_vfunc<T, 0, ProjectileAddImpactHook<T>>();
		installedImpactTypes |= bit;
	}
	if (HasEvent(a_events, Event::Motion) && (installedMotionTypes & bit) == 0) {
		stl::write_vfunc<T, 0, ProjectileUpdateHook<T>>();
		installedMotionTypes |= bit;
	}
}

void ProjectileHookDispatcher::ObserveProjectileType(const RE::BGSProjectile& a_projectile, Event a_events)
{
	using Type = RE::BGSProjectileData::Type;
	if (a_projectile.IsMissile())
		InstallProjectileHooks<RE::MissileProjectile>(a_events, Type::kMissile);
	if (a_projectile.IsGrenade())
		InstallProjectileHooks<RE::GrenadeProjectile>(a_events, Type::kGrenade);
	if (a_projectile.IsBeam())
		InstallProjectileHooks<RE::BeamProjectile>(a_events, Type::kBeam);
	if (a_projectile.IsFlamethrower())
		InstallProjectileHooks<RE::FlameProjectile>(a_events, Type::kFlamethrower);
	if (a_projectile.IsCone())
		InstallProjectileHooks<RE::ConeProjectile>(a_events, Type::kCone);
	if (a_projectile.IsBarrier())
		InstallProjectileHooks<RE::BarrierProjectile>(a_events, Type::kBarrier);
	if (a_projectile.IsArrow())
		InstallProjectileHooks<RE::ArrowProjectile>(a_events, Type::kArrow);
}

void ProjectileHookDispatcher::DispatchImpact(RE::Projectile& a_projectile,
	const RE::NiPoint3& a_position, const RE::NiPoint3& a_velocity)
{
	std::shared_lock lock(observersMutex);
	for (std::size_t index = 0; index < impactObserverCount; ++index) {
		const auto& registration = impactObservers[index];
		registration.observer(registration.owner, a_projectile, a_position, a_velocity);
	}
}

void ProjectileHookDispatcher::DispatchMotion(RE::Projectile& a_projectile, float a_deltaTime)
{
	std::shared_lock lock(observersMutex);
	for (std::size_t index = 0; index < motionObserverCount; ++index) {
		const auto& registration = motionObservers[index];
		registration.observer(registration.owner, a_projectile, a_deltaTime);
	}
}
