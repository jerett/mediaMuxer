#include "muxer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <iostream>


namespace jt
{

int InterruptCallBack(void* opaque) {
  Muxer *mux = static_cast<Muxer*>(opaque);
  return mux->io_interrupt_result_;
}

std::string FFmpegErrorString(int code)  {
  char buf[256];
  av_strerror(code, buf, 256);
//  std::cerr << buf << std::endl;
  return buf;
}

Muxer::Muxer(const std::string &format,
             const std::string &output_file)
  :output_format_(format), output_file_(output_file) {
  avformat_network_init();
  interrupt_cb_.reset(new AVIOInterruptCB);
  interrupt_cb_->callback = InterruptCallBack;
  interrupt_cb_->opaque = this;
}

bool Muxer::SetMetaData(const char *key, const char *val) {
  av_dict_set(&out_context_->metadata, key, val, 0);
  return true;
}

bool Muxer::Open(const std::map<std::string, std::string> &options) {
  io_interrupt_result_ = false;
  int ret = avformat_alloc_output_context2(&out_context_,
                                           nullptr,
                                           output_format_.data(),
                                           output_file_.data());
  out_context_->oformat->flags |= AVFMT_ALLOW_FLUSH;
  for(const auto &option : options) {
    av_opt_set(out_context_->priv_data, option.first.c_str(), option.second.c_str(), 0);
  }
  return ret >= 0;
}

bool Muxer::AddAudioStream(const uint8_t *aac_header,
                           int header_size,
                           int sample_rate,
                           int channels,
                           int bitrate) {
  audio_stream_ = avformat_new_stream(out_context_, nullptr);
  if (audio_stream_ == nullptr) {
    std::cerr << "new audio stream err" << std::endl;
    return false;
  }

  // AVCodecContext *audioCodecContext = audio_stream_->codec;
  AVCodecParameters *audioCodecPar = audio_stream_->codecpar;
  audioCodecPar->codec_type = AVMEDIA_TYPE_AUDIO;
  audioCodecPar->format = AV_SAMPLE_FMT_S16;
  audioCodecPar->frame_size = 1024;
  audioCodecPar->sample_rate = sample_rate;
  switch (channels) {
    case 1:
      audioCodecPar->channel_layout = AV_CH_LAYOUT_MONO;
      break;
    case 2:
      audioCodecPar->channel_layout = AV_CH_LAYOUT_STEREO;
      break;
    default:
      std::cerr << "You should handle here. " << std::endl;
      break;
  }
  audioCodecPar->channels = channels;
  audioCodecPar->bit_rate = bitrate;
  audioCodecPar->codec_id = AV_CODEC_ID_AAC;

  //copy header
  if (aac_header != nullptr) {
    audioCodecPar->extradata = new uint8_t[header_size];
    memcpy(audioCodecPar->extradata, aac_header, header_size);
    audioCodecPar->extradata_size = header_size;
  } else {
    audioCodecPar->extradata = nullptr;
    audioCodecPar->extradata_size = 0;
  }

  audioCodecPar->codec_tag = 0;
  // if (out_context_->oformat->flags & AVFMT_GLOBALHEADER) {
  //   // audioCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  // }
  return true;
}

bool Muxer::AddVideoStream(int width,
                           int height,
                           const uint8_t *video_header,
                           int header_size,
                           const std::map<std::string, std::string> &options) {
  video_stream_ = avformat_new_stream(out_context_, nullptr);
  if (video_stream_ == nullptr) {
    std::cerr << "alloc video stream err" << std::endl;
    return false;
  }

  AVCodecParameters *vCodecPar = video_stream_->codecpar;
  vCodecPar->format = AV_PIX_FMT_YUV420P;
  vCodecPar->width = width;
  vCodecPar->height = height;
  vCodecPar->codec_type = AVMEDIA_TYPE_VIDEO;
  vCodecPar->codec_id = AV_CODEC_ID_H264;

  //copy header
  vCodecPar->extradata = new uint8_t[header_size];
  memcpy(vCodecPar->extradata, video_header, header_size);
  vCodecPar->extradata_size = header_size;
  vCodecPar->codec_tag = 0;
  // if (out_context_->oformat->flags & AVFMT_GLOBALHEADER) {
  //   vCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  // }
  for(const auto &option : options) {
    av_dict_set(&video_stream_->metadata, option.first.c_str(), option.second.c_str(), 0);
  }
  return true;
}

bool Muxer::WriteHeader() {
  int ret;
  if (!(out_context_->oformat->flags & AVFMT_NOFILE)) {
//    ret = avio_open(&out_context_->pb, output_file_.data(), AVIO_FLAG_WRITE);
    ret = avio_open2(&out_context_->pb, output_file_.c_str(), AVIO_FLAG_WRITE, interrupt_cb_.get(), nullptr);
    if (ret < 0) {
      std::cerr << "create oformat failed:" << FFmpegErrorString(ret) << std::endl;
      if (!(out_context_->oformat->flags & AVFMT_NOFILE)) {
        avio_close(out_context_->pb);
      }
      avformat_free_context(out_context_);
      return false;
    }
  }

  ret = avformat_write_header(out_context_, nullptr);
  if (ret < 0) {
    std::cerr << "create header failed:" << FFmpegErrorString(ret) << std::endl;
    return false;
  }
  av_dump_format(out_context_, 0, output_file_.data(), 1);
  open_ = true;
  return true;
}

bool Muxer::Close() {
  std::lock_guard<std::mutex> lck(write_mtx_);
  if (open_) {
    open_ = false;
    int ret = av_write_trailer(out_context_);
    if (ret != 0) {
      std::cerr << "write trailer failed:" << FFmpegErrorString(ret) << std::endl;
//      return false;
    }
    if (!(out_context_->oformat->flags & AVFMT_NOFILE)) {
      avio_close(out_context_->pb);
    }
    avformat_free_context(out_context_);

    out_context_ = 0;
    std::cerr << "Close " << output_file_ << std::endl;
  }
  return true;
}

bool Muxer::WriteAAC(const uint8_t *aac, int size, int64_t pts) {
  std::lock_guard<std::mutex> lck(write_mtx_);
  if (!out_context_ || !open_) {
    std::cerr << "try write aac when not Open " << std::endl;
    return false;
  }
  AVPacket pkt = {0};
  av_init_packet(&pkt);

  pkt.data = (uint8_t *) aac;
  pkt.size = size;

  static AVRational rational = {1, 1000};
  pkt.pts = av_rescale_q(pts, rational, audio_stream_->time_base);
//  pkt.pts = pts / 1000.0 / av_q2d(audio_stream_->time_base);
  pkt.dts = pkt.pts;
  pkt.convergence_duration = AV_NOPTS_VALUE;
  pkt.pos = -1;

  pkt.stream_index = audio_stream_->index;
  //LOG(INFO) << "write aac";
  int ret = av_interleaved_write_frame(out_context_, &pkt);
  if (ret != 0) {
    std::cerr << "write audio frame err:" << FFmpegErrorString(ret) << std::endl;
  }
  return ret == 0;
}


bool Muxer::WriteVideoPacket(const uint8_t *nalu, int nalu_len, int64_t pts, int64_t dts, bool is_key) {
  int len = nalu_len;
  AVPacket pkt = {0};
  av_init_packet(&pkt);

  pkt.flags = is_key ? AV_PKT_FLAG_KEY : 0;
  pkt.data = (uint8_t *) nalu;
  pkt.size = len;
  pkt.stream_index = video_stream_->index;

  static AVRational rational = {1, 1000};
  pkt.pts = av_rescale_q(pts, rational, video_stream_->time_base);
  pkt.dts = av_rescale_q(dts, rational, video_stream_->time_base);
  if (!is_first_video_) {
    last_video_pkt_pts_ = pkt.pts;
    is_first_video_ = true;
  }
  pkt.duration = static_cast<int>(pkt.pts - last_video_pkt_pts_);
  last_video_pkt_pts_ = pkt.pts;
//  std::cout << " duration:" << pkt.duration << std::endl;
  pkt.convergence_duration = AV_NOPTS_VALUE;
  pkt.pos = -1;

  int ret = av_interleaved_write_frame(out_context_, &pkt);
  if (ret != 0) {
    std::cerr << "write video frame err:" << FFmpegErrorString(ret) << std::endl;
  }
  return ret == 0;
}

bool Muxer::WriteH264Nalu(const uint8_t *nalu,
                          int nalu_len,
                          int64_t pts,
                          int64_t dts,
                          bool is_key) {
    // std::cout << "is key:" << is_key << std::endl;
  std::lock_guard<std::mutex> lck(write_mtx_);
  if (!out_context_ || !open_) {
    std::cerr << "try write nalu when not Open " << std::endl;
    return false;
  }
  
  if (nalu == nullptr) {
    av_write_frame(out_context_, nullptr);
    return true;
  }

  return WriteVideoPacket(nalu, nalu_len, pts, dts, is_key);
}

}

