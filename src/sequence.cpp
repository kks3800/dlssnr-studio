#include "sequence.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace dlssnr {
namespace {

bool IsImageExtension(const std::wstring& ext) {
    // .exr matters: engine AOV passes are almost always EXR, because velocity
    // is signed and depth needs float range.
    static const wchar_t* kKnown[] = {L".png", L".jpg", L".jpeg", L".tif",
                                      L".tiff", L".bmp", L".tga", L".exr"};
    std::wstring lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (const wchar_t* k : kKnown) {
        if (lower == k) return true;
    }
    return false;
}

// Splits "Seq_Bench_01.0042" into prefix "Seq_Bench_01." and number 42.
// Uses the *trailing* digit run, so a stem that itself contains numbers
// (Bench_01) does not confuse the frame index.
bool SplitFrameNumber(const std::wstring& stem, std::wstring* prefix, int* number,
                      int* digits) {
    size_t end = stem.size();
    while (end > 0 && std::iswdigit(stem[end - 1])) --end;
    if (end == stem.size()) return false;  // no trailing digits
    const std::wstring number_part = stem.substr(end);
    if (number_part.size() > 9) return false;
    *prefix = stem.substr(0, end);
    *number = std::stoi(number_part);
    *digits = int(number_part.size());
    return true;
}

struct Group {
    std::wstring prefix, extension;
    int digits = 0;
    std::vector<std::pair<int, std::wstring>> frames;
};

}  // namespace

bool DetectSequence(const std::wstring& path, Sequence* out, std::string* error) {
    std::error_code ec;
    fs::path root(path);
    if (!fs::exists(root, ec)) {
        if (error) *error = "path does not exist";
        return false;
    }
    if (!fs::is_directory(root, ec)) root = root.parent_path();

    std::map<std::wstring, Group> groups;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto ext = entry.path().extension().wstring();
        if (!IsImageExtension(ext)) continue;

        std::wstring prefix;
        int number = 0, digits = 0;
        if (!SplitFrameNumber(entry.path().stem().wstring(), &prefix, &number, &digits)) {
            continue;
        }
        const std::wstring key = prefix + L"|" + ext;
        Group& g = groups[key];
        g.prefix = prefix;
        g.extension = ext;
        g.digits = (std::max)(g.digits, digits);
        g.frames.emplace_back(number, entry.path().wstring());
    }
    if (groups.empty()) {
        if (error) *error = "no numbered image sequence found in that folder";
        return false;
    }

    // Largest group wins; a render folder often also holds a stray still.
    const Group* best = nullptr;
    for (const auto& kv : groups) {
        if (!best || kv.second.frames.size() > best->frames.size()) best = &kv.second;
    }

    auto frames = best->frames;
    std::sort(frames.begin(), frames.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    out->directory = root.wstring();
    out->prefix = best->prefix;
    out->extension = best->extension;
    out->digits = best->digits;
    out->first = frames.front().first;
    out->last = frames.back().first;
    out->frames.clear();
    out->frames.reserve(frames.size());
    for (auto& f : frames) out->frames.push_back(std::move(f.second));

    if (out->frames.size() < 2) {
        if (error) *error = "only one numbered frame found; not a sequence";
        return false;
    }
    return true;
}

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// Which pass a sequence is, judged from its folder name and file prefix.
// 0 = unknown/beauty, 1 = depth, 2 = velocity.
int ClassifyPass(const Sequence& seq) {
    const std::wstring hay =
        Lower(std::filesystem::path(seq.directory).filename().wstring() + L"|" + seq.prefix);
    auto has = [&](const wchar_t* needle) { return hay.find(needle) != std::wstring::npos; };

    // Velocity first: "motionvectors" also contains no depth keyword, but
    // checking depth first would misfile a hypothetical "depthvelocity".
    if (has(L"velocity") || has(L"motionvector") || has(L"mvec") || has(L"motion")) return 2;
    if (has(L"scenedepth") || has(L"depth") || has(L"_z.") || has(L"zdepth")) return 1;
    return 0;
}

}  // namespace

bool DetectPasses(const std::wstring& root, PassSet* out, std::string* error) {
    std::error_code ec;
    std::filesystem::path base(root);
    if (!std::filesystem::exists(base, ec)) {
        if (error) *error = "path does not exist";
        return false;
    }
    if (!std::filesystem::is_directory(base, ec)) base = base.parent_path();

    // The root itself, plus one level of subfolders -- MRQ writes either way.
    std::vector<std::filesystem::path> candidates{base};
    for (auto it = std::filesystem::directory_iterator(
             base, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) candidates.push_back(it->path());
    }

    std::vector<Sequence> found;
    for (const auto& dir : candidates) {
        Sequence seq;
        std::string ignored;
        // Skip our own output folders so a second run does not ingest the first.
        const std::wstring name = Lower(dir.filename().wstring());
        if (name == L"nr_out" || name == L"nr_mv") continue;
        if (DetectSequence(dir.wstring(), &seq, &ignored)) found.push_back(std::move(seq));
    }
    if (found.empty()) {
        if (error) *error = "no numbered image sequence found";
        return false;
    }

    std::vector<const Sequence*> unclassified;
    for (const auto& seq : found) {
        switch (ClassifyPass(seq)) {
            case 1: if (!out->depth.Valid()) out->depth = seq; break;
            case 2: if (!out->velocity.Valid()) out->velocity = seq; break;
            default: unclassified.push_back(&seq); break;
        }
    }
    // Largest unclassified sequence is the beauty pass; falling back to the
    // largest overall means a plain folder of colour frames still just works.
    const Sequence* best = nullptr;
    for (const auto* s : unclassified) {
        if (!best || s->Count() > best->Count()) best = s;
    }
    if (best) out->beauty = *best;
    if (!out->beauty.Valid()) {
        if (error) *error = "found passes but no colour sequence among them";
        return false;
    }
    return true;
}

