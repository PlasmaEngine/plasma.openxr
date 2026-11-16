#include <OpenXRPlugin/OpenXRPluginPCH.h>

#include <GameEngine/GameApplication/GameApplication.h>
#include <OpenXRPlugin/OpenXRDeclarations.h>
#include <OpenXRPlugin/OpenXRHandTracking.h>
#include <OpenXRPlugin/OpenXRInputDevice.h>
#include <OpenXRPlugin/OpenXRRemoting.h>
#include <OpenXRPlugin/OpenXRSingleton.h>
#include <OpenXRPlugin/OpenXRSpatialAnchors.h>
#include <OpenXRPlugin/OpenXRSwapChain.h>
#include <RendererCore/Components/CameraComponent.h>
#include <RendererCore/Pipeline/View.h>
#include <RendererCore/Pipeline/XRScreenEffects.h>
#include <RendererCore/Pipeline/XRVisibilityMask.h>
#include <RendererFoundation/CommandEncoder/CommandEncoder.h>
#include <RendererFoundation/Resources/Texture.h>
#include <RendererCore/RenderWorld/RenderWorld.h>
#include <RendererCore/Textures/TextureUtils.h>
#include <Texture/Image/Formats/ImageFormatMappings.h>

#include <Core/ActorSystem/Actor.h>
#include <Core/World/World.h>
#include <Foundation/Configuration/CVar.h>
#include <GameEngine/Configuration/XRConfig.h>
#include <GameEngine/XR/StageSpaceComponent.h>
#include <GameEngine/XR/XRLateLatch.h>
#include <GameEngine/XR/XRWindow.h>

#include <RendererDX12/Device/DeviceDX12.h>
#include <RendererVulkan/Device/DeviceVulkan.h>

#include <algorithm>
#include <vector>

PL_DEFINE_AS_POD_TYPE(XrCompositionLayerQuad);
PL_DEFINE_AS_POD_TYPE(XrCompositionLayerCylinderKHR);
PL_DEFINE_AS_POD_TYPE(XrCompositionLayerColorScaleBiasKHR);
PL_DEFINE_AS_POD_TYPE(XrVector2f);

static_assert(plGALMSAASampleCount::None == 1);
static_assert(plGALMSAASampleCount::TwoSamples == 2);
static_assert(plGALMSAASampleCount::FourSamples == 4);
static_assert(plGALMSAASampleCount::EightSamples == 8);

// See the comment at the AddExtIfSupported block in SelectExtensions for why these default to off.
plCVarBool cvar_XrExtendedInteractionProfiles("XR.ExtendedInteractionProfiles", false, plCVarFlags::Save, "Request the interaction-profile extensions for Quest Pro, Vive Cosmos, Vive Focus 3, Pico and hand interaction.");
plCVarBool cvar_XrDepthComposition("XR.DepthComposition", false, plCVarFlags::Save, "Submit a depth layer so the runtime can do depth-aware late reprojection.");
plCVarBool cvar_XrHandTracking("XR.HandTracking", false, plCVarFlags::Save, "Request the hand-tracking extensions (XR_EXT_hand_tracking, XR_MSFT_hand_interaction). Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrLocalFloor("XR.LocalFloor", false, plCVarFlags::Save, "Request XR_EXT_local_floor, used as the standing space on runtimes without a STAGE space. Takes effect when the OpenXR instance is created.");

// Every cvar below requests an extension, so each one defaults to off for the reason given at the AddExtIfSupported
// block in SelectExtensions. The instance lives for the whole process, so they take effect at the next launch.
plCVarBool cvar_XrVisibilityMask("XR.VisibilityMask", false, plCVarFlags::Save, "Request XR_KHR_visibility_mask and hand the lens hidden-area mesh to the renderer (Rendering.XR.VisibilityMask). Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrDisplayRefreshRate("XR.DisplayRefreshRate", false, plCVarFlags::Save, "Request XR_FB_display_refresh_rate, so the display refresh rate can be read and changed. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrEyeGaze("XR.EyeGaze", false, plCVarFlags::Save, "Request XR_EXT_eye_gaze_interaction and report the gaze as the EyeGaze device on eye-tracked headsets. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrCompositionLayers("XR.CompositionLayers", false, plCVarFlags::Save, "Allow quad composition layers (AddCompositionLayer) and request XR_KHR_composition_layer_color_scale_bias for their fade. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrCylinderLayers("XR.CylinderLayers", false, plCVarFlags::Save, "Request XR_KHR_composition_layer_cylinder for cylinder composition layers. Needs XR.CompositionLayers. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrPassthrough("XR.Passthrough", false, plCVarFlags::Save, "Request XR_FB_passthrough for mixed reality on Quest-class headsets. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrRenderModels("XR.RenderModels", false, plCVarFlags::Save, "Request XR_MSFT_controller_model and XR_FB_render_model, so the runtime's controller models (plXRControllerModelComponent) are available. Takes effect when the OpenXR instance is created.");
// The input device also latches these three at session start, so turning one off takes effect at the next session.
plCVarBool cvar_XrBindingModification("XR.BindingModification", false, plCVarFlags::Save, "Request XR_KHR_binding_modification, which XR.DpadBinding needs. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrDpadBinding("XR.DpadBinding", false, plCVarFlags::Save, "Request XR_EXT_dpad_binding, so input action map XR paths ending in /dpad_* split a stick or trackpad into four buttons. Needs XR.BindingModification. Takes effect when the OpenXR instance is created.");
plCVarBool cvar_XrPalmPose("XR.PalmPose", false, plCVarFlags::Save, "Request XR_EXT_palm_pose and report the palm as a third pose per hand (plXRPoseLocation::Palm). Takes effect when the OpenXR instance is created.");

// Logs the OpenXR API layers the loader will insert, so a fault inside xrCreateInstance can be attributed to a vendor
// layer rather than to the application.
static void LogAvailableApiLayers()
{
  plUInt32 uiLayerCount = 0;
  if (xrEnumerateApiLayerProperties(0, &uiLayerCount, nullptr) != XR_SUCCESS || uiLayerCount == 0)
  {
    plLog::Info("OpenXR: No API layers are registered.");
    return;
  }

  std::vector<XrApiLayerProperties> layers(uiLayerCount, {XR_TYPE_API_LAYER_PROPERTIES});
  if (xrEnumerateApiLayerProperties(uiLayerCount, &uiLayerCount, layers.data()) != XR_SUCCESS)
    return;

  for (const XrApiLayerProperties& layer : layers)
  {
    plLog::Info("OpenXR: API layer present: {} (spec {}.{}, impl {})", layer.layerName,
      XR_VERSION_MAJOR(layer.specVersion), XR_VERSION_MINOR(layer.specVersion), layer.layerVersion);
  }
}

PL_IMPLEMENT_SINGLETON(plOpenXR);

static plOpenXR g_OpenXRSingleton;

XrBool32 XRAPI_CALL xrDebugCallback(XrDebugUtilsMessageSeverityFlagsEXT messageSeverity, XrDebugUtilsMessageTypeFlagsEXT messageTypes, const XrDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
  switch (messageSeverity)
  {
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
      plLog::Debug("XR: {}", pCallbackData->message);
      break;
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
      plLog::Info("XR: {}", pCallbackData->message);
      break;
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
      plLog::Warning("XR: {}", pCallbackData->message);
      break;
    case XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
      plLog::Error("XR: {}", pCallbackData->message);
      break;
    default:
      break;
  }
  // Only layers are allowed to return true here.
  return XR_FALSE;
}

plOpenXR::plOpenXR()
  : m_SingletonRegistrar(this)
{
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  m_pRemoting = PL_DEFAULT_NEW(plOpenXRRemoting, this);
#endif
}

plOpenXR::~plOpenXR()
{
  // Releases the process-lifetime instance during plugin unload, while the loader is still mapped.
  if (m_ExecutionEventsId != 0 && plGameApplicationBase::GetGameApplicationBaseInstance() != nullptr)
  {
    plGameApplicationBase::GetGameApplicationBaseInstance()->m_ExecutionEvents.RemoveEventHandler(m_ExecutionEventsId);
  }

  m_pInput = nullptr;

  if (m_pInstance)
  {
    xrDestroyInstance(m_pInstance);
    m_pInstance = XR_NULL_HANDLE;
  }
}

bool plOpenXR::GetDepthComposition() const
{
  return m_Extensions.m_bDepthComposition;
}

bool plOpenXR::IsHmdPresent() const
{
  // The interface documents this as callable before Initialize. The probe creates the real, process-lifetime instance
  // rather than a throwaway one: a second xrCreateInstance in the same process faults inside SteamVR when the Vive
  // implicit API layers are installed.
  if (const_cast<plOpenXR*>(this)->CreateInstance() != XR_SUCCESS)
    return false;

  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XrFormFactor::XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  uint64_t systemId = XR_NULL_SYSTEM_ID;
  return xrGetSystem(m_pInstance, &systemInfo, &systemId) == XrResult::XR_SUCCESS;
}

bool plOpenXR::IsExtensionEnabled(plStringView sExtension) const
{
  for (const plString& sEnabled : m_EnabledExtensions)
  {
    if (sEnabled == sExtension)
      return true;
  }

  return false;
}

XrResult plOpenXR::SelectExtensions(plHybridArray<const char*, 6>& extensions)
{
  // Fetch the list of extensions supported by the runtime.
  plUInt32 extensionCount = 0;
  if (const XrResult res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr); XR_FAILED_RESULT(res))
  {
    // The usual cause is that no OpenXR runtime is installed or active, which is not an error for a flat-screen run.
    plLog::Warning("OpenXR: No runtime is available ({}).", res);
    return res;
  }
  std::vector<XrExtensionProperties> extensionProperties(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()));

  // Add a specific extension to the list of extensions to be enabled, if it is supported. This is a lambda function that checks
  // if a certain extension has been installed on the target device. If yes, it will be added to the extensions list.
  auto AddExtIfSupported = [&](const char* extensionName, bool& enableFlag) -> XrResult
  {
    auto it = std::find_if(begin(extensionProperties), end(extensionProperties), [&](const XrExtensionProperties& prop)
      { return plStringUtils::IsEqual(prop.extensionName, extensionName); });
    if (it != end(extensionProperties))
    {
      extensions.PushBack(extensionName);
      enableFlag = true;
      return XR_SUCCESS;
    }
    enableFlag = false;
    return XR_ERROR_EXTENSION_NOT_PRESENT;
  };
  // The graphics binding extension must match the renderer. The instance lives for the whole process, so this choice does too.
  const plStringView sRenderer = plGALDevice::HasDefaultDevice() ? plGALDevice::GetDefaultDevice()->GetRenderer() : plGameApplication::GetActiveRenderer();
  m_GraphicsApi = sRenderer.IsEqual_NoCase("DX12") ? GraphicsApi::D3D12 : GraphicsApi::Vulkan;

  if (m_GraphicsApi == GraphicsApi::D3D12)
  {
    if (AddExtIfSupported(XR_KHR_D3D12_ENABLE_EXTENSION_NAME, m_Extensions.m_bD3D12) != XR_SUCCESS)
    {
      plLog::Error("OpenXR: The active runtime does not support XR_KHR_D3D12_enable. Run with '-renderer Vulkan' to use it.");
      return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    plLog::Info("OpenXR: Using XR_KHR_D3D12_enable");
  }
  // Prefer XR_KHR_vulkan_enable2 for better GPU synchronization, fall back to vulkan_enable
  else if (AddExtIfSupported(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, m_Extensions.m_bVulkan2) != XR_SUCCESS)
  {
    // Fall back to vulkan_enable (required so check that it was added)
    XR_SUCCEED_OR_RETURN_LOG(AddExtIfSupported(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, m_Extensions.m_bVulkan));
  }
  else
  {
    plLog::Info("OpenXR: Using XR_KHR_vulkan_enable2 for improved GPU synchronization");
  }

  // Submitting a depth layer lets the runtime do depth-aware late reprojection, which keeps geometry stable when the
  // application misses a frame. Behind a cvar because every enabled extension is also handed to whatever OpenXR API
  // layers are installed, and a layer that mishandles one takes down xrCreateInstance for the whole application.
  m_Extensions.m_bDepthComposition = false;
  if (cvar_XrDepthComposition)
  {
    AddExtIfSupported(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, m_Extensions.m_bDepthComposition);
    if (m_Extensions.m_bDepthComposition)
    {
      plLog::Info("OpenXR: Using XR_KHR_composition_layer_depth for depth-aware reprojection");
    }
  }
  AddExtIfSupported(XR_MSFT_UNBOUNDED_REFERENCE_SPACE_EXTENSION_NAME, m_Extensions.m_bUnboundedReferenceSpace);
  AddExtIfSupported(XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME, m_Extensions.m_bSpatialAnchor);

  // Off by default for the same reason as the interaction profiles below: hand-tracking extensions activate HTC's
  // ViveOpenXRHandTracking implicit layer, which aborts on headsets without hand-tracking cameras.
  m_Extensions.m_bHandTracking = false;
  m_Extensions.m_bHandInteraction = false;
  if (cvar_XrHandTracking)
  {
    AddExtIfSupported(XR_EXT_HAND_TRACKING_EXTENSION_NAME, m_Extensions.m_bHandTracking);
    AddExtIfSupported(XR_MSFT_HAND_INTERACTION_EXTENSION_NAME, m_Extensions.m_bHandInteraction);
  }

  // LOCAL_FLOOR is only the fallback standing space for runtimes without STAGE, see InitSession.
  m_Extensions.m_bLocalFloor = false;
  if (cvar_XrLocalFloor)
  {
    AddExtIfSupported(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME, m_Extensions.m_bLocalFloor);
  }

  // Interaction profiles for hardware beyond the original six. Without the extension enabled the runtime rejects the
  // profile's paths, so the suggested bindings in plOpenXRInputDevice are inert.
  //
  // Off by default, because requesting one of these can activate a vendor OpenXR API layer that then inserts itself
  // into every input call. XR_EXT_hand_interaction in particular brings up HTC's ViveOpenXRHandTracking layer, which
  // aborts inside aristo_interface.dll on a headset that has no hand-tracking cameras - taking the app with it. Only
  // enable this when running hardware that needs one of these profiles.
  if (cvar_XrExtendedInteractionProfiles)
  {
    AddExtIfSupported(XR_EXT_HAND_INTERACTION_EXTENSION_NAME, m_Extensions.m_bHandInteractionExt);
    AddExtIfSupported(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME, m_Extensions.m_bTouchControllerPro);
    AddExtIfSupported(XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME, m_Extensions.m_bViveCosmosController);
    AddExtIfSupported(XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME, m_Extensions.m_bViveFocus3Controller);
    AddExtIfSupported(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, m_Extensions.m_bByteDanceController);
  }

  m_Extensions.m_bVisibilityMask = false;
  if (cvar_XrVisibilityMask)
  {
    AddExtIfSupported(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, m_Extensions.m_bVisibilityMask);
  }

  m_Extensions.m_bDisplayRefreshRate = false;
  if (cvar_XrDisplayRefreshRate)
  {
    AddExtIfSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, m_Extensions.m_bDisplayRefreshRate);
  }

  m_Extensions.m_bEyeGaze = false;
  if (cvar_XrEyeGaze)
  {
    AddExtIfSupported(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, m_Extensions.m_bEyeGaze);
  }

  // Quad layers are core OpenXR; the cvar still gates them, because every extra layer is work for the compositor and
  // a path no headset has run yet.
  m_bCompositionLayersEnabled = cvar_XrCompositionLayers;
  m_Extensions.m_bCompositionLayerColorScaleBias = false;
  m_Extensions.m_bCompositionLayerCylinder = false;
  if (m_bCompositionLayersEnabled)
  {
    AddExtIfSupported(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME, m_Extensions.m_bCompositionLayerColorScaleBias);
    if (cvar_XrCylinderLayers)
    {
      AddExtIfSupported(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME, m_Extensions.m_bCompositionLayerCylinder);
    }
  }

  m_Extensions.m_bPassthroughFB = false;
  if (cvar_XrPassthrough)
  {
    AddExtIfSupported(XR_FB_PASSTHROUGH_EXTENSION_NAME, m_Extensions.m_bPassthroughFB);
  }

  m_Extensions.m_bControllerModelMSFT = false;
  m_Extensions.m_bRenderModelFB = false;
  if (cvar_XrRenderModels)
  {
    AddExtIfSupported(XR_MSFT_CONTROLLER_MODEL_EXTENSION_NAME, m_Extensions.m_bControllerModelMSFT);
    AddExtIfSupported(XR_FB_RENDER_MODEL_EXTENSION_NAME, m_Extensions.m_bRenderModelFB);
  }

  // XR_EXT_dpad_binding is only valid together with XR_KHR_binding_modification, which carries its structs.
  m_Extensions.m_bBindingModification = false;
  m_Extensions.m_bDpadBinding = false;
  if (cvar_XrBindingModification)
  {
    AddExtIfSupported(XR_KHR_BINDING_MODIFICATION_EXTENSION_NAME, m_Extensions.m_bBindingModification);
    if (m_Extensions.m_bBindingModification && cvar_XrDpadBinding)
    {
      AddExtIfSupported(XR_EXT_DPAD_BINDING_EXTENSION_NAME, m_Extensions.m_bDpadBinding);
    }
  }

  m_Extensions.m_bPalmPose = false;
  if (cvar_XrPalmPose)
  {
    AddExtIfSupported(XR_EXT_PALM_POSE_EXTENSION_NAME, m_Extensions.m_bPalmPose);
  }

  // Performance settings extension - allows setting performance hints
  AddExtIfSupported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, m_Extensions.m_bPerformanceSettings);

  // XR_KHR_locate_spaces - batched space location queries for better performance
  AddExtIfSupported(XR_KHR_LOCATE_SPACES_EXTENSION_NAME, m_Extensions.m_bLocateSpaces);
  AddExtIfSupported(XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME, m_Extensions.m_ViveTracker);

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
  AddExtIfSupported(XR_EXT_DEBUG_UTILS_EXTENSION_NAME, m_Extensions.m_bDebugUtils);
#endif

#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  AddExtIfSupported(XR_MSFT_HOLOGRAPHIC_REMOTING_EXTENSION_NAME, m_Extensions.m_bRemoting);
#endif

#ifdef BUILDSYSTEM_ENABLE_OPENXR_PREVIEW_SUPPORT
#endif
  return XR_SUCCESS;
}

XrResult plOpenXR::SelectLayers(plHybridArray<const char*, 6>& layers)
{
  plUInt32 layerCount;
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateApiLayerProperties(0, &layerCount, nullptr));
  std::vector<XrApiLayerProperties> layerProperties(layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateApiLayerProperties(layerCount, &layerCount, layerProperties.data()));

  // Add a specific extension to the list of extensions to be enabled, if it is supported.
  auto AddExtIfSupported = [&](const char* layerName, bool& enableFlag) -> XrResult
  {
    auto it = std::find_if(begin(layerProperties), end(layerProperties), [&](const XrApiLayerProperties& prop)
      { return plStringUtils::IsEqual(prop.layerName, layerName); });
    if (it != end(layerProperties))
    {
      layers.PushBack(layerName);
      enableFlag = true;
      return XR_SUCCESS;
    }
    enableFlag = false;
    return XR_ERROR_EXTENSION_NOT_PRESENT;
  };

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
  AddExtIfSupported("XR_APILAYER_LUNARG_core_validation", m_Extensions.m_bValidation);
#endif

  return XR_SUCCESS;
}

#define PL_GET_INSTANCE_PROC_ADDR(name) (void)xrGetInstanceProcAddr(m_pInstance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&m_Extensions.pfn_##name));

plResult plOpenXR::Initialize()
{
  if (CreateInstance() != XR_SUCCESS)
    return PL_FAILURE;

  // Registered here rather than in CreateInstance, because IsHmdPresent may create the instance before a game
  // application exists. Only the destructor removes it, so a later Initialize must not add a second one.
  if (m_ExecutionEventsId == 0)
  {
    plGameApplicationBase* pApp = plGameApplicationBase::GetGameApplicationBaseInstance();
    if (pApp == nullptr)
    {
      plLog::Error("OpenXR: Initialize needs a game application to drive the session.");
      return PL_FAILURE;
    }
    m_ExecutionEventsId = pApp->m_ExecutionEvents.AddEventHandler(plMakeDelegate(&plOpenXR::GameApplicationEventHandler, this));
  }

  // Either a fresh instance, or one that survived the previous session's Deinitialize, which only releases the system.
  if (m_SystemId == XR_NULL_SYSTEM_ID)
  {
    XrResult res = InitSystem();
    if (res != XR_SUCCESS)
    {
      plLog::Error("OpenXR: Acquiring the system failed: {}", res);
      return PL_FAILURE;
    }
  }

  return PL_SUCCESS;
}

XrResult plOpenXR::CreateInstance()
{
  if (m_pInstance != XR_NULL_HANDLE)
    return XR_SUCCESS;

  // Build out the extensions to enable. Some extensions are required and some are optional.
  plHybridArray<const char*, 6> enabledExtensions;
  if (const XrResult res = SelectExtensions(enabledExtensions); res != XR_SUCCESS)
    return res;

  plHybridArray<const char*, 6> enabledLayers;
  XR_SUCCEED_OR_RETURN_LOG(SelectLayers(enabledLayers));

  // Create the instance with desired extensions.
  XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
  createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.GetCount();
  createInfo.enabledExtensionNames = enabledExtensions.GetData();
  createInfo.enabledApiLayerCount = (uint32_t)enabledLayers.GetCount();
  createInfo.enabledApiLayerNames = enabledLayers.GetData();

  plStringUtils::Copy(createInfo.applicationInfo.applicationName, PL_ARRAY_SIZE(createInfo.applicationInfo.applicationName), plApplication::GetApplicationInstance()->GetApplicationName());
  plStringUtils::Copy(createInfo.applicationInfo.engineName, PL_ARRAY_SIZE(createInfo.applicationInfo.engineName), "plEngine");
  createInfo.applicationInfo.engineVersion = 1;
  // Use OpenXR 1.0 API version for maximum runtime compatibility
  // XR_API_VERSION_1_0 = XR_MAKE_VERSION(1, 0, patch) where patch comes from the SDK version
  createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  createInfo.applicationInfo.applicationVersion = 1;
  // Both lists go through every installed API layer, so record them before the call: if xrCreateInstance faults inside
  // a layer, this is the only record of what it was handed.
  LogAvailableApiLayers();
  for (const char* szExtension : enabledExtensions)
  {
    plLog::Info("OpenXR: Requesting extension {}", szExtension);
  }

  XrResult res = xrCreateInstance(&createInfo, &m_pInstance);
  if (res != XR_SUCCESS)
  {
    plLog::Error("OpenXR: xrCreateInstance failed: {}", res);
    m_pInstance = XR_NULL_HANDLE;
    return res;
  }

  m_EnabledExtensions.Clear();
  for (const char* szExtension : enabledExtensions)
  {
    m_EnabledExtensions.PushBack(szExtension);
  }
  XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
  res = xrGetInstanceProperties(m_pInstance, &instanceProperties);
  if (res != XR_SUCCESS)
  {
    plLog::Error("OpenXR: xrGetInstanceProperties failed: {}", res);
    xrDestroyInstance(m_pInstance);
    m_pInstance = XR_NULL_HANDLE;
    return res;
  }

  plStringBuilder sRuntimeVersion;
  sRuntimeVersion.SetFormat("{}.{}.{}", XR_VERSION_MAJOR(instanceProperties.runtimeVersion), XR_VERSION_MINOR(instanceProperties.runtimeVersion), XR_VERSION_PATCH(instanceProperties.runtimeVersion));
  plStringBuilder sDriver;
  sDriver.SetFormat("{} {}", instanceProperties.runtimeName, sRuntimeVersion);
  m_Info.m_sDeviceDriver = sDriver;
  m_Info.m_sRuntimeName = instanceProperties.runtimeName;
  m_Info.m_uiRuntimeVersion[0] = XR_VERSION_MAJOR(instanceProperties.runtimeVersion);
  m_Info.m_uiRuntimeVersion[1] = XR_VERSION_MINOR(instanceProperties.runtimeVersion);
  m_Info.m_uiRuntimeVersion[2] = XR_VERSION_PATCH(instanceProperties.runtimeVersion);

  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanInstanceExtensionsKHR);
  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanDeviceExtensionsKHR);
  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsDeviceKHR);
  PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsRequirementsKHR);

  if (m_Extensions.m_bD3D12)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrGetD3D12GraphicsRequirementsKHR);
  }

  // Load vulkan_enable2 function pointers if the extension is available
  if (m_Extensions.m_bVulkan2)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateVulkanInstanceKHR);
    PL_GET_INSTANCE_PROC_ADDR(xrCreateVulkanDeviceKHR);
    PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsDevice2KHR);
    PL_GET_INSTANCE_PROC_ADDR(xrGetVulkanGraphicsRequirements2KHR);
  }

  if (m_Extensions.m_bSpatialAnchor)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateSpatialAnchorMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrCreateSpatialAnchorSpaceMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroySpatialAnchorMSFT);
  }

  if (m_Extensions.m_bHandTracking)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateHandTrackerEXT);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroyHandTrackerEXT);
    PL_GET_INSTANCE_PROC_ADDR(xrLocateHandJointsEXT);
  }

  if (m_Extensions.m_bPerformanceSettings)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrPerfSettingsSetPerformanceLevelEXT);
  }

  if (m_Extensions.m_bLocateSpaces)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrLocateSpacesKHR);
    plLog::Info("OpenXR: Using XR_KHR_locate_spaces for batched space queries");
  }

  if (m_Extensions.m_bVisibilityMask)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrGetVisibilityMaskKHR);
    m_Extensions.m_bVisibilityMask = m_Extensions.pfn_xrGetVisibilityMaskKHR != nullptr;
  }

  if (m_Extensions.m_bDisplayRefreshRate)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrEnumerateDisplayRefreshRatesFB);
    PL_GET_INSTANCE_PROC_ADDR(xrGetDisplayRefreshRateFB);
    PL_GET_INSTANCE_PROC_ADDR(xrRequestDisplayRefreshRateFB);
    m_Extensions.m_bDisplayRefreshRate = m_Extensions.pfn_xrEnumerateDisplayRefreshRatesFB != nullptr && m_Extensions.pfn_xrGetDisplayRefreshRateFB != nullptr && m_Extensions.pfn_xrRequestDisplayRefreshRateFB != nullptr;
  }

  if (m_Extensions.m_bControllerModelMSFT)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrGetControllerModelKeyMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrLoadControllerModelMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrGetControllerModelPropertiesMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrGetControllerModelStateMSFT);
    m_Extensions.m_bControllerModelMSFT = m_Extensions.pfn_xrGetControllerModelKeyMSFT != nullptr && m_Extensions.pfn_xrLoadControllerModelMSFT != nullptr &&
                                          m_Extensions.pfn_xrGetControllerModelPropertiesMSFT != nullptr && m_Extensions.pfn_xrGetControllerModelStateMSFT != nullptr;
  }

  if (m_Extensions.m_bRenderModelFB)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrEnumerateRenderModelPathsFB);
    PL_GET_INSTANCE_PROC_ADDR(xrGetRenderModelPropertiesFB);
    PL_GET_INSTANCE_PROC_ADDR(xrLoadRenderModelFB);
    m_Extensions.m_bRenderModelFB = m_Extensions.pfn_xrEnumerateRenderModelPathsFB != nullptr && m_Extensions.pfn_xrGetRenderModelPropertiesFB != nullptr && m_Extensions.pfn_xrLoadRenderModelFB != nullptr;
  }

  if (m_Extensions.m_bPassthroughFB)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreatePassthroughFB);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroyPassthroughFB);
    PL_GET_INSTANCE_PROC_ADDR(xrPassthroughStartFB);
    PL_GET_INSTANCE_PROC_ADDR(xrPassthroughPauseFB);
    PL_GET_INSTANCE_PROC_ADDR(xrCreatePassthroughLayerFB);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroyPassthroughLayerFB);
    PL_GET_INSTANCE_PROC_ADDR(xrPassthroughLayerPauseFB);
    PL_GET_INSTANCE_PROC_ADDR(xrPassthroughLayerResumeFB);
    m_Extensions.m_bPassthroughFB = m_Extensions.pfn_xrCreatePassthroughFB != nullptr && m_Extensions.pfn_xrDestroyPassthroughFB != nullptr &&
                                    m_Extensions.pfn_xrPassthroughStartFB != nullptr && m_Extensions.pfn_xrPassthroughPauseFB != nullptr &&
                                    m_Extensions.pfn_xrCreatePassthroughLayerFB != nullptr && m_Extensions.pfn_xrDestroyPassthroughLayerFB != nullptr &&
                                    m_Extensions.pfn_xrPassthroughLayerPauseFB != nullptr && m_Extensions.pfn_xrPassthroughLayerResumeFB != nullptr;
  }

