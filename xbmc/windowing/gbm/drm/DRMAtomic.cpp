/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DRMAtomic.h"

#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "utils/log.h"

#include <errno.h>
#include <string.h>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <unistd.h>

using namespace KODI::WINDOWING::GBM;

namespace
{

const auto SETTING_VIDEOSCREEN_HW_SCALING_FILTER = "videoscreen.hwscalingfilter";

enum drm_scaling_filter
{
  DRM_SCALING_FILTER_DEFAULT,
  DRM_SCALING_FILTER_NEAREST_NEIGHBOR,
};

uint32_t GetScalingFilterType(const char* type)
{
  if (!strcmp(type, "Nearest Neighbor"))
    return DRM_SCALING_FILTER_NEAREST_NEIGHBOR;
  else
    return DRM_SCALING_FILTER_DEFAULT;
}

uint32_t GetScalingFactor(uint32_t srcWidth,
                          uint32_t srcHeight,
                          uint32_t destWidth,
                          uint32_t destHeight)
{
  uint32_t factor_W = destWidth / srcWidth;
  uint32_t factor_H = destHeight / srcHeight;
  if (factor_W != factor_H)
    return (factor_W < factor_H) ? factor_W : factor_H;
  return factor_W;
}

} // namespace

bool CDRMAtomic::SetScalingFilter(CDRMObject* object, const char* name, const char* type)
{
  uint32_t filter_type = GetScalingFilterType(type);
  if (object->SupportsPropertyAndValue(name, filter_type))
  {
    if (AddProperty(object, name, filter_type))
    {
      uint32_t mar_scale_factor =
          GetScalingFactor(m_width, m_height, m_mode->hdisplay, m_mode->vdisplay);
      uint32_t diff_w = m_mode->hdisplay - (mar_scale_factor * m_width);
      uint32_t diff_h = m_mode->vdisplay - (mar_scale_factor * m_height);
      AddProperty(object, "CRTC_X", (diff_w / 2));
      AddProperty(object, "CRTC_Y", (diff_h / 2));
      AddProperty(object, "CRTC_W", (mar_scale_factor * m_width));
      AddProperty(object, "CRTC_H", (mar_scale_factor * m_height));
      return true;
    }
  }
  return false;
}

void CDRMAtomic::DrmAtomicCommit(int fb_id, int flags, bool rendered, bool videoLayer)
{
  uint32_t blob_id;

  if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)
  {
    if (!AddProperty(m_connector, "CRTC_ID", m_crtc->GetCrtcId()))
      return;

    if (drmModeCreatePropertyBlob(m_fd, m_mode, sizeof(*m_mode), &blob_id) != 0)
      return;

    if (m_active && m_orig_crtc && m_orig_crtc->GetCrtcId() != m_crtc->GetCrtcId())
    {
      // if using a different CRTC than the original, disable original to avoid EINVAL
      if (!AddProperty(m_orig_crtc, "MODE_ID", 0))
        return;

      if (!AddProperty(m_orig_crtc, "ACTIVE", 0))
        return;
    }

    if (!AddProperty(m_crtc, "MODE_ID", blob_id))
      return;

    if (!AddProperty(m_crtc, "ACTIVE", m_active ? 1 : 0))
      return;
  }

  if (rendered)
  {
    AddProperty(m_gui_plane, "FB_ID", fb_id);
    AddProperty(m_gui_plane, "CRTC_ID", m_crtc->GetCrtcId());
    AddProperty(m_gui_plane, "SRC_X", 0);
    AddProperty(m_gui_plane, "SRC_Y", 0);
    AddProperty(m_gui_plane, "SRC_W", m_width << 16);
    AddProperty(m_gui_plane, "SRC_H", m_height << 16);
    //! @todo: disabled until upstream kernel changes are merged
    // if (DisplayHardwareScalingEnabled())
    // {
    //   SetScalingFilter(m_gui_plane, "SCALING_FILTER", "Nearest Neighbor");
    // }
    // else
    {
      AddProperty(m_gui_plane, "CRTC_X", 0);
      AddProperty(m_gui_plane, "CRTC_Y", 0);
      AddProperty(m_gui_plane, "CRTC_W", m_mode->hdisplay);
      AddProperty(m_gui_plane, "CRTC_H", m_mode->vdisplay);
    }

  }
  else if (videoLayer && !CServiceBroker::GetGUI()->GetWindowManager().HasVisibleControls())
  {
    // disable gui plane when video layer is active and gui has no visible controls
    AddProperty(m_gui_plane, "FB_ID", 0);
    AddProperty(m_gui_plane, "CRTC_ID", 0);
  }

  auto ret = drmModeAtomicCommit(m_fd, m_req, flags | DRM_MODE_ATOMIC_TEST_ONLY, nullptr);
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CDRMAtomic::{} - test commit failed: {}", __FUNCTION__, strerror(errno));
  }
  else if (ret == 0)
  {
    ret = drmModeAtomicCommit(m_fd, m_req, flags, nullptr);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CDRMAtomic::{} - atomic commit failed: {}", __FUNCTION__,
                strerror(errno));
    }
  }

  if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)
  {
    if (drmModeDestroyPropertyBlob(m_fd, blob_id) != 0)
      CLog::Log(LOGERROR, "CDRMAtomic::{} - failed to destroy property blob: {}", __FUNCTION__,
                strerror(errno));
  }

  drmModeAtomicFree(m_req);
  m_req = drmModeAtomicAlloc();
}

