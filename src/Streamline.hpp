#pragma once

#if F4R_HAS_DLSS

namespace F4R_Upscaling
{
	using PFun_slSetTagForFrame = sl::Result(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);

	struct Streamline
	{
		bool initialized = false;
		bool featureDLSS = false;
		bool featureReflex = false;
		bool nvidiaAdapter = false;

		HMODULE interposer = nullptr;

		sl::FrameToken* frameToken = nullptr;
		uint64_t frameTokenFrame = UINT64_MAX;
		sl::ViewportHandle viewport = sl::ViewportHandle(0);

		bool reflexOptionsValid = false;
		sl::ReflexMode reflexMode = sl::ReflexMode::eOff;
		uint32_t reflexFrameLimitUs = 0;
		uint32_t lastReflexFrame = UINT32_MAX;

		PFun_slInit* slInit = nullptr;
		PFun_slShutdown* slShutdown = nullptr;
		PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
		PFun_slIsFeatureLoaded* slIsFeatureLoaded = nullptr;
		PFun_slSetFeatureLoaded* slSetFeatureLoaded = nullptr;
		PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
		PFun_slAllocateResources* slAllocateResources = nullptr;
		PFun_slFreeResources* slFreeResources = nullptr;
		PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
		PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
		PFun_slGetFeatureVersion* slGetFeatureVersion = nullptr;
		PFun_slUpgradeInterface* slUpgradeInterface = nullptr;
		PFun_slSetConstants* slSetConstants = nullptr;
		PFun_slGetNativeInterface* slGetNativeInterface = nullptr;
		PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
		PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
		PFun_slSetD3DDevice* slSetD3DDevice = nullptr;

		PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
		PFun_slDLSSGetState* slDLSSGetState = nullptr;
		PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;

		PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
		PFun_slReflexSleep* slReflexSleep = nullptr;

		[[nodiscard]] static Streamline& GetSingleton();

		Streamline(const Streamline&) = delete;
		Streamline& operator=(const Streamline&) = delete;

		void LoadInterposer();
		void Initialize();
		void CheckFeatures(IDXGIAdapter* a_adapter);
		void PostDevice();

		bool AcquireFrameToken();
		void UpdateLatency();

		void UpdateConstants(float a_jitterX, float a_jitterY);

		void Evaluate(
			ID3D11Resource* a_colorResource,
			ID3D11ShaderResourceView* a_colorSRV,
			ID3D11Resource* a_colorOutResource,
			ID3D11Resource* a_motionVectorsResource,
			float a_jitterX,
			float a_jitterY,
			uint32_t a_renderWidth,
			uint32_t a_renderHeight,
			uint32_t a_qualityMode);

	private:
		Streamline() = default;
	};
}
#endif
