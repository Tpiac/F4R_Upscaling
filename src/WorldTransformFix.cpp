#include "PCH.hpp"
#include "WorldTransformFix.hpp"

#include <atomic>

#include <Detours.h>

namespace F4R_Upscaling
{
	namespace WorldTransformFix
	{
		namespace
		{
			using ShaderFlag = RE::BSShaderProperty::EShaderPropertyFlags;

			constexpr std::uint32_t kGetRenderPassesIndex = 0x2B;

			std::atomic<bool> g_sceneSweepPending{ false };
			bool g_loadingMenuWasOpen = false;

			[[nodiscard]] bool IsLoadingMenuOpen()
			{
				static const RE::BSFixedString loadingMenu("LoadingMenu");

				auto* ui = RE::UI::GetSingleton();
				if (!ui) return false;

				return ui->IsMenuOpen(loadingMenu).value_or(false);
			}

			using GetRenderPassesFunc = RE::BSShaderProperty::RenderPassArray* (*)(
				RE::BSShaderProperty* a_property,
				RE::BSGeometry* a_geometry,
				std::uint32_t a_renderMode,
				RE::BSShaderAccumulator* a_accumulator);
			GetRenderPassesFunc g_originalGetRenderPasses = nullptr;

			RE::BSShaderProperty::RenderPassArray* Hook_GetRenderPasses(
				RE::BSShaderProperty* a_property,
				RE::BSGeometry* a_geometry,
				std::uint32_t a_renderMode,
				RE::BSShaderAccumulator* a_accumulator)
			{
				if (a_property && a_geometry &&
					a_property->flags.any(
						ShaderFlag::kLODObjects,
						ShaderFlag::kLODLandscape,
						ShaderFlag::kLODLandBlend,
						ShaderFlag::kMultiTextureLandscape)) {
					a_geometry->previousWorld = a_geometry->world;
				}

				return g_originalGetRenderPasses(a_property, a_geometry, a_renderMode, a_accumulator);
			}

			void SweepObject(RE::NiAVObject* a_object)
			{
				if (!a_object) return;

				if (auto* node = a_object->AsNode()) {
					for (auto& child : node->children) {
						SweepObject(child.get());
					}
				}

				a_object->previousWorld = a_object->world;
			}
		}

		void InstallHooks()
		{
			const auto vtableAddress = REL::Relocation{ RE::VTABLE::BSLightingShaderProperty[0] }.GetAddress();

			const auto result = Detours::X64::DetourVTable(
				vtableAddress,
				reinterpret_cast<std::uintptr_t>(&Hook_GetRenderPasses),
				kGetRenderPassesIndex);
			g_originalGetRenderPasses = reinterpret_cast<GetRenderPassesFunc>(result);

			if (!result) {
				REX::LogWarning("WorldTransformFix: GetRenderPasses hook FAILED");
			}
		}

		void Update()
		{
			const bool loadingMenuOpen = IsLoadingMenuOpen();
			if (g_loadingMenuWasOpen && !loadingMenuOpen) {
				RequestSceneSweep();
			}
			g_loadingMenuWasOpen = loadingMenuOpen;

			if (!g_sceneSweepPending.load(std::memory_order_relaxed)) return;
			if (loadingMenuOpen) return;

			g_sceneSweepPending.store(false, std::memory_order_relaxed);

			auto& root = RE::Main::GetWorldRootNode();
			SweepObject(root.get());
		}

		void RequestSceneSweep()
		{
			g_sceneSweepPending.store(true, std::memory_order_relaxed);
		}
	}
}
