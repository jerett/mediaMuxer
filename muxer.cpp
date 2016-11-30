#include "muxer.h"

#define __STDC_CONSTANT_MACROS
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <iostream>

namespace ins
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
             const std::string &output_file) noexcept
  :output_file_(output_file), output_format_(format) {
  av_register_all();
  avformat_network_init();
  interrupt_cb_.reset(new AVIOInterruptCB);
  interrupt_cb_->callback = InterruptCallBack;
  interrupt_cb_->opaque = this;
}

bool Muxer::SetMetaData(const char *key, const char *val) noexcept {
  av_dict_set(&out_context_->metadata, key, val, 0);
  return true;
}

bool Muxer::Open(const std::map<std::string, std::string> &options) noexcept {
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
                           AudioChannelLayout channel_layout,
                           int channels,
                           int bitrate) noexcept {
  audio_stream_ = avformat_new_stream(out_context_, nullptr);
  if (audio_stream_ == nullptr) {
    std::cerr << "new audio stream err" << std::endl;
    return false;
  }

  AVCodecContext *audioCodecContext = audio_stream_->codec;
  audioCodecContext->codec_type = AVMEDIA_TYPE_AUDIO;
  audioCodecContext->sample_fmt = AV_SAMPLE_FMT_S16;
  audioCodecContext->frame_size = 1024;
//  audioCodecContext->sample_rate = 48000;
//  audioCodecContext->channel_layout = AV_CH_LAYOUT_MONO;
//  audioCodecContext->channels = 1;
//  audioCodecContext->bit_rate = 64000;
  audioCodecContext->sample_rate = sample_rate;
  switch (channel_layout) {
    case MONO:
      audioCodecContext->channel_layout = AV_CH_LAYOUT_MONO;
      break;
    case STEREO:
      audioCodecContext->channel_layout = AV_CH_LAYOUT_STEREO;
      break;
  }
  audioCodecContext->channels = channels;
  audioCodecContext->bit_rate = bitrate;
  audioCodecContext->codec_id = AV_CODEC_ID_AAC;

  //copy header
  if (aac_header != nullptr) {
    audioCodecContext->extradata = new uint8_t[header_size];
    memcpy(audioCodecContext->extradata, aac_header, header_size);
    audioCodecContext->extradata_size = header_size;
  } else {
    audioCodecContext->extradata = nullptr;
    audioCodecContext->extradata_size = 0;
  }

  if (out_context_->oformat->flags & AVFMT_GLOBALHEADER) {
    audioCodecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;
  }
  return true;
}

bool Muxer::AddVideoStream(int width,
                           int height,
                           const uint8_t *video_header,
                           int header_size,
                           const std::map<std::string, std::string> &options) noexcept {
  video_stream_ = avformat_new_stream(out_context_, nullptr);
  if (video_stream_ == 0) {
    std::cerr << "alloc video stream err" << std::endl;
    return false;
  }

  AVCodecContext *vCodecContext = video_stream_->codec;
  vCodecContext->pix_fmt = PIX_FMT_YUV420P;
  vCodecContext->width = width;
  vCodecContext->height = height;
  vCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
  vCodecContext->codec_id = AV_CODEC_ID_H264;

  //copy header
  vCodecContext->extradata = new uint8_t[header_size];
  memcpy(vCodecContext->extradata, video_header, header_size);
  vCodecContext->extradata_size = header_size;

  if (out_context_->oformat->flags & AVFMT_GLOBALHEADER) {
    vCodecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;
  }
  for(const auto &option : options) {
    av_dict_set(&video_stream_->metadata, option.first.c_str(), option.second.c_str(), 0);
  }
  return true;
}

