#pragma once

#include <OpenXRPlugin/Basics.h>
#include <OpenXRPlugin/OpenXRIncludes.h>

#include <Core/Input/ActionMap/InputActionMapDescriptor.h>
#include <GameEngine/XR/XRInputDevice.h>
#include <GameEngine/XR/XRInteractionProfiles.h>
#include <GameEngine/XR/XRInterface.h>
#include <Foundation/Threading/Thread.h>
#include <Foundation/Threading/Mutex.h>
#include <Foundation/Threading/AtomicInteger.h>

class plOpenXR;

PL_DEFINE_AS_POD_TYPE(XrActionSuggestedBinding);
PL_DEFINE_AS_POD_TYPE(XrActiveActionSet);
PL_DEFINE_AS_POD_TYPE(XrControllerModelNodeStateMSFT);
PL_DEFINE_AS_POD_TYPE(XrInteractionProfileDpadBindingEXT);
struct plInputActionMapEvent;

class PL_OPENXRPLUGIN_DLL plOpenXRInputDevice : public plXRInputDevice
{
  PL_ADD_DYNAMIC_REFLECTION(plOpenXRInputDevice, plXRInputDevice);

public:
  void GetDeviceList(plHybridArray<plXRDeviceID, 64>& out_devices) const override;
  plXRDeviceID GetDeviceIDByType(plXRDeviceType::Enum type) const override;
  const plXRDeviceState& GetDeviceState(plXRDeviceID deviceID) const override;
  plString GetDeviceName(plXRDeviceID deviceID) const override;
  plBitflags<plXRDeviceFeatures> GetDeviceFeatures(plXRDeviceID deviceID) const override;

  plString GetRenderModelKey(plXRDeviceID deviceID) const override;
  plResult TryGetRenderModel(plXRDeviceID deviceID, RenderModel& out_model) const override;
  plResult TryGetRenderModelNodePoses(plXRDeviceID deviceID, plDynamicArray<RenderModelNodePose>& out_poses) const override;

private:
  friend class plOpenXR;
  // Left-shoulder Vive tracker. Only created when XR_HTCX_vive_tracker_interaction is enabled: its subaction path is
  // not a valid top-level path otherwise, and a failed xrCreateAction would take every other action down with it.
  XrAction shoulderPoseAction = XR_NULL_HANDLE;
  XrSpace shoulderSpace = XR_NULL_HANDLE;

  // Eye gaze pose (XR_EXT_eye_gaze_interaction, cvar XR.EyeGaze). Only created when the extension is enabled and the
  // system reports eye gaze support; its top-level path /user/eyes_ext is invalid otherwise.
  XrAction m_EyeGazeAction = XR_NULL_HANDLE;
  XrSpace m_EyeGazeSpace = XR_NULL_HANDLE;

  struct Bind
  {
    XrAction action;
    const char* szPath;
  };

  struct Action
  {
    plXRDeviceFeatures::Enum m_Feature;
    XrAction m_Action;
    plString m_sKey[2];
  };

  struct Vec2Action
  {
    Vec2Action(plXRDeviceFeatures::Enum feature, XrAction pAction, plStringView sLeft, plStringView sRight);
    plXRDeviceFeatures::Enum m_Feature;
    XrAction m_Action;
    plString m_sKey_negx[2];
    plString m_sKey_posx[2];
    plString m_sKey_negy[2];
    plString m_sKey_posy[2];
  };

  /// \brief An OpenXR action created for an input action map action that has XR paths.
  ///
  /// It has no subaction paths, so the runtime combines every bound source, and any top-level path can be bound. The
  /// value goes into the action's generated 'xr_action_<context>_<action>' slots.
  struct MapAction
  {
    XrAction m_Action = XR_NULL_HANDLE;
    XrActionType m_Type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    plString m_sName;        ///< for logs: '<context>/<action>'
    plString m_sDisplayName; ///< input slot display name
    plString m_sSlot;        ///< base slot; boolean and float value, vector2 length
    plString m_sSlotNegX;    ///< float and vector2 only
    plString m_sSlotPosX;
    plString m_sSlotNegY;    ///< vector2 only
    plString m_sSlotPosY;
    plHybridArray<plInputXRPathDesc, 2> m_Paths;
  };

