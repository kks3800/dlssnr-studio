#include "video_stream.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dlssnr {
namespace {

// ---- colour conversion tables -------------------------------------------
// Built once. The forward direction has 256 possible inputs; the reverse has
// 65536, because that is how many distinct half-float bit patterns exist. Both
// fit comfortably in cache and replace a pow() per channel per pixel -- at 4K
// that is 25 million calls a frame otherwise.

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
    if (c <= 0.0f) return 0.0f;
    if (c >= 1.0f) return 1.0f;
    return c <= 0.0031308f ? c * 12.92f
                           : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct Tables {
    uint16_t srgb8_to_half[256];
    uint8_t half_to_srgb8[65536];

    Tables() {
        for (int i = 0; i < 256; ++i) {
            srgb8_to_half[i] = FloatToHalf(SrgbToLinear(float(i) / 255.0f));
        }
        for (int bits = 0; bits < 65536; ++bits) {
            const float linear = HalfToFloat(uint16_t(bits));
            // NaN and negatives land on black rather than on garbage.
            const float safe = (linear == linear) ? linear : 0.0f;
            half_to_srgb8[bits] =
                uint8_t(LinearToSrgb(safe) * 255.0f + 0.5f);
        }
    }
};

const Tables& LookupTables() {
    static const Tables tables;
    return tables;
}

HANDLE OpenNullDevice() {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                       OPEN_EXISTING, 0, nullptr);
}

// Spawns ffmpeg with one end of a pipe attached. `for_reading` decides which
// way the pipe points: the parent reads decoded frames, or writes them.
bool SpawnPiped(const std::wstring& command, bool for_reading, HANDLE* parent_end,
                HANDLE* process, std::string* error) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    // A generous buffer keeps ffmpeg from stalling between our reads; one 4K
    // frame is about 24 MB, so this is a couple of frames of slack.
    if (!CreatePipe(&read_end, &write_end, &sa, 1u << 22)) {
        if (error) *error = "could not create the frame pipe";
        return false;
    }

    // Only the child's end may be inherited, or the pipe never reports EOF.
    HANDLE mine = for_reading ? read_end : write_end;
    HANDLE theirs = for_reading ? write_end : read_end;
    SetHandleInformation(mine, HANDLE_FLAG_INHERIT, 0);

    HANDLE null_device = OpenNullDevice();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = null_device;
    if (for_reading) {
        si.hStdOutput = theirs;
        si.hStdInput = nullptr;
    } else {
        si.hStdInput = theirs;
        si.hStdOutput = null_device;
    }

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started =
        CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(theirs);
    if (null_device != INVALID_HANDLE_VALUE) CloseHandle(null_device);
    if (!started) {
        CloseHandle(mine);
        if (error) *error = "could not start ffmpeg";
        return false;
    }
    CloseHandle(pi.hThread);
    *parent_end = mine;
    *process = pi.hProcess;
    return true;
}

bool ReadExactly(HANDLE pipe, uint8_t* dst, size_t bytes, bool* eof) {
    size_t done = 0;
    while (done < bytes) {
        DWORD got = 0;
        const DWORD ask = DWORD((std::min)(bytes - done, size_t(1u << 20)));
        if (!ReadFile(pipe, dst + done, ask, &got, nullptr) || got == 0) {
            *eof = true;
            return done == 0;  // a clean end lands exactly on a frame boundary
        }
        done += got;
    }
    *eof = false;
    return true;
}

}  // namespace

// ---- reader --------------------------------------------------------------

VideoReader::~VideoReader() { Close(); }

bool VideoReader::Open(const std::wstring& path, int width, int height,
                       double start_seconds, int max_frames, std::string* error) {
    Close();
    if (width <= 0 || height <= 0) {
        if (error) *error = "invalid frame size";
        return false;
    }
    width_ = width;
    height_ = height;
    row_buffer_.assign(size_t(width) * height * 3, 0);

    remaining_ = max_frames > 0 ? max_frames : 0;

    std::wstring cmd = L"ffmpeg -v error -nostdin";
    if (start_seconds > 0.0) {
        wchar_t stamp[64]{};
        swprintf_s(stamp, L"%.3f", start_seconds);
        // Ahead of -i, so ffmpeg seeks instead of decoding everything first.
        cmd += L" -ss ";
        cmd += stamp;
    }
    cmd += L" -i " + QuoteArg(path);
    if (max_frames > 0) cmd += L" -frames:v " + std::to_wstring(max_frames);
    cmd += L" -f rawvideo -pix_fmt rgb24 -";

    HANDLE process = nullptr, pipe = nullptr;
    if (!SpawnPiped(cmd, true, &pipe, &process, error)) return false;
    process_ = process;
    read_ = pipe;
    return true;
}

