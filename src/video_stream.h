#pragma once

#include <cstdint>
#include <string>

#include "image_io.h"
#include "sequence.h"

namespace dlssnr {

// Frames streamed straight through ffmpeg pipes, never touching the disk.
//
// The file-based path writes every frame out as a PNG, reads it back, decodes
// it and re-encodes the result. Measured on 4K that cost roughly 660 ms a
// frame -- more than the neural pass and the optical flow put together -- plus
// 3 MB of disk each, which came to 19.8 GB for one clip.
//
// Both directions convert through lookup tables rather than calling pow() per
// channel: 8-bit sRGB in gives 256 possible inputs, and half-float out has
// only 65536 possible bit patterns, so each conversion is a table read.

class VideoReader {
public:
    VideoReader() = default;
    ~VideoReader();
    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    // start_seconds seeks before decoding; max_frames caps the run (0 = all).
    // Rendering a range matters on a long clip: the interesting part is rarely
    // at the head, and a 6000-frame render is hours.
    bool Open(const std::wstring& path, int width, int height, double start_seconds,
              int max_frames, std::string* error);

    // Fills `out` with the next frame. Returns false at end of stream, with
    // *error left empty -- a short read is how the stream ends, not a fault.
    bool Read(Image* out, std::string* error);

    void Close();

private:
    void* process_ = nullptr;
    void* read_ = nullptr;
    int width_ = 0, height_ = 0;
    int remaining_ = 0;  // 0 = unlimited
    std::vector<uint8_t> row_buffer_;
};

class VideoWriter {
public:
    VideoWriter() = default;
    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    // frame_count is how many frames will be written (0 = unknown). Stating it
    // is what keeps the last frame: with an audio track and ffmpeg left to
    // infer the length from a pipe, -shortest cuts one frame off the end.
    bool Open(const std::wstring& path, int width, int height, double fps,
              const EncodeOptions& options, int frame_count, std::string* error);

    bool Write(const Image& frame, std::string* error);

    // Closes the pipe and waits for ffmpeg to finish muxing.
    bool Close(std::string* error);

private:
    void* process_ = nullptr;
    void* write_ = nullptr;
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> row_buffer_;
};

// Encoder command line for a raw rgb24 stream arriving on stdin.
std::wstring BuildStreamEncodeCommand(int width, int height, double fps,
                                      const std::wstring& output,
                                      const EncodeOptions& options,
                                      int frame_count);

}  // namespace dlssnr