  /// \brief One binding to suggest for an interaction profile, with its dpad settings for dpad paths.
  struct SuggestedBinding
  {
    XrActionSuggestedBinding m_Binding{XR_NULL_HANDLE, XR_NULL_PATH};
    plString m_sPath;
    plString m_sOwner;      ///< for logs: the built-in action or the input action map action
    bool m_bDpad = false;
    XrInteractionProfileDpadBindingEXT m_Dpad{XR_TYPE_INTERACTION_PROFILE_DPAD_BINDING_EXT};
  };

  plOpenXRInputDevice(plOpenXR* pOpenXR);
  XrResult CreateActions(XrSession session, XrSpace m_sceneSpace);
  void DestroyActions();

  XrPath CreatePath(const char* szPath);
  XrResult CreateAction(plXRDeviceFeatures::Enum feature, const char* actionName, XrActionType actionType, XrAction& out_action);

  /// \brief Loads the interaction profile table, from XRInteractionProfiles.ddl or the built-in copy.
  void LoadInteractionProfiles();

  /// \brief Creates one action per input action map action with XR paths. Main thread, at session start.
  void CreateMapActions();

  /// \brief Suggests the built-in and the action map bindings for one interaction profile.
  ///
  /// The built-in bindings go in first. If the runtime rejects the full set, each action map binding (and palm pose) is
  /// tried on its own and only the ones the runtime accepts are kept, so one bad path does not lose the whole profile.
  XrResult SuggestProfile(const plXRInteractionProfileDesc& profile, const plHashTable<plString, XrAction>& builtInActions);
  XrResult TrySuggestBindings(XrPath profilePath, plArrayPtr<const SuggestedBinding> bindings);

  /// \brief Suggests bindings for one interaction profile.
  ///
  /// Profiles that come from an extension are only valid if that extension was enabled on the instance. Pass
  /// bOptionalProfile = true for those so an unsupported profile path is reported as info instead of an error.
  XrResult SuggestInteractionProfileBindings(const char* szInteractionProfile, const char* szNiceName, plArrayPtr<Bind> bindings, bool bOptionalProfile = false);
  XrResult AttachSessionActionSets(XrSession session);

  /// \brief The XR paths of the current input action map, as a string that changes when any of them change.
  static plString BuildMapBindingSignature(const plInputActionMapDescriptor& desc);
  void OnActionMapEvent(const plInputActionMapEvent& e);

  /// \brief Reads the runtime's names for the bound inputs into plInputGlyphs. Input thread, after a profile change.
  void UpdateGlyphs();
  /// \brief The runtime's name for the first source bound to the action, among the sources below sHandPrefix if given.
  plString GetLocalizedSourceName(XrAction action, plStringView sHandPrefix, XrInputSourceLocalizedNameFlags flags) const;

  /// \brief Asks the input thread to re-query the active interaction profiles on its next tick.
  ///
  /// Called for XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED. The query itself is an XrSession call, so it runs on the
  /// input thread.
  void RequestInteractionProfileUpdate();

  /// \brief Applies any queued haptic pulses. Input thread only - all XrSession calls stay on that one thread.
  void ApplyPendingHaptics();

  /// \brief Logs a pose-location failure once per hand until the result code changes.
  void ReportLocateFailure(plUInt32 uiSide, const char* szWhat, XrResult res, XrSpace baseSpace, XrTime time);

  /// \brief Applies a vibration to one controller. fAmplitude is clamped to 0-1, fFrequencyHz <= 0 uses the runtime default.
  plResult TriggerHapticPulse(plXRDeviceID deviceID, plTime duration, float fAmplitude, float fFrequencyHz);

  /// \brief Whether the active interaction profile bound the haptic output action for the given hand.
  bool IsHapticActionBound(plUInt32 uiSide) const;

  void InitializeDevice() override;
  void RegisterInputSlots() override;
  void UpdateInputSlotValues() override;

  XrResult UpdateActions();
  void UpdateControllerState();

private:
  plOpenXR* m_pOpenXR = nullptr;
  XrInstance m_pInstance = XR_NULL_HANDLE;
  XrSession m_pSession = XR_NULL_HANDLE;
public:
  // HMD, left hand, right hand, left-shoulder vive tracker, eye gaze. Every per-device array and every bounds check
  // derives from this.
  static constexpr plInt8 s_iMaxDevices = 5;

private:
  plXRDeviceState m_DeviceState[s_iMaxDevices];
  plString m_sActiveProfile[s_iMaxDevices];
  plBitflags<plXRDeviceFeatures> m_SupportedFeatures[s_iMaxDevices];
  const plInt8 m_iLeftControllerDeviceID = 1;
  const plInt8 m_iRightControllerDeviceID = 2;
  const plInt8 m_iLeftShoulderDeviceId = 3;
  const plInt8 m_iEyeGazeDeviceId = 4;