bool VideoReader::Read(Image* out, std::string* error) {
    if (!read_) {
        if (error) *error = "reader is not open";
        return false;
    }
    if (remaining_ < 0) return false;  // range exhausted
    bool eof = false;
    if (!ReadExactly(HANDLE(read_), row_buffer_.data(), row_buffer_.size(), &eof)) {
        return false;  // end of stream; error deliberately left empty
    }
    if (eof) return false;
    if (remaining_ > 0 && --remaining_ == 0) remaining_ = -1;  // last one

    const auto& lut = LookupTables();
    out->width = uint32_t(width_);
    out->height = uint32_t(height_);
    out->texels.resize(size_t(width_) * height_ * 4);

    const uint8_t* src = row_buffer_.data();
    uint16_t* dst = out->texels.data();
    const size_t pixels = size_t(width_) * height_;
    for (size_t i = 0; i < pixels; ++i) {
        dst[0] = lut.srgb8_to_half[src[0]];
        dst[1] = lut.srgb8_to_half[src[1]];
        dst[2] = lut.srgb8_to_half[src[2]];
        dst[3] = 0x3C00;  // 1.0 in half
        src += 3;
        dst += 4;
    }
    return true;
}

void VideoReader::Close() {
    if (read_) {
        CloseHandle(HANDLE(read_));
        read_ = nullptr;
    }
    if (process_) {
        // The decoder exits on its own once its output pipe is gone.
        WaitForSingleObject(HANDLE(process_), 5000);
        TerminateProcess(HANDLE(process_), 0);
        CloseHandle(HANDLE(process_));
        process_ = nullptr;
    }
}

// ---- writer --------------------------------------------------------------

VideoWriter::~VideoWriter() {
    std::string ignored;
    Close(&ignored);
}

bool VideoWriter::Open(const std::wstring& path, int width, int height, double fps,
                       const EncodeOptions& options, int frame_count,
                       std::string* error) {
    if (width <= 0 || height <= 0) {
        if (error) *error = "invalid frame size";
        return false;
    }
    width_ = width;
    height_ = height;
    row_buffer_.assign(size_t(width) * height * 3, 0);

    const std::wstring cmd =
        BuildStreamEncodeCommand(width, height, fps, path, options, frame_count);
    if (cmd.empty()) {
        if (error) *error = "ffmpeg not found on PATH";
        return false;
    }

    HANDLE process = nullptr, pipe = nullptr;
    if (!SpawnPiped(cmd, false, &pipe, &process, error)) return false;
    process_ = process;
    write_ = pipe;
    return true;
}

bool VideoWriter::Write(const Image& frame, std::string* error) {
    if (!write_) {
        if (error) *error = "writer is not open";
        return false;
    }
    if (int(frame.width) != width_ || int(frame.height) != height_) {
        if (error) *error = "frame size changed mid-stream";
        return false;
    }

    const auto& lut = LookupTables();
    const uint16_t* src = frame.texels.data();
    uint8_t* dst = row_buffer_.data();
    const size_t pixels = size_t(width_) * height_;
    for (size_t i = 0; i < pixels; ++i) {
        dst[0] = lut.half_to_srgb8[src[0]];
        dst[1] = lut.half_to_srgb8[src[1]];
        dst[2] = lut.half_to_srgb8[src[2]];
        src += 4;
        dst += 3;
    }

    size_t written = 0;
    while (written < row_buffer_.size()) {
        DWORD put = 0;
        const DWORD ask =
            DWORD((std::min)(row_buffer_.size() - written, size_t(1u << 20)));
        if (!WriteFile(HANDLE(write_), row_buffer_.data() + written, ask, &put,
                       nullptr) ||
            put == 0) {
            if (error) *error = "the encoder closed the pipe early";
            return false;
        }
        written += put;
    }
    return true;
}

