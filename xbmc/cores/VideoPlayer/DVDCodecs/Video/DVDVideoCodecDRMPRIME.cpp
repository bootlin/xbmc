/*
 *      Copyright (C) 2017 Team Kodi
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this Program; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "DVDVideoCodecDRMPRIME.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "settings/Settings.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "windowing/gbm/WinSystemGbm.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/hwcontext.h"
#include "libavutil/pixdesc.h"
}

//------------------------------------------------------------------------------
// Video Buffers
//------------------------------------------------------------------------------

CVideoBufferDRMPRIME::CVideoBufferDRMPRIME(IVideoBufferPool& pool, int id)
  : CVideoBuffer(id)
{
  m_pFrame = av_frame_alloc();
}

CVideoBufferDRMPRIME::~CVideoBufferDRMPRIME()
{
  Unref();
  av_frame_free(&m_pFrame);
}

void CVideoBufferDRMPRIME::SetRef(AVFrame* frame)
{
  av_frame_move_ref(m_pFrame, frame);
}

void CVideoBufferDRMPRIME::Unref()
{
  if (m_fb_id)
  {
    drmModeRmFB(m_drm_fd, m_fb_id);
    m_fb_id = 0;
  }

  for (int i = 0; i < AV_DRM_MAX_PLANES; i++)
  {
    if (m_handles[i])
    {
      struct drm_gem_close gem_close = { .handle = m_handles[i] };
      drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
      m_handles[i] = 0;
    }
  }

  av_frame_unref(m_pFrame);
}

//------------------------------------------------------------------------------

class CVideoBufferPoolDRMPRIME
  : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolDRMPRIME() override;
  void Return(int id) override;
  CVideoBuffer* Get() override;

protected:
  CCriticalSection m_critSection;
  std::vector<CVideoBufferDRMPRIME*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;
};

CVideoBufferPoolDRMPRIME::~CVideoBufferPoolDRMPRIME()
{
  for (auto buf : m_all)
    delete buf;
}

CVideoBuffer* CVideoBufferPoolDRMPRIME::Get()
{
  CSingleLock lock(m_critSection);

  CVideoBufferDRMPRIME* buf = nullptr;
  if (!m_free.empty())
  {
    int idx = m_free.front();
    m_free.pop_front();
    m_used.push_back(idx);
    buf = m_all[idx];
  }
  else
  {
    int id = m_all.size();
    buf = new CVideoBufferDRMPRIME(*this, id);
    m_all.push_back(buf);
    m_used.push_back(id);
  }

  buf->Acquire(GetPtr());
  return buf;
}

void CVideoBufferPoolDRMPRIME::Return(int id)
{
  CSingleLock lock(m_critSection);

  m_all[id]->Unref();
  auto it = m_used.begin();
  while (it != m_used.end())
  {
    if (*it == id)
    {
      m_used.erase(it);
      break;
    }
    else
      ++it;
  }
  m_free.push_back(id);
}

//------------------------------------------------------------------------------
// main class
//------------------------------------------------------------------------------

CDVDVideoCodecDRMPRIME::CDVDVideoCodecDRMPRIME(CProcessInfo& processInfo)
  : CDVDVideoCodec(processInfo)
{
  m_pFrame = av_frame_alloc();
  m_pHWFrame = av_frame_alloc();
  m_videoBufferPool = std::make_shared<CVideoBufferPoolDRMPRIME>();
}

CDVDVideoCodecDRMPRIME::~CDVDVideoCodecDRMPRIME()
{
  av_frame_free(&m_pFrame);
  av_frame_free(&m_pHWFrame);
  avcodec_free_context(&m_pCodecContext);
}

CDVDVideoCodec* CDVDVideoCodecDRMPRIME::Create(CProcessInfo& processInfo)
{
  if (CServiceBroker::GetSettings().GetBool(CSettings::SETTING_VIDEOPLAYER_USEPRIMEDECODER))
    return new CDVDVideoCodecDRMPRIME(processInfo);
  return nullptr;
}

void CDVDVideoCodecDRMPRIME::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("drm_prime", CDVDVideoCodecDRMPRIME::Create);
}

const AVCodec* CDVDVideoCodecDRMPRIME::FindDecoder(CDVDStreamInfo& hints)
{
  enum AVHWDeviceType DRMPrimeDeviceTypes[] = {
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_NONE
  };
  const AVCodec* codec = nullptr;
  const AVCodecHWConfig* config;
  void *i = 0;
  bool match;
  int j, k;

  while ((codec = av_codec_iterate(&i)))
  {
    if (av_codec_is_decoder(codec) && codec->id == hints.codec)
    {
      if (codec->pix_fmts)
      {
        const AVPixelFormat* fmt = codec->pix_fmts;
        while (*fmt != AV_PIX_FMT_NONE)
        {
          if (*fmt == AV_PIX_FMT_DRM_PRIME)
            return codec;
          fmt++;
        }
      } else {
        for (j = 0;; j++) {
          config = avcodec_get_hw_config(codec, j);
          if (!config)
            break;

          match = false;

          for (j = 0; DRMPrimeDeviceTypes[j] != AV_HWDEVICE_TYPE_NONE; j++)
          {
            if (config->device_type == DRMPrimeDeviceTypes[j])
            {
              match = true;
              break;
            }
          }

          if (!match)
            continue;

          if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
              config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)
          {
            m_HWDeviceType = DRMPrimeDeviceTypes[j];
            return codec;
          }
        }
      }
    }
  }

  return nullptr;
}

bool CDVDVideoCodecDRMPRIME::HWAccelEnabled()
{
  return (m_HWDeviceType != AV_HWDEVICE_TYPE_NONE);
}

enum AVPixelFormat CDVDVideoCodecDRMPRIME::GetFormatVAAPI(struct AVCodecContext* avctx, const AVPixelFormat* fmt)
{
  return AV_PIX_FMT_VAAPI; // FIXME
}

bool CDVDVideoCodecDRMPRIME::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  const AVCodec* pCodec = FindDecoder(hints);
  AVHWFramesContext* pHWFramesContext;
  AVHWFramesContext* pHWDRMFramesContext;
  bool supported;
  int ret;
  int i;

  if (!pCodec)
  {
    CLog::Log(LOGDEBUG, "CDVDVideoCodecDRMPRIME::%s - unable to find decoder for codec %d", __FUNCTION__, hints.codec);
    return false;
  }

  CLog::Log(LOGNOTICE, "CDVDVideoCodecDRMPRIME::%s - using decoder %s", __FUNCTION__, pCodec->long_name ? pCodec->long_name : pCodec->name);

  m_pCodecContext = avcodec_alloc_context3(pCodec);
  if (!m_pCodecContext)
    return false;

  if (HWAccelEnabled()) {
    CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());

    ret = av_hwdevice_ctx_create(&m_pHWDeviceRef, m_HWDeviceType, winSystem->GetDevicePath().c_str(), NULL, 0);
    if (ret < 0)
    {
      CLog::Log(LOGWARNING, "CDVDVideoCodecDRMPRIME::%s - unable to create hardware device context", __FUNCTION__);
      return false;
    }

    ret = av_hwdevice_ctx_create(&m_pHWDRMDeviceRef, AV_HWDEVICE_TYPE_DRM, winSystem->GetDevicePath().c_str(), NULL, 0);
    if (ret < 0)
    {
      CLog::Log(LOGWARNING, "CDVDVideoCodecDRMPRIME::%s - unable to create DRM hardware device context", __FUNCTION__);
      return false;
    }

    // TODO: av_hwdevice_get_hwframe_constraints

    m_pHWFrameRef = av_hwframe_ctx_alloc(m_pHWDeviceRef);
    if (!m_pHWFrameRef)
    {
      CLog::Log(LOGWARNING, "CDVDVideoCodecDRMPRIME::%s - unable to create hardware frame context", __FUNCTION__);
      return false;
    }

    m_pHWDRMFrameRef = av_hwframe_ctx_alloc(m_pHWDRMDeviceRef);
    if (!m_pHWDRMFrameRef)
    {
      CLog::Log(LOGWARNING, "CDVDVideoCodecDRMPRIME::%s - unable to create DRM hardware frame context", __FUNCTION__);
      return false;
    }

    pHWFramesContext = (AVHWFramesContext*)(m_pHWFrameRef->data);
    pHWFramesContext->format = AV_PIX_FMT_VAAPI; // FIXME
    pHWFramesContext->sw_format = AV_PIX_FMT_NV12; // FIXME
    pHWFramesContext->width = hints.width;
    pHWFramesContext->height = hints.height;

    ret = av_hwframe_ctx_init(m_pHWFrameRef);
    if (ret < 0)
    {
      CLog::Log(LOGWARNING, "CDVDVideoCodecDRMPRIME::%s - unable to init hardware frame context", __FUNCTION__);
      return false;
    }

    // FIXME: We can also get the DRM hw frame context by deriving the VAAPI hw frame context using the DRM device ref so that it automagically sets the right stuff.
    pHWDRMFramesContext = (AVHWFramesContext*)(m_pHWDRMFrameRef->data);
    pHWDRMFramesContext->format = AV_PIX_FMT_DRM_PRIME;
    pHWDRMFramesContext->sw_format = AV_PIX_FMT_NV12; // FIXME
    pHWDRMFramesContext->width = hints.width;
    pHWDRMFramesContext->height = hints.height;

    ret = av_hwframe_ctx_init(m_pHWDRMFrameRef);
    if (ret < 0)
    {
      CLog::Log(LOGWARNING, "CDVDVideoCodecDRMPRIME::%s - unable to init DRM hardware frame context", __FUNCTION__);
      return false;
    }

    // TODO: test ability to map to DRM_PRIME

    m_pCodecContext->get_format = GetFormatVAAPI;
    m_pCodecContext->hw_device_ctx = av_buffer_ref(m_pHWDeviceRef);
    m_pFrame->hw_frames_ctx = av_buffer_ref(m_pHWDRMFrameRef);
    m_pHWFrame->hw_frames_ctx = av_buffer_ref(m_pHWFrameRef);
  }

  m_pCodecContext->codec_tag = hints.codec_tag;
  m_pCodecContext->coded_width = hints.width;
  m_pCodecContext->coded_height = hints.height;
  m_pCodecContext->bits_per_coded_sample = hints.bitsperpixel;
  m_pCodecContext->time_base.num = 1;
  m_pCodecContext->time_base.den = DVD_TIME_BASE;

  if (hints.extradata && hints.extrasize > 0)
  {
    m_pCodecContext->extradata_size = hints.extrasize;
    m_pCodecContext->extradata = (uint8_t*)av_mallocz(hints.extrasize + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(m_pCodecContext->extradata, hints.extradata, hints.extrasize);
  }

  if (avcodec_open2(m_pCodecContext, pCodec, nullptr) < 0)
  {
    CLog::Log(LOGNOTICE, "CDVDVideoCodecDRMPRIME::%s - unable to open codec", __FUNCTION__);
    avcodec_free_context(&m_pCodecContext);
    return false;
  }

  const char* pixFmtName = av_get_pix_fmt_name(m_pCodecContext->pix_fmt);
  m_processInfo.SetVideoPixelFormat(pixFmtName ? pixFmtName : "");
  m_processInfo.SetVideoDimensions(hints.width, hints.height);
  m_processInfo.SetVideoDeintMethod("none");
  m_processInfo.SetVideoDAR(hints.aspect);

  if (pCodec->name)
    m_name = std::string("ff-") + pCodec->name;
  else
    m_name = "ffmpeg";

  m_processInfo.SetVideoDecoderName(m_name, true);

  return true;
}

bool CDVDVideoCodecDRMPRIME::AddData(const DemuxPacket& packet)
{
  if (!m_pCodecContext)
    return true;

  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = packet.pData;
  avpkt.size = packet.iSize;
  avpkt.dts = (packet.dts == DVD_NOPTS_VALUE) ? AV_NOPTS_VALUE : static_cast<int64_t>(packet.dts / DVD_TIME_BASE * AV_TIME_BASE);
  avpkt.pts = (packet.pts == DVD_NOPTS_VALUE) ? AV_NOPTS_VALUE : static_cast<int64_t>(packet.pts / DVD_TIME_BASE * AV_TIME_BASE);
  avpkt.side_data = static_cast<AVPacketSideData*>(packet.pSideData);
  avpkt.side_data_elems = packet.iSideDataElems;

  int ret = avcodec_send_packet(m_pCodecContext, &avpkt);
  if (ret == AVERROR(EAGAIN))
    return false;
  else if (ret == AVERROR_EOF)
    return true;
  else if (ret)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::%s - send packet failed, ret:%d", __FUNCTION__, ret);
    return false;
  }

  return true;
}

void CDVDVideoCodecDRMPRIME::Reset()
{
  if (!m_pCodecContext)
    return;

  avcodec_flush_buffers(m_pCodecContext);
  av_frame_unref(m_pFrame);
  m_codecControlFlags = 0;

  if (HWAccelEnabled()) {
    av_frame_unref(m_pHWFrame);

    av_buffer_unref(&m_pHWFrameRef);
    av_buffer_unref(&m_pHWDRMFrameRef);
    av_buffer_unref(&m_pHWDeviceRef);
    av_buffer_unref(&m_pHWDRMDeviceRef);
  }
}

void CDVDVideoCodecDRMPRIME::Drain()
{
  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = nullptr;
  avpkt.size = 0;
  avcodec_send_packet(m_pCodecContext, &avpkt);
}

void CDVDVideoCodecDRMPRIME::SetPictureParams(VideoPicture* pVideoPicture)
{
  pVideoPicture->iWidth = m_pFrame->width;
  pVideoPicture->iHeight = m_pFrame->height;

  double aspect_ratio = 0;
  AVRational pixel_aspect = m_pFrame->sample_aspect_ratio;
  if (pixel_aspect.num)
    aspect_ratio = av_q2d(pixel_aspect) * pVideoPicture->iWidth / pVideoPicture->iHeight;

  if (aspect_ratio <= 0.0)
    aspect_ratio = (float)pVideoPicture->iWidth / (float)pVideoPicture->iHeight;

  pVideoPicture->iDisplayWidth = ((int)lrint(pVideoPicture->iHeight * aspect_ratio)) & -3;
  pVideoPicture->iDisplayHeight = pVideoPicture->iHeight;
  if (pVideoPicture->iDisplayWidth > pVideoPicture->iWidth)
  {
    pVideoPicture->iDisplayWidth = pVideoPicture->iWidth;
    pVideoPicture->iDisplayHeight = ((int)lrint(pVideoPicture->iWidth / aspect_ratio)) & -3;
  }

  pVideoPicture->color_range = m_pFrame->color_range;
  pVideoPicture->color_primaries = m_pFrame->color_primaries;
  pVideoPicture->color_transfer = m_pFrame->color_trc;
  pVideoPicture->color_space = m_pFrame->colorspace;

  pVideoPicture->iFlags = 0;
  pVideoPicture->iFlags |= m_pFrame->interlaced_frame ? DVP_FLAG_INTERLACED : 0;
  pVideoPicture->iFlags |= m_pFrame->top_field_first ? DVP_FLAG_TOP_FIELD_FIRST: 0;
  pVideoPicture->iFlags |= m_pFrame->data[0] ? 0 : DVP_FLAG_DROPPED;

  int64_t pts = m_pFrame->pts;
  if (pts == AV_NOPTS_VALUE)
    pts = m_pFrame->best_effort_timestamp;
  pVideoPicture->pts = (pts == AV_NOPTS_VALUE) ? DVD_NOPTS_VALUE : (double)pts * DVD_TIME_BASE / AV_TIME_BASE;
  pVideoPicture->dts = DVD_NOPTS_VALUE;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecDRMPRIME::GetPicture(VideoPicture* pVideoPicture)
{
  int ret;

  if (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN)
    Drain();

  if (HWAccelEnabled())
    ret = avcodec_receive_frame(m_pCodecContext, m_pHWFrame);
  else
    ret = avcodec_receive_frame(m_pCodecContext, m_pFrame);
  if (ret == AVERROR(EAGAIN))
    return VC_BUFFER;
  else if (ret == AVERROR_EOF)
    return VC_EOF;
  else if (ret)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecDRMPRIME::%s - receive frame failed, ret:%d", __FUNCTION__, ret);
    return VC_ERROR;
  }

  if (HWAccelEnabled())
  {
    m_pFrame->format = AV_PIX_FMT_DRM_PRIME;

    ret = av_hwframe_map(m_pFrame, m_pHWFrame, 0);
    if (ret)
      return VC_ERROR;
  }

  if (pVideoPicture->videoBuffer)
    pVideoPicture->videoBuffer->Release();
  pVideoPicture->videoBuffer = nullptr;

  SetPictureParams(pVideoPicture);

  CVideoBufferDRMPRIME* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(m_videoBufferPool->Get());
  buffer->SetRef(m_pFrame);
  pVideoPicture->videoBuffer = buffer;

  return VC_PICTURE;
}
