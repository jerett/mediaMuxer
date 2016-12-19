//
// Created by jerett on 15/12/30.
//

#include <stdio.h>

extern "C" {
#include <libavformat/avformat.h>
};

#include <memory>
#include <iostream>
#include "muxer/muxer.h"

int main(int argc, char* argv[]) {
  AVFormatContext *ifmt_ctx = NULL;
  const char *in_filename  = "/Users/jerett/Desktop/shenda1.mp4";//Input file URL

  av_register_all();
  int ret = 0;
  //Input
  if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
    printf( "Could not Open input file.");
    return  -1;
  }
  if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
    printf( "Failed to retrieve input stream information");
    return -1;
  }

  jt::Muxer muxer("mp4", "/Users/jerett/Desktop/out.mp4");

  std::map<std::string, std::string> options;
  muxer.Open(options);

  int video_index = -1;
  int audio_index = -1;

  for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
    //Create output AVStream according to input AVStream
    AVStream *in_stream = ifmt_ctx->streams[i];

    if (in_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      std::map<std::string, std::string> options;
      muxer.AddVideoStream(in_stream->codec->width,
                           in_stream->codec->height,
                           in_stream->codec->extradata,
                           in_stream->codec->extradata_size,
                           options);
      video_index = i;
    } else if (in_stream->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_index = i;
      muxer.AddAudioStream(in_stream->codec->extradata,
                           in_stream->codec->extradata_size,
                           in_stream->codec->sample_rate,
                           in_stream->codec->channels,
                           in_stream->codec->bit_rate);
    } else if (in_stream->codec->codec_type == AVMEDIA_TYPE_SUBTITLE) {
    } else {
      break;
    }
  }

  muxer.WriteHeader();

  while (true) {
    AVPacket pkt;
    //Get an AVPacket
    if (av_read_frame(ifmt_ctx, &pkt) < 0) break;

    AVStream* read_stream = ifmt_ctx->streams[pkt.stream_index];
    if (pkt.stream_index == video_index) {
      uint8_t *nalu = pkt.data;
      int nalu_size = pkt.size;
      double pts_ms = pkt.pts * av_q2d(read_stream->time_base) * 1000;
      double dts_ms = pkt.dts * av_q2d(read_stream->time_base) * 1000;
      muxer.WriteH264Nalu(nalu, nalu_size, pts_ms, dts_ms, (pkt.flags | AV_PKT_FLAG_KEY));
    } else if (pkt.stream_index == audio_index) {
      double timestamp = pkt.pts * av_q2d(read_stream->time_base) * 1000;
      muxer.WriteAAC(pkt.data, pkt.size, timestamp);
    }
    av_free_packet(&pkt);
  }

  if (ret < 0 && ret != AVERROR_EOF) {
    printf( "Error occurred.\n");
    return -1;
  }
  return 0;
}

