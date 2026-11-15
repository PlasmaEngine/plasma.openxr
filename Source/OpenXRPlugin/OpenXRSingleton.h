#pragma once

#include <Core/Graphics/Camera.h>
#include <Foundation/Configuration/Singleton.h>
#include <Foundation/Threading/AtomicInteger.h>
#include <Foundation/Threading/Mutex.h>
#include <GameEngine/XR/XRInputDevice.h>
#include <GameEngine/XR/XRInterface.h>
#include <OpenXRPlugin/Basics.h>
#include <OpenXRPlugin/OpenXRIncludes.h>
#include <OpenXRPlugin/OpenXRSwapChain.h>
#include <RendererCore/Pipeline/Declarations.h>
#include <RendererCore/Shader/ConstantBufferStorage.h>
#include <RendererFoundation/Descriptors/Descriptors.h>
#include <RendererFoundation/Device/SwapChain.h>
#include <RendererFoundation/Resources/RenderTargetSetup.h>

class plOpenXRInputDevice;
class plOpenXRSpatialAnchors;
class plOpenXRHandTracking;
class plWindowOutputTargetXR;
struct plGameApplicationExecutionEvent;

PL_DEFINE_AS_POD_TYPE(XrViewConfigurationView);
PL_DEFINE_AS_POD_TYPE(XrEnvironmentBlendMode);
PL_DEFINE_AS_POD_TYPE(XrReferenceSpaceType);

class PL_OPENXRPLUGIN_DLL plOpenXR : public plXRInterface
{
  PL_DECLARE_SINGLETON_OF_INTERFACE(plOpenXR, plXRInterface);

public:
  plOpenXR();
  ~plOpenXR();

  XrInstance GetInstance() const { return m_pInstance; }
  uint64_t GetSystemId() const { return m_SystemId; }
  XrSession GetSession() const { return m_pSession; }
  XrViewConfigurationType GetViewType() const { return m_PrimaryViewConfigurationType; }
  bool GetDepthComposition() const;

  /// \brief The graphics API the session binds to. Chosen from the active renderer when the instance is created.
  enum class GraphicsApi : plUInt8
  {
    Vulkan,
    D3D12,
  };
  GraphicsApi GetGraphicsApi() const { return m_GraphicsApi; }

  virtual bool IsHmdPresent() const override;

  virtual plResult Initialize() override;
  virtual void Deinitialize() override;
  virtual bool IsInitialized() const override;

  virtual const plHMDInfo& GetHmdInfo() const override;
  virtual plXRInputDevice& GetXRInput() const override;

  virtual plGALTextureHandle GetCurrentTexture() override;

  virtual plResult TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz = 0.0f) override;

  /// \brief The display time xrWaitFrame predicted for the frame currently being produced.
  ///
  /// Read this instead of m_FrameState from any thread other than the main thread: the main thread overwrites the
  /// whole XrFrameState struct in BeginFrame and PreWaitNextFrame, so a concurrent struct read can tear.
  XrTime GetPredictedDisplayTime() const { return (XrTime)m_iPredictedDisplayTime; }

  void Present();
  void SubmitXrFrame();
  void PreWaitNextFrame();
  void EndFrame();

  virtual plUniquePtr<plActor> CreateActor(plView* pView, plGALMSAASampleCount::Enum msaaCount = plGALMSAASampleCount::None,
    plUniquePtr<plWindowBase> pCompanionWindow = nullptr, plUniquePtr<plWindowOutputTargetGAL> pCompanionWindowOutput = nullptr) override;
  virtual void OnActorDestroyed() override;
  virtual bool SupportsCompanionView() override;

  virtual plXRSessionFocus::Enum GetSessionFocus() const override { return m_SessionFocus; }

  virtual void GetFrameStats(plXRFrameStats& out_stats) const override;

  virtual plResult RequestDisplayRefreshRate(float fRefreshRateHz) override;

  virtual bool SupportsCompositionLayerType(plXRCompositionLayerType::Enum type) const override;
  virtual plXRCompositionLayerHandle AddCompositionLayer(const plXRCompositionLayerDesc& desc) override;
  virtual plResult UpdateCompositionLayer(plXRCompositionLayerHandle hLayer, const plXRCompositionLayerDesc& desc) override;
  virtual void RemoveCompositionLayer(plXRCompositionLayerHandle hLayer) override;

  virtual plResult SetPassthroughEnabled(bool bEnabled) override;
  virtual bool IsPassthroughActive() const override { return m_bPassthroughActive; }

  XrSpace GetBaseSpace() const;