  XrActionSet m_pActionSet = XR_NULL_HANDLE;
  plHashTable<XrPath, plString> m_InteractionProfileToNiceName;

  plStaticArray<const char*, 3> m_SubActionPrefix;
  plStaticArray<XrPath, 2> m_SubActionPath;
  plStaticArray<XrPath, 3> m_SubActionPathVive;

  plHybridArray<Action, 4> m_BooleanActions;
  plHybridArray<Action, 4> m_FloatActions;
  plHybridArray<Vec2Action, 4> m_Vec2Actions;
  plHybridArray<Action, 4> m_PoseActions;

  // Vibration output action, shared by both hands via the subaction paths.
  XrAction m_HapticAction = XR_NULL_HANDLE;

  // Cached indices of bound actions per controller (updated when interaction profile changes)
  plHybridArray<plUInt8, 16> m_BoundBooleanActions[2];
  plHybridArray<plUInt8, 4> m_BoundFloatActions[2];
  plHybridArray<plUInt8, 4> m_BoundVec2Actions[2];
  bool m_bBoundActionsCached[2] = {false, false};

  // Features found during action discovery. Owned by the input thread.
  plBitflags<plXRDeviceFeatures> m_DiscoveredFeatures[2];

  XrSpace m_gripSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace m_aimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

  // Palm pose (XR_EXT_palm_pose, cvar XR.PalmPose), the third pose per hand. Only created when the latched cvar and the
  // extension are both on.
  XrAction m_PalmPoseAction = XR_NULL_HANDLE;
  XrSpace m_palmSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

  // Interaction profile table, loaded at session start. Main thread.
  plDynamicArray<plXRInteractionProfileDesc> m_Profiles;

  // Input action map actions with XR paths. Created on the main thread before the input thread starts and not changed
  // until it is joined, so the input thread reads them without a lock.
  plDynamicArray<MapAction> m_MapActions;

  // Suggested bindings are frozen once the action set is attached. The map's XR paths at session start, so a later change
  // can be reported once.
  plString m_sMapBindingSignature;
  bool m_bMapChangeLogged = false;
  plEventSubscriptionID m_ActionMapEventId = 0;

  // XR.BindingModification + XR.DpadBinding and XR.PalmPose, latched at session start together with their extensions.
  bool m_bUseDpadBinding = false;
  bool m_bUsePalmPose = false;

  // Input thread: re-read the runtime's input names for the glyphs, set when a hand's interaction profile changes.
  bool m_bGlyphsDirty = false;

  //////////////////////////////////////////////////////////////////////////
  // Background Input Thread
  //////////////////////////////////////////////////////////////////////////

  class InputThread : public plThread
  {
  public:
    InputThread(plOpenXRInputDevice* pOwner);
    virtual plUInt32 Run() override;
    plOpenXRInputDevice* m_pOwner = nullptr;
  };

  // Input state produced by the input thread for devices 1..s_iMaxDevices-1. Slot 0 (the HMD) is owned by the main
  // thread and is never read from a snapshot. m_bDeviceIsConnected is also main-thread state: UpdateControllerState
  // derives it from the supported features.
  struct InputSnapshot
  {
    plXRDeviceState m_DeviceState[s_iMaxDevices];
    plBitflags<plXRDeviceFeatures> m_SupportedFeatures[s_iMaxDevices];
    plString m_sActiveProfile[s_iMaxDevices];
    plHashTable<plString, float> m_InputSlotValues;

    // Per hand: the animated node poses of the render model whose key is m_sRenderModelNodePoseKey (MSFT only).
    plString m_sRenderModelNodePoseKey[2];
    plDynamicArray<RenderModelNodePose> m_RenderModelNodePoses[2];

    void Clear();
  };

  // Render model state per hand. Owned by the input thread.
  struct RenderModelState
  {
    XrControllerModelKeyMSFT m_KeyMSFT = XR_NULL_CONTROLLER_MODEL_KEY_MSFT;
    XrRenderModelKeyFB m_KeyFB = XR_NULL_RENDER_MODEL_KEY_FB;
    plString m_sKey;                                ///< the key handed to the main thread, empty while there is no model
    plDynamicArray<plString> m_NodeNames;           ///< MSFT node names, index-aligned with xrGetControllerModelStateMSFT
    plDynamicArray<XrControllerModelNodeStateMSFT> m_NodeStates;
    bool m_bQueryKey = true;                        ///< re-read the key on the next tick
    plTime m_NextRetry;                             ///< while there is no model, the key is polled at this interval