#if PL_ENABLED(PL_COMPILE_FOR_DEBUG)
  if (m_Extensions.m_bDebugUtils)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrCreateDebugUtilsMessengerEXT);
    PL_GET_INSTANCE_PROC_ADDR(xrDestroyDebugUtilsMessengerEXT);
  }
#endif

#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  if (m_Extensions.m_bRemoting)
  {
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingSetContextPropertiesMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingConnectMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingDisconnectMSFT);
    PL_GET_INSTANCE_PROC_ADDR(xrRemotingGetConnectionStateMSFT);
  }
#endif

  if (m_pInput == nullptr)
  {
    m_pInput = PL_DEFAULT_NEW(plOpenXRInputDevice, this);
  }

  plLog::Success("OpenXR runtime {0} {1} initialized.", instanceProperties.runtimeName, sRuntimeVersion);
  return XR_SUCCESS;
}

void plOpenXR::Deinitialize()
{
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
  m_pRemoting->Disconnect().IgnoreResult();
#endif

  // Same ordering constraint as OnActorDestroyed: the queued xrDestroySwapchain has to be flushed while the session is
  // still alive.
  DestroySessionResources();

  DeinitSession();
  DeinitSystem();

}

bool plOpenXR::IsInitialized() const
{
  return m_pInstance != XR_NULL_HANDLE;
}

const plHMDInfo& plOpenXR::GetHmdInfo() const
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");
  return m_Info;
}

plXRInputDevice& plOpenXR::GetXRInput() const
{
  return *(m_pInput.Borrow());
}

plUniquePtr<plActor> plOpenXR::CreateActor(plView* pView, plGALMSAASampleCount::Enum msaaCount, plUniquePtr<plWindowBase> pCompanionWindow, plUniquePtr<plWindowOutputTargetGAL> pCompanionWindowOutput)
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  XrResult res = InitSession();
  if (res != XrResult::XR_SUCCESS)
  {
    plLog::Error("InitSession failed: {}", res);
    return {};
  }

  m_MsaaCount = msaaCount;

  PL_ASSERT_DEV(pView->GetCamera() != nullptr, "The provided view requires a camera to be set.");
  m_hView = pView->GetHandle();

  if (CreateSessionResources() != XrResult::XR_SUCCESS)
  {
    m_hView.Invalidate();
    DeinitSession();
    return {};
  }

  SetHMDCamera(pView->GetCamera());

  plUniquePtr<plActor> pActor = PL_DEFAULT_NEW(plActor, "OpenXR", this);

  PL_ASSERT_DEV((pCompanionWindow != nullptr) == (pCompanionWindowOutput != nullptr), "Both companionWindow and companionWindowOutput must either be null or valid.");
  PL_ASSERT_DEV(pCompanionWindow == nullptr || SupportsCompanionView(), "If a companionWindow is set, SupportsCompanionView() must be true.");

  plUniquePtr<plActorPluginWindowXR> pActorPlugin = PL_DEFAULT_NEW(plActorPluginWindowXR, this, std::move(pCompanionWindow), std::move(pCompanionWindowOutput));
  m_pCompanion = static_cast<plWindowOutputTargetXR*>(pActorPlugin->GetOutputTarget());
  pActor->AddPlugin(std::move(pActorPlugin));

  return std::move(pActor);
}