bool VideoWriter::Close(std::string* error) {
    if (write_) {
        // Closing the pipe is what tells ffmpeg the stream ended; it then
        // finishes muxing, which is why the wait below is generous.
        CloseHandle(HANDLE(write_));
        write_ = nullptr;
    }
    bool ok = true;
    if (process_) {
        if (WaitForSingleObject(HANDLE(process_), 120000) != WAIT_OBJECT_0) {
            TerminateProcess(HANDLE(process_), 1);
            if (error) *error = "the encoder did not finish in time";
            ok = false;
        } else {
            DWORD code = 1;
            GetExitCodeProcess(HANDLE(process_), &code);
            if (code != 0) {
                if (error) *error = "the encoder reported a failure";
                ok = false;
            }
        }
        CloseHandle(HANDLE(process_));
        process_ = nullptr;
    }
    return ok;
}

// ---- encoder command -----------------------------------------------------

std::wstring BuildStreamEncodeCommand(int width, int height, double fps,
                                      const std::wstring& output,
                                      const EncodeOptions& options,
                                      int frame_count) {
    if (!FfmpegAvailable()) return {};

    wchar_t size_arg[32]{}, rate_arg[32]{};
    swprintf_s(size_arg, L"%dx%d", width, height);
    swprintf_s(rate_arg, L"%.6f", fps > 0.0 ? fps : 24.0);

    std::wstring cmd = L"ffmpeg -y -hide_banner -loglevel error";
    cmd += L" -f rawvideo -pix_fmt rgb24 -s ";
    cmd += size_arg;
    cmd += L" -r ";
    cmd += rate_arg;
    cmd += L" -i -";

    int next_input = 1;
    int badge_input = -1, audio_input = -1;
    if (!options.watermark.empty()) {
        cmd += L" -i " + QuoteArg(options.watermark);
        badge_input = next_input++;
    }
    if (!options.audio.empty()) {
        cmd += L" -i " + QuoteArg(options.audio);
        audio_input = next_input++;
    }

    const std::wstring even = L"scale=trunc(iw/2)*2:trunc(ih/2)*2";
    if (badge_input >= 0) {
        wchar_t margin[32]{};
        swprintf_s(margin, L"%.4f", options.wm_margin_pct / 100.0);
        std::wstring placement = L"x=W-w-H*" + std::wstring(margin) + L":y=H-h-H*" +
                                 std::wstring(margin);
        if (options.wm_position == L"bottom-left") {
            placement =
                L"x=H*" + std::wstring(margin) + L":y=H-h-H*" + std::wstring(margin);
        } else if (options.wm_position == L"bottom-center") {
            placement = L"x=(W-w)/2:y=H-h-H*" + std::wstring(margin);
        }
        cmd += L" -filter_complex \"[0:v]" + even + L"[base];[base][" +
               std::to_wstring(badge_input) + L":v]overlay=" + placement +
               L":format=auto[v]\" -map \"[v]\"";
    } else {
        cmd += L" -vf \"" + even + L"\" -map 0:v";
    }

    if (audio_input >= 0) {
        cmd += L" -map " + std::to_wstring(audio_input) + L":a -c:a aac -b:a 384k";
    }

    // A raw stream carries no duration, so ffmpeg has to be told where the
    // video ends. -shortest used to do it and silently dropped the final frame;
    // stating the count is exact, and trimming the audio to match keeps a
    // longer track from padding the tail.
    if (frame_count > 0) {
        cmd += L" -frames:v " + std::to_wstring(frame_count);
        if (audio_input >= 0 && fps > 0.0) {
            wchar_t duration[32]{};
            swprintf_s(duration, L"%.4f", frame_count / fps);
            cmd += L" -t ";
            cmd += duration;
        }
    } else if (audio_input >= 0) {
        cmd += L" -shortest";
    }

    cmd += L" -c:v libx264 -preset " +
           std::wstring(options.crf <= 14 ? L"slow" : L"medium");
    cmd += L" -crf " + std::to_wstring(options.crf);
    cmd += L" -pix_fmt yuv420p -profile:v high -movflags +faststart";
    cmd += L" -colorspace bt709 -color_primaries bt709 -color_trc bt709";
    cmd += L" " + QuoteArg(output);
    return cmd;
}

}  // namespace dlssnr
