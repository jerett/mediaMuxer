#ifndef _MUXER_H
#define _MUXER_H

#include <string>
#include <mutex>
#include <memory>
#include <map>

struct AVFormatContext;
struct AVStream;
struct AVCodecContext;
struct AVCodec;
struct AVIOInterruptCB;

namespace ins
{

class Muxer;
int InterruptCallBack(void*);
std::string FFmpegErrorString(int code);

class Muxer {
public:
  enum AudioChannelLayout {
    MONO,
    STEREO,
  };

  Muxer(const std::string &format, const std::string &output_file) noexcept;

  ~Muxer() noexcept {
    Close();
  }

  /*
   * @param options: set context opt
   *
   */
  bool Open(const std::map<std::string, std::string> &options) noexcept;

  /**
   *  只有当Close前,如果发的是rtmp直播流,需要调用次函数;
   *  否则,可能造成文件损坏
   */
  void Interrupt() noexcept {
    io_interrupt_result_ = true;
  }

  bool Close() noexcept;

  /*
   * @param options: set stream dict
   *
   */
  bool AddVideoStream(int width,
                      int height,
                      const uint8_t *video_headrer,
                      int header_size,
                      const std::map<std::string, std::string> &options) noexcept;

  bool AddAudioStream(const uint8_t *aac_header,
                      int header_size,
                      int sample_rate,
                      AudioChannelLayout channel_layout,
                      int channels,
                      int bitrate) noexcept;
//  bool AddAudioStream(const uint8_t *aac_header, int header_size) noexcept(true);

  bool SetMetaData(const char *key, const char *val) noexcept;

  bool WriteHeader() noexcept;

  bool WriteH264Nalu(const uint8_t *nalu, int nalu_len, int64_t pts, int64_t dts, bool is_key) noexcept;

  void ConstructSei(const uint8_t *src, uint8_t len, uint8_t **out_buf, int *out_len);

  bool WriteNaluWithSei(const uint8_t *nalu, int nalu_len, const uint8_t *data, int len,
                        int64_t pts, int64_t dts, bool is_key) noexcept;

  bool WriteAAC(const uint8_t *aac, int size, int64_t pts) noexcept;

private:
  friend int InterruptCallBack(void*);
  bool WriteVideoPacket(const uint8_t *nalu,
                        int nalu_len,
                        int64_t pts,
                        int64_t dts,
                        bool is_key);

private:
  std::string output_format_;
  std::string output_file_;

  AVFormatContext *out_context_ = nullptr;
  AVStream *video_stream_ = nullptr;
  AVStream *audio_stream_ = nullptr;
  std::shared_ptr<AVIOInterruptCB> interrupt_cb_;

  bool is_first_video_ = false;
  int64_t last_video_pkt_pts_ = 0;
  bool open_ = false;
  bool io_interrupt_result_ = false;
  std::mutex write_mtx_;
};

}

#endif