XrResult plOpenXR::CreateSessionResources()
{
  PL_ASSERT_DEV(m_pSession != XR_NULL_HANDLE, "The session must exist before its resources can be created.");

  plView* pView = nullptr;
  if (!plRenderWorld::TryGetView(m_hView, pView))
  {
    plLog::Error("OpenXR: Cannot create session resources, the XR view is gone.");
    return XR_ERROR_INITIALIZATION_FAILED;
  }

  const plGALMSAASampleCount::Enum msaaCount = m_MsaaCount;
  plGALXRSwapChain::SetFactoryMethod([this, msaaCount](plXRInterface* pXrInterface) -> plGALSwapChainHandle
    { return plGALDevice::GetDefaultDevice()->CreateSwapChain([this, pXrInterface, msaaCount](plAllocator* pAllocator) -> plGALSwapChain*
        { return PL_NEW(pAllocator, plGALOpenXRSwapChain, this, msaaCount); }); });
  PL_SCOPE_EXIT(plGALXRSwapChain::SetFactoryMethod({}););

  m_hSwapChain = plGALXRSwapChain::Create(this);
  if (m_hSwapChain.IsInvalidated())
  {
    plLog::Error("OpenXR: Creating the XR swap chain failed.");
    return XR_ERROR_INITIALIZATION_FAILED;
  }

  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));
  m_Info.m_vEyeRenderTargetSize = pSwapChain->GetRenderTargetSize();

  pView->SetSwapChain(m_hSwapChain);

  // Both viewports are the eye render target: plGameState::SetupMainView skips render-target setup for XR, so nothing
  // else sets them. Leaving the target viewport at its 0x0 default makes plGlobalConstants::TargetViewportSize carry
  // 1/0 and forces SyncViewportFromSwapChain to correct it mid-frame, after the graph already read it.
  const plRectFloat eyeViewport(0.0f, 0.0f, (float)m_Info.m_vEyeRenderTargetSize.width, (float)m_Info.m_vEyeRenderTargetSize.height);
  pView->SetViewport(eyeViewport);
  pView->SetTargetViewport(eyeViewport);

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DestroySessionResources()
{
  if (m_hSwapChain.IsInvalidated())
    return;

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  pDevice->DestroySwapChain(m_hSwapChain);
  m_hSwapChain.Invalidate();

  // DestroySwapChain only queues the destruction; WaitIdle runs it (DestroyDeadObjects -> DeInitPlatform ->
  // xrDestroySwapchain). It has to happen while the session is still alive: xrDestroySession implicitly destroys the
  // session's swapchains, so a later flush would hand xrDestroySwapchain a dangling handle.
  pDevice->WaitIdle();
}

void plOpenXR::OnActorDestroyed()
{
  if (m_hView.IsInvalidated())
    return;

  m_pCompanion = nullptr;
  SetHMDCamera(nullptr);

  plRenderWorld::RemoveMainView(m_hView);
  m_hView.Invalidate();

  // Must run before DeinitSession: without it the flush arrives via the companion output target's own WaitIdle during
  // member teardown - after the session is gone - and crashes.
  DestroySessionResources();

  DeinitSession();
}

bool plOpenXR::SupportsCompanionView()
{
#if PL_ENABLED(PL_PLATFORM_WINDOWS_DESKTOP)
  return true;
#else
  // E.g. on UWP OpenXR creates its own main window and other resources that conflict with our window.
  // Thus we must prevent the creation of a companion view or OpenXR crashes.
  return false;
#endif
}

XrSpace plOpenXR::GetBaseSpace() const
{
  return m_StageSpace == plXRStageSpace::Standing ? m_pSceneSpace : m_pLocalSpace;
}

XrResult plOpenXR::InitSystem()
{
  PL_ASSERT_DEV(m_SystemId == XR_NULL_SYSTEM_ID, "OpenXR actor already exists.");
  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XrFormFactor::XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XR_SUCCEED_OR_CLEANUP_LOG(xrGetSystem(m_pInstance, &systemInfo, &m_SystemId), DeinitSystem);

  // Extension property structs are only chained when their extension is enabled: a struct type from an extension the
  // instance doesn't have is invalid input.
  XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
  XrSystemHandTrackingPropertiesEXT handTrackingProperties{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
  XrSystemEyeGazeInteractionPropertiesEXT eyeGazeProperties{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
  XrSystemPassthroughPropertiesFB passthroughProperties{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
  XrSystemPassthroughProperties2FB passthroughProperties2{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
  {
    void** ppNext = &systemProperties.next;
    auto Chain = [&ppNext](auto& properties)
    {
      *ppNext = &properties;
      ppNext = const_cast<void**>(&properties.next);
    };

    if (m_Extensions.m_bHandTracking)
      Chain(handTrackingProperties);
    if (m_Extensions.m_bEyeGaze)
      Chain(eyeGazeProperties);
    if (m_Extensions.m_bPassthroughFB)
    {
      Chain(passthroughProperties);
      Chain(passthroughProperties2);
    }
  }
  XR_SUCCEED_OR_CLEANUP_LOG(xrGetSystemProperties(m_pInstance, m_SystemId, &systemProperties), DeinitSystem);

  m_Info.m_sDeviceName = systemProperties.systemName;
  m_Info.m_sSystemName = systemProperties.systemName;
  m_Info.m_uiVendorId = systemProperties.vendorId;
  m_Info.m_bOrientationTracking = systemProperties.trackingProperties.orientationTracking == XR_TRUE;
  m_Info.m_bPositionTracking = systemProperties.trackingProperties.positionTracking == XR_TRUE;
  m_Info.m_vMaxSwapchainSize = {systemProperties.graphicsProperties.maxSwapchainImageWidth, systemProperties.graphicsProperties.maxSwapchainImageHeight};
  m_Info.m_uiMaxLayerCount = systemProperties.graphicsProperties.maxLayerCount;
  m_Info.m_bSupportsHandTracking = m_Extensions.m_bHandTracking && handTrackingProperties.supportsHandTracking == XR_TRUE;
  m_Info.m_bSupportsEyeGaze = m_Extensions.m_bEyeGaze && eyeGazeProperties.supportsEyeGazeInteraction == XR_TRUE;
  m_Info.m_bSupportsVisibilityMask = m_Extensions.m_bVisibilityMask;
  m_Info.m_bSupportsQuadLayers = true;
  m_Info.m_bSupportsCylinderLayers = m_Extensions.m_bCompositionLayerCylinder;

  m_bFBPassthroughSupported = m_Extensions.m_bPassthroughFB &&
                              (passthroughProperties.supportsPassthrough == XR_TRUE || (passthroughProperties2.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) != 0);

  // Alpha-blend composition is the other passthrough path. The enumeration takes the instance and system only, so it
  // is valid before a session exists.
  m_bAlphaBlendAvailable = false;
  {
    plUInt32 uiBlendModeCount = 0;
    if (!XR_FAILED_RESULT(xrEnumerateEnvironmentBlendModes(m_pInstance, m_SystemId, m_PrimaryViewConfigurationType, 0, &uiBlendModeCount, nullptr)) && uiBlendModeCount > 0)
    {
      plHybridArray<XrEnvironmentBlendMode, 4> blendModes;
      blendModes.SetCount(uiBlendModeCount);
      if (!XR_FAILED_RESULT(xrEnumerateEnvironmentBlendModes(m_pInstance, m_SystemId, m_PrimaryViewConfigurationType, uiBlendModeCount, &uiBlendModeCount, blendModes.GetData())))
      {
        blendModes.SetCount(uiBlendModeCount);
        m_bAlphaBlendAvailable = blendModes.Contains(XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
      }
    }
  }
  m_Info.m_bSupportsPassthrough = m_bFBPassthroughSupported || m_bAlphaBlendAvailable;

  if (m_Extensions.m_bEyeGaze && !m_Info.m_bSupportsEyeGaze)
  {
    plLog::Info("OpenXR: XR_EXT_eye_gaze_interaction is enabled, but the system reports no eye gaze support.");
  }

  plLog::Info("OpenXR: System '{}' (vendor {}), max swapchain {}x{}, max {} layers, tracking: orientation {}, position {}.", m_Info.m_sSystemName,
    m_Info.m_uiVendorId, m_Info.m_vMaxSwapchainSize.width, m_Info.m_vMaxSwapchainSize.height, m_Info.m_uiMaxLayerCount,
    m_Info.m_bOrientationTracking, m_Info.m_bPositionTracking);

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitSystem()
{
  m_SystemId = XR_NULL_SYSTEM_ID;
}

XrResult plOpenXR::InitSession()
{
  PL_ASSERT_DEV(m_pSession == XR_NULL_HANDLE, "");

  plUInt32 count;
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateEnvironmentBlendModes(m_pInstance, m_SystemId, m_PrimaryViewConfigurationType, 0, &count, nullptr), DeinitSystem);

  plHybridArray<XrEnvironmentBlendMode, 4> environmentBlendModes;
  environmentBlendModes.SetCount(count);
  XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateEnvironmentBlendModes(m_pInstance, m_SystemId, m_PrimaryViewConfigurationType, count, &count, environmentBlendModes.GetData()), DeinitSession);

  // Pick the blend mode the project asked for. Taking environmentBlendModes[0] unconditionally would put a VR title
  // into passthrough compositing on any device that happens to enumerate an AR mode first.
  {
    XrEnvironmentBlendMode wanted = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (const plXRConfig* pConfig = plGameApplicationBase::GetGameApplicationBaseInstance()->GetPlatformProfile().GetTypeConfig<plXRConfig>())
    {
      switch (pConfig->m_BlendMode)
      {
        case plXREnvironmentBlendMode::Additive:
          wanted = XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
          break;
        case plXREnvironmentBlendMode::AlphaBlend:
          wanted = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
          break;
        default:
          wanted = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
          break;
      }
    }

    if (environmentBlendModes.Contains(wanted))
    {
      m_BlendMode = wanted;
    }
    else
    {
      m_BlendMode = environmentBlendModes[0];
      plLog::Warning("OpenXR: The configured environment blend mode is not supported by this runtime, falling back to the runtime's first choice ({}).", (int)m_BlendMode);
    }

    // Passthrough over alpha-blend composition switches m_BlendMode while it is active and restores this afterwards.
    m_ConfiguredBlendMode = m_BlendMode;
  }

  XR_SUCCEED_OR_CLEANUP_LOG(InitGraphicsPlugin(), DeinitSession);

  if (m_Extensions.m_bDebugUtils)
  {
    XR_SUCCEED_OR_CLEANUP_LOG(InitDebugMessenger(), DeinitSession);
  }

  XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
  sessionCreateInfo.systemId = m_SystemId;
  if (m_GraphicsApi == GraphicsApi::D3D12)
  {
    sessionCreateInfo.next = &m_XrGraphicsBindingD3D12;
  }
  else
  {
    sessionCreateInfo.next = &m_XrGraphicsBindingVulkan;
  }

  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateSession(m_pInstance, &sessionCreateInfo, &m_pSession), DeinitSession);

  // The standing space is STAGE when the runtime has one. Seated setups and headsets without a configured boundary may
  // not, so fall back to LOCAL_FLOOR (XR_EXT_local_floor) and then LOCAL, instead of failing the session.
  plHybridArray<XrReferenceSpaceType, 8> referenceSpaces;
  {
    plUInt32 uiSpaceCount = 0;
    XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateReferenceSpaces(m_pSession, 0, &uiSpaceCount, nullptr), DeinitSession);
    referenceSpaces.SetCount(uiSpaceCount);
    XR_SUCCEED_OR_CLEANUP_LOG(xrEnumerateReferenceSpaces(m_pSession, uiSpaceCount, &uiSpaceCount, referenceSpaces.GetData()), DeinitSession);
  }

  m_Info.m_bHasStageSpace = referenceSpaces.Contains(XR_REFERENCE_SPACE_TYPE_STAGE);

  XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  spaceCreateInfo.poseInReferenceSpace = ConvertTransform(plTransform::MakeIdentity());
  if (m_Info.m_bHasStageSpace)
  {
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  }
  else if (m_Extensions.m_bLocalFloor && referenceSpaces.Contains(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT))
  {
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    plLog::Info("OpenXR: The runtime has no STAGE space, using LOCAL_FLOOR for standing experiences.");
  }
  else
  {
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    plLog::Warning("OpenXR: The runtime has no STAGE space, using LOCAL for standing experiences. The floor is not at height 0.");
  }
  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateReferenceSpace(m_pSession, &spaceCreateInfo, &m_pSceneSpace), DeinitSession);

  spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateReferenceSpace(m_pSession, &spaceCreateInfo, &m_pLocalSpace), DeinitSession);

  // VIEW is core and always present. Only head-locked composition layers use it, so a failure only costs those.
  if (m_bCompositionLayersEnabled)
  {
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_LOG_ERROR(xrCreateReferenceSpace(m_pSession, &spaceCreateInfo, &m_pViewSpace));
  }

  XR_SUCCEED_OR_CLEANUP_LOG(m_pInput->CreateActions(m_pSession, m_pSceneSpace), DeinitSession);
  XR_SUCCEED_OR_CLEANUP_LOG(m_pInput->AttachSessionActionSets(m_pSession), DeinitSession);

  // Set performance hints to request sustained high performance from the runtime.
  // This may help prevent aggressive throttling by runtimes like SteamVR.
  if (m_Extensions.m_bPerformanceSettings && m_Extensions.pfn_xrPerfSettingsSetPerformanceLevelEXT)
  {
    XrResult cpuResult = m_Extensions.pfn_xrPerfSettingsSetPerformanceLevelEXT(m_pSession, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    XrResult gpuResult = m_Extensions.pfn_xrPerfSettingsSetPerformanceLevelEXT(m_pSession, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    if (cpuResult == XR_SUCCESS && gpuResult == XR_SUCCESS)
    {
      plLog::Info("OpenXR: Set performance level to SUSTAINED_HIGH for CPU and GPU");
    }
  }

  m_GALdeviceEventsId = plGALDevice::s_Events.AddEventHandler(plMakeDelegate(&plOpenXR::GALDeviceEventHandler, this));

  SetStageSpace(plXRStageSpace::Standing);
  if (m_Extensions.m_bSpatialAnchor)
  {
    m_pAnchors = PL_DEFAULT_NEW(plOpenXRSpatialAnchors, this);
  }
  if (m_Extensions.m_bHandTracking && plOpenXRHandTracking::IsHandTrackingSupported(this))
  {
    m_pHandTracking = PL_DEFAULT_NEW(plOpenXRHandTracking, this);
  }

  InitDisplayRefreshRate();
  InitPassthrough();

  // Fetched once the views have a valid FOV, see UpdateVisibilityMask.
  m_bVisibilityMaskDirty = m_Extensions.m_bVisibilityMask;
  m_bViewFovValid = false;

  // A passthrough request survives a session restart.
  ApplyPassthroughState();
  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitSession()
{
  // m_pCompanion is owned by the actor plugin, which outlives a session restart - only OnActorDestroyed clears it.
  m_bSessionRunning = false;
  m_bExitRenderLoop = false;
  m_bRequestRestart = false;
  m_bRenderInProgress = false;
  m_bPreWaitComplete = false;
  m_PreWaitResult = XR_SUCCESS;
  m_iPredictedDisplayTime = 0;
  m_SessionState = XR_SESSION_STATE_UNKNOWN;
  SetSessionFocus(plXRSessionFocus::Hidden);

  // Layers belong to the session: their swapchains go with it, so handles held by game code report failure from here on.
  DestroyAllCompositionLayers();
  m_SwapchainFormats.Clear();

  DeinitPassthrough();
  m_BlendMode = m_ConfiguredBlendMode;
  SetPassthroughActive(false);

  if (m_bVisibilityMaskSet)
  {
    plXRVisibilityMask::ClearMask();
    m_bVisibilityMaskSet = false;
  }
  m_bVisibilityMaskDirty = false;
  m_bViewFovValid = false;

  // Session-scoped capability values, see plHMDInfo.
  m_Info.m_bHasStageSpace = false;
  m_Info.m_DisplayRefreshRates.Clear();
  m_Info.m_fDisplayRefreshRate = 0.0f;
  m_bRefreshRateRequestPending = m_bHasRefreshRateRequest;
  m_iPredictedDisplayPeriod = 0;

  m_pHandTracking = nullptr;
  m_pAnchors = nullptr;
  if (m_GALdeviceEventsId != 0)
  {
    plGALDevice::s_Events.RemoveEventHandler(m_GALdeviceEventsId);
  }

  // Must come before the reference spaces are destroyed: DestroyActions stops the input thread, which locates poses
  // against GetBaseSpace() until it is joined. The input device doesn't exist yet if instance creation failed.
  if (m_pInput != nullptr)
  {
    m_pInput->DestroyActions();
  }

  if (m_pSceneSpace)
  {
    xrDestroySpace(m_pSceneSpace);
    m_pSceneSpace = XR_NULL_HANDLE;
  }

  if (m_pLocalSpace)
  {
    xrDestroySpace(m_pLocalSpace);
    m_pLocalSpace = XR_NULL_HANDLE;
  }

  if (m_pViewSpace)
  {
    xrDestroySpace(m_pViewSpace);
    m_pViewSpace = XR_NULL_HANDLE;
  }

  if (m_pSession)
  {
    // The session's swapchain images are GAL textures that in-flight command buffers may still reference, and
    // xrDestroySession implicitly destroys those swapchains. Flush first so nothing is submitted against freed images.
    if (plGALDevice::HasDefaultDevice())
    {
      plGALDevice::GetDefaultDevice()->WaitIdle();
    }

    xrDestroySession(m_pSession);
    m_pSession = XR_NULL_HANDLE;
  }

  DeinitGraphicsPlugin();
  if (m_Extensions.m_bDebugUtils)
  {
    DeinitInitDebugMessenger();
  }
}

XrResult plOpenXR::InitGraphicsPlugin()
{
  // The binding extension was fixed at instance creation; a renderer that differs from it cannot bind a session.
  const plStringView sRenderer = plGALDevice::GetDefaultDevice()->GetRenderer();
  const GraphicsApi deviceApi = sRenderer.IsEqual_NoCase("DX12") ? GraphicsApi::D3D12 : GraphicsApi::Vulkan;
  if (deviceApi != m_GraphicsApi)
  {
    plLog::Error("OpenXR: The instance was created for {} but the renderer is '{}'.", m_GraphicsApi == GraphicsApi::D3D12 ? "D3D12" : "Vulkan", sRenderer);
    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  }

  return m_GraphicsApi == GraphicsApi::D3D12 ? InitGraphicsPluginD3D12() : InitGraphicsPluginVulkan();
}

XrResult plOpenXR::InitGraphicsPluginD3D12()
{
  PL_ASSERT_DEV(m_XrGraphicsBindingD3D12.device == nullptr, "");

  plGALDeviceDX12* pDX12Device = static_cast<plGALDeviceDX12*>(plGALDevice::GetDefaultDevice());

  // Required before xrCreateSession. Names the adapter the headset is connected to.
  XrGraphicsRequirementsD3D12KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetD3D12GraphicsRequirementsKHR(m_pInstance, m_SystemId, &graphicsRequirements), DeinitGraphicsPlugin);

  const LUID& adapterLuid = pDX12Device->GetAdapterDesc().AdapterLuid;
  if (adapterLuid.LowPart != graphicsRequirements.adapterLuid.LowPart || adapterLuid.HighPart != graphicsRequirements.adapterLuid.HighPart)
  {
    plLog::Warning("OpenXR: The headset is connected to a different adapter than the D3D12 device in use ('{}'). The runtime may refuse the session.", pDX12Device->GetCapabilities().m_sAdapterName);
  }

  m_XrGraphicsBindingD3D12.device = pDX12Device->GetD3DDevice();
  m_XrGraphicsBindingD3D12.queue = pDX12Device->GetGraphicsQueue().m_pQueue;

  return XrResult::XR_SUCCESS;
}

XrResult plOpenXR::InitGraphicsPluginVulkan()
{
  PL_ASSERT_DEV(m_XrGraphicsBindingVulkan.device == VK_NULL_HANDLE, "");
  
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALDeviceVulkan* pVulkanDevice = static_cast<plGALDeviceVulkan*>(pDevice);

  // Get Vulkan instance from the device
  VkInstance vkInstance = (VkInstance)pVulkanDevice->GetVulkanInstance();
  VkPhysicalDevice xrPhysicalDevice = VK_NULL_HANDLE;

  // The same struct layout serves both extensions; only the type tag differs.
  XrGraphicsRequirementsVulkanKHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};

  if (m_Extensions.m_bVulkan2)
  {
    // Use vulkan_enable2 extension for better GPU synchronization
    graphicsRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = m_SystemId;
    deviceGetInfo.vulkanInstance = vkInstance;

    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsRequirements2KHR(m_pInstance, m_SystemId, &graphicsRequirements), DeinitGraphicsPlugin);
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsDevice2KHR(m_pInstance, &deviceGetInfo, &xrPhysicalDevice), DeinitGraphicsPlugin);

    // vulkan_enable2 expects the VkInstance and VkDevice to be created through xrCreateVulkanInstanceKHR and
    // xrCreateVulkanDeviceKHR, so the runtime can add the extensions it needs. The engine creates both itself, before
    // XR starts, and has no way to query that list under this extension. It works on runtimes whose requirements the
    // engine's device already meets (external memory and semaphores).
    plLog::Info("OpenXR: Using XR_KHR_vulkan_enable2 with an engine-created Vulkan device; the runtime's extension requirements cannot be verified.");
  }
  else
  {
    // Fall back to vulkan_enable extension
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsRequirementsKHR(m_pInstance, m_SystemId, &graphicsRequirements), DeinitGraphicsPlugin);
    XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrGetVulkanGraphicsDeviceKHR(m_pInstance, m_SystemId, vkInstance, &xrPhysicalDevice), DeinitGraphicsPlugin);

    // Under vulkan_enable the runtime lists the extensions it needs; the device was created before XR started, so all
    // that can be done is to report what is missing.
    auto CheckRequiredExtensions = [this](PFN_xrGetVulkanInstanceExtensionsKHR pfnQuery, const plDynamicArray<plString>& enabled, const char* szKind)
    {
      plUInt32 uiLength = 0;
      if (pfnQuery == nullptr || XR_FAILED_RESULT(pfnQuery(m_pInstance, m_SystemId, 0, &uiLength, nullptr)) || uiLength == 0)
        return;

      plDynamicArray<char> buffer;
      buffer.SetCount(uiLength);
      if (XR_FAILED_RESULT(pfnQuery(m_pInstance, m_SystemId, uiLength, &uiLength, buffer.GetData())))
        return;

      const plStringBuilder sRequiredList = buffer.GetData();
      plHybridArray<plStringView, 16> required;
      sRequiredList.Split(false, required, " ");
      for (plStringView sRequired : required)
      {
        if (!enabled.Contains(sRequired))
        {
          plLog::Warning("OpenXR: The runtime requires Vulkan {} extension '{}', which the engine's Vulkan {} was not created with.", szKind, sRequired, szKind);
        }
      }
    };

    // PFN_xrGetVulkanDeviceExtensionsKHR has the same signature as the instance query.
    CheckRequiredExtensions(m_Extensions.pfn_xrGetVulkanInstanceExtensionsKHR, pVulkanDevice->GetEnabledInstanceExtensions(), "instance");
    CheckRequiredExtensions(m_Extensions.pfn_xrGetVulkanDeviceExtensionsKHR, pVulkanDevice->GetEnabledDeviceExtensions(), "device");
  }

  // The requirement applies to the API version the VkInstance was created with, not to what the device supports.
  const plUInt32 uiInstanceApiVersion = pVulkanDevice->GetInstanceApiVersion();
  const XrVersion instanceVersion = XR_MAKE_VERSION(VK_API_VERSION_MAJOR(uiInstanceApiVersion), VK_API_VERSION_MINOR(uiInstanceApiVersion), 0);
  if (instanceVersion < graphicsRequirements.minApiVersionSupported)
  {
    plLog::Warning("OpenXR: The runtime needs Vulkan {}.{} or newer, the engine's Vulkan instance uses {}.{}.", XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported), XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported), VK_API_VERSION_MAJOR(uiInstanceApiVersion), VK_API_VERSION_MINOR(uiInstanceApiVersion));
  }
  
  // Verify that OpenXR selected the same physical device we're using
  VkPhysicalDevice ourPhysicalDevice = (VkPhysicalDevice)pVulkanDevice->GetVulkanPhysicalDevice();
  if (xrPhysicalDevice != ourPhysicalDevice)
  {
    plLog::Warning("OpenXR selected a different Vulkan physical device than the one in use. This may cause issues.");
  }

  m_XrGraphicsBindingVulkan.instance = vkInstance;
  m_XrGraphicsBindingVulkan.physicalDevice = xrPhysicalDevice;
  m_XrGraphicsBindingVulkan.device = (VkDevice)pVulkanDevice->GetVulkanDevice();
  m_XrGraphicsBindingVulkan.queueFamilyIndex = pVulkanDevice->GetGraphicsQueue().m_uiQueueFamily;
  m_XrGraphicsBindingVulkan.queueIndex = pVulkanDevice->GetGraphicsQueue().m_uiQueueIndex;

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitGraphicsPlugin()
{
  m_XrGraphicsBindingVulkan.device = VK_NULL_HANDLE;
  m_XrGraphicsBindingD3D12.device = nullptr;
  m_XrGraphicsBindingD3D12.queue = nullptr;
}

