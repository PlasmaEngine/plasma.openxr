#include <OpenXRPlugin/OpenXRPluginPCH.h>

#include <Core/World/World.h>
#include <GameEngine/XR/StageSpaceComponent.h>
#include <OpenXRPlugin/OpenXRDeclarations.h>
#include <OpenXRPlugin/OpenXRSingleton.h>
#include <OpenXRPlugin/OpenXRSpatialAnchors.h>

PL_IMPLEMENT_SINGLETON(plOpenXRSpatialAnchors);

plOpenXRSpatialAnchors::plOpenXRSpatialAnchors(plOpenXR* pOpenXR)
  : m_SingletonRegistrar(this)
  , m_pOpenXR(pOpenXR)
{
  PL_ASSERT_DEV(m_pOpenXR->m_Extensions.m_bSpatialAnchor, "Spatial anchors not supported");
}

plOpenXRSpatialAnchors::~plOpenXRSpatialAnchors()
{
  for (auto it = m_Anchors.GetIterator(); it.IsValid(); ++it)
  {
    AnchorData anchorData;
    if (m_Anchors.TryGetValue(it.Id(), anchorData))
    {
      XR_LOG_ERROR(m_pOpenXR->m_Extensions.pfn_xrDestroySpatialAnchorMSFT(anchorData.m_Anchor));
      XR_LOG_ERROR(xrDestroySpace(anchorData.m_Space));
    }
  }
  m_Anchors.Clear();
}

plXRSpatialAnchorID plOpenXRSpatialAnchors::CreateAnchor(const plTransform& globalTransform)
{
  plWorld* pWorld = m_pOpenXR->GetWorld();
  if (pWorld == nullptr)
    return plXRSpatialAnchorID();

  plTransform globalStageTransform;
  globalStageTransform.SetIdentity();
  if (const plStageSpaceComponentManager* pStageMan = pWorld->GetComponentManager<plStageSpaceComponentManager>())
  {
    if (const plStageSpaceComponent* pStage = pStageMan->GetSingletonComponent())
    {
      globalStageTransform = pStage->GetOwner()->GetGlobalTransform();
    }
  }
  plTransform local = plTransform::MakeLocalTransform(globalStageTransform, globalTransform);

  XrSpatialAnchorCreateInfoMSFT createInfo{XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_MSFT};
  createInfo.space = m_pOpenXR->GetBaseSpace();
  createInfo.pose.position = plOpenXR::ConvertPosition(local.m_vPosition);
  createInfo.pose.orientation = plOpenXR::ConvertOrientation(local.m_qRotation);
  // Components call this during the world update, which can overlap the main thread rewriting m_FrameState.
  createInfo.time = m_pOpenXR->GetPredictedDisplayTime();

  XrSpatialAnchorMSFT anchor = XR_NULL_HANDLE;
  XrResult res = m_pOpenXR->m_Extensions.pfn_xrCreateSpatialAnchorMSFT(m_pOpenXR->m_pSession, &createInfo, &anchor);
  if (res != XrResult::XR_SUCCESS)
    return plXRSpatialAnchorID();

  XrSpatialAnchorSpaceCreateInfoMSFT createSpaceInfo{XR_TYPE_SPATIAL_ANCHOR_SPACE_CREATE_INFO_MSFT};
  createSpaceInfo.anchor = anchor;
  createSpaceInfo.poseInAnchorSpace = plOpenXR::ConvertTransform(plTransform::MakeIdentity());

  XrSpace space = XR_NULL_HANDLE;
  res = m_pOpenXR->m_Extensions.pfn_xrCreateSpatialAnchorSpaceMSFT(m_pOpenXR->m_pSession, &createSpaceInfo, &space);
  if (res != XrResult::XR_SUCCESS)
  {
    plLog::Error("OpenXR: Creating the space for a spatial anchor failed: {}", res);
    XR_LOG_ERROR(m_pOpenXR->m_Extensions.pfn_xrDestroySpatialAnchorMSFT(anchor));
    return plXRSpatialAnchorID();
  }

  return m_Anchors.Insert({anchor, space});
}

plResult plOpenXRSpatialAnchors::DestroyAnchor(plXRSpatialAnchorID id)
{
  AnchorData anchorData;
  if (!m_Anchors.TryGetValue(id, anchorData))
    return PL_FAILURE;

  XR_LOG_ERROR(m_pOpenXR->m_Extensions.pfn_xrDestroySpatialAnchorMSFT(anchorData.m_Anchor));
  XR_LOG_ERROR(xrDestroySpace(anchorData.m_Space));
  m_Anchors.Remove(id);

  return PL_SUCCESS;
}

plResult plOpenXRSpatialAnchors::TryGetAnchorTransform(plXRSpatialAnchorID id, plTransform& out_globalTransform)
{
  plWorld* pWorld = m_pOpenXR->GetWorld();
  if (!pWorld)
    return PL_FAILURE;

  AnchorData anchorData;
  if (!m_Anchors.TryGetValue(id, anchorData))
    return PL_FAILURE;

  const XrTime time = m_pOpenXR->GetPredictedDisplayTime();
  XrSpaceLocation viewInScene = {XR_TYPE_SPACE_LOCATION};
  XrResult res = xrLocateSpace(anchorData.m_Space, m_pOpenXR->m_pSceneSpace, time, &viewInScene);
  if (res != XrResult::XR_SUCCESS)
    return PL_FAILURE;

  if ((viewInScene.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
      (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
  {
    plTransform globalStageTransform;
    globalStageTransform.SetIdentity();
    if (const plStageSpaceComponentManager* pStageMan = pWorld->GetComponentManager<plStageSpaceComponentManager>())
    {
      if (const plStageSpaceComponent* pStage = pStageMan->GetSingletonComponent())
      {
        globalStageTransform = pStage->GetOwner()->GetGlobalTransform();
      }
    }
    plTransform local(plOpenXR::ConvertPosition(viewInScene.pose.position), plOpenXR::ConvertOrientation(viewInScene.pose.orientation));
    out_globalTransform = plTransform::MakeGlobalTransform(globalStageTransform, local);

    return PL_SUCCESS;
  }
  return PL_FAILURE;
}