private:
  /// \brief Creates the process-lifetime XrInstance, the input device and the event subscription, if not done yet.
  XrResult CreateInstance();
  XrResult SelectExtensions(plHybridArray<const char*, 6>& extensions);
  XrResult SelectLayers(plHybridArray<const char*, 6>& layers);
  XrResult InitSystem();
  void DeinitSystem();
  XrResult InitSession();
  void DeinitSession();
  /// \brief Creates everything that depends on a live XrSession: swap chain, eye render target size, view wiring.
  ///
  /// Split out of CreateActor so a session restart can rebuild it without recreating the actor.
  XrResult CreateSessionResources();
  void DestroySessionResources();
  /// \brief Runs a pending session restart. Main thread only - it destroys and recreates GAL resources.
  void RestartSession();
  /// \brief Tears the session down after the runtime asked us to stop. Main thread only.
  void ShutdownSession();
  XrResult InitGraphicsPlugin();
  XrResult InitGraphicsPluginVulkan();
  XrResult InitGraphicsPluginD3D12();
  void DeinitGraphicsPlugin();
  XrResult InitDebugMessenger();
  void DeinitInitDebugMessenger();

  void GameApplicationEventHandler(const plGameApplicationExecutionEvent& e);
  void GALDeviceEventHandler(const plGALDeviceEvent& e);

  void BeforeUpdatePlugins();
  /// \brief Drains the runtime's event queue and runs the session state machine. Main thread only, at BeginAppTick.
  void PollEvents();
  void SetSessionFocus(plXRSessionFocus::Enum focus);
  void UpdatePoses();
  void UpdateCamera();
  void BeginFrame();

  void SetStageSpace(plXRStageSpace::Enum space);
  void SetHMDCamera(plCamera* pCamera);

  /// \brief Reads the display refresh rates (XR_FB_display_refresh_rate) into m_Info. Main thread, session start.
  void InitDisplayRefreshRate();
  /// \brief Sends a request held by RequestDisplayRefreshRate once the session is running and focused. BeginAppTick.
  void ApplyPendingDisplayRefreshRate();

  /// \brief Fetches the hidden-area mesh of both eyes (XR_KHR_visibility_mask) and hands it to plXRVisibilityMask.
  ///
  /// Main thread, BeginAppTick. Needs the FOV of a successful xrLocateViews, so it waits for the first frame with
  /// valid views.
  void UpdateVisibilityMask();

  /// \brief Creates the XR_FB_passthrough objects, paused. Main thread, InitSession.
  void InitPassthrough();
  void DeinitPassthrough();
  /// \brief Applies m_bPassthroughRequested to the session: starts or pauses FB passthrough, or switches the blend
  /// mode to alpha blend. Broadcasts PassthroughStateChanged when the state changes.
  void ApplyPassthroughState();
  void SetPassthroughActive(bool bActive);

  struct CompositionLayer;
  XrResult CreateCompositionLayerSwapchain(CompositionLayer& layer, plUInt32 uiWidth, plUInt32 uiHeight, plGALResourceFormat::Enum sourceFormat);
  /// \brief Queues the layer's GAL wrappers and XrSwapchain for destruction once the GPU is done with the frame.
  void ReleaseCompositionLayerResources(CompositionLayer& layer);
  void DestroyDeadCompositionLayerSwapchains(bool bForce);
  /// \brief Acquires each layer's swapchain image and records the copy of its source texture. Main thread, at
  /// BeforeEndFrame, so the copy is submitted with the frame and the images are released in SubmitXrFrame.
  void CopyCompositionLayers();
  void ReleaseCompositionLayerImages();
  void DestroyAllCompositionLayers();

  plWorld* GetWorld();