bool Muxer::WriteHeader() noexcept {
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

bool Muxer::Close() noexcept {
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

bool Muxer::WriteAAC(const uint8_t *aac, int size, int64_t pts) noexcept {
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
                          bool is_key) noexcept {
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

void Muxer::ConstructSei(const uint8_t *src,
                         uint8_t len,
                         uint8_t **out_buf,
                         int *out_len) {
  uint8_t payload_size = 16 + len;
  int sei_data_size = 4 + 1 + 1 + 1 + payload_size + 1;
  uint8_t *pkt_data = new uint8_t[sei_data_size];

  int index = 0;
  pkt_data[index++] = 0x00;
  pkt_data[index++] = 0x00;
  pkt_data[index++] = 0x00;
  pkt_data[index++] = 0x01;
  //type SEI
  pkt_data[index++] = 0x26;
  //rbsp type
  pkt_data[index++] = 0x05;
  //rbsp payload size fix17
  pkt_data[index++] = payload_size;

  //uuid 16byte 03b76fe0-af86-11e5-afbb-0002a5d5c51b
  pkt_data[index++] = 0x03;
  pkt_data[index++] = 0xb7;
  pkt_data[index++] = 0x6f;
  pkt_data[index++] = 0xe0;
  pkt_data[index++] = 0xaf;
  pkt_data[index++] = 0x86;
  pkt_data[index++] = 0x11;
  pkt_data[index++] = 0xe5;
  pkt_data[index++] = 0xaf;
  pkt_data[index++] = 0xbb;
  pkt_data[index++] = 0x00;
  pkt_data[index++] = 0x02;
  pkt_data[index++] = 0xa5;
  pkt_data[index++] = 0xd5;
  pkt_data[index++] = 0xc5;
  pkt_data[index++] = 0x1b;

  //write data
  memcpy(pkt_data + index, src, len);
  index += len;

  //trailer
  pkt_data[index++] = 0x80;

  //malloc a bigger one
  uint8_t *buf = new uint8_t[sei_data_size * 2];
  memcpy(buf, pkt_data, 5);
  int out_index = 5;
  static char tran_code_0[] = {0x00, 0x00, 0x00};
  static char tran_code_1[] = {0x00, 0x00, 0x01};
  static char tran_code_2[] = {0x00, 0x00, 0x02};
  static char tran_code_3[] = {0x00, 0x00, 0x03};

  for (int i = 5; i < sei_data_size; ++i) {
    if (memcmp(pkt_data + i - 3, tran_code_0, 3) == 0) {
      std::cerr << "occur tran_code_0" << std::endl;
      buf[out_index++] = 0x03;
      buf[out_index++] = 0x00;
    } else if (memcmp(pkt_data + i - 3, tran_code_1, 3) == 0) {
      std::cerr << "occur tran_code_1" << std::endl;
      buf[out_index++] = 0x03;
      buf[out_index++] = 0x01;
    } else if (memcmp(pkt_data + i - 3, tran_code_2, 3) == 0) {
      std::cerr << "occur tran_code_2" << std::endl;
      buf[out_index++] = 0x03;
      buf[out_index++] = 0x02;
    } else if (memcmp(pkt_data + i - 3, tran_code_3, 3) == 0) {
      std::cerr << "occur tran_code_3" << std::endl;
      buf[out_index++] = pkt_data[i];
      buf[out_index++] = 0x03;
      buf[out_index++] = 0x03;
    } else {
      buf[out_index++] = pkt_data[i];
    }
  }
  *out_buf = buf;
  *out_len = out_index;
//  for (int i = 0; i < out_index; ++i) {
//    fprintf(stderr, "%02x ", buf[i]);
//  }
//  printf("\n");
  delete[] pkt_data;
}

bool Muxer::WriteNaluWithSei(const uint8_t *nalu, int nalu_len,
                             const uint8_t *data, int data_len,
                             int64_t pts, int64_t dts, bool is_key) noexcept {
  std::lock_guard<std::mutex> lck(write_mtx_);
  if (!out_context_ || !open_) {
    std::cerr << "try write sei when not Open " << std::endl;
    return false;
  }

  int sei_size = 0;
  uint8_t *sei_data = nullptr;
  ConstructSei(data, data_len, &sei_data, &sei_size);

  uint8_t *pkt_data = new uint8_t[sei_size + nalu_len];
  memcpy(pkt_data, sei_data, sei_size);
  memcpy(pkt_data + sei_size, nalu, nalu_len);
//  pkt_data[sei_size+0] = nalu_len >> 24;
//  pkt_data[sei_size+1] = nalu_len >> 16;
//  pkt_data[sei_size+2] = nalu_len >> 8;
//  pkt_data[sei_size+3] = nalu_len & 0xff;

//  AVPacket pkt = {0};
//  av_init_packet(&pkt);
//
//  pkt.flags = is_key ? AV_PKT_FLAG_KEY : 0;
//
//  if (is_key) {
////    std::cerr << "write key sei" << std::endl;
//  }
//
//  pkt.data = pkt_data;
//  pkt.size = sei_size + nalu_len;
//  pkt.stream_index = video_stream_->index;
//
//  static AVRational rational = {1, 1000};
//  pkt.pts = av_rescale_q(pts, rational, video_stream_->time_base);
//  pkt.dts = av_rescale_q(dts, rational, video_stream_->time_base);
////  int64_t pts = pts / av_q2d(video_stream_->time_base);
////  pkt.pts = pts;
////  pkt.dts = pkt.pts;
//  pkt.convergence_duration = AV_NOPTS_VALUE;
//  pkt.pos = -1;
//
//  int ret = av_interleaved_write_frame(out_context_, &pkt);
//  delete[] pkt_data;
//  delete[] sei_data;
//  if (ret != 0) {
//    std::cerr << "write video frame err:" << FFmpegErrorString(ret) << std::endl;
//  }
//  return ret == 0;

  return WriteVideoPacket(pkt_data, sei_size + nalu_len, pts, dts, is_key);
}

}