std::string DescribePasses(const PassSet& passes) {
    char buf[160];
    sprintf_s(buf, "beauty %zu frames%s%s", passes.beauty.Count(),
              passes.HasDepth() ? ", depth yes" : ", depth no",
              passes.HasVelocity() ? ", velocity yes" : ", velocity no");
    return buf;
}

namespace {

// CreateProcess rather than _wsystem: scrubbing spawns ffmpeg on every seek,
// and _wsystem pops a console window each time, which strobes the screen.
bool RunHidden(const std::wstring& command, const std::wstring& capture_to = {}) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE sink = INVALID_HANDLE_VALUE;
    if (!capture_to.empty()) {
        sink = CreateFileW(capture_to.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (sink == INVALID_HANDLE_VALUE) return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (sink != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = sink;
        si.hStdError = sink;
    }

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started =
        CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr,
                       sink != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi);
    if (sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
    if (!started) return false;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

std::wstring TempPath(const wchar_t* extension) {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    wchar_t name[MAX_PATH]{};
    GetTempFileNameW(dir, L"nr", 0, name);
    std::wstring path = name;
    DeleteFileW(path.c_str());  // GetTempFileName creates it; we want the extension
    return path + extension;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// ffprobe reports rates as "30000/1001".
double ParseRational(const std::string& text) {
    const size_t slash = text.find('/');
    if (slash == std::string::npos) return std::atof(text.c_str());
    const double num = std::atof(text.substr(0, slash).c_str());
    const double den = std::atof(text.substr(slash + 1).c_str());
    return den > 0.0 ? num / den : 0.0;
}

std::map<std::string, std::string> ParseKeyValues(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}



}  // namespace

bool FfmpegAvailable() {
    return RunHidden(L"ffmpeg -version");
}

bool RunCommandHidden(const std::wstring& command) { return RunHidden(command); }

std::wstring QuoteArg(const std::wstring& argument) {
    // A leading '-' would be taken for an option. Only a relative path can
    // start that way, so pointing at the current directory is always safe.
    const std::wstring safe =
        (!argument.empty() && argument[0] == L'-') ? L".\\" + argument : argument;

    std::wstring out;
    out.reserve(safe.size() + 8);
    out.push_back(L'"');
    for (size_t i = 0; i < safe.size();) {
        size_t slashes = 0;
        while (i < safe.size() && safe[i] == L'\\') {
            ++slashes;
            ++i;
        }
        if (i == safe.size()) {
            // Trailing backslashes precede the closing quote, so they double.
            out.append(slashes * 2, L'\\');
            break;
        }
        if (safe[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(safe[i]);
        }
        ++i;
    }
    out.push_back(L'"');
    return out;
}


std::wstring BuildEncodeCommand(const std::wstring& pattern, int first_frame,
                                const std::wstring& output, int fps, int crf) {
    if (!FfmpegAvailable()) return {};
    std::wstring cmd = L"ffmpeg -y -hide_banner -loglevel error";
    cmd += L" -framerate " + std::to_wstring(fps);
    cmd += L" -start_number " + std::to_wstring(first_frame);
    cmd += L" -i \"" + pattern + L"\"";
    // yuv420p and even dimensions keep the result playable everywhere.
    cmd += L" -c:v libx264 -preset slow -crf " + std::to_wstring(crf);
    cmd += L" -pix_fmt yuv420p -vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\"";
    cmd += L" \"" + output + L"\"";
    return cmd;
}

// ---- video sources -------------------------------------------------------

bool ProbeVideo(const std::wstring& path, VideoInfo* out, std::string* error) {
    if (!FfmpegAvailable()) {
        if (error) *error = "ffmpeg/ffprobe not found on PATH";
        return false;
    }

    const std::wstring report = TempPath(L".txt");
    std::wstring cmd = L"ffprobe -v error -select_streams v:0 -show_entries ";
    cmd += L"stream=width,height,avg_frame_rate,nb_frames:format=duration ";
    cmd += L"-of default=noprint_wrappers=1 " + QuoteArg(path);
    if (!RunHidden(cmd, report)) {
        DeleteFileW(report.c_str());
        if (error) *error = "ffprobe could not read the file";
        return false;
    }
    const auto fields = ParseKeyValues(ReadAll(report));
    DeleteFileW(report.c_str());

    VideoInfo info;
    auto find = [&fields](const char* key) -> std::string {
        const auto it = fields.find(key);
        return it == fields.end() ? std::string() : it->second;
    };
    info.width = std::atoi(find("width").c_str());
    info.height = std::atoi(find("height").c_str());
    info.fps = ParseRational(find("avg_frame_rate"));
    info.duration = std::atof(find("duration").c_str());
    info.frames = std::atoi(find("nb_frames").c_str());
    // nb_frames is absent on plenty of containers; derive it instead.
    if (info.frames <= 0 && info.fps > 0.0) {
        info.frames = int(info.duration * info.fps + 0.5);
    }

    const std::wstring audio_report = TempPath(L".txt");
    std::wstring audio_cmd = L"ffprobe -v error -select_streams a -show_entries ";
    audio_cmd += L"stream=codec_type -of default=noprint_wrappers=1 " + QuoteArg(path);
    if (RunHidden(audio_cmd, audio_report)) {
        info.has_audio = ReadAll(audio_report).find("audio") != std::string::npos;
    }
    DeleteFileW(audio_report.c_str());

    if (!info.Valid()) {
        if (error) *error = "no usable video stream in the file";
        return false;
    }
    *out = info;
    return true;
}

bool ExtractFrameAt(const std::wstring& src, double seconds,
                    const std::wstring& out_png, std::string* error) {
    if (seconds < 0.0) seconds = 0.0;
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"%.3f", seconds);

    // -ss ahead of -i is the fast seek: ffmpeg jumps to the nearest keyframe
    // and decodes forward, instead of decoding the whole file to that point.
    std::wstring cmd = L"ffmpeg -y -hide_banner -loglevel error -ss ";
    cmd += stamp;
    cmd += L" -i " + QuoteArg(src) + L" -frames:v 1 -update 1 " + QuoteArg(out_png);
    if (!RunHidden(cmd)) {
        if (error) *error = "could not extract a frame at that position";
        return false;
    }
    return true;
}

bool ExtractAllFrames(const std::wstring& src, const std::wstring& dir,
                      std::string* error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == L".png") fs::remove(entry.path(), ec);
    }

    // rgb24, not rgb48: the source is an 8-bit video, so a 16-bit intermediate
    // stores no extra information and triples an already large frame dump.
    const std::wstring pattern = (fs::path(dir) / L"frame.%06d.png").wstring();
    std::wstring cmd = L"ffmpeg -y -hide_banner -loglevel error -i " + QuoteArg(src);
    cmd += L" -pix_fmt rgb24 " + QuoteArg(pattern);
    if (!RunHidden(cmd)) {
        if (error) *error = "frame extraction failed";
        return false;
    }
    return true;
}

bool ExtractAudio(const std::wstring& src, const std::wstring& out_audio,
                  std::string* error) {
    std::wstring copy = L"ffmpeg -y -hide_banner -loglevel error -i " + QuoteArg(src);
    copy += L" -vn -c:a copy " + QuoteArg(out_audio);
    if (RunHidden(copy)) return true;

    // Stream copy fails when the source codec cannot sit in the target
    // container; re-encoding once is the usual escape.
    std::wstring reencode = L"ffmpeg -y -hide_banner -loglevel error -i " + QuoteArg(src);
    reencode += L" -vn -c:a aac -b:a 384k " + QuoteArg(out_audio);
    if (RunHidden(reencode)) return true;

    if (error) *error = "no audio track carried over";
    return false;
}

std::wstring BuildVideoEncodeCommand(const std::wstring& pattern, int first_frame,
                                     const std::wstring& output,
                                     const EncodeOptions& options) {
    if (!FfmpegAvailable()) return {};

    std::wstring cmd = L"ffmpeg -y -hide_banner -loglevel error";
    cmd += L" -framerate " + std::to_wstring(options.fps);
    cmd += L" -start_number " + std::to_wstring(first_frame);
    cmd += L" -i " + QuoteArg(pattern);

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

    // Even dimensions keep yuv420p happy; that scale has to happen inside the
    // filter graph when a badge is being composited, because -vf and
    // -filter_complex cannot both drive the same stream.
    const std::wstring even = L"scale=trunc(iw/2)*2:trunc(ih/2)*2";

    if (badge_input >= 0) {
        wchar_t margin[32]{};
        swprintf_s(margin, L"%.4f", options.wm_margin_pct / 100.0);
        // Margin and badge placement are expressed against main-input height,
        // so one setting looks the same at 1080p and 4K.
        std::wstring placement = L"x=W-w-H*" + std::wstring(margin) +
                                 L":y=H-h-H*" + std::wstring(margin);
        if (options.wm_position == L"bottom-left") {
            placement = L"x=H*" + std::wstring(margin) + L":y=H-h-H*" +
                        std::wstring(margin);
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

    cmd += L" -c:v libx264 -preset slow -crf " + std::to_wstring(options.crf);
    cmd += L" -pix_fmt yuv420p -profile:v high -movflags +faststart";
    cmd += L" -colorspace bt709 -color_primaries bt709 -color_trc bt709";
    cmd += L" " + QuoteArg(output);
    return cmd;
}

}  // namespace dlssnr