XrResult plOpenXR::InitDebugMessenger()
{
  XrDebugUtilsMessengerCreateInfoEXT create_info{XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  create_info.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                  XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                  XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  create_info.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  create_info.userCallback = xrDebugCallback;

  XR_SUCCEED_OR_CLEANUP_LOG(m_Extensions.pfn_xrCreateDebugUtilsMessengerEXT(m_pInstance, &create_info, &m_pDebugMessenger), DeinitInitDebugMessenger);

  return XrResult::XR_SUCCESS;
}

void plOpenXR::DeinitInitDebugMessenger()
{
  if (m_pDebugMessenger != XR_NULL_HANDLE)
  {
    XR_LOG_ERROR(m_Extensions.pfn_xrDestroyDebugUtilsMessengerEXT(m_pDebugMessenger));
    m_pDebugMessenger = XR_NULL_HANDLE;
  }
}

void plOpenXR::BeforeUpdatePlugins()
{
  PL_PROFILE_SCOPE("BeforeUpdatePlugins");

  // Caching the stage transform BEFORE world simulation runs prevents jitter caused by the timing mismatch between XR
  // pose prediction and world transform updates (especially noticeable in editor mode). This runs on the update task,
  // which can overlap the main thread's BeginFrame, hence the lock.
  {
    PL_LOCK(m_StageTransformMutex);
    m_bStageTransformCached = false;
  }
  if (plWorld* pWorld = GetWorld())
  {
    bool bNeedsStereoMode = false;

    {
      // Must go through a const plWorld: the non-const GetComponentManager overload asserts WRITE access, so calling it
      // under a read marker trips plWorld::CheckForWriteAccess.
      const plWorld* pReadOnlyWorld = pWorld;

      PL_LOCK(pReadOnlyWorld->GetReadMarker());

      if (const plStageSpaceComponentManager* pStageMan = pReadOnlyWorld->GetComponentManager<plStageSpaceComponentManager>())
      {
        if (const plStageSpaceComponent* pStage = pStageMan->GetSingletonComponent())
        {
          PL_LOCK(m_StageTransformMutex);
          m_CachedStageTransform = pStage->GetOwner()->GetGlobalTransform();
          m_bStageTransformCached = true;
        }
      }

      if (const plCameraComponentManager* pCCM = pReadOnlyWorld->GetComponentManager<plCameraComponentManager>())
      {
        if (const plCameraComponent* pCameraComponent = pCCM->GetCameraByUsageHint(plCameraUsageHint::MainView))
        {
          bNeedsStereoMode = pCameraComponent->GetCameraMode() != plCameraMode::Stereo;
        }
      }
    }

    // Only escalate to a write lock when the mode actually has to change; this runs on a task worker every frame and
    // the write marker serialises against the whole world update.
    if (bNeedsStereoMode)
    {
      PL_LOCK(pWorld->GetWriteMarker());

      if (auto* pCCM = pWorld->GetComponentManager<plCameraComponentManager>())
      {
        if (plCameraComponent* pCameraComponent = pCCM->GetCameraByUsageHint(plCameraUsageHint::MainView))
        {
          pCameraComponent->SetCameraMode(plCameraMode::Stereo);
        }
      }
    }
  }
}

void plOpenXR::SetSessionFocus(plXRSessionFocus::Enum focus)
{
  if (m_SessionFocus == focus)
    return;

  m_SessionFocus = focus;

  // Runtimes reject refresh-rate requests while unfocused and may switch the rate themselves meanwhile, so the
  // request is sent again when focus returns.
  if (focus == plXRSessionFocus::Focused && m_bHasRefreshRateRequest && m_fRequestedRefreshRate != m_Info.m_fDisplayRefreshRate)
  {
    m_bRefreshRateRequestPending = true;
  }

  plXRSessionEvent e;
  e.m_Type = plXRSessionEvent::Type::FocusChanged;
  e.m_Focus = focus;
  e.m_fRefreshRate = m_Info.m_fDisplayRefreshRate;
  e.m_bPassthroughActive = m_bPassthroughActive;
  m_SessionEvents.Broadcast(e);
}

void plOpenXR::PollEvents()
{
  PL_PROFILE_SCOPE("PollEvents");

  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER, nullptr};

  while (m_pInstance != XR_NULL_HANDLE && xrPollEvent(m_pInstance, &event) == XR_SUCCESS)
  {
#ifdef BUILDSYSTEM_ENABLE_OPENXR_REMOTING_SUPPORT
    m_pRemoting->HandleEvent(event);
#endif
    switch (event.type)
    {
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
      {
        if (m_pInput != nullptr)
        {
          m_pInput->RequestInteractionProfileUpdate();
        }
      }
      break;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      {
        const XrEventDataReferenceSpaceChangePending& changeEvent = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
        plLog::Info("OpenXR: The runtime is changing reference space {} (recenter or boundary change).", (int)changeEvent.referenceSpaceType);

        plXRSessionEvent e;
        e.m_Type = plXRSessionEvent::Type::ReferenceSpaceChanged;
        e.m_Focus = m_SessionFocus;
        m_SessionEvents.Broadcast(e);
      }
      break;
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
      {
        const XrEventDataSessionStateChanged& session_state_changed_event = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        m_SessionState = session_state_changed_event.state;
        switch (m_SessionState)
        {
          case XR_SESSION_STATE_READY:
          {
            XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            sessionBeginInfo.primaryViewConfigurationType = m_PrimaryViewConfigurationType;
            const XrResult res = xrBeginSession(m_pSession, &sessionBeginInfo);
            if (res == XR_SUCCESS)
            {
              m_bSessionRunning = true;
            }
            else
            {
              plLog::Error("OpenXR: xrBeginSession failed: {}", res);
            }
            break;
          }
          case XR_SESSION_STATE_STOPPING:
          {
            m_bSessionRunning = false;
            // A frame waited on by PreWaitNextFrame is never begun; the next session starts with a fresh xrWaitFrame.
            m_bPreWaitComplete = false;
            const XrResult res = xrEndSession(m_pSession);
            if (res != XR_SUCCESS)
            {
              plLog::Error("OpenXR: xrEndSession failed: {}", res);
            }
            break;
          }
          case XR_SESSION_STATE_EXITING:
          {
            // Do not attempt to restart because user closed this session.
            m_bExitRenderLoop = true;
            m_bRequestRestart = false;
            break;
          }
          case XR_SESSION_STATE_LOSS_PENDING:
          {
            // Poll for a new systemId
            m_bExitRenderLoop = true;
            m_bRequestRestart = true;
            break;
          }
          default:
            break;
        }

        switch (m_SessionState)
        {
          case XR_SESSION_STATE_FOCUSED:
            SetSessionFocus(plXRSessionFocus::Focused);
            break;
          case XR_SESSION_STATE_VISIBLE:
            SetSessionFocus(plXRSessionFocus::Visible);
            break;
          default:
            SetSessionFocus(plXRSessionFocus::Hidden);
            break;
        }
      }
      break;
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      {
        m_bExitRenderLoop = true;
        m_bRequestRestart = false;
        m_bInstanceLost = true;
      }
      break;
      case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
      {
        const XrEventDataDisplayRefreshRateChangedFB& rateEvent = *reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&event);
        plLog::Info("OpenXR: Display refresh rate changed from {} Hz to {} Hz.", rateEvent.fromDisplayRefreshRate, rateEvent.toDisplayRefreshRate);
        m_Info.m_fDisplayRefreshRate = rateEvent.toDisplayRefreshRate;

        plXRSessionEvent e;
        e.m_Type = plXRSessionEvent::Type::DisplayRefreshRateChanged;
        e.m_Focus = m_SessionFocus;
        e.m_fRefreshRate = rateEvent.toDisplayRefreshRate;
        e.m_bPassthroughActive = m_bPassthroughActive;
        m_SessionEvents.Broadcast(e);
      }
      break;
      case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
      {
        const XrEventDataVisibilityMaskChangedKHR& maskEvent = *reinterpret_cast<XrEventDataVisibilityMaskChangedKHR*>(&event);
        if (maskEvent.session == m_pSession && maskEvent.viewConfigurationType == m_PrimaryViewConfigurationType)
        {
          // Both eyes are fetched again; the mask is tiny and SetMask takes both at once.
          m_bVisibilityMaskDirty = m_Extensions.m_bVisibilityMask;
        }
      }
      break;
      default:
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER, nullptr};
  }

  if (m_bExitRenderLoop)
  {
    m_bSessionRunning = false;
    m_bExitRenderLoop = false;

    if (m_bRequestRestart)
    {
      m_bRequestRestart = false;
      RestartSession();
    }
    else
    {
      ShutdownSession();
    }
  }
}