public:
  static XrPosef ConvertTransform(const plTransform& tr);
  static XrQuaternionf ConvertOrientation(const plQuat& q);
  static XrVector3f ConvertPosition(const plVec3& vPos);
  static plQuat ConvertOrientation(const XrQuaternionf& q);
  static plVec3 ConvertPosition(const XrVector3f& pos);
  static plMat4 ConvertPoseToMatrix(const XrPosef& pose);
  static plGALResourceFormat::Enum ConvertTextureFormat(int64_t format, GraphicsApi api);

private:
  friend class plOpenXRInputDevice;
  friend class plOpenXRSpatialAnchors;
  friend class plOpenXRHandTracking;
  friend class plOpenXRRemoting;
  friend class plGALOpenXRSwapChain;

  struct Extensions
  {
    bool m_bValidation = false;
    bool m_bDebugUtils = false;
    PFN_xrCreateDebugUtilsMessengerEXT pfn_xrCreateDebugUtilsMessengerEXT;
    PFN_xrDestroyDebugUtilsMessengerEXT pfn_xrDestroyDebugUtilsMessengerEXT;

    bool m_bVulkan = false;
    PFN_xrGetVulkanInstanceExtensionsKHR pfn_xrGetVulkanInstanceExtensionsKHR;
    PFN_xrGetVulkanDeviceExtensionsKHR pfn_xrGetVulkanDeviceExtensionsKHR;
    PFN_xrGetVulkanGraphicsDeviceKHR pfn_xrGetVulkanGraphicsDeviceKHR;
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn_xrGetVulkanGraphicsRequirementsKHR;

    // XR_KHR_vulkan_enable2 extension (preferred for better GPU synchronization)
    bool m_bVulkan2 = false;
    PFN_xrCreateVulkanInstanceKHR pfn_xrCreateVulkanInstanceKHR;
    PFN_xrCreateVulkanDeviceKHR pfn_xrCreateVulkanDeviceKHR;
    PFN_xrGetVulkanGraphicsDevice2KHR pfn_xrGetVulkanGraphicsDevice2KHR;
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn_xrGetVulkanGraphicsRequirements2KHR;

    // XR_KHR_D3D12_enable extension
    bool m_bD3D12 = false;
    PFN_xrGetD3D12GraphicsRequirementsKHR pfn_xrGetD3D12GraphicsRequirementsKHR = nullptr;

    bool m_bDepthComposition = false;

    bool m_bUnboundedReferenceSpace = false;

    bool m_bSpatialAnchor = false;
    PFN_xrCreateSpatialAnchorMSFT pfn_xrCreateSpatialAnchorMSFT;
    PFN_xrCreateSpatialAnchorSpaceMSFT pfn_xrCreateSpatialAnchorSpaceMSFT;
    PFN_xrDestroySpatialAnchorMSFT pfn_xrDestroySpatialAnchorMSFT;

    bool m_bHandInteraction = false;

    // Interaction-profile-only extensions: they add no entry points, they just make the runtime accept the profile's
    // paths in xrSuggestInteractionProfileBindings.
    bool m_bHandInteractionExt = false;
    bool m_bTouchControllerPro = false;
    bool m_bViveCosmosController = false;
    bool m_bViveFocus3Controller = false;
    bool m_bByteDanceController = false;

    // XR_KHR_binding_modification (cvar XR.BindingModification) and XR_EXT_dpad_binding (cvar XR.DpadBinding, needs
    // binding modification), for action map paths ending in /dpad_*. XR_EXT_palm_pose (cvar XR.PalmPose). None of them
    // adds entry points.
    bool m_bBindingModification = false;
    bool m_bDpadBinding = false;
    bool m_bPalmPose = false;

    bool m_bHandTracking = false;
    PFN_xrCreateHandTrackerEXT pfn_xrCreateHandTrackerEXT;
    PFN_xrDestroyHandTrackerEXT pfn_xrDestroyHandTrackerEXT;
    PFN_xrLocateHandJointsEXT pfn_xrLocateHandJointsEXT;

    bool m_bLocalFloor = false;

    bool m_bHolographicWindowAttachment = false;

    // XR_EXT_performance_settings extension
    bool m_bPerformanceSettings = false;
    PFN_xrPerfSettingsSetPerformanceLevelEXT pfn_xrPerfSettingsSetPerformanceLevelEXT = nullptr;

    // XR_KHR_locate_spaces extension (batched space queries for better performance)
    bool m_bLocateSpaces = false;
    PFN_xrLocateSpacesKHR pfn_xrLocateSpacesKHR = nullptr;

    // XR_KHR_visibility_mask (cvar XR.VisibilityMask)
    bool m_bVisibilityMask = false;
    PFN_xrGetVisibilityMaskKHR pfn_xrGetVisibilityMaskKHR = nullptr;

    // XR_FB_display_refresh_rate (cvar XR.DisplayRefreshRate)
    bool m_bDisplayRefreshRate = false;
    PFN_xrEnumerateDisplayRefreshRatesFB pfn_xrEnumerateDisplayRefreshRatesFB = nullptr;
    PFN_xrGetDisplayRefreshRateFB pfn_xrGetDisplayRefreshRateFB = nullptr;
    PFN_xrRequestDisplayRefreshRateFB pfn_xrRequestDisplayRefreshRateFB = nullptr;

    // XR_EXT_eye_gaze_interaction (cvar XR.EyeGaze). Interaction-profile-only, no entry points.
    bool m_bEyeGaze = false;

    // XR_KHR_composition_layer_cylinder (cvar XR.CylinderLayers) and XR_KHR_composition_layer_color_scale_bias
    // (cvar XR.CompositionLayers, for the layer fade). Neither adds entry points.
    bool m_bCompositionLayerCylinder = false;
    bool m_bCompositionLayerColorScaleBias = false;

    // XR_FB_passthrough (cvar XR.Passthrough)
    bool m_bPassthroughFB = false;
    PFN_xrCreatePassthroughFB pfn_xrCreatePassthroughFB = nullptr;
    PFN_xrDestroyPassthroughFB pfn_xrDestroyPassthroughFB = nullptr;
    PFN_xrPassthroughStartFB pfn_xrPassthroughStartFB = nullptr;
    PFN_xrPassthroughPauseFB pfn_xrPassthroughPauseFB = nullptr;
    PFN_xrCreatePassthroughLayerFB pfn_xrCreatePassthroughLayerFB = nullptr;
    PFN_xrDestroyPassthroughLayerFB pfn_xrDestroyPassthroughLayerFB = nullptr;
    PFN_xrPassthroughLayerPauseFB pfn_xrPassthroughLayerPauseFB = nullptr;
    PFN_xrPassthroughLayerResumeFB pfn_xrPassthroughLayerResumeFB = nullptr;

    // XR_MSFT_controller_model and XR_FB_render_model (cvar XR.RenderModels). Called by the input device on its thread.
    bool m_bControllerModelMSFT = false;
    PFN_xrGetControllerModelKeyMSFT pfn_xrGetControllerModelKeyMSFT = nullptr;
    PFN_xrLoadControllerModelMSFT pfn_xrLoadControllerModelMSFT = nullptr;
    PFN_xrGetControllerModelPropertiesMSFT pfn_xrGetControllerModelPropertiesMSFT = nullptr;
    PFN_xrGetControllerModelStateMSFT pfn_xrGetControllerModelStateMSFT = nullptr;

    bool m_bRenderModelFB = false;
    PFN_xrEnumerateRenderModelPathsFB pfn_xrEnumerateRenderModelPathsFB = nullptr;
    PFN_xrGetRenderModelPropertiesFB pfn_xrGetRenderModelPropertiesFB = nullptr;
    PFN_xrLoadRenderModelFB pfn_xrLoadRenderModelFB = nullptr;

    bool m_bRemoting = false;
    bool m_ViveTracker = false;
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
    PFN_xrRemotingSetContextPropertiesMSFT pfn_xrRemotingSetContextPropertiesMSFT;
    PFN_xrRemotingConnectMSFT pfn_xrRemotingConnectMSFT;
    PFN_xrRemotingDisconnectMSFT pfn_xrRemotingDisconnectMSFT;
    PFN_xrRemotingGetConnectionStateMSFT pfn_xrRemotingGetConnectionStateMSFT;
#endif
  };

  // Process-lifetime: SteamVR's vrclient faults on a second xrCreateInstance in one process, so Deinitialize keeps
  // the instance and only the destructor releases it.
  XrInstance m_pInstance = XR_NULL_HANDLE;
  Extensions m_Extensions;
  // The extensions m_pInstance was created with.
  plHybridArray<plString, 32> m_EnabledExtensions;

  /// \brief Whether the instance was created with the named extension.
  bool IsExtensionEnabled(plStringView sExtension) const;
  // Fixed with the instance, because the graphics binding extension is enabled by xrCreateInstance.
  GraphicsApi m_GraphicsApi = GraphicsApi::Vulkan;
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  plUniquePtr<class plOpenXRRemoting> m_pRemoting;
#endif

  // System
  uint64_t m_SystemId = XR_NULL_SYSTEM_ID;

  // Session
  XrSession m_pSession = XR_NULL_HANDLE;
  XrSpace m_pSceneSpace = XR_NULL_HANDLE;
  XrSpace m_pLocalSpace = XR_NULL_HANDLE;
  // VIEW reference space, for head-locked composition layers.
  XrSpace m_pViewSpace = XR_NULL_HANDLE;
  plEventSubscriptionID m_ExecutionEventsId = 0;
  plEventSubscriptionID m_BeginRenderEventsId = 0;
  plEventSubscriptionID m_GALdeviceEventsId = 0;
  XrDebugUtilsMessengerEXT m_pDebugMessenger = XR_NULL_HANDLE;

  // Graphics plugin
  XrEnvironmentBlendMode m_BlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  XrGraphicsBindingVulkanKHR m_XrGraphicsBindingVulkan{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  XrGraphicsBindingD3D12KHR m_XrGraphicsBindingD3D12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  XrFormFactor m_FormFactor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
  XrViewConfigurationType m_PrimaryViewConfigurationType{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};

  plGALSwapChainHandle m_hSwapChain;
  // Kept so a session restart can recreate the swap chain with the sample count the actor was created with.
  plGALMSAASampleCount::Enum m_MsaaCount = plGALMSAASampleCount::None;

  // Views
  XrViewState m_ViewState{XR_TYPE_VIEW_STATE};
  XrView m_Views[2];
  bool m_bProjectionChanged = true;
  XrCompositionLayerProjection m_Layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView m_ProjectionLayerViews[2];
  XrCompositionLayerDepthInfoKHR m_DepthLayerViews[2];

  // State. Main thread only: PollEvents runs the state machine at BeginAppTick, BeginFrame/EndFrame read it.
  bool m_bSessionRunning = false;
  bool m_bExitRenderLoop = false;
  bool m_bRequestRestart = false;
  bool m_bRenderInProgress = false;
  XrSessionState m_SessionState{XR_SESSION_STATE_UNKNOWN};
  plXRSessionFocus::Enum m_SessionFocus = plXRSessionFocus::Hidden;
  // The runtime told us the instance is going away, so it must be destroyed rather than reused.
  bool m_bInstanceLost = false;

  XrFrameWaitInfo m_FrameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState m_FrameState{XR_TYPE_FRAME_STATE};
  XrFrameBeginInfo m_FrameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
  // Published copy of m_FrameState.predictedDisplayTime for the input thread; see GetPredictedDisplayTime.
  plAtomicInteger64 m_iPredictedDisplayTime{0};

  // Pre-wait state for pipelined frame timing
  XrResult m_PreWaitResult = XR_SUCCESS;
  bool m_bPreWaitComplete = false;

  // XR interface state
  plHMDInfo m_Info;
  mutable plUniquePtr<plOpenXRInputDevice> m_pInput;
  plUniquePtr<plOpenXRSpatialAnchors> m_pAnchors;
  plUniquePtr<plOpenXRHandTracking> m_pHandTracking;

  plCamera* m_pCameraToSynchronize = nullptr;
  plEnum<plXRStageSpace> m_StageSpace;
  plUInt32 m_uiSettingsModificationCounter = 0;
  plViewHandle m_hView;

  // Cached stage transform to prevent jitter from simulation/render timing mismatch. Written on the update task
  // (BeforeUpdatePlugins), read on the main thread (UpdateCamera), which can overlap.
  plMutex m_StageTransformMutex;
  plTransform m_CachedStageTransform;
  bool m_bStageTransformCached = false;

  plWindowOutputTargetXR* m_pCompanion = nullptr;
  bool m_bCompanionFrameBegun = false; ///< BeginFrame started the companion view this frame (not when shouldRender is false).

  // Frame statistics (GetFrameStats). Written on the main thread, atomics so a stats overlay on another thread reads
  // whole values.
  plAtomicInteger64 m_iFramesSubmitted{0};
  plAtomicInteger64 m_iFramesNotRendered{0};
  plAtomicInteger64 m_iFramesDiscarded{0};
  plAtomicInteger64 m_iPredictedDisplayPeriod{0};
  plAtomicInteger32 m_iRenderedEyeWidth{0};
  plAtomicInteger32 m_iRenderedEyeHeight{0};

  // Display refresh rate (XR_FB_display_refresh_rate). Main thread only.
  float m_fRequestedRefreshRate = 0.0f; ///< The last rate RequestDisplayRefreshRate accepted, 0 = runtime default.
  bool m_bHasRefreshRateRequest = false; ///< RequestDisplayRefreshRate was called at least once.
  bool m_bRefreshRateRequestPending = false; ///< m_fRequestedRefreshRate still has to be sent to the runtime.

  // Visibility mask (XR_KHR_visibility_mask). Main thread only.
  bool m_bVisibilityMaskDirty = false; ///< Fetch the mask again once the views have a valid FOV.
  bool m_bVisibilityMaskSet = false;   ///< plXRVisibilityMask holds a mask from this session.
  bool m_bViewFovValid = false;        ///< m_Views holds the FOV of a successful xrLocateViews, not the fallback.

  // Passthrough. Main thread only.
  XrPassthroughFB m_pPassthrough = XR_NULL_HANDLE;
  XrPassthroughLayerFB m_pPassthroughLayer = XR_NULL_HANDLE;
  bool m_bPassthroughRequested = false; ///< SetPassthroughEnabled's last value, kept across session restarts.
  bool m_bPassthroughActive = false;
  bool m_bPassthroughUsesFB = false;    ///< Active through the XR_FB_passthrough layer rather than alpha-blend composition.
  bool m_bAlphaBlendAvailable = false;  ///< The system enumerates XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND.
  bool m_bFBPassthroughSupported = false; ///< XR_FB_passthrough is enabled and the system reports passthrough.
  XrEnvironmentBlendMode m_ConfiguredBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; ///< m_BlendMode without passthrough.

  struct CompositionLayer
  {
    plXRCompositionLayerHandle m_hHandle = plInvalidXRCompositionLayer;
    plXRCompositionLayerDesc m_Desc;
    XrSwapchain m_Swapchain = XR_NULL_HANDLE;
    plUInt32 m_uiWidth = 0;
    plUInt32 m_uiHeight = 0;
    plGALResourceFormat::Enum m_Format = plGALResourceFormat::Invalid;
    plHybridArray<plGALTextureHandle, 3> m_Images;
    plHybridArray<XrSwapchainImageVulkanKHR, 3> m_ImagesVulkan;
    plHybridArray<XrSwapchainImageD3D12KHR, 3> m_ImagesD3D12;
    bool m_bAcquired = false;   ///< An image is acquired and its copy recorded; SubmitXrFrame releases it.
    bool m_bHasContent = false; ///< At least one image was released with the source texture in it.
  };

  struct DeadLayerSwapchain
  {
    XrSwapchain m_Swapchain = XR_NULL_HANDLE;
    plUInt64 m_uiFrame = 0; ///< plGALDevice::GetCurrentFrame when it was retired.
  };

  // Composition layers. Main thread only. Dropped when the session ends; their handles then report failure.
  plDynamicArray<CompositionLayer> m_CompositionLayers;
  plDynamicArray<DeadLayerSwapchain> m_DeadLayerSwapchains;
  plXRCompositionLayerHandle m_uiNextLayerHandle = 1;
  bool m_bCompositionLayersEnabled = false; ///< XR.CompositionLayers, latched at session start.
  plHybridArray<int64_t, 16> m_SwapchainFormats; ///< The session's swapchain formats, queried with the first layer.
};