void CDRMAtomic::FlipPage(struct gbm_bo *bo, bool rendered, bool videoLayer)
{
  struct drm_fb *drm_fb = nullptr;

  if (rendered)
  {
    if (videoLayer)
      m_gui_plane->SetFormat(CDRMUtils::FourCCWithAlpha(m_gui_plane->GetFormat()));
    else
      m_gui_plane->SetFormat(CDRMUtils::FourCCWithoutAlpha(m_gui_plane->GetFormat()));

    drm_fb = CDRMUtils::DrmFbGetFromBo(bo);
    if (!drm_fb)
    {
      CLog::Log(LOGERROR, "CDRMAtomic::{} - Failed to get a new FBO", __FUNCTION__);
      return;
    }
  }

  uint32_t flags = 0;

  if (m_need_modeset)
  {
    flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    m_need_modeset = false;
    CLog::Log(LOGDEBUG, "CDRMAtomic::{} - Execute modeset at next commit", __FUNCTION__);
  }

  DrmAtomicCommit(!drm_fb ? 0 : drm_fb->fb_id, flags, rendered, videoLayer);
}

bool CDRMAtomic::InitDrm()
{
  if (!CDRMUtils::OpenDrm(true))
    return false;

  auto ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret)
  {
    CLog::Log(LOGERROR, "CDRMAtomic::{} - no atomic modesetting support: {}", __FUNCTION__,
              strerror(errno));
    return false;
  }

  m_req = drmModeAtomicAlloc();

  if (!CDRMUtils::InitDrm())
    return false;

  for (auto& plane : m_planes)
  {
    AddProperty(plane.get(), "FB_ID", 0);
    AddProperty(plane.get(), "CRTC_ID", 0);
  }

  CLog::Log(LOGDEBUG, "CDRMAtomic::{} - initialized atomic DRM", __FUNCTION__);

  //! @todo: disabled until upstream kernel changes are merged
  // if (m_gui_plane->SupportsProperty("SCALING_FILTER"))
  // {
  //   const std::shared_ptr<CSettings> settings =
  //       CServiceBroker::GetSettingsComponent()->GetSettings();
  //   settings->GetSetting(SETTING_VIDEOSCREEN_HW_SCALING_FILTER)->SetVisible(true);
  // }

  return true;
}

void CDRMAtomic::DestroyDrm()
{
  CDRMUtils::DestroyDrm();

  drmModeAtomicFree(m_req);
  m_req = nullptr;
}

bool CDRMAtomic::SetVideoMode(const RESOLUTION_INFO& res, struct gbm_bo *bo)
{
  m_need_modeset = true;

  return true;
}

bool CDRMAtomic::SetActive(bool active)
{
  m_need_modeset = true;
  m_active = active;

  return true;
}

bool CDRMAtomic::AddProperty(CDRMObject* object, const char* name, uint64_t value)
{
  uint32_t propId = object->GetPropertyId(name);
  if (propId == 0)
    return false;

  int ret = drmModeAtomicAddProperty(m_req, object->GetId(), propId, value);
  if (ret < 0)
    return false;

  return true;
}

bool CDRMAtomic::DisplayHardwareScalingEnabled()
{
  auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  if (settings && settings->GetBool(SETTING_VIDEOSCREEN_HW_SCALING_FILTER))
    return true;

  return false;
}