void plOpenXR::ShutdownSession()
{
  PL_PROFILE_SCOPE("ShutdownSession");

  // The runtime asked us to stop: either the user closed the session from its overlay (XR_SESSION_STATE_EXITING) or the
  // instance is going away. Release the session so the runtime gets the HMD back instead of holding it until the
  // process exits.
  DestroySessionResources();
  DeinitSession();
  DeinitSystem();

  if (m_bInstanceLost)
  {
    // Nothing may be called on a lost instance, not even xrPollEvent, so drop it. XR stays down until something calls
    // Initialize again.
    if (m_pInstance != XR_NULL_HANDLE)
    {
      xrDestroyInstance(m_pInstance);
      m_pInstance = XR_NULL_HANDLE;
    }
    m_bInstanceLost = false;
    plLog::Warning("OpenXR: The runtime instance was lost, XR has been shut down.");
  }
  else
  {
    plLog::Info("OpenXR: The runtime ended the session, XR has been shut down.");
  }
}

void plOpenXR::RestartSession()
{
  PL_PROFILE_SCOPE("RestartSession");
  plLog::Info("OpenXR: Restarting the session after runtime or device loss.");

  const bool bHadView = !m_hView.IsInvalidated();
  plCamera* pCamera = m_pCameraToSynchronize;

  DestroySessionResources();
  DeinitSession();
  DeinitSystem();

  if (InitSystem() != XR_SUCCESS)
  {
    plLog::Error("OpenXR: Could not re-acquire a system after session loss, XR stays down.");
    return;
  }

  if (InitSession() != XR_SUCCESS)
  {
    plLog::Error("OpenXR: Could not re-create the session after loss, XR stays down.");
    DeinitSystem();
    return;
  }

  if (bHadView && CreateSessionResources() != XrResult::XR_SUCCESS)
  {
    plLog::Error("OpenXR: Could not re-create the session's resources after loss, XR stays down.");
    DeinitSession();
    DeinitSystem();
    return;
  }

  // SetHMDCamera early-outs when the pointer is unchanged, so force the projection to be rebuilt for the new session.
  m_pCameraToSynchronize = nullptr;
  SetHMDCamera(pCamera);
  m_bProjectionChanged = true;

  plLog::Success("OpenXR: Session restarted.");
}

void plOpenXR::UpdatePoses()
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  PL_PROFILE_SCOPE("UpdatePoses");
  m_ViewState = XrViewState{XR_TYPE_VIEW_STATE};
  plUInt32 viewCapacityInput = 2;
  plUInt32 viewCountOutput;

  XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
  viewLocateInfo.viewConfigurationType = m_PrimaryViewConfigurationType;
  viewLocateInfo.displayTime = m_FrameState.predictedDisplayTime;
  viewLocateInfo.space = GetBaseSpace();
  m_Views[0].type = XR_TYPE_VIEW;
  m_Views[1].type = XR_TYPE_VIEW;
  XrFovf previousFov[2];
  previousFov[0] = m_Views[0].fov;
  previousFov[1] = m_Views[1].fov;

  XrResult res = xrLocateViews(m_pSession, &viewLocateInfo, &m_ViewState, viewCapacityInput, &viewCountOutput, m_Views);

  if (!XR_FAILED_RESULT(res))
  {
    m_pInput->m_DeviceState[0].m_bGripPoseIsValid = ((m_ViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (m_ViewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT));
    m_pInput->m_DeviceState[0].m_bAimPoseIsValid = m_pInput->m_DeviceState[0].m_bGripPoseIsValid;
    m_pInput->m_DeviceState[0].m_bAimPoseIsTracked = ((m_ViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT) && (m_ViewState.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT));
  }
  else
  {
    m_pInput->m_DeviceState[0].m_bGripPoseIsValid = false;
    m_pInput->m_DeviceState[0].m_bAimPoseIsValid = false;
    m_pInput->m_DeviceState[0].m_bAimPoseIsTracked = false;
  }

  // Needed as workaround for broken XR runtimes.
  auto FovIsNull = [](const XrFovf& fov)
  {
    return fov.angleLeft == 0.0f && fov.angleRight == 0.0f && fov.angleDown == 0.0f && fov.angleUp == 0.0f;
  };

  auto IdentityFov = [](XrFovf& fov)
  {
    fov.angleLeft = -plAngle::MakeFromDegree(45.0f).GetRadian();
    fov.angleRight = plAngle::MakeFromDegree(45.0f).GetRadian();
    fov.angleUp = plAngle::MakeFromDegree(45.0f).GetRadian();
    fov.angleDown = -plAngle::MakeFromDegree(45.0f).GetRadian();
  };

  const bool bRuntimeFov = !XR_FAILED_RESULT(res) && viewCountOutput == 2 && !FovIsNull(m_Views[0].fov) && !FovIsNull(m_Views[1].fov);
  if (FovIsNull(m_Views[0].fov) || FovIsNull(m_Views[1].fov))
  {
    IdentityFov(m_Views[0].fov);
    IdentityFov(m_Views[1].fov);
  }

  m_bProjectionChanged = plMemoryUtils::Compare(&previousFov[0], &m_Views[0].fov, 1) != 0 || plMemoryUtils::Compare(&previousFov[1], &m_Views[1].fov, 1) != 0;

  // The visibility mask is converted to NDC with these FOVs, so a new FOV needs a new conversion.
  if (bRuntimeFov)
  {
    if (m_Extensions.m_bVisibilityMask && (!m_bViewFovValid || m_bProjectionChanged))
    {
      m_bVisibilityMaskDirty = true;
    }
    m_bViewFovValid = true;
  }

  for (plUInt32 uiEyeIndex : {0, 1})
  {
    plQuat rot = ConvertOrientation(m_Views[uiEyeIndex].pose.orientation);
    if (!rot.IsValid())
    {
      m_Views[uiEyeIndex].pose.orientation = XrQuaternionf{0, 0, 0, 1};
    }
  }

  UpdateCamera();
  m_pInput->UpdateActions();

  if (m_pHandTracking)
  {
    m_pHandTracking->UpdateJointTransforms();
  }
}

plResult plOpenXR::TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz)
{
  if (m_pSession == XR_NULL_HANDLE || m_pInput == nullptr)
    return PL_FAILURE;

  return m_pInput->TriggerHapticPulse(deviceID, duration, fAmplitude, fFrequencyHz);
}

void plOpenXR::UpdateCamera()
{
  if (!m_pCameraToSynchronize)
  {
    return;
  }
  // Update camera projection
  if (m_uiSettingsModificationCounter != m_pCameraToSynchronize->GetSettingsModificationCounter() || m_bProjectionChanged)
  {
    m_bProjectionChanged = false;
    const float fAspectRatio = (float)m_Info.m_vEyeRenderTargetSize.width / (float)m_Info.m_vEyeRenderTargetSize.height;
    auto CreateProjection = [](const XrView& view, plCamera* cam)
    {
      return plGraphicsUtils::CreatePerspectiveProjectionMatrix(plMath::Tan(plAngle::MakeFromRadian(view.fov.angleLeft)) * cam->GetNearPlane(), plMath::Tan(plAngle::MakeFromRadian(view.fov.angleRight)) * cam->GetNearPlane(), plMath::Tan(plAngle::MakeFromRadian(view.fov.angleDown)) * cam->GetNearPlane(),
        plMath::Tan(plAngle::MakeFromRadian(view.fov.angleUp)) * cam->GetNearPlane(), cam->GetNearPlane(), cam->GetFarPlane());
    };

    // Update projection with newest near/ far values. If not sync camera is set, just use the last value from XR
    // camera.
    const plMat4 projLeft = CreateProjection(m_Views[0], m_pCameraToSynchronize);
    const plMat4 projRight = CreateProjection(m_Views[1], m_pCameraToSynchronize);
    m_pCameraToSynchronize->SetStereoProjection(projLeft, projRight, fAspectRatio);
    m_uiSettingsModificationCounter = m_pCameraToSynchronize->GetSettingsModificationCounter();
  }

  // Update camera view
  {
    plTransform add;
    add.SetIdentity();
    
    // Use cached stage transform from BeforeUpdatePlugins to prevent jitter.
    // The stage transform was captured before world simulation, ensuring it's
    // consistent with the XR pose prediction timing.
    {
      PL_LOCK(m_StageTransformMutex);
      if (m_bStageTransformCached)
      {
        add = m_CachedStageTransform;
      }
    }
    
    // Update stage space setting if needed
    plView* pView = nullptr;
    if (plRenderWorld::TryGetView(m_hView, pView))
    {
      if (const plWorld* pWorld = pView->GetWorld())
      {
        PL_LOCK(pWorld->GetReadMarker());
        if (const plStageSpaceComponentManager* pStageMan = pWorld->GetComponentManager<plStageSpaceComponentManager>())
        {
          if (const plStageSpaceComponent* pStage = pStageMan->GetSingletonComponent())
          {
            plEnum<plXRStageSpace> stageSpace = pStage->GetStageSpace();
            if (m_StageSpace != stageSpace)
              SetStageSpace(pStage->GetStageSpace());
          }
        }
      }
    }

    if (m_pInput->m_DeviceState[0].m_bGripPoseIsValid)
    {
      // Update device state (average of both eyes).
      const plQuat rot = plQuat::MakeSlerp(ConvertOrientation(m_Views[0].pose.orientation), ConvertOrientation(m_Views[1].pose.orientation), 0.5f);
      const plVec3 pos = plMath::Lerp(ConvertPosition(m_Views[0].pose.position), ConvertPosition(m_Views[1].pose.position), 0.5f);

      m_pInput->m_DeviceState[0].m_vGripPosition = pos;
      m_pInput->m_DeviceState[0].m_qGripRotation = rot;
      m_pInput->m_DeviceState[0].m_vAimPosition = pos;
      m_pInput->m_DeviceState[0].m_qAimRotation = rot;
      m_pInput->m_DeviceState[0].m_Type = plXRDeviceType::HMD;
      m_pInput->m_DeviceState[0].m_bGripPoseIsValid = true;
      m_pInput->m_DeviceState[0].m_bAimPoseIsValid = true;
      m_pInput->m_DeviceState[0].m_bDeviceIsConnected = true;
    }

    // Set view matrix
    if (m_pInput->m_DeviceState[0].m_bGripPoseIsValid)
    {
      const plMat4 mStageTransform = add.GetAsMat4();
      const plMat4 poseLeft = mStageTransform * ConvertPoseToMatrix(m_Views[0].pose);
      const plMat4 poseRight = mStageTransform * ConvertPoseToMatrix(m_Views[1].pose);

      // PL Forward is +X, need to add this to align the forward projection
      const plMat4 viewMatrix = plGraphicsUtils::CreateLookAtViewMatrix(plVec3::MakeZero(), plVec3(1, 0, 0), plVec3(0, 0, 1));
      const plMat4 mViewTransformLeft = viewMatrix * poseLeft.GetInverse();
      const plMat4 mViewTransformRight = viewMatrix * poseRight.GetInverse();

      m_pCameraToSynchronize->SetViewMatrix(mViewTransformLeft, plCameraEye::Left);
      m_pCameraToSynchronize->SetViewMatrix(mViewTransformRight, plCameraEye::Right);
    }
  }
}

void plOpenXR::BeginFrame()
{
  if (m_hView.IsInvalidated() || !m_bSessionRunning)
    return;

  PL_PROFILE_SCOPE("OpenXrBeginFrame");

  // Use pre-waited frame state if available from previous EndFrame
  if (m_bPreWaitComplete)
  {
    m_bPreWaitComplete = false;
    if (XR_FAILED_RESULT(m_PreWaitResult))
    {
      m_bRenderInProgress = false;
      return;
    }
    // m_FrameState already populated by EndFrame
  }
  else
  {
    // Fallback: wait here if this is the first frame or EndFrame wasn't called
    PL_PROFILE_SCOPE("xrWaitFrame");
    m_FrameWaitInfo = XrFrameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    m_FrameState = XrFrameState{XR_TYPE_FRAME_STATE};
    XrResult result = xrWaitFrame(m_pSession, &m_FrameWaitInfo, &m_FrameState);
    if (XR_FAILED_RESULT(result))
    {
      m_iPredictedDisplayTime = 0;
      m_bRenderInProgress = false;
      return;
    }
    m_iPredictedDisplayTime = (plInt64)m_FrameState.predictedDisplayTime;
  }

  {
    PL_PROFILE_SCOPE("xrBeginFrame");
    m_FrameBeginInfo = XrFrameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult result = xrBeginFrame(m_pSession, &m_FrameBeginInfo);
    if (XR_FAILED_RESULT(result))
    {
      m_bRenderInProgress = false;
      return;
    }

    // XR_FRAME_DISCARDED is a success code: the runtime dropped a frame we began but never ended, and this frame
    // takes its place. Bailing out here instead would skip UpdatePoses and leave m_bRenderInProgress false, so the
    // next xrEndFrame is skipped too and every following frame discards - tracking freezes and nothing is presented.
    if (result == XR_FRAME_DISCARDED)
    {
      m_iFramesDiscarded.Increment();

      static plTime s_LastLog;
      if (plOpenXRShouldLogThrottled(s_LastLog))
      {
        plLog::Debug("OpenXR discarded the previously begun frame, continuing with the current one ({} discarded so far).", (plInt64)m_iFramesDiscarded);
      }
    }
  }

  m_iPredictedDisplayPeriod = (plInt64)m_FrameState.predictedDisplayPeriod;
  if (!m_FrameState.shouldRender)
  {
    m_iFramesNotRendered.Increment();
  }

  // #TODO_XR Swap chain acquire here?

  UpdatePoses();

  // Moves the extracted instance data of tracked devices (cvar XR.LateLatch) to the controller poses that UpdatePoses
  // just copied from the input thread's newest snapshot.
  plXRLateLatch::Apply(*m_pInput);

  // This will update the extracted view from last frame with the new data we got
  // this frame just before starting to render.
  plView* pView = nullptr;
  if (plRenderWorld::TryGetView(m_hView, pView))
  {
    pView->UpdateViewData(plRenderWorld::GetDataIndexForRendering());

    // The runtime will not display this frame (e.g. the session is visible but not focused, or the headset is off):
    // render nothing. The frame is still ended with zero layers (SubmitXrFrame).
    if (!m_FrameState.shouldRender)
    {
      pView->SkipRenderThisFrame();
    }
  }

  m_bCompanionFrameBegun = false;
  if (m_pCompanion && m_FrameState.shouldRender)
  {
    // Throttled: the companion window is a preview nobody is looking at while the headset is on, and the blit sits on
    // the frame's critical path at full eye resolution. A headset runs at 90-120 Hz; the window does not need to.
    m_pCompanion->CompanionViewBeginFrame(true);
    m_bCompanionFrameBegun = true;
  }
  m_bRenderInProgress = true;
}

void plOpenXR::Present()
{
  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));
  if (!pSwapChain)
    return;

  pSwapChain->PresentRenderTarget();
}

plGALTextureHandle plOpenXR::GetCurrentTexture()
{
  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));
  if (!pSwapChain)
    return plGALTextureHandle();

  return pSwapChain->m_hColorRT;
}

