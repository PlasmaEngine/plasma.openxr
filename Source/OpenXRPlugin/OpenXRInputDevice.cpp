#include <OpenXRPlugin/OpenXRPluginPCH.h>

#include <Core/Input/ActionMap/InputActionMap.h>
#include <Core/Input/ActionMap/InputGlyphs.h>
#include <Core/Input/InputManager.h>
#include <Foundation/Configuration/CVar.h>
#include <Foundation/Math/Math.h>
#include <Foundation/Profiling/Profiling.h>
#include <Foundation/Threading/ThreadUtils.h>
#include <OpenXRPlugin/OpenXRDeclarations.h>
#include <OpenXRPlugin/OpenXRInputDevice.h>
#include <OpenXRPlugin/OpenXRSingleton.h>

// Off by default: SteamVR's XR_KHR_locate_spaces implementation faults inside vrclient_x64.dll on a well-formed
// xrLocateSpacesKHR call. The per-space xrLocateSpace path below is the conventional one and costs two extra calls per
// hand per input tick, which is not measurable. Turn this on only to test a runtime's batched path.
PL_DEFINE_AS_POD_TYPE(XrControllerModelNodePropertiesMSFT);
PL_DEFINE_AS_POD_TYPE(XrRenderModelPathInfoFB);

plCVarBool cvar_XrBatchedSpaceLocation("XR.BatchedSpaceLocation", false, plCVarFlags::Save, "Use xrLocateSpacesKHR to locate both hand spaces in one call.");

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plOpenXRInputDevice, 1, plRTTINoAllocator);
// no properties or message handlers
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on


// Defined in OpenXRSingleton.cpp, which requests the extensions. Latched again at session start.
extern plCVarBool cvar_XrBindingModification;
extern plCVarBool cvar_XrDpadBinding;
extern plCVarBool cvar_XrPalmPose;

namespace
{
  struct BuiltInAction
  {
    plXRDeviceFeatures::Enum m_Feature;
    const char* m_szName;
    XrActionType m_Type;
  };

