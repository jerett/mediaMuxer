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
  /**
   * can throw std::runtime_error
   */
  Muxer(const std::string &format, const std::string &output_file) noexcept(true);

  ~Muxer() noexcept(true) {
    Close();
  }

  /*
   * @param options: set context opt
   *
   */
  bool Open(const std::map<std::string, std::string> &options) noexcept(true);

  /**
   *  只有当Close前,如果发的是rtmp直播流,需要调用次函数;
   *  否则,可能造成文件损坏
   */
  void Interrupt() noexcept {
    io_interrupt_result_ = true;
  }

  bool Close() noexcept(true);

  /*
   * @param options: set stream dict
   *
   */
  bool AddVideoStream(int width,
                      int height,
                      const uint8_t *video_headrer,
                      int header_size,
                      const std::map<std::string, std::string> &options) noexcept(true);

  bool AddAudioStream(const uint8_t *aac_header,
                             int header_size,
                             int sample_rate,
                             AudioChannelLayout channel_layout,
                             int channels,
                             int bitrate) noexcept(true);
//  bool AddAudioStream(const uint8_t *aac_header, int header_size) noexcept(true);

  bool SetMetaData(const char *key, const char *val) noexcept(true);

  bool WriteHeader() noexcept(true);

  bool WriteH264Nalu(const uint8_t *nalu,
                     int nalu_len,
                     double timestamp) noexcept(true);

  void ConstructSei
      (const uint8_t *src, uint8_t len, uint8_t **out_buf, int *out_len);

  bool WriteNaluWithSei(const uint8_t *nalu, int nalu_len,
                        const uint8_t *data, int len,
                        double timestamp) noexcept(true);

  bool WriteAAC(const uint8_t *aac, int size, double timestamp) noexcept(true);

private:
  friend int InterruptCallBack(void*);

private:
  std::string output_format_;
  std::string output_file_;

  AVFormatContext *out_context_ = nullptr;
  AVStream *video_stream_ = nullptr;
  AVStream *audio_stream_ = nullptr;
  std::shared_ptr<AVIOInterruptCB> interrupt_cb_;

  bool open_ = false;
  bool io_interrupt_result_ = false;
  std::mutex write_mtx_;
};

}

#endif