void plOpenXR::SubmitXrFrame()
{
  const plGALOpenXRSwapChain* pSwapChain = static_cast<const plGALOpenXRSwapChain*>(plGALDevice::GetDefaultDevice()->GetSwapChain(m_hSwapChain));

  // Every xrBeginFrame must be matched by an xrEndFrame. Returning without ending would make the next xrBeginFrame
  // report XR_FRAME_DISCARDED, and the frame after that, permanently. A missing swap chain only costs us the layer.
  if (!m_bRenderInProgress)
    return;

  // The layer copies recorded at BeforeEndFrame were submitted by plGALDevice::EndFrame, which OpenXR requires before
  // an image is released.
  ReleaseCompositionLayerImages();

  plHybridArray<XrCompositionLayerBaseHeader*, 8> layers;

  // Storage for the extra layers; it has to stay alive until xrEndFrame. Sized before any pointer into it is taken.
  plHybridArray<XrCompositionLayerQuad, 4> quadLayers;
  plHybridArray<XrCompositionLayerCylinderKHR, 4> cylinderLayers;
  plHybridArray<XrCompositionLayerColorScaleBiasKHR, 8> colorScales;
  XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};

  // Only submit layers if runtime wants us to render (per OpenXR spec)
  if (m_FrameState.shouldRender && pSwapChain != nullptr)
  {
    // The view can render into a sub-rect of the eye texture (XR.RenderScale); the runtime samples only that rect.
    XrExtent2Di imageExtent = {(plInt32)m_Info.m_vEyeRenderTargetSize.width, (plInt32)m_Info.m_vEyeRenderTargetSize.height};
    plView* pView = nullptr;
    if (plRenderWorld::TryGetView(m_hView, pView))
    {
      const plRectFloat& viewport = pView->GetViewport();
      imageExtent.width = plMath::Clamp((plInt32)viewport.width, 1, imageExtent.width);
      imageExtent.height = plMath::Clamp((plInt32)viewport.height, 1, imageExtent.height);
    }

    /// NOTE: (Only Applies When Tracy is Enabled.)Tracy Seems to declare Timers in the same scope, so dual profile macros can throw: '__tracy_scoped_zone' : redefinition; multitple initalization, so we must scope the two events.
    {
      PL_PROFILE_SCOPE("OpenXrEndFrame");
      for (uint32_t i = 0; i < 2; i++)
      {
        m_ProjectionLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        m_ProjectionLayerViews[i].pose = m_Views[i].pose;
        m_ProjectionLayerViews[i].fov = m_Views[i].fov;
        m_ProjectionLayerViews[i].subImage.swapchain = pSwapChain->GetColorSwapchain();
        m_ProjectionLayerViews[i].subImage.imageRect.offset = {0, 0};
        m_ProjectionLayerViews[i].subImage.imageRect.extent = imageExtent;
        m_ProjectionLayerViews[i].subImage.imageArrayIndex = i;

        if (m_Extensions.m_bDepthComposition && m_pCameraToSynchronize)
        {
          m_DepthLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
          m_DepthLayerViews[i].minDepth = 0;
          m_DepthLayerViews[i].maxDepth = 1;
          m_DepthLayerViews[i].nearZ = m_pCameraToSynchronize->GetNearPlane();
          m_DepthLayerViews[i].farZ = m_pCameraToSynchronize->GetFarPlane();
          m_DepthLayerViews[i].subImage.swapchain = pSwapChain->GetDepthSwapchain();
          m_DepthLayerViews[i].subImage.imageRect.offset = {0, 0};
          m_DepthLayerViews[i].subImage.imageRect.extent = imageExtent;
          m_DepthLayerViews[i].subImage.imageArrayIndex = i;

          m_ProjectionLayerViews[i].next = &m_DepthLayerViews[i];
        }
      }
    }

    m_iRenderedEyeWidth = imageExtent.width;
    m_iRenderedEyeHeight = imageExtent.height;

    // Only alpha-blended environments composite against the texture's alpha. Setting the bit for an opaque layer lets any
    // pixel whose alpha ended up below 1 show through on runtimes that honour it. With passthrough active the frame's
    // alpha is coverage over the camera image, and bloom adds to RGB regardless of alpha, so it is unpremultiplied.
    m_Layer.layerFlags = m_BlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
    if (m_bPassthroughActive)
    {
      m_Layer.layerFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    }
    m_Layer.space = GetBaseSpace();
    m_Layer.viewCount = 2;
    m_Layer.views = m_ProjectionLayerViews;

    // The FB passthrough layer is the camera image, so it goes beneath everything else.
    const bool bPassthroughLayer = m_bPassthroughActive && m_bPassthroughUsesFB && m_pPassthroughLayer != XR_NULL_HANDLE;
    if (bPassthroughLayer)
    {
      passthroughLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      passthroughLayer.space = XR_NULL_HANDLE;
      passthroughLayer.layerHandle = m_pPassthroughLayer;
      layers.PushBack(reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthroughLayer));
    }

    // Composition layers in sort order. Equal sort orders keep the order they were added in.
    plHybridArray<const CompositionLayer*, 8> sortedLayers;
    for (const CompositionLayer& layer : m_CompositionLayers)
    {
      if (layer.m_bHasContent && layer.m_Desc.m_fAlpha > 0.0f)
      {
        sortedLayers.PushBack(&layer);
      }
    }
    std::stable_sort(sortedLayers.GetData(), sortedLayers.GetData() + sortedLayers.GetCount(), [](const CompositionLayer* a, const CompositionLayer* b)
      { return a->m_Desc.m_iSortOrder < b->m_Desc.m_iSortOrder; });

    // maxLayerCount includes the projection layer and the passthrough layer. The spec guarantees at least 16.
    const plUInt32 uiMaxLayers = m_Info.m_uiMaxLayerCount > 0 ? m_Info.m_uiMaxLayerCount : 16u;
    const plUInt32 uiReserved = 1u + (bPassthroughLayer ? 1u : 0u);
    const plUInt32 uiBudget = uiMaxLayers > uiReserved ? uiMaxLayers - uiReserved : 0u;
    if (sortedLayers.GetCount() > uiBudget)
    {
      static plTime s_LastLog;
      if (plOpenXRShouldLogThrottled(s_LastLog, plTime::MakeFromSeconds(10.0)))
      {
        plLog::Warning("OpenXR: {} composition layers exceed the runtime's limit of {} layers per frame, the topmost ones are not shown.", sortedLayers.GetCount(), uiMaxLayers);
      }
      sortedLayers.SetCount(uiBudget);
    }

    quadLayers.SetCount(sortedLayers.GetCount());
    cylinderLayers.SetCount(sortedLayers.GetCount());
    colorScales.SetCount(sortedLayers.GetCount());

    bool bProjectionAdded = false;
    for (plUInt32 i = 0; i < sortedLayers.GetCount(); ++i)
    {
      const CompositionLayer& layer = *sortedLayers[i];
      const plXRCompositionLayerDesc& desc = layer.m_Desc;

      const XrSpace space = desc.m_bHeadLocked ? m_pViewSpace : GetBaseSpace();
      if (space == XR_NULL_HANDLE)
        continue;

      // Negative sort orders go beneath the scene.
      if (!bProjectionAdded && desc.m_iSortOrder >= 0)
      {
        layers.PushBack(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_Layer));
        bProjectionAdded = true;
      }

      XrSwapchainSubImage subImage{};
      subImage.swapchain = layer.m_Swapchain;
      subImage.imageRect.offset = {0, 0};
      subImage.imageRect.extent = {(plInt32)layer.m_uiWidth, (plInt32)layer.m_uiHeight};
      subImage.imageArrayIndex = 0;

      // The conversion maps the engine's axes onto OpenXR's (engine +X forward is OpenXR -Z, +Y right is +X, +Z up is
      // +Y). The desc's "faces along -X" therefore lands on OpenXR's "faces +Z" for a quad, and the cylinder's arc,
      // centred on the pose's -Z, sits along the engine's +X with its inside towards the pose, without an extra rotation.
      const XrPosef pose = ConvertTransform(desc.m_Pose);

      // Without XR_KHR_composition_layer_color_scale_bias the fade only reaches 0 (the layer is skipped above).
      const void* pNext = nullptr;
      if (m_Extensions.m_bCompositionLayerColorScaleBias && desc.m_fAlpha < 1.0f)
      {
        // Premultiplied alpha, so the fade scales the colour as well.
        const float fAlpha = plMath::Clamp(desc.m_fAlpha, 0.0f, 1.0f);
        XrCompositionLayerColorScaleBiasKHR& scale = colorScales[i];
        scale = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
        scale.colorScale = {fAlpha, fAlpha, fAlpha, fAlpha};
        scale.colorBias = {0.0f, 0.0f, 0.0f, 0.0f};
        pNext = &scale;
      }

      if (desc.m_Type == plXRCompositionLayerType::Cylinder && m_Extensions.m_bCompositionLayerCylinder)
      {
        XrCompositionLayerCylinderKHR& cylinder = cylinderLayers[i];
        cylinder = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        cylinder.next = pNext;
        cylinder.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        cylinder.space = space;
        cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        cylinder.subImage = subImage;
        cylinder.pose = pose;
        cylinder.radius = plMath::Max(desc.m_fCylinderRadius, 0.01f);
        cylinder.centralAngle = plMath::Clamp(desc.m_CylinderAngle.GetRadian(), 0.001f, 2.0f * plMath::Pi<float>());
        // The arc length divided by the height: OpenXR derives the height from it.
        cylinder.aspectRatio = (cylinder.radius * cylinder.centralAngle) / plMath::Max(desc.m_vSize.y, 0.001f);
        layers.PushBack(reinterpret_cast<XrCompositionLayerBaseHeader*>(&cylinder));
      }
      else
      {
        XrCompositionLayerQuad& quad = quadLayers[i];
        quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        quad.next = pNext;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage = subImage;
        quad.pose = pose;
        quad.size = {plMath::Max(desc.m_vSize.x, 0.001f), plMath::Max(desc.m_vSize.y, 0.001f)};
        layers.PushBack(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
      }
    }

    if (!bProjectionAdded)
    {
      layers.PushBack(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_Layer));
    }
  }

  // Submit the composition layers for the predicted display time.
  // When shouldRender is false, we submit zero layers per OpenXR spec.
  XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
  frameEndInfo.displayTime = m_FrameState.predictedDisplayTime;
  frameEndInfo.environmentBlendMode = m_BlendMode;
  frameEndInfo.layerCount = layers.GetCount();
  frameEndInfo.layers = layers.GetData();

  {
    PL_PROFILE_SCOPE("xrEndFrame");
    const XrResult res = xrEndFrame(m_pSession, &frameEndInfo);
    if (XR_FAILED_RESULT(res))
    {
      static plTime s_LastLog;
      if (plOpenXRShouldLogThrottled(s_LastLog))
      {
        plLog::Error("OpenXR: xrEndFrame failed with {} ({} layers).", res, layers.GetCount());
      }
    }
    else if (layers.GetCount() > 0)
    {
      m_iFramesSubmitted.Increment();
    }
  }

  m_bRenderInProgress = false;
}

void plOpenXR::PreWaitNextFrame()
{
  // Pre-wait for next frame to reduce latency.
  // This allows xrWaitFrame to complete while GPU is still processing,
  // so BeginFrame doesn't have to wait as long.
  if (m_bSessionRunning && !m_hView.IsInvalidated())
  {
    PL_PROFILE_SCOPE("xrWaitFrame_PreWait");
    m_FrameWaitInfo = XrFrameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    m_FrameState = XrFrameState{XR_TYPE_FRAME_STATE};
    m_PreWaitResult = xrWaitFrame(m_pSession, &m_FrameWaitInfo, &m_FrameState);
    m_bPreWaitComplete = true;
    m_iPredictedDisplayTime = XR_FAILED_RESULT(m_PreWaitResult) ? 0 : (plInt64)m_FrameState.predictedDisplayTime;
  }
}

void plOpenXR::EndFrame()
{
  SubmitXrFrame();
  PreWaitNextFrame();
}

void plOpenXR::GALDeviceEventHandler(const plGALDeviceEvent& e)
{
  // Begin frame and end frame need to be encompassing all workload, XR and otherwise as xrWaitFrame will use this time interval to decide when to wake up the application.
  if (e.m_Type == plGALDeviceEvent::Type::BeforeBeginFrame)
  {
    BeginFrame();
  }
  else if (e.m_Type == plGALDeviceEvent::Type::BeforeEndFrame)
  {
    if (m_bRenderInProgress && m_FrameState.shouldRender)
    {
      CopyCompositionLayers();
    }

    if (m_bRenderInProgress && m_bCompanionFrameBegun && m_pCompanion)
    {
      m_pCompanion->CompanionViewEndFrame();
    }
    m_bCompanionFrameBegun = false;
  }
  else if (e.m_Type == plGALDeviceEvent::Type::AfterEndFrame)
  {
    EndFrame();
  }
}

void plOpenXR::GameApplicationEventHandler(const plGameApplicationExecutionEvent& e)
{
  // The handler outlives the instance when the runtime loses it; XR stays idle until Initialize creates a new one.
  if (!IsInitialized())
    return;

  if (e.m_Type == plGameApplicationExecutionEvent::Type::BeginAppTick)
  {
    // Main thread, before the world update task starts and outside any plGALDevice::BeginFrame/EndFrame bracket. The
    // session state machine runs here so xrBeginSession/xrEndSession never land between xrBeginFrame and xrEndFrame,
    // and so a restart can destroy and recreate the swap chain.
    PollEvents();

    // Session-level requests that the runtime only takes outside a frame or while focused.
    if (m_pSession != XR_NULL_HANDLE)
    {
      ApplyPendingDisplayRefreshRate();
      UpdateVisibilityMask();
    }
    DestroyDeadCompositionLayerSwapchains(false);
  }
  else if (e.m_Type == plGameApplicationExecutionEvent::Type::BeforeUpdatePlugins)
  {
    BeforeUpdatePlugins();
  }
}

void plOpenXR::SetStageSpace(plXRStageSpace::Enum space)
{
  m_StageSpace = space;
}

void plOpenXR::SetHMDCamera(plCamera* pCamera)
{
  PL_ASSERT_DEV(IsInitialized(), "Need to call 'Initialize' first.");

  if (m_pCameraToSynchronize == pCamera)
    return;

  m_pCameraToSynchronize = pCamera;
  if (m_pCameraToSynchronize)
  {
    m_uiSettingsModificationCounter = m_pCameraToSynchronize->GetSettingsModificationCounter() + 1;
    m_pCameraToSynchronize->SetCameraMode(plCameraMode::Stereo, m_pCameraToSynchronize->GetFovOrDim(), m_pCameraToSynchronize->GetNearPlane(), m_pCameraToSynchronize->GetFarPlane());
  }
}

plWorld* plOpenXR::GetWorld()
{
  plView* pView = nullptr;
  if (plRenderWorld::TryGetView(m_hView, pView))
  {
    return pView->GetWorld();
  }
  return nullptr;
}

//////////////////////////////////////////////////////////////////////////
// Frame statistics

void plOpenXR::GetFrameStats(plXRFrameStats& out_stats) const
{
  out_stats = {};
  out_stats.m_PredictedDisplayPeriod = plTime::MakeFromNanoseconds((double)(plInt64)m_iPredictedDisplayPeriod);
  out_stats.m_uiFramesSubmitted = (plUInt64)(plInt64)m_iFramesSubmitted;
  out_stats.m_uiFramesNotRendered = (plUInt64)(plInt64)m_iFramesNotRendered;
  out_stats.m_uiFramesDiscarded = (plUInt64)(plInt64)m_iFramesDiscarded;

  const plUInt32 uiWidth = (plUInt32)(plInt32)m_iRenderedEyeWidth;
  const plUInt32 uiHeight = (plUInt32)(plInt32)m_iRenderedEyeHeight;
  out_stats.m_vRenderedEyeSize = {uiWidth, uiHeight};

  // XR.RenderScale shrinks the rendered rect per axis, so the ratio of the rect to the eye texture is the scale in use.
  const plUInt32 uiEyeWidth = m_Info.m_vEyeRenderTargetSize.width;
  out_stats.m_fRenderScale = (uiEyeWidth > 0 && uiWidth > 0) ? (float)uiWidth / (float)uiEyeWidth : 1.0f;
}

//////////////////////////////////////////////////////////////////////////
// Display refresh rate

void plOpenXR::InitDisplayRefreshRate()
{
  m_Info.m_DisplayRefreshRates.Clear();
  m_Info.m_fDisplayRefreshRate = 0.0f;

  if (!m_Extensions.m_bDisplayRefreshRate || m_pSession == XR_NULL_HANDLE)
    return;

  plUInt32 uiCount = 0;
  if (!XR_FAILED_RESULT(m_Extensions.pfn_xrEnumerateDisplayRefreshRatesFB(m_pSession, 0, &uiCount, nullptr)) && uiCount > 0)
  {
    plHybridArray<float, 8> rates;
    rates.SetCount(uiCount);
    if (!XR_FAILED_RESULT(m_Extensions.pfn_xrEnumerateDisplayRefreshRatesFB(m_pSession, uiCount, &uiCount, rates.GetData())))
    {
      rates.SetCount(uiCount);
      for (float fRate : rates)
      {
        m_Info.m_DisplayRefreshRates.PushBack(fRate);
      }
    }
  }

  float fCurrent = 0.0f;
  if (!XR_FAILED_RESULT(m_Extensions.pfn_xrGetDisplayRefreshRateFB(m_pSession, &fCurrent)))
  {
    m_Info.m_fDisplayRefreshRate = fCurrent;
  }

  plStringBuilder sRates;
  for (float fRate : m_Info.m_DisplayRefreshRates)
  {
    sRates.AppendFormat(sRates.IsEmpty() ? "{}" : ", {}", fRate);
  }
  plLog::Info("OpenXR: Display refresh rate {} Hz, supported: {}.", m_Info.m_fDisplayRefreshRate, sRates.IsEmpty() ? plStringView("unknown") : sRates.GetView());
}