    void Reset();
  };

  // A newly loaded (or vanished) model, handed from the input thread to the main thread under m_RenderModelMutex.
  struct PendingRenderModel
  {
    RenderModel m_Model;
    bool m_bPending = false;
  };

  // Haptic request handed from the main thread to the input thread, so only one thread ever calls into the XrSession.
  struct PendingHaptic
  {
    plTime m_Duration;
    float m_fAmplitude = 0.0f;
    float m_fFrequencyHz = 0.0f;
    bool m_bPending = false;
  };

  plUniquePtr<InputThread> m_pInputThread;
  plAtomicBool m_bInputThreadRunning{false};

  // The input thread builds each tick in m_WorkingSnapshot, which no other thread touches, then copies it into
  // m_PublishedSnapshot under m_SnapshotMutex. The main thread copies out of m_PublishedSnapshot under the same lock.
  // Both copies only assign values to existing keys once every slot has been seen, so neither side rehashes the other's
  // table.
  InputSnapshot m_WorkingSnapshot;
  InputSnapshot m_PublishedSnapshot;
  plMutex m_SnapshotMutex;

  // Set by RequestInteractionProfileUpdate, consumed by the input thread. Starts set so the first tick fills in the
  // profile names.
  plAtomicBool m_bInteractionProfileDirty{true};

  PendingHaptic m_PendingHaptics[2];
  plMutex m_HapticMutex;

  // Last reported pose-location result per hand, so the input thread's tick rate cannot spam the log.
  XrResult m_LastLocateFailure[2] = {XR_SUCCESS, XR_SUCCESS};

  // XR.BatchedSpaceLocation, latched at action creation (session start). The cvar must NOT be read live on the
  // input thread: it can now be toggled from inside the headset (VR CVars panel), and switching the input thread
  // onto the xrLocateSpacesKHR path mid-session faults inside SteamVR's vrclient - a full system hang on this
  // hardware. Latching turns the toggle into a deliberate next-session decision.
  bool m_bUseBatchedSpaceLocation = false;

  // Render models (cvar XR.RenderModels).
  RenderModelState m_RenderModelState[2];
  PendingRenderModel m_PendingRenderModel[2];
  plMutex m_RenderModelMutex;
  XrPath m_RenderModelPathFB[2] = {XR_NULL_PATH, XR_NULL_PATH}; ///< /model_fb/controller/left and right
  bool m_bRenderModelPathsEnumeratedFB = false;                 ///< input thread; xrEnumerateRenderModelPathsFB must come first
  // Main-thread copies, filled in CopySnapshotToMainThread.
  RenderModel m_RenderModel[2];
  plString m_sRenderModelNodePoseKey[2];
  plDynamicArray<RenderModelNodePose> m_RenderModelNodePoses[2];

  /// \brief Returns 0 or 1 for the hand controllers, -1 for every other device.
  plInt32 GetRenderModelSide(plXRDeviceID deviceID) const;
  void UpdateRenderModels(InputSnapshot& snapshot);                                   // Called by input thread
  void UpdateRenderModelMSFT(plUInt32 uiSide, InputSnapshot& snapshot);               // Called by input thread
  void UpdateRenderModelFB(plUInt32 uiSide);                                          // Called by input thread
  void PublishRenderModel(plUInt32 uiSide, const plString& sKey, plDynamicArray<plUInt8>&& glb); // Called by input thread
  void ResetRenderModels();

  void StartInputThread();
  void StopInputThread();
  void UpdateLeftShoulderTracking(InputSnapshot& snapshot, XrSpace baseSpace, XrTime time); // Called by input thread
  void UpdateEyeGaze(InputSnapshot& snapshot, XrSpace baseSpace, XrTime time);              // Called by input thread

  void UpdateInteractionProfiles(InputSnapshot& snapshot); // Called by input thread
  void UpdateMapActions(InputSnapshot& snapshot);          // Called by input thread
  void UpdatePalmPose(plUInt32 uiSide, InputSnapshot& snapshot, XrSpace baseSpace, XrTime time); // Called by input thread
  void UpdateActionsOnInputThread();  // Called by input thread
  void PublishSnapshot();             // Called by input thread
  void CopySnapshotToMainThread();    // Called by main thread in UpdateActions
};