  // The actions behind the 'xr_hand_<side>_<name>' input slots. XRInteractionProfiles.ddl binds them by name. The grip
  // pose must stay the first pose action: the input thread asks it whether a controller is active.
  constexpr BuiltInAction s_BuiltInActions[] = {
    {plXRDeviceFeatures::Trigger, "trigger_value", XR_ACTION_TYPE_FLOAT_INPUT},
    {plXRDeviceFeatures::Grip, "grip_value", XR_ACTION_TYPE_FLOAT_INPUT},
    {plXRDeviceFeatures::TriggerTouch, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::Select, "select_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::Menu, "menu_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::GripClick, "grip_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::ButtonA, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::ButtonATouch, "button_a_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::ButtonB, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::ButtonBTouch, "button_b_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::Thumbrest, "thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::PrimaryAnalogStick, "primary_analog_stick", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {plXRDeviceFeatures::PrimaryAnalogStickClick, "primary_analog_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::PrimaryAnalogStickTouch, "primary_analog_stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::SecondaryAnalogStick, "secondary_analog_stick", XR_ACTION_TYPE_VECTOR2F_INPUT},
    {plXRDeviceFeatures::SecondaryAnalogStickClick, "secondary_analog_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::SecondaryAnalogStickTouch, "secondary_analog_stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
    {plXRDeviceFeatures::GripPose, "grip_pose", XR_ACTION_TYPE_POSE_INPUT},
    {plXRDeviceFeatures::AimPose, "aim_pose", XR_ACTION_TYPE_POSE_INPUT},
    {plXRDeviceFeatures::Haptics, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
  };

  // Used when Data/Base/RuntimeConfigs/XRInteractionProfiles.ddl is missing: the profiles and built-in bindings of that
  // file, without the components nothing is bound to.
  constexpr const char* s_szBuiltInProfiles =
    R"(
Profile { string %Path { "/interaction_profiles/khr/simple_controller" } string %DisplayName { "Simple Controller" }
  Component { string %Path { "/input/select/click" } string %Type { "Boolean" } string %Actions { "select_click" } }
  Component { string %Path { "/input/menu/click" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
Profile { string %Path { "/interaction_profiles/microsoft/motion_controller" } string %DisplayName { "Mixed Reality Motion Controller" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value", "select_click" } }
  Component { string %Path { "/input/menu/click" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/click" } string %Type { "Boolean" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/trackpad" } string %Type { "Vector2" } string %Actions { "secondary_analog_stick" } }
  Component { string %Path { "/input/trackpad/click" } string %Type { "Boolean" } string %Actions { "secondary_analog_stick_click" } }
  Component { string %Path { "/input/trackpad/touch" } string %Type { "Boolean" } string %Actions { "secondary_analog_stick_touch" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
Profile { string %Path { "/interaction_profiles/oculus/touch_controller" } string %DisplayName { "Oculus Touch Controller" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value", "select_click" } }
  Component { string %Path { "/input/trigger/touch" } string %Type { "Boolean" } string %Actions { "trigger_touch" } }
  Component { string %Path { "/input/menu/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/value" } string %Type { "Float" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/thumbstick/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch" } }
  Component { string %Path { "/input/x/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/x/touch" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a_touch" } }
  Component { string %Path { "/input/y/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/y/touch" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b_touch" } }
  Component { string %Path { "/input/a/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/a/touch" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a_touch" } }
  Component { string %Path { "/input/b/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/b/touch" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b_touch" } }
  Component { string %Path { "/input/thumbrest/touch" } string %Type { "Boolean" } string %Actions { "thumbrest" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
)"
    R"(
Profile { string %Path { "/interaction_profiles/valve/index_controller" } string %DisplayName { "Valve Index Controller" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value" } }
  Component { string %Path { "/input/trigger/touch" } string %Type { "Boolean" } string %Actions { "trigger_touch" } }
  Component { string %Path { "/input/trigger/click" } string %Type { "Boolean" } string %Actions { "select_click" } }
  Component { string %Path { "/input/squeeze/value" } string %Type { "Float" } string %Actions { "grip_value" } }
  Component { string %Path { "/input/squeeze/force" } string %Type { "Float" } string %Actions { "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/thumbstick/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch", "thumbrest" } }
  Component { string %Path { "/input/trackpad" } string %Type { "Vector2" } string %Actions { "secondary_analog_stick" } }
  Component { string %Path { "/input/trackpad/touch" } string %Type { "Boolean" } string %Actions { "secondary_analog_stick_touch" } }
  Component { string %Path { "/input/a/click" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/a/touch" } string %Type { "Boolean" } string %Actions { "button_a_touch" } }
  Component { string %Path { "/input/b/click" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/b/touch" } string %Type { "Boolean" } string %Actions { "button_b_touch" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
Profile { string %Path { "/interaction_profiles/htc/vive_controller" } string %DisplayName { "HTC Vive Controller" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value" } }
  Component { string %Path { "/input/trigger/click" } string %Type { "Boolean" } string %Actions { "select_click" } }
  Component { string %Path { "/input/menu/click" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/click" } string %Type { "Boolean" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/trackpad" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/trackpad/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/trackpad/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
Profile { string %Path { "/interaction_profiles/microsoft/hand_interaction" } string %DisplayName { "Hand Interaction" } string %Extension { "XR_MSFT_hand_interaction" }
  Component { string %Path { "/input/select/value" } string %Type { "Float" } string %Actions { "select_click" } }
  Component { string %Path { "/input/squeeze/value" } string %Type { "Float" } string %Actions { "grip_value" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
}
Profile { string %Path { "/interaction_profiles/ext/hand_interaction_ext" } string %DisplayName { "Hand Interaction" } string %Extension { "XR_EXT_hand_interaction" }
  Component { string %Path { "/input/aim_activate_ext/value" } string %Type { "Float" } string %Actions { "trigger_value", "select_click" } }
  Component { string %Path { "/input/grasp_ext/value" } string %Type { "Float" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
}
)"
    R"(
Profile { string %Path { "/interaction_profiles/facebook/touch_controller_pro" } string %DisplayName { "Meta Quest Touch Pro" } string %Extension { "XR_FB_touch_controller_pro" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value", "select_click" } }
  Component { string %Path { "/input/trigger/touch" } string %Type { "Boolean" } string %Actions { "trigger_touch" } }
  Component { string %Path { "/input/menu/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/value" } string %Type { "Float" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/thumbstick/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch" } }
  Component { string %Path { "/input/x/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/x/touch" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a_touch" } }
  Component { string %Path { "/input/y/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/y/touch" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b_touch" } }
  Component { string %Path { "/input/a/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/a/touch" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a_touch" } }
  Component { string %Path { "/input/b/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/b/touch" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b_touch" } }
  Component { string %Path { "/input/thumbrest/touch" } string %Type { "Boolean" } string %Actions { "thumbrest" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
)"
    R"(
Profile { string %Path { "/interaction_profiles/htc/vive_cosmos_controller" } string %DisplayName { "HTC Vive Cosmos" } string %Extension { "XR_HTC_vive_cosmos_controller_interaction" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value" } }
  Component { string %Path { "/input/trigger/click" } string %Type { "Boolean" } string %Actions { "select_click" } }
  Component { string %Path { "/input/menu/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/click" } string %Type { "Boolean" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/thumbstick/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch" } }
  Component { string %Path { "/input/x/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/y/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/a/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/b/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
Profile { string %Path { "/interaction_profiles/htc/vive_focus3_controller" } string %DisplayName { "HTC Vive Focus 3" } string %Extension { "XR_HTC_vive_focus3_controller_interaction" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value" } }
  Component { string %Path { "/input/trigger/click" } string %Type { "Boolean" } string %Actions { "select_click" } }
  Component { string %Path { "/input/menu/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/click" } string %Type { "Boolean" } string %Actions { "grip_value", "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/thumbstick/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch" } }
  Component { string %Path { "/input/x/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/y/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/a/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/b/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
Profile { string %Path { "/interaction_profiles/bytedance/pico_neo3_controller" } string %DisplayName { "Pico Neo3" } string %Extension { "XR_BD_controller_interaction" }
  Component { string %Path { "/input/trigger/value" } string %Type { "Float" } string %Actions { "trigger_value" } }
  Component { string %Path { "/input/trigger/click" } string %Type { "Boolean" } string %Actions { "select_click" } }
  Component { string %Path { "/input/menu/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "menu_click" } }
  Component { string %Path { "/input/squeeze/value" } string %Type { "Float" } string %Actions { "grip_value" } }
  Component { string %Path { "/input/squeeze/click" } string %Type { "Boolean" } string %Actions { "grip_click" } }
  Component { string %Path { "/input/thumbstick" } string %Type { "Vector2" } string %Actions { "primary_analog_stick" } }
  Component { string %Path { "/input/thumbstick/click" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_click" } }
  Component { string %Path { "/input/thumbstick/touch" } string %Type { "Boolean" } string %Actions { "primary_analog_stick_touch" } }
  Component { string %Path { "/input/x/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/y/click" } string %Hands { "left" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/a/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_a" } }
  Component { string %Path { "/input/b/click" } string %Hands { "right" } string %Type { "Boolean" } string %Actions { "button_b" } }
  Component { string %Path { "/input/grip/pose" } string %Type { "Pose" } string %Actions { "grip_pose" } }
  Component { string %Path { "/input/aim/pose" } string %Type { "Pose" } string %Actions { "aim_pose" } }
  Component { string %Path { "/output/haptic" } string %Type { "Vibration" } string %Actions { "haptic" } }
}
)";

  /// Copies sName into an OpenXR action name: lower-case a-z, 0-9, '-', '_' and '.', shorter than XR_MAX_ACTION_NAME_SIZE.
  void SanitizeActionName(plStringView sName, plStringBuilder& out_sName)
  {
    out_sName.Clear();
    for (auto it = sName.GetIteratorFront(); it.IsValid(); ++it)
    {
      const plUInt32 c = plStringUtils::ToLowerChar(it.GetCharacter());
      const bool bValid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
      out_sName.Append(bValid ? c : static_cast<plUInt32>('_'));
    }

    if (out_sName.IsEmpty())
      out_sName = "action";

    if (out_sName.GetElementCount() >= XR_MAX_ACTION_NAME_SIZE)
      out_sName.Shrink(0, out_sName.GetElementCount() - (XR_MAX_ACTION_NAME_SIZE - 1));
  }

  constexpr plStringView s_sHandPrefix[2] = {"/user/hand/left"_plsv, "/user/hand/right"_plsv};
} // namespace

void plOpenXRInputDevice::GetDeviceList(plHybridArray<plXRDeviceID, 64>& out_devices) const
{
  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  // The HMD (0) is always present; start the scan at 1 so it is not listed twice.
  out_devices.PushBack(0);
  for (plXRDeviceID i = 1; i < s_iMaxDevices; i++)
  {
    if (m_DeviceState[i].m_bDeviceIsConnected)
    {
      out_devices.PushBack(i);
    }
  }
}

plXRDeviceID plOpenXRInputDevice::GetDeviceIDByType(plXRDeviceType::Enum type) const
{
  plXRDeviceID deviceID = -1;
  switch (type)
  {
    case plXRDeviceType::HMD:
      deviceID = 0;
      break;
    case plXRDeviceType::LeftController:
      deviceID = m_iLeftControllerDeviceID;
      break;
    case plXRDeviceType::RightController:
      deviceID = m_iRightControllerDeviceID;
      break;
    case plXRDeviceType::LeftShoulder:
      deviceID = m_iLeftShoulderDeviceId;
      break;
    case plXRDeviceType::EyeGaze:
      deviceID = m_iEyeGazeDeviceId;
      break;
    default:
      deviceID = -1;
      break;
  }

  if (deviceID != -1 && !m_DeviceState[deviceID].m_bDeviceIsConnected)
  {
    deviceID = -1;
  }
  return deviceID;
}

const plXRDeviceState& plOpenXRInputDevice::GetDeviceState(plXRDeviceID deviceID) const
{

  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  PL_ASSERT_DEV(deviceID < s_iMaxDevices && deviceID >= 0, "Invalid device ID.");
  PL_ASSERT_DEV(m_DeviceState[deviceID].m_bDeviceIsConnected, "Invalid device ID.");
  return m_DeviceState[deviceID];
}

plString plOpenXRInputDevice::GetDeviceName(plXRDeviceID deviceID) const
{
  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  PL_ASSERT_DEV(deviceID < s_iMaxDevices && deviceID >= 0, "Invalid device ID.");
  PL_ASSERT_DEV(m_DeviceState[deviceID].m_bDeviceIsConnected, "Invalid device ID.");
  return m_sActiveProfile[deviceID];
}

plBitflags<plXRDeviceFeatures> plOpenXRInputDevice::GetDeviceFeatures(plXRDeviceID deviceID) const
{
  PL_ASSERT_DEV(m_pOpenXR->IsInitialized(), "Need to call 'Initialize' first.");
  PL_ASSERT_DEV(deviceID < s_iMaxDevices && deviceID >= 0, "Invalid device ID.");
  PL_ASSERT_DEV(m_DeviceState[deviceID].m_bDeviceIsConnected, "Invalid device ID.");
  return m_SupportedFeatures[deviceID];
}

plInt32 plOpenXRInputDevice::GetRenderModelSide(plXRDeviceID deviceID) const
{
  if (deviceID == m_iLeftControllerDeviceID)
    return 0;
  if (deviceID == m_iRightControllerDeviceID)
    return 1;
  return -1;
}

plString plOpenXRInputDevice::GetRenderModelKey(plXRDeviceID deviceID) const
{
  const plInt32 iSide = GetRenderModelSide(deviceID);
  if (iSide < 0)
    return {};

  return m_RenderModel[iSide].m_sKey;
}

plResult plOpenXRInputDevice::TryGetRenderModel(plXRDeviceID deviceID, RenderModel& out_model) const
{
  const plInt32 iSide = GetRenderModelSide(deviceID);
  if (iSide < 0 || m_RenderModel[iSide].m_sKey.IsEmpty() || m_RenderModel[iSide].m_Glb.IsEmpty())
    return PL_FAILURE;

  out_model = m_RenderModel[iSide];
  return PL_SUCCESS;
}

plResult plOpenXRInputDevice::TryGetRenderModelNodePoses(plXRDeviceID deviceID, plDynamicArray<RenderModelNodePose>& out_poses) const
{
  const plInt32 iSide = GetRenderModelSide(deviceID);
  if (iSide < 0 || m_RenderModel[iSide].m_sKey.IsEmpty())
    return PL_FAILURE;

  // The poses belong to one model; while the input thread already animates a newer one than the main thread holds,
  // they would address the wrong nodes.
  if (m_sRenderModelNodePoseKey[iSide] != m_RenderModel[iSide].m_sKey || m_RenderModelNodePoses[iSide].IsEmpty())
    return PL_FAILURE;

  out_poses = m_RenderModelNodePoses[iSide];
  return PL_SUCCESS;
}

plOpenXRInputDevice::plOpenXRInputDevice(plOpenXR* pOpenXR)
  : plXRInputDevice()
  , m_pOpenXR(pOpenXR)
{
  m_pInstance = m_pOpenXR->m_pInstance;
}

XrResult plOpenXRInputDevice::CreateActions(XrSession session, XrSpace sceneSpace)
{
  // The input device outlives the instance when the runtime loses it and plOpenXR creates a new one.
  m_pInstance = m_pOpenXR->m_pInstance;
  m_pSession = session;

  // Latch the batched-location choice for this session - see the member comment for why the input thread
  // must never read the cvar live.
  m_bUseBatchedSpaceLocation = cvar_XrBatchedSpaceLocation && m_pOpenXR->m_Extensions.m_bLocateSpaces && m_pOpenXR->m_Extensions.pfn_xrLocateSpacesKHR != nullptr;
  if (m_bUseBatchedSpaceLocation)
  {
    plLog::SeriousWarning("XR.BatchedSpaceLocation is ON: SteamVR's xrLocateSpacesKHR is known to fault inside vrclient_x64.dll on some setups (documented full system hang on Vive Pro + implicit Vive API layers). Disable the cvar and restart the session if VR becomes unstable.");
  }

  // HMD is always connected or we wouldn't have been able to create a session.
  m_DeviceState[0] = plXRDeviceState();
  m_DeviceState[0].m_bDeviceIsConnected = true;
  m_sActiveProfile[0] = "HMD";
  m_SupportedFeatures[0] = plXRDeviceFeatures::AimPose | plXRDeviceFeatures::GripPose;

  // Devices
  for (plUInt32 uiControllerId : {m_iLeftControllerDeviceID, m_iRightControllerDeviceID, m_iLeftShoulderDeviceId, m_iEyeGazeDeviceId})
  {
    m_DeviceState[uiControllerId] = plXRDeviceState();
    m_sActiveProfile[uiControllerId].Clear();
  }

  // The input thread is not running yet, so the working snapshot can be set up here.
  m_WorkingSnapshot.m_DeviceState[m_iLeftControllerDeviceID].m_Type = plXRDeviceType::LeftController;
  m_WorkingSnapshot.m_DeviceState[m_iRightControllerDeviceID].m_Type = plXRDeviceType::RightController;
  m_WorkingSnapshot.m_DeviceState[m_iLeftShoulderDeviceId].m_Type = plXRDeviceType::LeftShoulder;
  m_WorkingSnapshot.m_DeviceState[m_iEyeGazeDeviceId].m_Type = plXRDeviceType::EyeGaze;

  XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  plStringUtils::Copy(actionSetInfo.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "gameplay");
  plStringUtils::Copy(actionSetInfo.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "Gameplay");
  actionSetInfo.priority = 0;
  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSet(m_pInstance, &actionSetInfo, &m_pActionSet), DestroyActions);

  m_SubActionPrefix.SetCount(3);
  m_SubActionPrefix[0] = "xr_hand_left_";
  m_SubActionPrefix[1] = "xr_hand_right_";
  m_SubActionPrefix[2] = "xr_shoulder_left";

  m_SubActionPath.SetCount(2);
  m_SubActionPath[0] = CreatePath("/user/hand/left");
  m_SubActionPath[1] = CreatePath("/user/hand/right");
  m_SubActionPathVive.SetCount(3);
  m_SubActionPathVive[0] = CreatePath("/interaction_profiles/htc/vive_tracker_htcx");
  m_SubActionPathVive[1] = CreatePath("/user/vive_tracker_htcx/role/left_shoulder");
  m_SubActionPathVive[2] = CreatePath("/user/vive_tracker_htcx/role/left_shoulder/input/grip/pose");

  // The input thread is not running yet, so its render model state can be reset here.
  ResetRenderModels();
  if (m_pOpenXR->m_Extensions.m_bRenderModelFB)
  {
    m_RenderModelPathFB[0] = CreatePath("/model_fb/controller/left");
    m_RenderModelPathFB[1] = CreatePath("/model_fb/controller/right");
  }

  // XR.DpadBinding needs XR.BindingModification, which carries the dpad structs. Both cvars and XR.PalmPose are latched
  // here: they change what the session is handed, so a toggle mid-session must not reach the input thread.
  m_bUseDpadBinding = cvar_XrBindingModification && cvar_XrDpadBinding && m_pOpenXR->m_Extensions.m_bBindingModification && m_pOpenXR->m_Extensions.m_bDpadBinding;
  m_bUsePalmPose = cvar_XrPalmPose && m_pOpenXR->m_Extensions.m_bPalmPose;
  m_bGlyphsDirty = false;

  LoadInteractionProfiles();

  // The built-in actions behind the xr_hand_<side>_* input slots, by name for the profile table.
  plHashTable<plString, XrAction> builtInActions;
  for (const BuiltInAction& builtIn : s_BuiltInActions)
  {
    XrAction action = XR_NULL_HANDLE;
    XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(builtIn.m_Feature, builtIn.m_szName, builtIn.m_Type, action), DestroyActions);
    builtInActions.Insert(plString(builtIn.m_szName), action);
  }

  XrAction GripPose = XR_NULL_HANDLE;
  XrAction AimPose = XR_NULL_HANDLE;
  builtInActions.TryGetValue("grip_pose", GripPose);
  builtInActions.TryGetValue("aim_pose", AimPose);
  builtInActions.TryGetValue("haptic", m_HapticAction);

  // Palm pose. Not in the profile table: the extension adds the same component to every controller profile, so it is
  // suggested next to each grip pose and dropped wherever the runtime rejects it.
  if (m_bUsePalmPose)
  {
    XR_SUCCEED_OR_CLEANUP_LOG(CreateAction(plXRDeviceFeatures::PalmPose, "palm_pose", XR_ACTION_TYPE_POSE_INPUT, m_PalmPoseAction), DestroyActions);
    plLog::Info("OpenXR: XR.PalmPose is on; the palm is reported as the third pose of each hand.");
  }

  CreateMapActions();

  // Left-shoulder tracker pose. Not created through CreateAction: it has no input slots and uses the tracker's
  // subaction path instead of the hands'.
  const bool bViveTracker = m_pOpenXR->m_Extensions.m_ViveTracker;
  if (bViveTracker)
  {
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    plStringUtils::Copy(actionInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "left_shoulder_pose");
    plStringUtils::Copy(actionInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Left Shoulder Pose");
    actionInfo.countSubactionPaths = 1;
    actionInfo.subactionPaths = &m_SubActionPathVive[1];
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateAction(m_pActionSet, &actionInfo, &shoulderPoseAction), DestroyActions);
  }

  // Eye gaze pose. No subaction paths: /user/eyes_ext is the only path it binds to.
  const bool bEyeGaze = m_pOpenXR->m_Extensions.m_bEyeGaze && m_pOpenXR->m_Info.m_bSupportsEyeGaze;
  if (bEyeGaze)
  {
    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    plStringUtils::Copy(actionInfo.actionName, XR_MAX_ACTION_NAME_SIZE, "eye_gaze_pose");
    plStringUtils::Copy(actionInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Eye Gaze Pose");
    actionInfo.countSubactionPaths = 0;
    actionInfo.subactionPaths = nullptr;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateAction(m_pActionSet, &actionInfo, &m_EyeGazeAction), DestroyActions);
  }

  // Every profile of the table: the built-in xr_hand_* bindings, palm pose and the action map paths.
  for (const plXRInteractionProfileDesc& profile : m_Profiles)
  {
    SuggestProfile(profile, builtInActions);
  }

  if (bViveTracker)
  {
    XrActionSuggestedBinding binding{shoulderPoseAction, m_SubActionPathVive[2]};

    XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = m_SubActionPathVive[0];
    suggestedBindings.countSuggestedBindings = 1;
    suggestedBindings.suggestedBindings = &binding;
    XR_LOG_ERROR(xrSuggestInteractionProfileBindings(m_pInstance, &suggestedBindings));
  }

  if (bEyeGaze)
  {
    Bind eyeGaze[] = {
      {m_EyeGazeAction, "/user/eyes_ext/input/gaze_ext/pose"},
    };
    if (SuggestInteractionProfileBindings("/interaction_profiles/ext/eye_gaze_interaction", "Eye Gaze", eyeGaze, true) != XR_SUCCESS)
    {
      // Nothing can be bound, so the action space below would never locate.
      XR_LOG_ERROR(xrDestroyAction(m_EyeGazeAction));
      m_EyeGazeAction = XR_NULL_HANDLE;
    }
  }

  XrActionSpaceCreateInfo spaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
  spaceCreateInfo.poseInActionSpace = m_pOpenXR->ConvertTransform(plTransform::MakeIdentity());
  for (plUInt32 uiSide : {0, 1})
  {
    spaceCreateInfo.subactionPath = m_SubActionPath[uiSide];
    spaceCreateInfo.action = GripPose;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &m_gripSpace[uiSide]), DestroyActions);

    spaceCreateInfo.action = AimPose;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &m_aimSpace[uiSide]), DestroyActions);

    if (m_PalmPoseAction != XR_NULL_HANDLE)
    {
      spaceCreateInfo.action = m_PalmPoseAction;
      XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &m_palmSpace[uiSide]), DestroyActions);
    }
  }

  // Suggestions are frozen once AttachSessionActionSets runs; later map edits are reported, not applied.
  m_sMapBindingSignature = BuildMapBindingSignature(plInputActionMap::GetPlayer(0).GetDescriptor());
  m_bMapChangeLogged = false;
  if (m_ActionMapEventId == 0)
  {
    m_ActionMapEventId = plInputActionMap::s_Events.AddEventHandler(plMakeDelegate(&plOpenXRInputDevice::OnActionMapEvent, this));
  }

  if (bViveTracker)
  {
    spaceCreateInfo.subactionPath = m_SubActionPathVive[1];
    spaceCreateInfo.action = shoulderPoseAction;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &shoulderSpace), DestroyActions);
  }

  if (m_EyeGazeAction != XR_NULL_HANDLE)
  {
    spaceCreateInfo.subactionPath = XR_NULL_PATH;
    spaceCreateInfo.action = m_EyeGazeAction;
    XR_SUCCEED_OR_CLEANUP_LOG(xrCreateActionSpace(m_pSession, &spaceCreateInfo, &m_EyeGazeSpace), DestroyActions);

    // On Quest the runtime only reports gaze when the app holds the eye-tracking permission.
    plLog::Info("OpenXR: Eye gaze is available as the EyeGaze device. Android needs the eye-tracking permission granted before the session starts.");
  }

  // Register input slots now that actions are created
  // This must happen after CreateAction calls populate the action arrays
  RegisterInputSlots();

  // The input thread is deliberately NOT started here. Actions and the spaces derived from them are only usable once
  // the action set is attached to the session, so polling starts in AttachSessionActionSets.

  return XR_SUCCESS;
}

void plOpenXRInputDevice::DestroyActions()
{
  // Stop input thread before destroying actions
  StopInputThread();

  // Action spaces first: destroying an action (or the action set that owns it) implicitly destroys the spaces derived
  // from it, so the spaces must go while their handles are still valid.
  for (plUInt32 uiSide : {0, 1})
  {
    if (m_gripSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_gripSpace[uiSide]));
      m_gripSpace[uiSide] = XR_NULL_HANDLE;
    }
    if (m_aimSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_aimSpace[uiSide]));
      m_aimSpace[uiSide] = XR_NULL_HANDLE;
    }
    if (m_palmSpace[uiSide])
    {
      XR_LOG_ERROR(xrDestroySpace(m_palmSpace[uiSide]));
      m_palmSpace[uiSide] = XR_NULL_HANDLE;
    }
  }
  if (shoulderSpace)
  {
    XR_LOG_ERROR(xrDestroySpace(shoulderSpace));
    shoulderSpace = XR_NULL_HANDLE;
  }
  if (m_EyeGazeSpace)
  {
    XR_LOG_ERROR(xrDestroySpace(m_EyeGazeSpace));
    m_EyeGazeSpace = XR_NULL_HANDLE;
  }

  for (Action& action : m_BooleanActions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_BooleanActions.Clear();

  for (Action& action : m_FloatActions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_FloatActions.Clear();

  for (Vec2Action& action : m_Vec2Actions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_Vec2Actions.Clear();

  // The palm pose action is one of m_PoseActions.
  for (Action& action : m_PoseActions)
  {
    XR_LOG_ERROR(xrDestroyAction(action.m_Action));
  }
  m_PoseActions.Clear();
  m_PalmPoseAction = XR_NULL_HANDLE;

  for (MapAction& action : m_MapActions)
  {
    if (action.m_Action)
    {
      XR_LOG_ERROR(xrDestroyAction(action.m_Action));
    }
  }
  m_MapActions.Clear();

  if (m_ActionMapEventId != 0)
  {
    plInputActionMap::s_Events.RemoveEventHandler(m_ActionMapEventId);
    m_ActionMapEventId = 0;
  }
  m_sMapBindingSignature.Clear();

  // The vibration output action is tracked on its own as it has no input slots.
  if (m_HapticAction)
  {
    XR_LOG_ERROR(xrDestroyAction(m_HapticAction));
    m_HapticAction = XR_NULL_HANDLE;
  }

  if (shoulderPoseAction)
  {
    XR_LOG_ERROR(xrDestroyAction(shoulderPoseAction));
    shoulderPoseAction = XR_NULL_HANDLE;
  }

  if (m_EyeGazeAction)
  {
    XR_LOG_ERROR(xrDestroyAction(m_EyeGazeAction));
    m_EyeGazeAction = XR_NULL_HANDLE;
  }

  if (m_pActionSet)
  {
    XR_LOG_ERROR(xrDestroyActionSet(m_pActionSet));
    m_pActionSet = XR_NULL_HANDLE;
  }

  for (plUInt32 i = 0; i < s_iMaxDevices; i++)
  {
    m_sActiveProfile[i].Clear();
    m_SupportedFeatures[i].Clear();
    m_DeviceState[i].m_bDeviceIsConnected = false;
  }

  for (plUInt32 uiSide : {0, 1})
  {
    m_DiscoveredFeatures[uiSide].Clear();
    m_bBoundActionsCached[uiSide] = false;
  }

  // The input thread is joined, so the working snapshot and the render model state are free to reset.
  ResetRenderModels();
  m_WorkingSnapshot.Clear();
  {
    PL_LOCK(m_SnapshotMutex);
    m_PublishedSnapshot.Clear();
  }
  m_bInteractionProfileDirty = true;
}

XrPath plOpenXRInputDevice::CreatePath(const char* szPath)
{
  XrInstance instance = m_pOpenXR->m_pInstance;

  XrPath path = XR_NULL_PATH;
  if (xrStringToPath(instance, szPath, &path) != XR_SUCCESS)
  {
    plLog::Error("OpenXR path conversion failure: {0}", szPath);
    return XR_NULL_PATH;
  }
  return path;
}

XrResult plOpenXRInputDevice::CreateAction(plXRDeviceFeatures::Enum feature, const char* actionName, XrActionType actionType, XrAction& out_action)
{
  XrActionCreateInfo actionCreateInfo{XR_TYPE_ACTION_CREATE_INFO};
  actionCreateInfo.actionType = actionType;
  actionCreateInfo.countSubactionPaths = 2;
  actionCreateInfo.subactionPaths = m_SubActionPath.GetData();
  plStringUtils::Copy(actionCreateInfo.actionName, XR_MAX_ACTION_NAME_SIZE, actionName);
  plStringUtils::Copy(actionCreateInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, actionName);

  XR_SUCCEED_OR_CLEANUP_LOG(xrCreateAction(m_pActionSet, &actionCreateInfo, &out_action), voidFunction);

  plStringBuilder sLeft(m_SubActionPrefix[0], actionName);
  plStringBuilder sRight(m_SubActionPrefix[1], actionName);

  switch (actionType)
  {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
      m_BooleanActions.PushBack({feature, out_action, sLeft, sRight});
      break;
    case XR_ACTION_TYPE_FLOAT_INPUT:
      m_FloatActions.PushBack({feature, out_action, sLeft, sRight});
      break;
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
      m_Vec2Actions.PushBack(Vec2Action(feature, out_action, sLeft, sRight));
      break;
    case XR_ACTION_TYPE_POSE_INPUT:
      m_PoseActions.PushBack({feature, out_action, sLeft, sRight});
      break;
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
      // Output actions have no state to poll and no input slot, the caller keeps the handle.
      break;
    default:
      PL_ASSERT_NOT_IMPLEMENTED;
  }

  return XR_SUCCESS;
}

XrResult plOpenXRInputDevice::SuggestInteractionProfileBindings(
  const char* szInteractionProfile, const char* szNiceName, plArrayPtr<Bind> bindings, bool bOptionalProfile)
{
  XrInstance instance = m_pOpenXR->m_pInstance;

  XrPath InteractionProfile = CreatePath(szInteractionProfile);

  plDynamicArray<XrActionSuggestedBinding> xrBindings;
  xrBindings.Reserve(bindings.GetCount());
  for (const Bind& binding : bindings)
  {
    xrBindings.PushBack({binding.action, CreatePath(binding.szPath)});
  }


  XrInteractionProfileSuggestedBinding profileBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  profileBindings.interactionProfile = InteractionProfile;
  profileBindings.suggestedBindings = xrBindings.GetData();
  profileBindings.countSuggestedBindings = xrBindings.GetCount();

  const XrResult res = xrSuggestInteractionProfileBindings(instance, &profileBindings);
  if (XR_FAILED_RESULT(res))
  {
    if (bOptionalProfile)
    {
      plLog::Info("OpenXR: Interaction profile '{0}' is unavailable ({1}), its bindings are skipped.", szInteractionProfile, res);
    }
    else
    {
      plLog::Error("OpenXR: Suggesting bindings for interaction profile '{0}' failed with: {1}", szInteractionProfile, res);
    }
    return res;
  }

  m_InteractionProfileToNiceName[InteractionProfile] = szNiceName;

  return XR_SUCCESS;
}

void plOpenXRInputDevice::LoadInteractionProfiles()
{
  if (plXRInteractionProfiles::LoadFromFile(plXRInteractionProfiles::s_sDefaultFile, m_Profiles).Succeeded() && !m_Profiles.IsEmpty())
    return;

  plLog::Warning("OpenXR: '{}' is missing or has no profiles; the built-in interaction profiles are used.", plXRInteractionProfiles::s_sDefaultFile);
  plXRInteractionProfiles::LoadFromDDL(s_szBuiltInProfiles, m_Profiles).IgnoreResult();
}

void plOpenXRInputDevice::CreateMapActions()
{
  m_MapActions.Clear();

  const plInputActionMapDescriptor& desc = plInputActionMap::GetPlayer(0).GetDescriptor();
  if (!desc.HasXRPaths())
    return;

  // Action names and localized names must both be unique within the action set.
  plSet<plString> usedNames;
  plSet<plString> usedLocalizedNames;
  for (const BuiltInAction& builtIn : s_BuiltInActions)
  {
    usedNames.Insert(plString(builtIn.m_szName));
    usedLocalizedNames.Insert(plString(builtIn.m_szName));
  }
  for (const char* szName : {"palm_pose", "left_shoulder_pose", "eye_gaze_pose"})
  {
    usedNames.Insert(plString(szName));
    usedLocalizedNames.Insert(plString(szName));
  }
  usedLocalizedNames.Insert(plString("Left Shoulder Pose"));
  usedLocalizedNames.Insert(plString("Eye Gaze Pose"));

  plStringBuilder sSlot, sName, sUniqueName, sLocalized, sUniqueLocalized, sTmp;

  for (const plInputContextDesc& ctx : desc.m_Contexts)
  {
    for (const plInputActionDesc& action : ctx.m_Actions)
    {
      plHybridArray<plInputXRPathDesc, 2> paths;
      for (const plInputBindingDesc& binding : action.m_Bindings)
      {
        for (const plInputXRPathDesc& path : binding.m_XRPaths)
        {
          if (path.m_sPath.IsEmpty())
            continue;

          if (!path.m_sPath.StartsWith("/user/"))
          {
            plLog::Warning("OpenXR: '{}' of '{}/{}' is not an OpenXR input path; it is skipped.", path.m_sPath, ctx.m_sName, action.m_sName);
            continue;
          }

          if (path.IsDpadPath())
          {
            if (!m_bUseDpadBinding)
            {
              plLog::Warning("OpenXR: '{}' of '{}/{}' needs XR.BindingModification and XR.DpadBinding (and a runtime with XR_EXT_dpad_binding); it is skipped.", path.m_sPath, ctx.m_sName, action.m_sName);
              continue;
            }

            if (action.m_ValueType != plInputValueType::Button)
            {
              plLog::Warning("OpenXR: '{}' of '{}/{}' is a dpad direction, which only button actions can use; it is skipped.", path.m_sPath, ctx.m_sName, action.m_sName);
              continue;
            }
          }

          paths.PushBack(path);
        }
      }

      if (paths.IsEmpty())
        continue;

      XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
      switch (action.m_ValueType)
      {
        case plInputValueType::Button:
          type = XR_ACTION_TYPE_BOOLEAN_INPUT;
          break;
        case plInputValueType::Axis1D:
          type = XR_ACTION_TYPE_FLOAT_INPUT;
          break;
        case plInputValueType::Axis2D:
          type = XR_ACTION_TYPE_VECTOR2F_INPUT;
          break;
        default:
          plLog::Warning("OpenXR: '{}/{}' is a 3D axis, which OpenXR has no action type for; its XR paths are skipped.", ctx.m_sName, action.m_sName);
          continue;
      }

      plInputActionMapDescriptor::GetXRActionSlotName(ctx.m_sName, action.m_sName, sSlot);

      SanitizeActionName(sSlot, sName);
      sUniqueName = sName;
      for (plUInt32 i = 2; usedNames.Contains(sUniqueName); ++i)
      {
        sTmp.SetFormat("_{}", i);
        sUniqueName = sName;
        if (sUniqueName.GetElementCount() + sTmp.GetElementCount() >= XR_MAX_ACTION_NAME_SIZE)
          sUniqueName.Shrink(0, sUniqueName.GetElementCount() + sTmp.GetElementCount() - (XR_MAX_ACTION_NAME_SIZE - 1));
        sUniqueName.Append(sTmp);
      }

      sLocalized.SetFormat("{} / {}", ctx.m_sName, action.m_sDisplayName.IsEmpty() ? action.m_sName : action.m_sDisplayName);
      while (sLocalized.GetElementCount() >= XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 8)
        sLocalized.Shrink(0, 1);
      sUniqueLocalized = sLocalized;
      for (plUInt32 i = 2; usedLocalizedNames.Contains(sUniqueLocalized); ++i)
      {
        sUniqueLocalized.SetFormat("{} ({})", sLocalized, i);
      }

      XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
      actionInfo.actionType = type;
      plStringUtils::Copy(actionInfo.actionName, XR_MAX_ACTION_NAME_SIZE, sUniqueName);
      plStringUtils::Copy(actionInfo.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, sUniqueLocalized);
      actionInfo.countSubactionPaths = 0;
      actionInfo.subactionPaths = nullptr;

      XrAction xrAction = XR_NULL_HANDLE;
      const XrResult res = xrCreateAction(m_pActionSet, &actionInfo, &xrAction);
      if (XR_FAILED_RESULT(res))
      {
        plLog::Error("OpenXR: Creating the action '{}' for '{}/{}' failed ({}); its XR paths are skipped.", sUniqueName, ctx.m_sName, action.m_sName, res);
        continue;
      }

      usedNames.Insert(plString(sUniqueName));
      usedLocalizedNames.Insert(plString(sUniqueLocalized));

      MapAction& mapAction = m_MapActions.ExpandAndGetRef();
      mapAction.m_Action = xrAction;
      mapAction.m_Type = type;
      sTmp.SetFormat("{}/{}", ctx.m_sName, action.m_sName);
      mapAction.m_sName = sTmp;
      mapAction.m_sDisplayName = action.m_sDisplayName.IsEmpty() ? action.m_sName : action.m_sDisplayName;
      mapAction.m_sSlot = sSlot;
      mapAction.m_Paths = paths;

      if (type != XR_ACTION_TYPE_BOOLEAN_INPUT)
      {
        sTmp.Set(sSlot, "_negx");
        mapAction.m_sSlotNegX = sTmp;
        sTmp.Set(sSlot, "_posx");
        mapAction.m_sSlotPosX = sTmp;
      }

      if (type == XR_ACTION_TYPE_VECTOR2F_INPUT)
      {
        sTmp.Set(sSlot, "_negy");
        mapAction.m_sSlotNegY = sTmp;
        sTmp.Set(sSlot, "_posy");
        mapAction.m_sSlotPosY = sTmp;
      }
    }
  }

  plLog::Info("OpenXR: {} input action map action(s) have XR paths.", m_MapActions.GetCount());
}

XrResult plOpenXRInputDevice::TrySuggestBindings(XrPath profilePath, plArrayPtr<const SuggestedBinding> bindings)
{
  plDynamicArray<XrActionSuggestedBinding> xrBindings;
  xrBindings.Reserve(bindings.GetCount());

  // One dpad modification per split stick or trackpad, shared by its four directions and the centre.
  plHybridArray<const XrBindingModificationBaseHeaderKHR*, 8> modifications;
  plHybridArray<XrPath, 8> dpadParents;

  for (const SuggestedBinding& binding : bindings)
  {
    xrBindings.PushBack(binding.m_Binding);

    if (binding.m_bDpad && !dpadParents.Contains(binding.m_Dpad.binding))
    {
      dpadParents.PushBack(binding.m_Dpad.binding);
      modifications.PushBack(reinterpret_cast<const XrBindingModificationBaseHeaderKHR*>(&binding.m_Dpad));
    }
  }

  XrInteractionProfileSuggestedBinding profileBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  profileBindings.interactionProfile = profilePath;
  profileBindings.suggestedBindings = xrBindings.GetData();
  profileBindings.countSuggestedBindings = xrBindings.GetCount();

  XrBindingModificationsKHR bindingModifications{XR_TYPE_BINDING_MODIFICATIONS_KHR};
  if (!modifications.IsEmpty())
  {
    bindingModifications.bindingModificationCount = modifications.GetCount();
    bindingModifications.bindingModifications = modifications.GetData();
    profileBindings.next = &bindingModifications;
  }

  return xrSuggestInteractionProfileBindings(m_pInstance, &profileBindings);
}

XrResult plOpenXRInputDevice::SuggestProfile(const plXRInteractionProfileDesc& profile, const plHashTable<plString, XrAction>& builtInActions)
{
  if (!profile.m_sExtension.IsEmpty() && !m_pOpenXR->IsExtensionEnabled(profile.m_sExtension))
  {
    plLog::Debug("OpenXR: Interaction profile '{}' needs {}, which is not enabled; its bindings are skipped.", profile.m_sPath, profile.m_sExtension);
    return XR_ERROR_EXTENSION_NOT_PRESENT;
  }

  const XrPath profilePath = CreatePath(profile.m_sPath);
  if (profilePath == XR_NULL_PATH)
    return XR_ERROR_PATH_INVALID;

  // Built-in bindings must all be accepted. Palm pose and action map bindings are kept where the runtime accepts them.
  plDynamicArray<SuggestedBinding> required;
  plDynamicArray<SuggestedBinding> optional;
  plStringBuilder sPath;

  for (const plXRInteractionProfileDesc::Component& component : profile.m_Components)
  {
    for (const plString& sAction : component.m_Actions)
    {
      XrAction action = XR_NULL_HANDLE;
      if (!builtInActions.TryGetValue(sAction, action) || action == XR_NULL_HANDLE)
      {
        plLog::Warning("OpenXR: Interaction profile '{}' binds '{}' to the unknown action '{}'.", profile.m_sPath, component.m_sPath, sAction);
        continue;
      }

      const XrPath path = CreatePath(component.m_sPath);
      if (path == XR_NULL_PATH)
        continue;

      SuggestedBinding& binding = required.ExpandAndGetRef();
      binding.m_Binding = {action, path};
      binding.m_sPath = component.m_sPath;
      binding.m_sOwner = sAction;
    }

    if (m_PalmPoseAction != XR_NULL_HANDLE && component.m_Type == plXRComponentType::Pose && component.m_sPath.EndsWith("/input/grip/pose"))
    {
      sPath = component.m_sPath;
      sPath.ReplaceLast("/input/grip/pose", "/input/palm_ext/pose");

      const XrPath path = CreatePath(sPath);
      if (path != XR_NULL_PATH)
      {
        SuggestedBinding& binding = optional.ExpandAndGetRef();
        binding.m_Binding = {m_PalmPoseAction, path};
        binding.m_sPath = sPath;
        binding.m_sOwner = "palm_pose";
      }
    }
  }

  for (const MapAction& mapAction : m_MapActions)
  {
    for (const plInputXRPathDesc& mapPath : mapAction.m_Paths)
    {
      // A path for every profile only goes to the profiles that list it; a path for a named profile is always tried.
      if (mapPath.m_sProfile.IsEmpty())
      {
        if (!plXRInteractionProfiles::ProfileHasPath(profile, mapPath.m_sPath))
          continue;
      }
      else if (!profile.MatchesName(mapPath.m_sProfile))
      {
        continue;
      }

      const XrPath path = CreatePath(mapPath.m_sPath);
      if (path == XR_NULL_PATH)
        continue;

      SuggestedBinding& binding = optional.ExpandAndGetRef();
      binding.m_Binding = {mapAction.m_Action, path};
      binding.m_sPath = mapPath.m_sPath;
      binding.m_sOwner = mapAction.m_sName;

      if (mapPath.IsDpadPath())
      {
        sPath = plXRInteractionProfiles::GetDpadParentPath(mapPath.m_sPath);

        binding.m_bDpad = true;
        binding.m_Dpad = {XR_TYPE_INTERACTION_PROFILE_DPAD_BINDING_EXT};
        binding.m_Dpad.binding = CreatePath(sPath);
        binding.m_Dpad.actionSet = m_pActionSet;
        binding.m_Dpad.forceThreshold = 0.5f;
        binding.m_Dpad.forceThresholdReleased = 0.4f;
        binding.m_Dpad.centerRegion = plMath::Clamp(mapPath.m_fDpadCenterRegion, 0.01f, 0.99f);
        binding.m_Dpad.wedgeAngle = plMath::Clamp(mapPath.m_DpadWedgeAngle.GetRadian(), 0.01f, plMath::Pi<float>() - 0.01f);
        binding.m_Dpad.isSticky = mapPath.m_bDpadSticky ? XR_TRUE : XR_FALSE;
        binding.m_Dpad.onHaptic = nullptr;
        binding.m_Dpad.offHaptic = nullptr;

        if (binding.m_Dpad.binding == XR_NULL_PATH)
        {
          optional.PopBack();
          continue;
        }
      }
    }
  }

  plDynamicArray<SuggestedBinding> accepted = required;
  accepted.PushBackRange(optional);

  XrResult res = TrySuggestBindings(profilePath, accepted);
  if (XR_FAILED_RESULT(res) && !optional.IsEmpty())
  {
    // One rejected path fails the whole call, so find the ones the runtime does not accept. Each successful call
    // replaces the previous suggestion for the profile.
    accepted = required;
    res = TrySuggestBindings(profilePath, accepted);

    if (!XR_FAILED_RESULT(res))
    {
      bool bLastFailed = false;
      for (const SuggestedBinding& candidate : optional)
      {
        accepted.PushBack(candidate);
        const XrResult candidateRes = TrySuggestBindings(profilePath, accepted);
        bLastFailed = XR_FAILED_RESULT(candidateRes);

        if (bLastFailed)
        {
          accepted.PopBack();

          if (candidate.m_sOwner == "palm_pose")
            plLog::Info("OpenXR: '{}' has no palm pose ({}).", profile.m_sDisplayName, candidateRes);
          else
            plLog::Warning("OpenXR: '{}' rejects '{}' ({}); that binding of '{}' is skipped.", profile.m_sDisplayName, candidate.m_sPath, candidateRes, candidate.m_sOwner);
        }
      }

      if (bLastFailed)
      {
        res = TrySuggestBindings(profilePath, accepted);
      }
    }
  }

  if (XR_FAILED_RESULT(res))
  {
    plLog::Error("OpenXR: Suggesting bindings for interaction profile '{0}' failed with: {1}", profile.m_sPath, res);
    return res;
  }

  m_InteractionProfileToNiceName[profilePath] = profile.m_sDisplayName;
  return XR_SUCCESS;
}

plString plOpenXRInputDevice::BuildMapBindingSignature(const plInputActionMapDescriptor& desc)
{
  plStringBuilder sSignature;

  for (const plInputContextDesc& ctx : desc.m_Contexts)
  {
    for (const plInputActionDesc& action : ctx.m_Actions)
    {
      for (const plInputBindingDesc& binding : action.m_Bindings)
      {
        for (const plInputXRPathDesc& path : binding.m_XRPaths)
        {
          sSignature.AppendFormat("{}|{}|{}|{}|{}|{}|{}|{};", ctx.m_sName, action.m_sName, static_cast<plUInt32>(action.m_ValueType.GetValue()), path.m_sProfile, path.m_sPath,
            path.m_fDpadCenterRegion, path.m_DpadWedgeAngle.GetDegree(), path.m_bDpadSticky);
        }
      }
    }
  }

  return sSignature;
}

void plOpenXRInputDevice::OnActionMapEvent(const plInputActionMapEvent& e)
{
  if (e.m_Type != plInputActionMapEvent::Type::MapChanged || e.m_uiPlayer != 0 || m_bMapChangeLogged || m_pActionSet == XR_NULL_HANDLE)
    return;

  if (BuildMapBindingSignature(plInputActionMap::GetPlayer(0).GetDescriptor()) == m_sMapBindingSignature)
    return;

  m_bMapChangeLogged = true;
  plLog::Warning("XR bindings apply at the next session");
}

void plOpenXRInputDevice::UpdateMapActions(InputSnapshot& snapshot)
{
  if (m_MapActions.IsEmpty())
    return;

  // No subaction path: the runtime combines every source bound to the action.
  XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
  getInfo.subactionPath = XR_NULL_PATH;

  for (const MapAction& action : m_MapActions)
  {
    getInfo.action = action.m_Action;

    switch (action.m_Type)
    {
      case XR_ACTION_TYPE_BOOLEAN_INPUT:
      {
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (xrGetActionStateBoolean(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          snapshot.m_InputSlotValues[action.m_sSlot] = state.currentState ? 1.0f : 0.0f;
        }
        break;
      }

      case XR_ACTION_TYPE_FLOAT_INPUT:
      {
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (xrGetActionStateFloat(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          const float x = state.currentState;
          snapshot.m_InputSlotValues[action.m_sSlot] = plMath::Max(x, 0.0f);
          snapshot.m_InputSlotValues[action.m_sSlotNegX] = x < 0 ? -x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sSlotPosX] = x > 0 ? x : 0.0f;
        }
        break;
      }

      case XR_ACTION_TYPE_VECTOR2F_INPUT:
      {
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (xrGetActionStateVector2f(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          const float x = state.currentState.x;
          const float y = state.currentState.y;
          snapshot.m_InputSlotValues[action.m_sSlot] = plMath::Min(plMath::Sqrt(x * x + y * y), 1.0f);
          snapshot.m_InputSlotValues[action.m_sSlotNegX] = x < 0 ? -x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sSlotPosX] = x > 0 ? x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sSlotNegY] = y < 0 ? -y : 0.0f;
          snapshot.m_InputSlotValues[action.m_sSlotPosY] = y > 0 ? y : 0.0f;
        }
        break;
      }

      default:
        break;
    }
  }
}

void plOpenXRInputDevice::UpdatePalmPose(plUInt32 uiSide, InputSnapshot& snapshot, XrSpace baseSpace, XrTime time)
{
  if (m_palmSpace[uiSide] == XR_NULL_HANDLE)
    return;

  const plUInt32 uiControllerId = uiSide == 0 ? m_iLeftControllerDeviceID : m_iRightControllerDeviceID;
  plXRDeviceState& deviceState = snapshot.m_DeviceState[uiControllerId];

  constexpr XrSpaceLocationFlags validFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  const XrResult res = xrLocateSpace(m_palmSpace[uiSide], baseSpace, time, &location);
  if (res != XR_SUCCESS)
  {
    ReportLocateFailure(uiSide, "xrLocateSpace(palm)", res, baseSpace, time);
    deviceState.m_bPalmPoseIsValid = false;
    return;
  }

  if ((location.locationFlags & validFlags) != validFlags)
  {
    deviceState.m_bPalmPoseIsValid = false;
    return;
  }

  deviceState.m_vPalmPosition = plOpenXR::ConvertPosition(location.pose.position);
  deviceState.m_qPalmRotation = plOpenXR::ConvertOrientation(location.pose.orientation);
  deviceState.m_bPalmPoseIsValid = true;
  m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::PalmPose);
}

plString plOpenXRInputDevice::GetLocalizedSourceName(XrAction action, plStringView sHandPrefix, XrInputSourceLocalizedNameFlags flags) const
{
  if (action == XR_NULL_HANDLE)
    return {};

  XrBoundSourcesForActionEnumerateInfo enumerateInfo{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
  enumerateInfo.action = action;

  plUInt32 uiSourceCount = 0;
  if (XR_FAILED_RESULT(xrEnumerateBoundSourcesForAction(m_pSession, &enumerateInfo, 0, &uiSourceCount, nullptr)) || uiSourceCount == 0)
    return {};

  plHybridArray<XrPath, 4> sources;
  sources.SetCount(uiSourceCount);
  if (XR_FAILED_RESULT(xrEnumerateBoundSourcesForAction(m_pSession, &enumerateInfo, uiSourceCount, &uiSourceCount, sources.GetData())))
    return {};
  sources.SetCount(uiSourceCount);

  for (XrPath source : sources)
  {
    if (!sHandPrefix.IsEmpty())
    {
      char szPath[XR_MAX_PATH_LENGTH] = {};
      plUInt32 uiWritten = 0;
      if (xrPathToString(m_pInstance, source, XR_MAX_PATH_LENGTH, &uiWritten, szPath) != XR_SUCCESS || !plStringView(szPath).StartsWith(sHandPrefix))
        continue;
    }

    XrInputSourceLocalizedNameGetInfo nameInfo{XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
    nameInfo.sourcePath = source;
    nameInfo.whichComponents = flags;

    plUInt32 uiSize = 0;
    if (XR_FAILED_RESULT(xrGetInputSourceLocalizedName(m_pSession, &nameInfo, 0, &uiSize, nullptr)) || uiSize == 0)
      continue;

    plHybridArray<char, 128> buffer;
    buffer.SetCount(uiSize);
    if (XR_FAILED_RESULT(xrGetInputSourceLocalizedName(m_pSession, &nameInfo, uiSize, &uiSize, buffer.GetData())))
      continue;

    buffer[buffer.GetCount() - 1] = '\0';
    return plString(buffer.GetData());
  }

  return {};
}

void plOpenXRInputDevice::UpdateGlyphs()
{
  PL_PROFILE_SCOPE("UpdateXRGlyphs");

  plStringBuilder sLabel;
  auto SetLabel = [&](const plString& sSlot, plStringView sName, plStringView sSuffix)
  {
    if (sSlot.IsEmpty() || sName.IsEmpty())
      return;

    // The sprite of a project glyph stays; only the label follows the connected controller.
    sLabel.Set(sName, sSuffix);
    plInputGlyphs::SetGlyph(sSlot, sLabel, plInputGlyphs::GetSprite(sSlot));
  };

  plUInt32 uiNames = 0;
  plString sName;

  for (plUInt32 uiSide : {0, 1})
  {
    for (const Action& action : m_BooleanActions)
    {
      sName = GetLocalizedSourceName(action.m_Action, s_sHandPrefix[uiSide], XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT);
      SetLabel(action.m_sKey[uiSide], sName, "");
      uiNames += sName.IsEmpty() ? 0 : 1;
    }

    for (const Action& action : m_FloatActions)
    {
      sName = GetLocalizedSourceName(action.m_Action, s_sHandPrefix[uiSide], XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT);
      SetLabel(action.m_sKey[uiSide], sName, "");
      uiNames += sName.IsEmpty() ? 0 : 1;
    }

    for (const Vec2Action& action : m_Vec2Actions)
    {
      sName = GetLocalizedSourceName(action.m_Action, s_sHandPrefix[uiSide], XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT);
      SetLabel(action.m_sKey_negx[uiSide], sName, " Left");
      SetLabel(action.m_sKey_posx[uiSide], sName, " Right");
      SetLabel(action.m_sKey_negy[uiSide], sName, " Down");
      SetLabel(action.m_sKey_posy[uiSide], sName, " Up");
      uiNames += sName.IsEmpty() ? 0 : 1;
    }
  }

  // Action map actions are not tied to a hand, so their names say which one.
  for (const MapAction& action : m_MapActions)
  {
    sName = GetLocalizedSourceName(action.m_Action, {}, XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT | XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT);
    SetLabel(action.m_sSlot, sName, "");
    SetLabel(action.m_sSlotNegX, sName, " Left");
    SetLabel(action.m_sSlotPosX, sName, " Right");
    SetLabel(action.m_sSlotNegY, sName, " Down");
    SetLabel(action.m_sSlotPosY, sName, " Up");
    uiNames += sName.IsEmpty() ? 0 : 1;
  }

  plLog::Debug("OpenXR: Read {} input names for the button prompts.", uiNames);
}

XrResult plOpenXRInputDevice::AttachSessionActionSets(XrSession session)
{
  m_pSession = session;
  XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  plHybridArray<XrActionSet, 1> actionSets;
  actionSets.PushBack(m_pActionSet);

  attachInfo.countActionSets = actionSets.GetCount();
  attachInfo.actionSets = actionSets.GetData();
  XR_SUCCEED_OR_RETURN_LOG(xrAttachSessionActionSets(session, &attachInfo));

  // The poll thread must not start before attachment: until then xrSyncActions and xrGetActionState* return
  // XR_ERROR_ACTIONSET_NOT_ATTACHED and the action spaces are not locatable.
  StartInputThread();

  return XR_SUCCESS;
}

void plOpenXRInputDevice::RequestInteractionProfileUpdate()
{
  m_bInteractionProfileDirty = true;
}

void plOpenXRInputDevice::UpdateInteractionProfiles(InputSnapshot& snapshot)
{
  // XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED does not say which top-level path changed, so both hands are
  // queried again.
  for (plUInt32 uiSide : {0, 1})
  {
    const plUInt32 uiControllerId = uiSide == 0 ? m_iLeftControllerDeviceID : m_iRightControllerDeviceID;

    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(m_pSession, m_SubActionPath[uiSide], &state) != XR_SUCCESS)
      continue;

    plString sNewProfile;
    if (state.interactionProfile != XR_NULL_PATH && !m_InteractionProfileToNiceName.TryGetValue(state.interactionProfile, sNewProfile))
    {
      char szBuffer[XR_MAX_PATH_LENGTH] = {};
      plUInt32 uiWritten = 0;
      xrPathToString(m_pInstance, state.interactionProfile, XR_MAX_PATH_LENGTH, &uiWritten, szBuffer);
      plLog::Warning("OpenXR: The runtime selected interaction profile '{}', which has no suggested bindings.", szBuffer);
      sNewProfile = szBuffer;
    }

    if (snapshot.m_sActiveProfile[uiControllerId] != sNewProfile)
    {
      plLog::Info("OpenXR: {} controller profile is now '{}'.", uiSide == 0 ? "Left" : "Right", sNewProfile);
      snapshot.m_sActiveProfile[uiControllerId] = sNewProfile;
      m_bBoundActionsCached[uiSide] = false;
      m_bGlyphsDirty |= !sNewProfile.IsEmpty();
    }

    // A new interaction profile usually means a different controller, so the render model key is read again.
    m_RenderModelState[uiSide].m_bQueryKey = true;
  }
}

void plOpenXRInputDevice::InitializeDevice()
{
  RegisterInputSlots();
}

void plOpenXRInputDevice::RegisterInputSlots()
{
  for (const Action& action : m_BooleanActions)
  {
    for (plUInt32 uiSide : {0, 1})
    {
      RegisterInputSlot(action.m_sKey[uiSide], action.m_sKey[uiSide], plInputSlotFlags::IsButton);
    }
  }
  for (const Action& action : m_FloatActions)
  {
    for (plUInt32 uiSide : {0, 1})
    {
      RegisterInputSlot(action.m_sKey[uiSide], action.m_sKey[uiSide], plInputSlotFlags::IsAnalogTrigger);
    }
  }
  for (const Vec2Action& action : m_Vec2Actions)
  {
    for (plUInt32 uiSide : {0, 1})
    {
      RegisterInputSlot(action.m_sKey_negx[uiSide], action.m_sKey_negx[uiSide], plInputSlotFlags::IsAnalogStick);
      RegisterInputSlot(action.m_sKey_posx[uiSide], action.m_sKey_posx[uiSide], plInputSlotFlags::IsAnalogStick);
      RegisterInputSlot(action.m_sKey_negy[uiSide], action.m_sKey_negy[uiSide], plInputSlotFlags::IsAnalogStick);
      RegisterInputSlot(action.m_sKey_posy[uiSide], action.m_sKey_posy[uiSide], plInputSlotFlags::IsAnalogStick);
    }
  }

  // The generated 'xr_action_<context>_<action>' slots of input action map actions with XR paths.
  for (const MapAction& action : m_MapActions)
  {
    switch (action.m_Type)
    {
      case XR_ACTION_TYPE_BOOLEAN_INPUT:
        RegisterInputSlot(action.m_sSlot, action.m_sDisplayName, plInputSlotFlags::IsButton);
        break;
      case XR_ACTION_TYPE_FLOAT_INPUT:
        RegisterInputSlot(action.m_sSlot, action.m_sDisplayName, plInputSlotFlags::IsAnalogTrigger);
        RegisterInputSlot(action.m_sSlotNegX, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        RegisterInputSlot(action.m_sSlotPosX, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        break;
      case XR_ACTION_TYPE_VECTOR2F_INPUT:
        RegisterInputSlot(action.m_sSlot, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        RegisterInputSlot(action.m_sSlotNegX, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        RegisterInputSlot(action.m_sSlotPosX, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        RegisterInputSlot(action.m_sSlotNegY, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        RegisterInputSlot(action.m_sSlotPosY, action.m_sDisplayName, plInputSlotFlags::IsAnalogStick);
        break;
      default:
        break;
    }
  }
}

void plOpenXRInputDevice::UpdateInputSlotValues()
{
  // Input values are updated by UpdateActions() called from plOpenXR::UpdatePoses().
  // m_InputSlotValues already contains current values for the input manager to read.
}

XrResult plOpenXRInputDevice::UpdateActions()
{
  if (m_pSession == XR_NULL_HANDLE)
    return XR_SUCCESS;

  PL_PROFILE_SCOPE("UpdateActions");

  // Copy latest input state from background thread's snapshot
  CopySnapshotToMainThread();

  UpdateControllerState();
  return XR_SUCCESS;
}

void plOpenXRInputDevice::CopySnapshotToMainThread()
{
  PL_PROFILE_SCOPE("CopyInputSnapshot");

  PL_LOCK(m_SnapshotMutex);
  const InputSnapshot& snapshot = m_PublishedSnapshot;

  // Slot 0 is the HMD, which plOpenXR::UpdatePoses writes directly. The connected flag stays main-thread state.
  for (plUInt32 i = 1; i < s_iMaxDevices; ++i)
  {
    const bool bConnected = m_DeviceState[i].m_bDeviceIsConnected;
    m_DeviceState[i] = snapshot.m_DeviceState[i];
    m_DeviceState[i].m_bDeviceIsConnected = bConnected;
    m_SupportedFeatures[i] = snapshot.m_SupportedFeatures[i];
    m_sActiveProfile[i] = snapshot.m_sActiveProfile[i];
  }

  for (auto it = snapshot.m_InputSlotValues.GetIterator(); it.IsValid(); ++it)
  {
    m_InputSlotValues[it.Key()] = it.Value();
  }

  for (plUInt32 uiSide : {0, 1})
  {
    if (m_sRenderModelNodePoseKey[uiSide] != snapshot.m_sRenderModelNodePoseKey[uiSide])
    {
      m_sRenderModelNodePoseKey[uiSide] = snapshot.m_sRenderModelNodePoseKey[uiSide];
    }
    m_RenderModelNodePoses[uiSide] = snapshot.m_RenderModelNodePoses[uiSide];
  }

  // A model the input thread loaded (or dropped) since the last frame. Moved, not copied: the bytes can be megabytes.
  {
    PL_LOCK(m_RenderModelMutex);
    for (plUInt32 uiSide : {0, 1})
    {
      if (m_PendingRenderModel[uiSide].m_bPending)
      {
        m_RenderModel[uiSide] = std::move(m_PendingRenderModel[uiSide].m_Model);
        m_PendingRenderModel[uiSide].m_Model = RenderModel();
        m_PendingRenderModel[uiSide].m_bPending = false;
      }
    }
  }
}

void plOpenXRInputDevice::PublishSnapshot()
{
  PL_PROFILE_SCOPE("PublishInputSnapshot");

  PL_LOCK(m_SnapshotMutex);

  for (plUInt32 i = 1; i < s_iMaxDevices; ++i)
  {
    m_PublishedSnapshot.m_DeviceState[i] = m_WorkingSnapshot.m_DeviceState[i];
    m_PublishedSnapshot.m_SupportedFeatures[i] = m_WorkingSnapshot.m_SupportedFeatures[i];
    if (m_PublishedSnapshot.m_sActiveProfile[i] != m_WorkingSnapshot.m_sActiveProfile[i])
    {
      m_PublishedSnapshot.m_sActiveProfile[i] = m_WorkingSnapshot.m_sActiveProfile[i];
    }
  }

  for (auto it = m_WorkingSnapshot.m_InputSlotValues.GetIterator(); it.IsValid(); ++it)
  {
    m_PublishedSnapshot.m_InputSlotValues[it.Key()] = it.Value();
  }

  for (plUInt32 uiSide : {0, 1})
  {
    if (m_PublishedSnapshot.m_sRenderModelNodePoseKey[uiSide] != m_WorkingSnapshot.m_sRenderModelNodePoseKey[uiSide])
    {
      m_PublishedSnapshot.m_sRenderModelNodePoseKey[uiSide] = m_WorkingSnapshot.m_sRenderModelNodePoseKey[uiSide];
    }
    m_PublishedSnapshot.m_RenderModelNodePoses[uiSide] = m_WorkingSnapshot.m_RenderModelNodePoses[uiSide];
  }
}

void plOpenXRInputDevice::InputSnapshot::Clear()
{
  for (plUInt32 i = 0; i < s_iMaxDevices; ++i)
  {
    m_DeviceState[i] = plXRDeviceState();
    m_SupportedFeatures[i].Clear();
    m_sActiveProfile[i].Clear();
  }
  m_InputSlotValues.Clear();

  for (plUInt32 uiSide : {0, 1})
  {
    m_sRenderModelNodePoseKey[uiSide].Clear();
    m_RenderModelNodePoses[uiSide].Clear();
  }
}


void plOpenXRInputDevice::UpdateLeftShoulderTracking(InputSnapshot& snapshot, XrSpace baseSpace, XrTime time)
{
  plXRDeviceState& deviceState = snapshot.m_DeviceState[m_iLeftShoulderDeviceId];
  plBitflags<plXRDeviceFeatures>& supportedFeatures = snapshot.m_SupportedFeatures[m_iLeftShoulderDeviceId];

  // Uses the caller's xrSyncActions. Syncing again filtered to the tracker's subaction path would leave the hand actions
  // inactive for the rest of the tick.
  XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
  getInfo.action = shoulderPoseAction;
  getInfo.subactionPath = m_SubActionPathVive[1];

  XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
  if (shoulderSpace == XR_NULL_HANDLE || xrGetActionStatePose(m_pSession, &getInfo, &poseState) != XR_SUCCESS || !poseState.isActive)
  {
    // UpdateControllerState reports the tracker as disconnected once AimPose is gone.
    supportedFeatures.Clear();
    deviceState.m_bGripPoseIsValid = false;
    deviceState.m_bAimPoseIsValid = false;
    return;
  }

  // A tracker has a single pose; it is reported as both grip and aim so it looks like any other tracked device.
  supportedFeatures = plXRDeviceFeatures::GripPose | plXRDeviceFeatures::AimPose;

  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  constexpr XrSpaceLocationFlags validFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if (xrLocateSpace(shoulderSpace, baseSpace, time, &location) == XR_SUCCESS && (location.locationFlags & validFlags) == validFlags)
  {
    deviceState.m_vGripPosition = plOpenXR::ConvertPosition(location.pose.position);
    deviceState.m_qGripRotation = plOpenXR::ConvertOrientation(location.pose.orientation);
    deviceState.m_bGripPoseIsValid = true;
    deviceState.m_vAimPosition = deviceState.m_vGripPosition;
    deviceState.m_qAimRotation = deviceState.m_qGripRotation;
    deviceState.m_bAimPoseIsValid = true;
  }
  else
  {
    deviceState.m_bGripPoseIsValid = false;
    deviceState.m_bAimPoseIsValid = false;
  }
}

void plOpenXRInputDevice::UpdateEyeGaze(InputSnapshot& snapshot, XrSpace baseSpace, XrTime time)
{
  if (m_EyeGazeAction == XR_NULL_HANDLE || m_EyeGazeSpace == XR_NULL_HANDLE || baseSpace == XR_NULL_HANDLE)
    return;

  plXRDeviceState& deviceState = snapshot.m_DeviceState[m_iEyeGazeDeviceId];
  plBitflags<plXRDeviceFeatures>& supportedFeatures = snapshot.m_SupportedFeatures[m_iEyeGazeDeviceId];

  XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
  getInfo.action = m_EyeGazeAction;
  getInfo.subactionPath = XR_NULL_PATH;

  XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
  if (xrGetActionStatePose(m_pSession, &getInfo, &poseState) != XR_SUCCESS || !poseState.isActive)
  {
    // UpdateControllerState reports the device as disconnected once AimPose is gone.
    supportedFeatures.Clear();
    deviceState.m_bGripPoseIsValid = false;
    deviceState.m_bAimPoseIsValid = false;
    deviceState.m_bAimPoseIsTracked = false;
    return;
  }

  // The gaze ray is the aim pose. It is mirrored into the grip pose so tracking components that follow the grip work too.
  supportedFeatures = plXRDeviceFeatures::GripPose | plXRDeviceFeatures::AimPose;
  if (snapshot.m_sActiveProfile[m_iEyeGazeDeviceId].IsEmpty())
  {
    snapshot.m_sActiveProfile[m_iEyeGazeDeviceId] = "Eye Gaze";
  }

  XrEyeGazeSampleTimeEXT sampleTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  location.next = &sampleTime;

  constexpr XrSpaceLocationFlags validFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  constexpr XrSpaceLocationFlags trackedFlags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
  if (xrLocateSpace(m_EyeGazeSpace, baseSpace, time, &location) == XR_SUCCESS && (location.locationFlags & validFlags) == validFlags)
  {
    deviceState.m_vAimPosition = plOpenXR::ConvertPosition(location.pose.position);
    deviceState.m_qAimRotation = plOpenXR::ConvertOrientation(location.pose.orientation);
    deviceState.m_bAimPoseIsValid = true;
    // Valid but untracked is the last known gaze, e.g. while the eyes are closed.
    deviceState.m_bAimPoseIsTracked = (location.locationFlags & trackedFlags) == trackedFlags;
    deviceState.m_vGripPosition = deviceState.m_vAimPosition;
    deviceState.m_qGripRotation = deviceState.m_qAimRotation;
    deviceState.m_bGripPoseIsValid = true;
  }
  else
  {
    deviceState.m_bGripPoseIsValid = false;
    deviceState.m_bAimPoseIsValid = false;
    deviceState.m_bAimPoseIsTracked = false;
  }
}

void plOpenXRInputDevice::RenderModelState::Reset()
{
  m_KeyMSFT = XR_NULL_CONTROLLER_MODEL_KEY_MSFT;
  m_KeyFB = XR_NULL_RENDER_MODEL_KEY_FB;
  m_sKey.Clear();
  m_NodeNames.Clear();
  m_NodeStates.Clear();
  m_bQueryKey = true;
  m_NextRetry = plTime::MakeZero();
}

void plOpenXRInputDevice::ResetRenderModels()
{
  // Main thread, with the input thread stopped (or not started yet).
  m_bRenderModelPathsEnumeratedFB = false;
  for (plUInt32 uiSide : {0, 1})
  {
    m_RenderModelState[uiSide].Reset();
    m_RenderModel[uiSide] = RenderModel();
    m_sRenderModelNodePoseKey[uiSide].Clear();
    m_RenderModelNodePoses[uiSide].Clear();
    m_RenderModelPathFB[uiSide] = XR_NULL_PATH;
  }

  PL_LOCK(m_RenderModelMutex);
  for (plUInt32 uiSide : {0, 1})
  {
    m_PendingRenderModel[uiSide] = PendingRenderModel();
  }
}

void plOpenXRInputDevice::PublishRenderModel(plUInt32 uiSide, const plString& sKey, plDynamicArray<plUInt8>&& glb)
{
  m_RenderModelState[uiSide].m_sKey = sKey;

  PL_LOCK(m_RenderModelMutex);
  PendingRenderModel& pending = m_PendingRenderModel[uiSide];
  pending.m_Model.m_sKey = sKey;
  pending.m_Model.m_Glb = std::move(glb);
  pending.m_bPending = true;
}

void plOpenXRInputDevice::UpdateRenderModels(InputSnapshot& snapshot)
{
  const bool bMSFT = m_pOpenXR->m_Extensions.m_bControllerModelMSFT;
  const bool bFB = m_pOpenXR->m_Extensions.m_bRenderModelFB;
  if (!bMSFT && !bFB)
    return;

  PL_PROFILE_SCOPE("UpdateRenderModels");

  const plTime now = plTime::Now();
  for (plUInt32 uiSide : {0, 1})
  {
    RenderModelState& state = m_RenderModelState[uiSide];

    // Without a model the key is polled: a controller that is switched on later gets its model without a profile change.
    if (!state.m_bQueryKey && state.m_sKey.IsEmpty() && now >= state.m_NextRetry)
    {
      state.m_bQueryKey = true;
    }

    if (bMSFT)
    {
      UpdateRenderModelMSFT(uiSide, snapshot);
    }
    else
    {
      UpdateRenderModelFB(uiSide);
    }

    if (state.m_sKey.IsEmpty())
    {
      if (now >= state.m_NextRetry)
      {
        state.m_NextRetry = now + plTime::MakeFromSeconds(1.0);
      }
      snapshot.m_sRenderModelNodePoseKey[uiSide].Clear();
      snapshot.m_RenderModelNodePoses[uiSide].Clear();
    }
  }
}

void plOpenXRInputDevice::UpdateRenderModelMSFT(plUInt32 uiSide, InputSnapshot& snapshot)
{
  RenderModelState& state = m_RenderModelState[uiSide];
  const plOpenXR::Extensions& ext = m_pOpenXR->m_Extensions;

  if (state.m_bQueryKey)
  {
    state.m_bQueryKey = false;

    XrControllerModelKeyStateMSFT keyState{XR_TYPE_CONTROLLER_MODEL_KEY_STATE_MSFT};
    if (XR_FAILED_RESULT(ext.pfn_xrGetControllerModelKeyMSFT(m_pSession, m_SubActionPath[uiSide], &keyState)))
    {
      keyState.modelKey = XR_NULL_CONTROLLER_MODEL_KEY_MSFT;
    }

    if (keyState.modelKey != state.m_KeyMSFT)
    {
      state.m_KeyMSFT = XR_NULL_CONTROLLER_MODEL_KEY_MSFT;
      state.m_NodeNames.Clear();
      state.m_NodeStates.Clear();

      if (keyState.modelKey == XR_NULL_CONTROLLER_MODEL_KEY_MSFT)
      {
        if (!state.m_sKey.IsEmpty())
        {
          PublishRenderModel(uiSide, plString(), plDynamicArray<plUInt8>());
        }
      }
      else
      {
        // Two-call idiom for the .glb bytes. A failure leaves the key unset, so the next poll tries again.
        plUInt32 uiSize = 0;
        plDynamicArray<plUInt8> glb;
        XrResult res = ext.pfn_xrLoadControllerModelMSFT(m_pSession, keyState.modelKey, 0, &uiSize, nullptr);
        if (!XR_FAILED_RESULT(res) && uiSize > 0)
        {
          glb.SetCountUninitialized(uiSize);
          res = ext.pfn_xrLoadControllerModelMSFT(m_pSession, keyState.modelKey, uiSize, &uiSize, glb.GetData());
          glb.SetCount(uiSize);
        }

        // Node names, once per key. xrGetControllerModelStateMSFT reports the poses in this order.
        plHybridArray<XrControllerModelNodePropertiesMSFT, 16> nodeProperties;
        if (!XR_FAILED_RESULT(res) && !glb.IsEmpty())
        {
          XrControllerModelPropertiesMSFT properties{XR_TYPE_CONTROLLER_MODEL_PROPERTIES_MSFT};
          if (!XR_FAILED_RESULT(ext.pfn_xrGetControllerModelPropertiesMSFT(m_pSession, keyState.modelKey, &properties)) && properties.nodeCountOutput > 0)
          {
            nodeProperties.SetCount(properties.nodeCountOutput, {XR_TYPE_CONTROLLER_MODEL_NODE_PROPERTIES_MSFT});
            properties.nodeCapacityInput = nodeProperties.GetCount();
            properties.nodeProperties = nodeProperties.GetData();
            if (XR_FAILED_RESULT(ext.pfn_xrGetControllerModelPropertiesMSFT(m_pSession, keyState.modelKey, &properties)))
            {
              nodeProperties.Clear();
            }
            else
            {
              nodeProperties.SetCount(properties.nodeCountOutput);
            }
          }
        }

        if (XR_FAILED_RESULT(res) || glb.IsEmpty())
        {
          plLog::Warning("OpenXR: Loading the {} controller model failed: {}", uiSide == 0 ? "left" : "right", res);

          // The previous model belongs to another controller; dropping it also re-enables the poll that retries the load.
          if (!state.m_sKey.IsEmpty())
          {
            PublishRenderModel(uiSide, plString(), plDynamicArray<plUInt8>());
          }
        }
        else
        {
          state.m_KeyMSFT = keyState.modelKey;
          for (const XrControllerModelNodePropertiesMSFT& node : nodeProperties)
          {
            state.m_NodeNames.PushBack(node.nodeName);
          }
          state.m_NodeStates.SetCount(state.m_NodeNames.GetCount(), {XR_TYPE_CONTROLLER_MODEL_NODE_STATE_MSFT});

          plStringBuilder sKey;
          sKey.SetFormat("msft:{}", plArgU((plUInt64)keyState.modelKey, 16, true, 16));
          plLog::Info("OpenXR: Loaded the {} controller model {} ({} bytes, {} nodes).", uiSide == 0 ? "left" : "right", sKey, glb.GetCount(), state.m_NodeNames.GetCount());
          PublishRenderModel(uiSide, sKey, std::move(glb));
        }
      }
    }
  }

  if (state.m_KeyMSFT == XR_NULL_CONTROLLER_MODEL_KEY_MSFT || state.m_NodeStates.IsEmpty())
    return;

  // Node poses every tick: buttons, triggers and sticks move with the user's fingers.
  XrControllerModelStateMSFT modelState{XR_TYPE_CONTROLLER_MODEL_STATE_MSFT};
  modelState.nodeCapacityInput = state.m_NodeStates.GetCount();
  modelState.nodeStates = state.m_NodeStates.GetData();
  if (XR_FAILED_RESULT(ext.pfn_xrGetControllerModelStateMSFT(m_pSession, state.m_KeyMSFT, &modelState)))
    return;

  const plUInt32 uiCount = plMath::Min(modelState.nodeCountOutput, state.m_NodeStates.GetCount());
  plDynamicArray<RenderModelNodePose>& poses = snapshot.m_RenderModelNodePoses[uiSide];
  poses.SetCount(uiCount);
  for (plUInt32 i = 0; i < uiCount; ++i)
  {
    // Each pose is in its parent node's space, converted into engine axes like every other pose; plGlbLoader converts
    // the glTF hierarchy with the same mapping.
    const XrPosef& pose = state.m_NodeStates[i].nodePose;
    RenderModelNodePose& out = poses[i];
    if (out.m_sNodeName != state.m_NodeNames[i])
    {
      out.m_sNodeName = state.m_NodeNames[i];
    }
    out.m_Transform = plTransform(plOpenXR::ConvertPosition(pose.position), plOpenXR::ConvertOrientation(pose.orientation));
    out.m_bVisible = true;
  }

  if (snapshot.m_sRenderModelNodePoseKey[uiSide] != state.m_sKey)
  {
    snapshot.m_sRenderModelNodePoseKey[uiSide] = state.m_sKey;
  }
}

void plOpenXRInputDevice::UpdateRenderModelFB(plUInt32 uiSide)
{
  RenderModelState& state = m_RenderModelState[uiSide];
  if (!state.m_bQueryKey || m_RenderModelPathFB[uiSide] == XR_NULL_PATH)
    return;

  state.m_bQueryKey = false;
  const plOpenXR::Extensions& ext = m_pOpenXR->m_Extensions;

  // The runtime rejects property queries for paths the application has not enumerated first.
  if (!m_bRenderModelPathsEnumeratedFB)
  {
    plUInt32 uiPathCount = 0;
    if (XR_FAILED_RESULT(ext.pfn_xrEnumerateRenderModelPathsFB(m_pSession, 0, &uiPathCount, nullptr)))
      return;

    plHybridArray<XrRenderModelPathInfoFB, 8> paths;
    paths.SetCount(uiPathCount, {XR_TYPE_RENDER_MODEL_PATH_INFO_FB});
    if (uiPathCount > 0 && XR_FAILED_RESULT(ext.pfn_xrEnumerateRenderModelPathsFB(m_pSession, uiPathCount, &uiPathCount, paths.GetData())))
      return;

    m_bRenderModelPathsEnumeratedFB = true;
  }

  // Subset 1 is plain glTF 2.0 without texture compression extensions, which plGlbLoader reads.
  XrRenderModelCapabilitiesRequestFB capabilities{XR_TYPE_RENDER_MODEL_CAPABILITIES_REQUEST_FB};
  capabilities.flags = XR_RENDER_MODEL_SUPPORTS_GLTF_2_0_SUBSET_1_BIT_FB;
  XrRenderModelPropertiesFB properties{XR_TYPE_RENDER_MODEL_PROPERTIES_FB};
  properties.next = &capabilities;

  XrResult res = ext.pfn_xrGetRenderModelPropertiesFB(m_pSession, m_RenderModelPathFB[uiSide], &properties);
  // XR_RENDER_MODEL_UNAVAILABLE_FB is a success code meaning "no model for this path right now".
  if (XR_FAILED_RESULT(res) || res == XR_RENDER_MODEL_UNAVAILABLE_FB || properties.modelKey == XR_NULL_RENDER_MODEL_KEY_FB)
  {
    state.m_KeyFB = XR_NULL_RENDER_MODEL_KEY_FB;
    if (!state.m_sKey.IsEmpty())
    {
      PublishRenderModel(uiSide, plString(), plDynamicArray<plUInt8>());
    }
    return;
  }

  if (properties.modelKey == state.m_KeyFB)
    return;

  XrRenderModelLoadInfoFB loadInfo{XR_TYPE_RENDER_MODEL_LOAD_INFO_FB};
  loadInfo.modelKey = properties.modelKey;

  XrRenderModelBufferFB buffer{XR_TYPE_RENDER_MODEL_BUFFER_FB};
  plDynamicArray<plUInt8> glb;
  res = ext.pfn_xrLoadRenderModelFB(m_pSession, &loadInfo, &buffer);
  if (!XR_FAILED_RESULT(res) && res != XR_RENDER_MODEL_UNAVAILABLE_FB && buffer.bufferCountOutput > 0)
  {
    glb.SetCountUninitialized(buffer.bufferCountOutput);
    buffer.bufferCapacityInput = glb.GetCount();
    buffer.buffer = glb.GetData();
    res = ext.pfn_xrLoadRenderModelFB(m_pSession, &loadInfo, &buffer);
    glb.SetCount(buffer.bufferCountOutput);
  }

  if (XR_FAILED_RESULT(res) || res == XR_RENDER_MODEL_UNAVAILABLE_FB || glb.IsEmpty())
  {
    // Not ready yet; the key stays unset so the next poll tries again.
    plLog::Debug("OpenXR: The {} controller render model is not available yet ({}).", uiSide == 0 ? "left" : "right", res);
    return;
  }

  state.m_KeyFB = properties.modelKey;

  plStringBuilder sKey;
  sKey.SetFormat("fb:{}:{}:{}", properties.modelName, properties.modelVersion, plArgU((plUInt64)properties.modelKey, 16, true, 16));
  plLog::Info("OpenXR: Loaded the {} controller model {} ({} bytes).", uiSide == 0 ? "left" : "right", sKey, glb.GetCount());
  PublishRenderModel(uiSide, sKey, std::move(glb));
}

void plOpenXRInputDevice::UpdateActionsOnInputThread()
{
  if (m_pSession == XR_NULL_HANDLE)
    return;
  PL_PROFILE_SCOPE("UpdateActionsThreaded");

  InputSnapshot& snapshot = m_WorkingSnapshot;

  // Sync actions
  XrActiveActionSet activeActionSet{m_pActionSet, XR_NULL_PATH};
  XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
  syncInfo.countActiveActionSets = 1;
  syncInfo.activeActionSets = &activeActionSet;

  XrResult res = xrSyncActions(m_pSession, &syncInfo);
  if (XR_FAILED_RESULT(res))
    return;

  if (res == XR_SESSION_NOT_FOCUSED)
  {
    // Another application (e.g. the runtime's system menu) has input focus: every action reports inactive. Release all
    // buttons and axes so nothing stays held, and drop haptics instead of letting them pile up until focus returns.
    {
      PL_LOCK(m_HapticMutex);
      m_PendingHaptics[0].m_bPending = false;
      m_PendingHaptics[1].m_bPending = false;
    }

    for (auto it = snapshot.m_InputSlotValues.GetIterator(); it.IsValid(); ++it)
    {
      it.Value() = 0.0f;
    }

    PublishSnapshot();
    return;
  }

  if (m_bInteractionProfileDirty.Set(false))
  {
    UpdateInteractionProfiles(snapshot);
  }

  if (m_bGlyphsDirty)
  {
    m_bGlyphsDirty = false;
    UpdateGlyphs();
  }

  ApplyPendingHaptics();

  // Read the published copy rather than plOpenXR::m_FrameState: the main thread overwrites that whole struct in
  // BeginFrame and PreWaitNextFrame, so reading it from this thread can tear.
  const XrTime time = m_pOpenXR->GetPredictedDisplayTime();
  if (time == 0)
    return;

  const XrSpace baseSpace = m_pOpenXR->GetBaseSpace();
  UpdateLeftShoulderTracking(snapshot, baseSpace, time);
  UpdateEyeGaze(snapshot, baseSpace, time);

  for (plUInt32 uiSide : {0, 1})
  {
    const plUInt32 uiControllerId = uiSide == 0 ? m_iLeftControllerDeviceID : m_iRightControllerDeviceID;
    const XrPath subActionPath = m_SubActionPath[uiSide];

    // Check if controller is active
    XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
    XrActionStateGetInfo poseGetInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    poseGetInfo.action = m_PoseActions[0].m_Action;
    poseGetInfo.subactionPath = subActionPath;
    if (xrGetActionStatePose(m_pSession, &poseGetInfo, &poseState) != XR_SUCCESS)
      continue;

    const bool bControllerActive = poseState.isActive;

    if (!bControllerActive)
    {
      if (m_bBoundActionsCached[uiSide])
      {
        // Clear values for disconnected controller
        for (plUInt8 idx : m_BoundBooleanActions[uiSide])
          snapshot.m_InputSlotValues[m_BooleanActions[idx].m_sKey[uiSide]] = 0.0f;
        for (plUInt8 idx : m_BoundFloatActions[uiSide])
          snapshot.m_InputSlotValues[m_FloatActions[idx].m_sKey[uiSide]] = 0.0f;
        for (plUInt8 idx : m_BoundVec2Actions[uiSide])
        {
          const Vec2Action& action = m_Vec2Actions[idx];
          snapshot.m_InputSlotValues[action.m_sKey_negx[uiSide]] = 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posx[uiSide]] = 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_negy[uiSide]] = 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posy[uiSide]] = 0.0f;
        }
        m_bBoundActionsCached[uiSide] = false;
      }
      m_DiscoveredFeatures[uiSide].Clear();
      snapshot.m_SupportedFeatures[uiControllerId].Clear();
      continue;
    }

    // Discover or query bound actions
    if (!m_bBoundActionsCached[uiSide])
    {
      m_BoundBooleanActions[uiSide].Clear();
      m_BoundFloatActions[uiSide].Clear();
      m_BoundVec2Actions[uiSide].Clear();
      m_DiscoveredFeatures[uiSide] = plXRDeviceFeatures::GripPose | plXRDeviceFeatures::AimPose;

      if (IsHapticActionBound(uiSide))
      {
        m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::Haptics);
      }

      XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
      getInfo.subactionPath = subActionPath;

      for (plUInt8 i = 0; i < m_BooleanActions.GetCount(); ++i)
      {
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        getInfo.action = m_BooleanActions[i].m_Action;
        if (xrGetActionStateBoolean(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          if (state.isActive)
          {
            m_BoundBooleanActions[uiSide].PushBack(i);
            m_DiscoveredFeatures[uiSide].Add(m_BooleanActions[i].m_Feature);
          }
          snapshot.m_InputSlotValues[m_BooleanActions[i].m_sKey[uiSide]] = state.currentState ? 1.0f : 0.0f;
        }
      }

      for (plUInt8 i = 0; i < m_FloatActions.GetCount(); ++i)
      {
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        getInfo.action = m_FloatActions[i].m_Action;
        if (xrGetActionStateFloat(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          if (state.isActive)
          {
            m_BoundFloatActions[uiSide].PushBack(i);
            m_DiscoveredFeatures[uiSide].Add(m_FloatActions[i].m_Feature);
          }
          snapshot.m_InputSlotValues[m_FloatActions[i].m_sKey[uiSide]] = state.currentState;
        }
      }

      for (plUInt8 i = 0; i < m_Vec2Actions.GetCount(); ++i)
      {
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        getInfo.action = m_Vec2Actions[i].m_Action;
        if (xrGetActionStateVector2f(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          if (state.isActive)
          {
            m_BoundVec2Actions[uiSide].PushBack(i);
            m_DiscoveredFeatures[uiSide].Add(m_Vec2Actions[i].m_Feature);
          }
          const float x = state.currentState.x;
          const float y = state.currentState.y;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_negx[uiSide]] = x < 0 ? -x : 0.0f;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_posx[uiSide]] = x > 0 ? x : 0.0f;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_negy[uiSide]] = y < 0 ? -y : 0.0f;
          snapshot.m_InputSlotValues[m_Vec2Actions[i].m_sKey_posy[uiSide]] = y > 0 ? y : 0.0f;
        }
      }

      plLog::Info("OpenXR: Controller {} bound actions - Boolean: {}, Float: {}, Vec2: {}",
        uiSide == 0 ? "Left" : "Right",
        m_BoundBooleanActions[uiSide].GetCount(),
        m_BoundFloatActions[uiSide].GetCount(),
        m_BoundVec2Actions[uiSide].GetCount());

      m_bBoundActionsCached[uiSide] = true;
    }
    else
    {
      // Fast path: query only bound actions
      XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
      getInfo.subactionPath = subActionPath;

      for (plUInt8 idx : m_BoundBooleanActions[uiSide])
      {
        const Action& action = m_BooleanActions[idx];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        getInfo.action = action.m_Action;
        if (xrGetActionStateBoolean(m_pSession, &getInfo, &state) == XR_SUCCESS)
          snapshot.m_InputSlotValues[action.m_sKey[uiSide]] = state.currentState ? 1.0f : 0.0f;
      }

      for (plUInt8 idx : m_BoundFloatActions[uiSide])
      {
        const Action& action = m_FloatActions[idx];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        getInfo.action = action.m_Action;
        if (xrGetActionStateFloat(m_pSession, &getInfo, &state) == XR_SUCCESS)
          snapshot.m_InputSlotValues[action.m_sKey[uiSide]] = state.currentState;
      }

      for (plUInt8 idx : m_BoundVec2Actions[uiSide])
      {
        const Vec2Action& action = m_Vec2Actions[idx];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        getInfo.action = action.m_Action;
        if (xrGetActionStateVector2f(m_pSession, &getInfo, &state) == XR_SUCCESS)
        {
          const float x = state.currentState.x;
          const float y = state.currentState.y;
          snapshot.m_InputSlotValues[action.m_sKey_negx[uiSide]] = x < 0 ? -x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posx[uiSide]] = x > 0 ? x : 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_negy[uiSide]] = y < 0 ? -y : 0.0f;
          snapshot.m_InputSlotValues[action.m_sKey_posy[uiSide]] = y > 0 ? y : 0.0f;
        }
      }
    }

    // Update poses. Never hand the runtime a null space: SteamVR breaks inside vrclient rather than returning
    // XR_ERROR_HANDLE_INVALID, which turns a recoverable state into a debugger stop.
    if (m_gripSpace[uiSide] == XR_NULL_HANDLE || m_aimSpace[uiSide] == XR_NULL_HANDLE || baseSpace == XR_NULL_HANDLE)
      continue;

    plXRDeviceState& deviceState = snapshot.m_DeviceState[uiControllerId];
    XrSpaceLocation viewInScene = {XR_TYPE_SPACE_LOCATION};

    if (m_bUseBatchedSpaceLocation)
    {
      // Batched pose query
      XrSpace spaces[2] = {m_gripSpace[uiSide], m_aimSpace[uiSide]};
      XrSpacesLocateInfo locateInfo{XR_TYPE_SPACES_LOCATE_INFO_KHR};
      locateInfo.baseSpace = baseSpace;
      locateInfo.time = time;
      locateInfo.spaceCount = 2;
      locateInfo.spaces = spaces;

      XrSpaceLocationData locationData[2] = {};
      XrSpaceLocations locations{XR_TYPE_SPACE_LOCATIONS_KHR};
      locations.locationCount = 2;
      locations.locations = locationData;

      const XrResult locateRes = m_pOpenXR->m_Extensions.pfn_xrLocateSpacesKHR(m_pSession, &locateInfo, &locations);
      if (locateRes != XR_SUCCESS)
      {
        ReportLocateFailure(uiSide, "xrLocateSpacesKHR", locateRes, baseSpace, time);
      }
      else
      {
        const XrSpaceLocationData& gripLoc = locationData[0];
        const XrSpaceLocationData& aimLoc = locationData[1];

        if ((gripLoc.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vGripPosition = plOpenXR::ConvertPosition(gripLoc.pose.position);
          deviceState.m_qGripRotation = plOpenXR::ConvertOrientation(gripLoc.pose.orientation);
          deviceState.m_bGripPoseIsValid = true;
        }
        else
          deviceState.m_bGripPoseIsValid = false;

        if ((aimLoc.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vAimPosition = plOpenXR::ConvertPosition(aimLoc.pose.position);
          deviceState.m_qAimRotation = plOpenXR::ConvertOrientation(aimLoc.pose.orientation);
          deviceState.m_bAimPoseIsValid = true;
        }
        else
          deviceState.m_bAimPoseIsValid = false;
      }
    }
    else
    {
      // Individual pose queries
      const XrResult gripRes = xrLocateSpace(m_gripSpace[uiSide], baseSpace, time, &viewInScene);
      if (gripRes != XR_SUCCESS)
      {
        ReportLocateFailure(uiSide, "xrLocateSpace(grip)", gripRes, baseSpace, time);
      }
      else
      {
        if ((viewInScene.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vGripPosition = plOpenXR::ConvertPosition(viewInScene.pose.position);
          deviceState.m_qGripRotation = plOpenXR::ConvertOrientation(viewInScene.pose.orientation);
          deviceState.m_bGripPoseIsValid = true;
        }
        else
          deviceState.m_bGripPoseIsValid = false;
      }

      const XrResult aimRes = xrLocateSpace(m_aimSpace[uiSide], baseSpace, time, &viewInScene);
      if (aimRes != XR_SUCCESS)
      {
        ReportLocateFailure(uiSide, "xrLocateSpace(aim)", aimRes, baseSpace, time);
      }
      else
      {
        if ((viewInScene.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        {
          deviceState.m_vAimPosition = plOpenXR::ConvertPosition(viewInScene.pose.position);
          deviceState.m_qAimRotation = plOpenXR::ConvertOrientation(viewInScene.pose.orientation);
          deviceState.m_bAimPoseIsValid = true;
        }
        else
          deviceState.m_bAimPoseIsValid = false;
      }
    }

    ReportPoseValidity(uiSide, deviceState);

    UpdatePalmPose(uiSide, snapshot, baseSpace, time);

    m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::GripPose);
    m_DiscoveredFeatures[uiSide].Add(plXRDeviceFeatures::AimPose);
    snapshot.m_SupportedFeatures[uiControllerId] = m_DiscoveredFeatures[uiSide];
  }

  UpdateMapActions(snapshot);
  UpdateRenderModels(snapshot);

  PublishSnapshot();
}

void plOpenXRInputDevice::UpdateControllerState()
{
  for (plUInt32 uiControllerId : {m_iLeftControllerDeviceID, m_iRightControllerDeviceID, m_iLeftShoulderDeviceId, m_iEyeGazeDeviceId})
  {
    const bool bDeviceConnected = m_SupportedFeatures[uiControllerId].IsSet(plXRDeviceFeatures::AimPose);

    if (!m_DeviceState[uiControllerId].m_bDeviceIsConnected && bDeviceConnected)
    {
      // Connected
      m_DeviceState[uiControllerId].m_bDeviceIsConnected = true;

      plXRDeviceEventData data;
      data.m_Type = plXRDeviceEventData::Type::DeviceAdded;
      data.uiDeviceID = uiControllerId;
      m_InputEvents.Broadcast(data);
    }
    else if (m_DeviceState[uiControllerId].m_bDeviceIsConnected && !bDeviceConnected)
    {
      // Disconnected
      m_DeviceState[uiControllerId] = plXRDeviceState();
      m_SupportedFeatures[uiControllerId] = plXRDeviceFeatures::None;

      plXRDeviceEventData data;
      data.m_Type = plXRDeviceEventData::Type::DeviceRemoved;
      data.uiDeviceID = uiControllerId;
      m_InputEvents.Broadcast(data);
    }
  }
}

bool plOpenXRInputDevice::IsHapticActionBound(plUInt32 uiSide) const
{
  if (m_pSession == XR_NULL_HANDLE || m_HapticAction == XR_NULL_HANDLE)
    return false;

  XrBoundSourcesForActionEnumerateInfo enumerateInfo{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
  enumerateInfo.action = m_HapticAction;

  plUInt32 uiSourceCount = 0;
  if (XR_FAILED_RESULT(xrEnumerateBoundSourcesForAction(m_pSession, &enumerateInfo, 0, &uiSourceCount, nullptr)) || uiSourceCount == 0)
    return false;

  plHybridArray<XrPath, 4> sources;
  sources.SetCount(uiSourceCount);
  if (XR_FAILED_RESULT(xrEnumerateBoundSourcesForAction(m_pSession, &enumerateInfo, uiSourceCount, &uiSourceCount, sources.GetData())))
    return false;

  // The enumeration covers both subaction paths, so the hand is identified by the source path prefix.
  const char* szPrefix = uiSide == 0 ? "/user/hand/left" : "/user/hand/right";
  for (XrPath source : sources)
  {
    char szBuffer[XR_MAX_PATH_LENGTH] = {};
    plUInt32 uiWritten = 0;
    if (xrPathToString(m_pInstance, source, XR_MAX_PATH_LENGTH, &uiWritten, szBuffer) == XR_SUCCESS && plStringUtils::StartsWith(szBuffer, szPrefix))
      return true;
  }

  return false;
}

plResult plOpenXRInputDevice::TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz)
{
  if (m_pSession == XR_NULL_HANDLE || m_HapticAction == XR_NULL_HANDLE)
    return PL_FAILURE;

  plUInt32 uiSide = 0;
  if (deviceID == m_iLeftControllerDeviceID)
    uiSide = 0;
  else if (deviceID == m_iRightControllerDeviceID)
    uiSide = 1;
  else
    return PL_FAILURE;

  // Queued rather than applied here. This runs on the main thread while the input thread is calling into the same
  // XrSession at 200 Hz, and SteamVR's client library breaks when two threads touch one session; the input thread
  // applies the pulse on its next tick instead. Success therefore means "accepted", not "the runtime played it".
  {
    PL_LOCK(m_HapticMutex);

    PendingHaptic& pending = m_PendingHaptics[uiSide];
    pending.m_fAmplitude = plMath::Clamp(fAmplitude, 0.0f, 1.0f);
    pending.m_fFrequencyHz = fFrequencyHz;
    pending.m_Duration = duration;
    pending.m_bPending = true;
  }

  return PL_SUCCESS;
}

void plOpenXRInputDevice::ReportPoseValidity(plUInt32 uiSide, const plXRDeviceState& state)
{
  // Runs at the input thread's tick rate, so only a change is logged. A controller whose poses never become valid is
  // hidden by its plDeviceTrackingComponent, which otherwise looks like a missing model or a hand that cannot grab.
  const plInt8 iGrip = state.m_bGripPoseIsValid ? 1 : 0;
  const plInt8 iAim = state.m_bAimPoseIsValid ? 1 : 0;

  if (m_iLastGripValid[uiSide] == iGrip && m_iLastAimValid[uiSide] == iAim)
    return;

  m_iLastGripValid[uiSide] = iGrip;
  m_iLastAimValid[uiSide] = iAim;

  plLog::Info("OpenXR: {} controller poses: grip {}, aim {} ({}).", uiSide == 0 ? "Left" : "Right", iGrip ? "valid" : "INVALID",
    iAim ? "valid" : "INVALID", m_bUseBatchedSpaceLocation ? "xrLocateSpacesKHR" : "xrLocateSpace");
}

void plOpenXRInputDevice::ReportLocateFailure(plUInt32 uiSide, const char* szWhat, XrResult res, XrSpace baseSpace, XrTime time)
{
  // Runs at the input thread's tick rate, so only report a given hand's result once until it changes.
  if (m_LastLocateFailure[uiSide] == res)
    return;

  m_LastLocateFailure[uiSide] = res;

  plLog::Error("OpenXR: {} failed for the {} hand: {}. grip={}, aim={}, base={}, displayTime={}", szWhat, uiSide == 0 ? "left" : "right", res,
    (plUInt64)m_gripSpace[uiSide], (plUInt64)m_aimSpace[uiSide], (plUInt64)baseSpace, (plInt64)time);
}

void plOpenXRInputDevice::ApplyPendingHaptics()
{
  if (m_pSession == XR_NULL_HANDLE || m_HapticAction == XR_NULL_HANDLE)
    return;

  for (plUInt32 uiSide : {0, 1})
  {
    PendingHaptic request;
    {
      PL_LOCK(m_HapticMutex);

      if (!m_PendingHaptics[uiSide].m_bPending)
        continue;

      request = m_PendingHaptics[uiSide];
      m_PendingHaptics[uiSide].m_bPending = false;
    }

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = request.m_fAmplitude;
    vibration.frequency = request.m_fFrequencyHz > 0.0f ? request.m_fFrequencyHz : XR_FREQUENCY_UNSPECIFIED;
    vibration.duration = request.m_Duration > plTime::MakeZero() ? static_cast<XrDuration>(request.m_Duration.GetNanoseconds()) : XR_MIN_HAPTIC_DURATION;

    XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
    hapticActionInfo.action = m_HapticAction;
    hapticActionInfo.subactionPath = m_SubActionPath[uiSide];

    xrApplyHapticFeedback(m_pSession, &hapticActionInfo, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
  }
}

plOpenXRInputDevice::Vec2Action::Vec2Action(plXRDeviceFeatures::Enum feature, XrAction pAction, plStringView sLeft, plStringView sRight)
{
  m_Feature = feature;
  m_Action = pAction;

  plStringView sides[2] = {sLeft, sRight};
  for (plUInt32 uiSide : {0, 1})
  {
    plStringBuilder temp = sides[uiSide];
    temp.Append("_negx");
    m_sKey_negx[uiSide] = temp;

    temp.Shrink(0, 5);
    temp.Append("_posx");
    m_sKey_posx[uiSide] = temp;

    temp.Shrink(0, 5);
    temp.Append("_negy");
    m_sKey_negy[uiSide] = temp;

    temp.Shrink(0, 5);
    temp.Append("_posy");
    m_sKey_posy[uiSide] = temp;
  }
}

// ============================================================================
// Input Thread Implementation
// ============================================================================

void plOpenXRInputDevice::StartInputThread()
{
  if (m_pInputThread != nullptr)
    return;

  m_bInputThreadRunning = true;

  m_pInputThread = PL_DEFAULT_NEW(InputThread, this);
  m_pInputThread->Start();

  plLog::Info("OpenXR: Input thread started");
}

void plOpenXRInputDevice::StopInputThread()
{
  if (m_pInputThread == nullptr)
    return;

  m_bInputThreadRunning = false;
  m_pInputThread->Join();
  m_pInputThread = nullptr;

  plLog::Info("OpenXR: Input thread stopped");
}

plOpenXRInputDevice::InputThread::InputThread(plOpenXRInputDevice* pOwner)
  : plThread("OpenXR Input")
  , m_pOwner(pOwner)
{
}

plUInt32 plOpenXRInputDevice::InputThread::Run()
{
  plLog::Info("OpenXR: Input thread running at 200Hz");

  while (m_pOwner->m_bInputThreadRunning)
  {
    // Poll OpenXR actions and update the back buffer
    m_pOwner->UpdateActionsOnInputThread();

    // Run at approximately 200Hz (5ms between updates)
    // This is faster than typical VR frame rates (90Hz = 11ms)
    // to minimize latency while keeping CPU usage low
    plThreadUtils::Sleep(plTime::MakeFromMilliseconds(5));
  }

  return 0;
}


PL_STATICLINK_FILE(OpenXRPlugin, OpenXRPlugin_OpenXRInputDevice);