plResult plOpenXR::RequestDisplayRefreshRate(float fRefreshRateHz)
{
  if (!m_Extensions.m_bDisplayRefreshRate || fRefreshRateHz < 0.0f)
    return PL_FAILURE;

  float fRate = fRefreshRateHz;
  if (fRate > 0.0f && !m_Info.m_DisplayRefreshRates.IsEmpty())
  {
    // Snap to the enumerated value: runtimes compare the request exactly, and 72 vs 72.0001 is a rejection.
    bool bFound = false;
    for (float fSupported : m_Info.m_DisplayRefreshRates)
    {
      if (plMath::Abs(fSupported - fRate) < 0.5f)
      {
        fRate = fSupported;
        bFound = true;
        break;
      }
    }

    if (!bFound)
    {
      plLog::Warning("OpenXR: Display refresh rate {} Hz is not supported by this headset.", fRefreshRateHz);
      return PL_FAILURE;
    }
  }

  // Sent at the next BeginAppTick while the session is running and focused, see ApplyPendingDisplayRefreshRate.
  m_fRequestedRefreshRate = fRate;
  m_bHasRefreshRateRequest = true;
  m_bRefreshRateRequestPending = true;
  return PL_SUCCESS;
}

void plOpenXR::ApplyPendingDisplayRefreshRate()
{
  if (!m_bRefreshRateRequestPending || !m_Extensions.m_bDisplayRefreshRate)
    return;

  if (!m_bSessionRunning || m_SessionFocus != plXRSessionFocus::Focused)
    return;

  m_bRefreshRateRequestPending = false;

  const XrResult res = m_Extensions.pfn_xrRequestDisplayRefreshRateFB(m_pSession, m_fRequestedRefreshRate);
  if (XR_FAILED_RESULT(res))
  {
    plLog::Warning("OpenXR: The runtime rejected display refresh rate {} Hz: {}", m_fRequestedRefreshRate, res);
  }
  else
  {
    // The rate in plHMDInfo changes with XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, once it took effect.
    plLog::Info("OpenXR: Requested display refresh rate {} Hz.", m_fRequestedRefreshRate);
  }
}

//////////////////////////////////////////////////////////////////////////
// Visibility mask

void plOpenXR::UpdateVisibilityMask()
{
  if (!m_bVisibilityMaskDirty || !m_Extensions.m_bVisibilityMask || m_pSession == XR_NULL_HANDLE)
    return;

  // The mesh is converted with the FOV of a real xrLocateViews, and only matters once the eye swapchain exists.
  if (!m_bViewFovValid || m_hSwapChain.IsInvalidated())
    return;

  PL_PROFILE_SCOPE("UpdateVisibilityMask");
  m_bVisibilityMaskDirty = false;

  plDynamicArray<plVec2> eyeTriangles[2];

  for (plUInt32 uiEye : {0u, 1u})
  {
    XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
    XrResult res = m_Extensions.pfn_xrGetVisibilityMaskKHR(m_pSession, m_PrimaryViewConfigurationType, uiEye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
    if (XR_FAILED_RESULT(res))
    {
      plLog::Warning("OpenXR: xrGetVisibilityMaskKHR failed for eye {}: {}", uiEye, res);
      continue;
    }

    if (mask.vertexCountOutput == 0 || mask.indexCountOutput == 0)
      continue;

    plDynamicArray<XrVector2f> vertices;
    plDynamicArray<plUInt32> indices;
    vertices.SetCount(mask.vertexCountOutput);
    indices.SetCount(mask.indexCountOutput);

    mask.vertexCapacityInput = vertices.GetCount();
    mask.vertices = vertices.GetData();
    mask.indexCapacityInput = indices.GetCount();
    mask.indices = indices.GetData();
    res = m_Extensions.pfn_xrGetVisibilityMaskKHR(m_pSession, m_PrimaryViewConfigurationType, uiEye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);
    if (XR_FAILED_RESULT(res))
    {
      plLog::Warning("OpenXR: xrGetVisibilityMaskKHR failed for eye {}: {}", uiEye, res);
      continue;
    }

    // The vertices lie on the view-space plane z = -1 (x right, y up), so they are tangents of the view angles. The
    // eye's projection maps tangent t on [tanLeft, tanRight] to [-1, 1] (likewise for y): see UpdateCamera, which builds
    // it with plClipSpaceYMode::Regular on both renderers, so NDC +y is the top of the view and no flip is needed.
    const XrFovf& fov = m_Views[uiEye].fov;
    const float fTanLeft = plMath::Tan(plAngle::MakeFromRadian(fov.angleLeft));
    const float fTanRight = plMath::Tan(plAngle::MakeFromRadian(fov.angleRight));
    const float fTanDown = plMath::Tan(plAngle::MakeFromRadian(fov.angleDown));
    const float fTanUp = plMath::Tan(plAngle::MakeFromRadian(fov.angleUp));
    if (fTanRight - fTanLeft <= 0.0001f || fTanUp - fTanDown <= 0.0001f)
      continue;

    auto ToNdc = [&](const XrVector2f& v) -> plVec2
    {
      return plVec2((2.0f * v.x - (fTanRight + fTanLeft)) / (fTanRight - fTanLeft), (2.0f * v.y - (fTanUp + fTanDown)) / (fTanUp - fTanDown));
    };

    const plUInt32 uiVertexCount = mask.vertexCountOutput;
    const plUInt32 uiIndexCount = mask.indexCountOutput - (mask.indexCountOutput % 3);
    eyeTriangles[uiEye].Reserve(uiIndexCount);
    for (plUInt32 i = 0; i < uiIndexCount; i += 3)
    {
      if (indices[i] >= uiVertexCount || indices[i + 1] >= uiVertexCount || indices[i + 2] >= uiVertexCount)
        continue;

      eyeTriangles[uiEye].PushBack(ToNdc(vertices[indices[i]]));
      eyeTriangles[uiEye].PushBack(ToNdc(vertices[indices[i + 1]]));
      eyeTriangles[uiEye].PushBack(ToNdc(vertices[indices[i + 2]]));
    }
  }

  if (eyeTriangles[0].IsEmpty() && eyeTriangles[1].IsEmpty())
  {
    if (m_bVisibilityMaskSet)
    {
      plXRVisibilityMask::ClearMask();
      m_bVisibilityMaskSet = false;
    }
    plLog::Info("OpenXR: The runtime reports no hidden area for this headset.");
    return;
  }

  plXRVisibilityMask::SetMask(eyeTriangles[0].GetArrayPtr(), eyeTriangles[1].GetArrayPtr());
  m_bVisibilityMaskSet = true;
  plLog::Info("OpenXR: Visibility mask set, {} and {} hidden triangles.", eyeTriangles[0].GetCount() / 3, eyeTriangles[1].GetCount() / 3);
}

//////////////////////////////////////////////////////////////////////////
// Passthrough

void plOpenXR::InitPassthrough()
{
  if (!m_bFBPassthroughSupported || m_pSession == XR_NULL_HANDLE)
    return;

  // Created paused. ApplyPassthroughState starts it while SetPassthroughEnabled(true) is in effect.
  XrPassthroughCreateInfoFB passthroughInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  passthroughInfo.flags = 0;
  XrResult res = m_Extensions.pfn_xrCreatePassthroughFB(m_pSession, &passthroughInfo, &m_pPassthrough);
  if (XR_FAILED_RESULT(res))
  {
    plLog::Warning("OpenXR: xrCreatePassthroughFB failed: {}. Passthrough falls back to alpha-blend composition where available.", res);
    m_pPassthrough = XR_NULL_HANDLE;
    return;
  }

  XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  layerInfo.passthrough = m_pPassthrough;
  layerInfo.flags = 0;
  layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
  res = m_Extensions.pfn_xrCreatePassthroughLayerFB(m_pSession, &layerInfo, &m_pPassthroughLayer);
  if (XR_FAILED_RESULT(res))
  {
    plLog::Warning("OpenXR: xrCreatePassthroughLayerFB failed: {}.", res);
    m_pPassthroughLayer = XR_NULL_HANDLE;
    DeinitPassthrough();
  }
}

void plOpenXR::DeinitPassthrough()
{
  if (m_pPassthroughLayer != XR_NULL_HANDLE)
  {
    XR_LOG_ERROR(m_Extensions.pfn_xrDestroyPassthroughLayerFB(m_pPassthroughLayer));
    m_pPassthroughLayer = XR_NULL_HANDLE;
  }

  if (m_pPassthrough != XR_NULL_HANDLE)
  {
    XR_LOG_ERROR(m_Extensions.pfn_xrDestroyPassthroughFB(m_pPassthrough));
    m_pPassthrough = XR_NULL_HANDLE;
  }

  m_bPassthroughUsesFB = false;
}

plResult plOpenXR::SetPassthroughEnabled(bool bEnabled)
{
  if (bEnabled && !m_bFBPassthroughSupported && !m_bAlphaBlendAvailable)
    return PL_FAILURE;

  m_bPassthroughRequested = bEnabled;

  // Without a session the request is applied when one starts (InitSession).
  ApplyPassthroughState();

  if (m_pSession != XR_NULL_HANDLE && bEnabled && !m_bPassthroughActive)
    return PL_FAILURE;

  return PL_SUCCESS;
}

void plOpenXR::ApplyPassthroughState()
{
  if (m_pSession == XR_NULL_HANDLE)
    return;

  if (!m_bPassthroughRequested)
  {
    if (m_bPassthroughUsesFB)
    {
      XR_LOG_ERROR(m_Extensions.pfn_xrPassthroughLayerPauseFB(m_pPassthroughLayer));
      XR_LOG_ERROR(m_Extensions.pfn_xrPassthroughPauseFB(m_pPassthrough));
      m_bPassthroughUsesFB = false;
    }

    m_BlendMode = m_ConfiguredBlendMode;
    SetPassthroughActive(false);
    return;
  }

  if (m_bPassthroughActive)
    return;

  // XR_FB_passthrough first: Quest reports opaque composition and shows the camera only through this layer.
  if (m_pPassthroughLayer != XR_NULL_HANDLE)
  {
    const XrResult startRes = m_Extensions.pfn_xrPassthroughStartFB(m_pPassthrough);
    const XrResult resumeRes = XR_FAILED_RESULT(startRes) ? startRes : m_Extensions.pfn_xrPassthroughLayerResumeFB(m_pPassthroughLayer);
    if (!XR_FAILED_RESULT(startRes) && !XR_FAILED_RESULT(resumeRes))
    {
      m_bPassthroughUsesFB = true;
      m_BlendMode = m_ConfiguredBlendMode;
      SetPassthroughActive(true);
      return;
    }

    plLog::Warning("OpenXR: Starting FB passthrough failed (start {}, resume {}).", startRes, resumeRes);
    if (!XR_FAILED_RESULT(startRes))
    {
      XR_LOG_ERROR(m_Extensions.pfn_xrPassthroughPauseFB(m_pPassthrough));
    }
  }

  // Runtimes with alpha-blend composition show the real world wherever the frame's alpha is below 1.
  if (m_bAlphaBlendAvailable)
  {
    m_BlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    m_bPassthroughUsesFB = false;
    SetPassthroughActive(true);
    return;
  }

  plLog::Warning("OpenXR: Passthrough was requested, but this runtime offers neither XR_FB_passthrough nor alpha-blend composition.");
}

void plOpenXR::SetPassthroughActive(bool bActive)
{
  if (m_bPassthroughActive == bActive)
    return;

  m_bPassthroughActive = bActive;

  // The compositor blends the camera image by alpha, so the renderer writes coverage there while passthrough shows.
  plXRScreenEffects::SetPassthroughAlpha(bActive);

  plXRSessionEvent e;
  e.m_Type = plXRSessionEvent::Type::PassthroughStateChanged;
  e.m_Focus = m_SessionFocus;
  e.m_fRefreshRate = m_Info.m_fDisplayRefreshRate;
  e.m_bPassthroughActive = bActive;
  m_SessionEvents.Broadcast(e);
}

//////////////////////////////////////////////////////////////////////////
// Composition layers

// The native formats a layer swapchain can use for a source texture, best first. A copy needs the same texel layout, so
// only the format itself and its sRGB / UNORM twin qualify.
static plHybridArray<int64_t, 2> plOpenXR_GetLayerFormatCandidates(plGALResourceFormat::Enum format, plOpenXR::GraphicsApi api)
{
  const bool bD3D12 = api == plOpenXR::GraphicsApi::D3D12;
  plHybridArray<int64_t, 2> candidates;

  switch (format)
  {
    case plGALResourceFormat::RGBAUByteNormalizedsRGB:
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : (int64_t)VK_FORMAT_R8G8B8A8_SRGB);
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_UNORM);
      break;
    case plGALResourceFormat::RGBAUByteNormalized:
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_UNORM);
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : (int64_t)VK_FORMAT_R8G8B8A8_SRGB);
      break;
    case plGALResourceFormat::BGRAUByteNormalizedsRGB:
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB);
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM);
      break;
    case plGALResourceFormat::BGRAUByteNormalized:
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM);
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB);
      break;
    case plGALResourceFormat::RGBAHalf:
      candidates.PushBack(bD3D12 ? (int64_t)DXGI_FORMAT_R16G16B16A16_FLOAT : (int64_t)VK_FORMAT_R16G16B16A16_SFLOAT);
      break;
    default:
      break;
  }

  return candidates;
}

bool plOpenXR::SupportsCompositionLayerType(plXRCompositionLayerType::Enum type) const
{
  if (!m_bCompositionLayersEnabled || m_pSession == XR_NULL_HANDLE)
    return false;

  switch (type)
  {
    case plXRCompositionLayerType::Quad:
      return true;
    case plXRCompositionLayerType::Cylinder:
      return m_Extensions.m_bCompositionLayerCylinder;
    default:
      return false;
  }
}

XrResult plOpenXR::CreateCompositionLayerSwapchain(CompositionLayer& layer, plUInt32 uiWidth, plUInt32 uiHeight, plGALResourceFormat::Enum sourceFormat)
{
  if (m_SwapchainFormats.IsEmpty())
  {
    plUInt32 uiCount = 0;
    XR_SUCCEED_OR_RETURN_LOG(xrEnumerateSwapchainFormats(m_pSession, 0, &uiCount, nullptr));
    m_SwapchainFormats.SetCount(uiCount);
    XR_SUCCEED_OR_RETURN_LOG(xrEnumerateSwapchainFormats(m_pSession, uiCount, &uiCount, m_SwapchainFormats.GetData()));
    m_SwapchainFormats.SetCount(uiCount);
  }

  int64_t nativeFormat = 0;
  for (int64_t candidate : plOpenXR_GetLayerFormatCandidates(sourceFormat, m_GraphicsApi))
  {
    if (m_SwapchainFormats.Contains(candidate))
    {
      nativeFormat = candidate;
      break;
    }
  }

  if (nativeFormat == 0)
  {
    plLog::Warning("OpenXR: The runtime has no swapchain format matching the composition layer's source texture format ({}). Use an RGBA8, BGRA8 or RGBA16F texture.", (int)sourceFormat);
    return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
  }

  XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  createInfo.format = nativeFormat;
  createInfo.sampleCount = 1;
  createInfo.width = uiWidth;
  createInfo.height = uiHeight;
  createInfo.faceCount = 1;
  createInfo.arraySize = 1;
  createInfo.mipCount = 1;
  XR_SUCCEED_OR_RETURN_LOG(xrCreateSwapchain(m_pSession, &createInfo, &layer.m_Swapchain));

  plUInt32 uiImageCount = 0;
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateSwapchainImages(layer.m_Swapchain, 0, &uiImageCount, nullptr));

  XrSwapchainImageBaseHeader* pImages = nullptr;
  if (m_GraphicsApi == GraphicsApi::D3D12)
  {
    layer.m_ImagesD3D12.SetCount(uiImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    pImages = reinterpret_cast<XrSwapchainImageBaseHeader*>(layer.m_ImagesD3D12.GetData());
  }
  else
  {
    layer.m_ImagesVulkan.SetCount(uiImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    pImages = reinterpret_cast<XrSwapchainImageBaseHeader*>(layer.m_ImagesVulkan.GetData());
  }
  XR_SUCCEED_OR_RETURN_LOG(xrEnumerateSwapchainImages(layer.m_Swapchain, uiImageCount, &uiImageCount, pImages));

  // Wrapped like the eye images (see plGALOpenXRSwapChain::CreateSwapchainImages): a render target around the
  // runtime-owned resource, which therefore rests in the render-target state the runtime hands it over in.
  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  const plGALResourceFormat::Enum galFormat = ConvertTextureFormat(nativeFormat, m_GraphicsApi);
  for (plUInt32 i = 0; i < uiImageCount; ++i)
  {
    void* pNativeImage = m_GraphicsApi == GraphicsApi::D3D12 ? static_cast<void*>(layer.m_ImagesD3D12[i].texture) : (void*)(plUInt64)layer.m_ImagesVulkan[i].image;

    plGALTextureCreationDescription textureDesc;
    textureDesc.SetAsRenderTarget(uiWidth, uiHeight, galFormat);
    textureDesc.m_pExisitingNativeObject = pNativeImage;

    const plGALTextureHandle hImage = pDevice->CreateTexture(textureDesc);
    if (hImage.IsInvalidated())
    {
      plLog::Error("OpenXR: Wrapping a composition layer swapchain image failed.");
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    layer.m_Images.PushBack(hImage);
  }

  layer.m_uiWidth = uiWidth;
  layer.m_uiHeight = uiHeight;
  layer.m_Format = sourceFormat;
  layer.m_bHasContent = false;
  return XR_SUCCESS;
}

void plOpenXR::ReleaseCompositionLayerResources(CompositionLayer& layer)
{
  if (layer.m_bAcquired && layer.m_Swapchain != XR_NULL_HANDLE)
  {
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_LOG_ERROR(xrReleaseSwapchainImage(layer.m_Swapchain, &releaseInfo));
  }
  layer.m_bAcquired = false;
  layer.m_bHasContent = false;

  if (plGALDevice::HasDefaultDevice())
  {
    plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
    for (plGALTextureHandle hImage : layer.m_Images)
    {
      pDevice->DestroyTexture(hImage);
    }

    // A copy into one of the images may still be in flight, so the swapchain outlives this frame on the GPU.
    if (layer.m_Swapchain != XR_NULL_HANDLE)
    {
      m_DeadLayerSwapchains.PushBack({layer.m_Swapchain, pDevice->GetCurrentFrame()});
      layer.m_Swapchain = XR_NULL_HANDLE;
    }
  }
  else if (layer.m_Swapchain != XR_NULL_HANDLE)
  {
    xrDestroySwapchain(layer.m_Swapchain);
    layer.m_Swapchain = XR_NULL_HANDLE;
  }

  layer.m_Images.Clear();
  layer.m_ImagesD3D12.Clear();
  layer.m_ImagesVulkan.Clear();
}

void plOpenXR::DestroyDeadCompositionLayerSwapchains(bool bForce)
{
  if (m_DeadLayerSwapchains.IsEmpty())
    return;

  const plUInt64 uiSafeFrame = plGALDevice::HasDefaultDevice() ? plGALDevice::GetDefaultDevice()->GetSafeFrame() : 0;
  for (plUInt32 i = m_DeadLayerSwapchains.GetCount(); i-- > 0;)
  {
    // GetSafeFrame passing the retire frame means the GPU finished every copy into the swapchain's images, and the GAL
    // has released its wrappers of them.
    if (bForce || uiSafeFrame > m_DeadLayerSwapchains[i].m_uiFrame)
    {
      xrDestroySwapchain(m_DeadLayerSwapchains[i].m_Swapchain);
      m_DeadLayerSwapchains.RemoveAtAndSwap(i);
    }
  }
}

void plOpenXR::DestroyAllCompositionLayers()
{
  if (m_CompositionLayers.IsEmpty() && m_DeadLayerSwapchains.IsEmpty())
    return;

  for (CompositionLayer& layer : m_CompositionLayers)
  {
    ReleaseCompositionLayerResources(layer);
  }
  m_CompositionLayers.Clear();

  // Same constraint as DestroySessionResources: the GAL wrappers have to be gone before the swapchains, and the
  // swapchains before the session.
  if (plGALDevice::HasDefaultDevice())
  {
    plGALDevice::GetDefaultDevice()->WaitIdle();
  }
  DestroyDeadCompositionLayerSwapchains(true);
}

plXRCompositionLayerHandle plOpenXR::AddCompositionLayer(const plXRCompositionLayerDesc& desc)
{
  if (!SupportsCompositionLayerType(desc.m_Type))
  {
    plLog::Warning("OpenXR: Composition layer type {} is not available (cvars XR.CompositionLayers / XR.CylinderLayers, a running session and runtime support are needed).", (int)desc.m_Type.GetValue());
    return plInvalidXRCompositionLayer;
  }

  const plUInt32 uiMaxLayers = m_Info.m_uiMaxLayerCount > 0 ? m_Info.m_uiMaxLayerCount : 16u;
  const plUInt32 uiReserved = 1u + (m_bFBPassthroughSupported ? 1u : 0u);
  if (m_CompositionLayers.GetCount() + uiReserved >= uiMaxLayers)
  {
    plLog::Warning("OpenXR: The runtime allows {} composition layers per frame, which are all in use.", uiMaxLayers);
    return plInvalidXRCompositionLayer;
  }

  const plGALTexture* pTexture = plGALDevice::GetDefaultDevice()->GetTexture(desc.m_hTexture);
  if (pTexture == nullptr)
  {
    plLog::Warning("OpenXR: The composition layer's texture is invalid.");
    return plInvalidXRCompositionLayer;
  }

  const plGALTextureCreationDescription& textureDesc = pTexture->GetDescription();
  if (textureDesc.m_Type != plGALTextureType::Texture2D || textureDesc.m_SampleCount != plGALMSAASampleCount::None)
  {
    plLog::Warning("OpenXR: A composition layer needs a single-sample 2D texture.");
    return plInvalidXRCompositionLayer;
  }

  CompositionLayer& layer = m_CompositionLayers.ExpandAndGetRef();
  if (XR_FAILED_RESULT(CreateCompositionLayerSwapchain(layer, textureDesc.m_uiWidth, textureDesc.m_uiHeight, textureDesc.m_Format)))
  {
    ReleaseCompositionLayerResources(layer);
    m_CompositionLayers.PopBack();
    return plInvalidXRCompositionLayer;
  }

  layer.m_Desc = desc;
  layer.m_hHandle = m_uiNextLayerHandle++;
  if (m_uiNextLayerHandle == plInvalidXRCompositionLayer)
  {
    m_uiNextLayerHandle = 1;
  }

  return layer.m_hHandle;
}

plResult plOpenXR::UpdateCompositionLayer(plXRCompositionLayerHandle hLayer, const plXRCompositionLayerDesc& desc)
{
  CompositionLayer* pLayer = nullptr;
  for (CompositionLayer& layer : m_CompositionLayers)
  {
    if (layer.m_hHandle == hLayer)
    {
      pLayer = &layer;
      break;
    }
  }

  if (pLayer == nullptr || !SupportsCompositionLayerType(desc.m_Type))
    return PL_FAILURE;

  if (desc.m_hTexture != pLayer->m_Desc.m_hTexture)
  {
    const plGALTexture* pTexture = plGALDevice::GetDefaultDevice()->GetTexture(desc.m_hTexture);
    if (pTexture == nullptr)
      return PL_FAILURE;

    const plGALTextureCreationDescription& textureDesc = pTexture->GetDescription();
    if (textureDesc.m_Type != plGALTextureType::Texture2D || textureDesc.m_SampleCount != plGALMSAASampleCount::None)
      return PL_FAILURE;

    // A texture of another size or format needs a new swapchain. The old one stays in use until the new one exists.
    if (textureDesc.m_uiWidth != pLayer->m_uiWidth || textureDesc.m_uiHeight != pLayer->m_uiHeight || textureDesc.m_Format != pLayer->m_Format)
    {
      CompositionLayer replacement;
      if (XR_FAILED_RESULT(CreateCompositionLayerSwapchain(replacement, textureDesc.m_uiWidth, textureDesc.m_uiHeight, textureDesc.m_Format)))
      {
        ReleaseCompositionLayerResources(replacement);
        return PL_FAILURE;
      }

      ReleaseCompositionLayerResources(*pLayer);
      replacement.m_hHandle = pLayer->m_hHandle;
      *pLayer = std::move(replacement);
    }
  }

  pLayer->m_Desc = desc;
  return PL_SUCCESS;
}

void plOpenXR::RemoveCompositionLayer(plXRCompositionLayerHandle hLayer)
{
  for (plUInt32 i = 0; i < m_CompositionLayers.GetCount(); ++i)
  {
    if (m_CompositionLayers[i].m_hHandle == hLayer)
    {
      ReleaseCompositionLayerResources(m_CompositionLayers[i]);
      // Keeps the order of the remaining layers, which breaks ties between equal sort orders.
      m_CompositionLayers.RemoveAtAndCopy(i);
      return;
    }
  }
}

void plOpenXR::CopyCompositionLayers()
{
  if (m_CompositionLayers.IsEmpty())
    return;

  PL_PROFILE_SCOPE("CopyCompositionLayers");

  plGALDevice* pDevice = plGALDevice::GetDefaultDevice();
  plGALCommandEncoder* pEncoder = nullptr;

  for (CompositionLayer& layer : m_CompositionLayers)
  {
    if (layer.m_Swapchain == XR_NULL_HANDLE || layer.m_Images.IsEmpty() || layer.m_bAcquired)
      continue;

    // A texture destroyed by game code keeps the last image it produced on screen.
    const plGALTexture* pSource = pDevice->GetTexture(layer.m_Desc.m_hTexture);
    if (pSource == nullptr)
      continue;

    const plGALTextureCreationDescription& sourceDesc = pSource->GetDescription();
    if (sourceDesc.m_uiWidth != layer.m_uiWidth || sourceDesc.m_uiHeight != layer.m_uiHeight || sourceDesc.m_Format != layer.m_Format)
      continue;

    plUInt32 uiImageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED_RESULT(xrAcquireSwapchainImage(layer.m_Swapchain, &acquireInfo, &uiImageIndex)) || uiImageIndex >= layer.m_Images.GetCount())
      continue;

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (xrWaitSwapchainImage(layer.m_Swapchain, &waitInfo) != XR_SUCCESS)
    {
      XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(layer.m_Swapchain, &releaseInfo);
      continue;
    }

    layer.m_bAcquired = true;

    if (pEncoder == nullptr)
    {
      pEncoder = pDevice->BeginCommands("XR Composition Layers");
    }

    // A copy rather than a blit: the swapchain has the source's format and size (CreateCompositionLayerSwapchain), so no
    // shader is involved. Both textures leave their rest states for the copy and return to them, which for the
    // swapchain image is the render-target state the runtime expects on release.
    const plGALTextureHandle hDestination = layer.m_Images[uiImageIndex];
    const plBitflags<plGALResourceState> sourceRestState = sourceDesc.GetDefaultState();
    const plBitflags<plGALResourceState> destinationRestState = pDevice->GetTexture(hDestination)->GetDescription().GetDefaultState();

    pEncoder->TextureBarrier(layer.m_Desc.m_hTexture, sourceRestState, plGALResourceState::CopySource);
    pEncoder->TextureBarrier(hDestination, destinationRestState, plGALResourceState::CopyDestination);

    plGALTextureSubresource subresource;
    subresource.m_uiMipLevel = 0;
    subresource.m_uiArraySlice = 0;
    const plBoundingBoxu32 box = plBoundingBoxu32::MakeFromMinMax(plVec3U32(0, 0, 0), plVec3U32(layer.m_uiWidth, layer.m_uiHeight, 1));
    pEncoder->CopyTextureRegion(hDestination, subresource, plVec3U32(0, 0, 0), layer.m_Desc.m_hTexture, subresource, box);

    pEncoder->TextureBarrier(layer.m_Desc.m_hTexture, plGALResourceState::CopySource, sourceRestState);
    pEncoder->TextureBarrier(hDestination, plGALResourceState::CopyDestination, destinationRestState);
  }

  if (pEncoder != nullptr)
  {
    pDevice->EndCommands(pEncoder);
  }
}

void plOpenXR::ReleaseCompositionLayerImages()
{
  for (CompositionLayer& layer : m_CompositionLayers)
  {
    if (!layer.m_bAcquired)
      continue;

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_LOG_ERROR(xrReleaseSwapchainImage(layer.m_Swapchain, &releaseInfo));
    layer.m_bAcquired = false;
    layer.m_bHasContent = true;
  }
}

XrPosef plOpenXR::ConvertTransform(const plTransform& tr)
{
  XrPosef pose;
  pose.orientation = ConvertOrientation(tr.m_qRotation);
  pose.position = ConvertPosition(tr.m_vPosition);
  return pose;
}

XrQuaternionf plOpenXR::ConvertOrientation(const plQuat& q)
{
  return {q.y, q.z, -q.x, -q.w};
}

XrVector3f plOpenXR::ConvertPosition(const plVec3& vPos)
{
  return {vPos.y, vPos.z, -vPos.x};
}

plQuat plOpenXR::ConvertOrientation(const XrQuaternionf& q)
{
  return {-q.z, q.x, q.y, -q.w};
}

plVec3 plOpenXR::ConvertPosition(const XrVector3f& pos)
{
  return {-pos.z, pos.x, pos.y};
}

plMat4 plOpenXR::ConvertPoseToMatrix(const XrPosef& pose)
{
  plMat4 m;
  plMat3 rot = ConvertOrientation(pose.orientation).GetAsMat3();
  plVec3 pos = ConvertPosition(pose.position);
  m.SetTransformationMatrix(rot, pos);
  return m;
}

plGALResourceFormat::Enum plOpenXR::ConvertTextureFormat(int64_t format, GraphicsApi api)
{
  // Swap chain formats are in the session graphics API's own enumeration, and the VkFormat and DXGI_FORMAT ranges overlap.
  if (api == GraphicsApi::D3D12)
  {
    switch (static_cast<DXGI_FORMAT>(format))
    {
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return plGALResourceFormat::RGBAUByteNormalizedsRGB;
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return plGALResourceFormat::BGRAUByteNormalizedsRGB;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        return plGALResourceFormat::RGBAUByteNormalized;
      case DXGI_FORMAT_B8G8R8A8_UNORM:
        return plGALResourceFormat::BGRAUByteNormalized;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return plGALResourceFormat::RGBAHalf;
      case DXGI_FORMAT_D32_FLOAT:
        return plGALResourceFormat::DFloat;
      case DXGI_FORMAT_D16_UNORM:
        return plGALResourceFormat::D16;
      case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return plGALResourceFormat::D24S8;
      default:
        plLog::Warning("Unknown DXGI format {0}, defaulting to RGBAUByteNormalized", (int)format);
        return plGALResourceFormat::RGBAUByteNormalized;
    }
  }

  switch (static_cast<VkFormat>(format))
  {
    case VK_FORMAT_R8G8B8A8_SRGB:
      return plGALResourceFormat::RGBAUByteNormalizedsRGB;
    case VK_FORMAT_B8G8R8A8_SRGB:
      return plGALResourceFormat::BGRAUByteNormalizedsRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
      return plGALResourceFormat::RGBAUByteNormalized;
    case VK_FORMAT_B8G8R8A8_UNORM:
      return plGALResourceFormat::BGRAUByteNormalized;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
      return plGALResourceFormat::RGBAHalf;
    case VK_FORMAT_D32_SFLOAT:
      return plGALResourceFormat::DFloat;
    case VK_FORMAT_D16_UNORM:
      return plGALResourceFormat::D16;
    case VK_FORMAT_D24_UNORM_S8_UINT:
      return plGALResourceFormat::D24S8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return plGALResourceFormat::DFloat; // Closest match
    default:
      plLog::Warning("Unknown Vulkan format {0}, defaulting to RGBAUByteNormalized", (int)format);
      return plGALResourceFormat::RGBAUByteNormalized;
  }
}
