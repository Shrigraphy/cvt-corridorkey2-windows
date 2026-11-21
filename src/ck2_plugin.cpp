// ---------------------------------------------------------------------------
// CorridorKey 2 for Resolve - the OFX node
//
// From Creative Video Tips (CVT)
// Created by Chadwick  |  https://creativevideotips.com
// An unofficial plugin around CorridorKey 2.0 by Corridor Digital.
// Licence: CC BY-NC-SA 4.0 with the CorridorKey additional terms (see LICENSE).
//
// Process hands the source frames to the Python worker (worker/ck2_worker.py),
// which keys them with CorridorKey and fills the matte cache. Rendering reads
// the cache and applies the look (output, despill) live.
// ---------------------------------------------------------------------------
#include "ck2_core.h"
#include "ck2_exr.h"
#include "ck2_job.h"

#include "ofxsImageEffect.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace fs = std::filesystem;

namespace {

const char* kPluginID = "com.creativevideotips.corridorkey2";
const char* kDefaultModel = "~/CorridorKey/CorridorKey2.0";
#ifdef _WIN32
const char* kDefaultPython = "~/CorridorKey/.venv/Scripts/python.exe";
#else
const char* kDefaultPython = "~/CorridorKey/.venv/bin/python";
#endif

const char* kProcess = "process";
const char* kStatus = "status";
const char* kRefresh = "refresh";
const char* kOutput = "output";
const char* kDespill = "despill";
const char* kScreen = "screen";
const char* kInputIs = "inputIs";
const char* kWindow = "window";
const char* kPrecision = "precision";
const char* kModelFolder = "modelFolder";
const char* kPythonPath = "pythonPath";
const char* kNodeId = "nodeId";
const char* kRefreshTick = "refreshTick";
const char* kHintClip = "Hint";
const char* kHintFrom = "hintFrom";
const char* kHintFrames = "hintFrames";
const char* kHintStrength = "hintStrength";
const char* kRangeMode = "rangeMode";
const char* kFirstFrame = "firstFrame";
const char* kLastFrame = "lastFrame";
const char* kCacheLimit = "cacheLimit";
const char* kCacheFolder = "cacheFolder";
const char* kClearCache = "clearCache";
const char* kClearAllCache = "clearAllCache";
const char* kInstallModel = "installModel";

const int kWindowChoices[] = {0, 8, 16, 24, 40, 80};          // 0 = Auto
const char* kPrecisionChoices[] = {"auto", "bf16", "fp16"};

std::string homePath() {
#ifdef _WIN32
    wchar_t path[32768];
    const auto length = GetEnvironmentVariableW(L"USERPROFILE", path, 32768);
    return length && length < 32768 ? fs::path(path).generic_u8string() : "";
#else
    const char* home = std::getenv("HOME");
    return home ? home : "";
#endif
}

std::string expandHome(const std::string& p) {
    if (!p.empty() && p[0] == '~') return homePath() + p.substr(1);
    return p;
}

// Logs always stay on this Mac; results and staging go in the Cache Folder.
fs::path localRoot() { return fs::u8path(expandHome("~/.cache/cvt-corridorkey2")); }

// The Cache Folder for new nodes: ~/.config/cvt-corridorkey2/settings.json
// {"cache_folder": "..."} lets each Mac choose (e.g. a fast media drive).
std::string settingsValue(const char* key, const char* fallback) {
    std::ifstream in(fs::u8path(expandHome("~/.config/cvt-corridorkey2/settings.json")));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string value = ck2::jsonField(text, key);
    return value.empty() ? fallback : value;
}

std::string defaultCacheFolder() { return settingsValue("cache_folder", "~/.cache/cvt-corridorkey2"); }

// Where CorridorKey and its Python are: always this Mac's own setting
// (settings.json "model_folder" / "python", else Install Model's folders),
// never a value saved in a comp. A comp from someone else could otherwise
// make Process run any program as "Python".
std::string modelFolderPath() { return expandHome(settingsValue("model_folder", kDefaultModel)); }
std::string pythonPath() { return expandHome(settingsValue("python", kDefaultPython)); }

fs::path bundleResources() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&bundleResources), &module)) {
        wchar_t path[32768];
        const auto length = GetModuleFileNameW(module, path, 32768);
        if (length && length < 32768) return fs::path(path).parent_path().parent_path() / "Resources";
    }
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&bundleResources), &info) && info.dli_fname)
        return fs::path(info.dli_fname).parent_path().parent_path() / "Resources";
#endif
    return {};
}

void setMode(const fs::path& path, int mode) {
#ifdef _WIN32
    // Windows files inherit access control from their folders.
    (void)path;
    (void)mode;
#else
    chmod(path.c_str(), static_cast<mode_t>(mode));
#endif
}

// One line in ~/.cache/cvt-corridorkey2/logs/plugin.log, for troubleshooting.
void plog(const std::string& line) {
    const fs::path dir = localRoot() / "logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    setMode(dir.parent_path(), 0700);
    setMode(dir, 0700);
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &local);
    std::ofstream(dir / "plugin.log", std::ios::app) << ts << "  " << ck2::kVersion << "  " << line << "\n";
}

std::string newNodeId() {
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd());
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(rng()));
    return buf;
}


// A Cache Folder inside the home folder stays private to this user; anywhere
// else (a media drive, a shared network folder) it is readable by others, so
// other Macs and users on the same project can use the results.
bool keepPrivate(const fs::path& root) {
    const auto home = homePath();
    if (home.empty()) return true;
#ifdef _WIN32
    const auto h = fs::u8path(home).lexically_normal().generic_wstring() + L"/";
    const auto r = root.lexically_normal().generic_wstring() + L"/";
    return r.size() >= h.size() && CompareStringOrdinal(r.c_str(), static_cast<int>(h.size()),
        h.c_str(), static_cast<int>(h.size()), TRUE) == CSTR_EQUAL;
#else
    const auto h = fs::u8path(home).lexically_normal().generic_u8string() + "/";
    const auto r = root.lexically_normal().generic_u8string() + "/";
    return r.compare(0, h.size(), h) == 0;
#endif
}

void setFileMode(const fs::path& file, const fs::path& root) {
    setMode(file, keepPrivate(root) ? 0600 : 0644);
}

// A node ID is exactly 16 lowercase hex digits. It comes from the comp, so
// anything else (say "../../Documents") is refused before it is used in a
// folder name the plugin creates or deletes.
bool isNodeId(const std::string& id) {
    return id.size() == 16 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// Makes `root`/`sub` (sub is relative, e.g. "staging/<id>/frames"). Resolve
// runs plugins with umask 000, so each folder this creates gets an explicit
// mode (owner-only in the home folder, readable elsewhere). Folders that
// already existed, the Cache Folder itself included, are never changed: a
// comp's Cache Folder could point at any folder. Returns false on failure.
bool makePrivateDirs(const fs::path& root, const fs::path& sub) {
    const int mode = keepPrivate(root) ? 0700 : 0755;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        fs::create_directories(root, ec);
        if (ec) return false;
        setMode(root, mode);
    }
    if (!fs::is_directory(root, ec)) return false;
    fs::path q = root;
    for (const auto& part : sub.lexically_normal()) {
        const std::string name = part.generic_u8string();
        if (name.empty() || name == "." || name == ".." || name == "/") return false;
        q /= part;
        if (!fs::exists(q, ec)) {
            if (!fs::create_directory(q, ec)) return false;
            setMode(q, mode);
        }
    }
    return fs::is_directory(q, ec);
}

uintmax_t folderBytes(const fs::path& p) {
    uintmax_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    return total;
}

// Keeps the stored results under `limitGB` by removing the least recently
// used nodes' folders (never `keep`).
void pruneCache(const fs::path& mattes, const fs::path& keep, double limitGB) {
    struct Dir { fs::path path; fs::file_time_type time; uintmax_t bytes; };
    std::vector<Dir> dirs;
    uintmax_t total = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(mattes, ec)) {
        // Only the plugin's own node folders, never anything else in the folder.
        if (!e.is_directory(ec) || !isNodeId(e.path().filename().generic_u8string())) continue;
        Dir d{e.path(), fs::last_write_time(e.path(), ec), folderBytes(e.path())};
        total += d.bytes;
        dirs.push_back(d);
    }
    const uintmax_t limit = static_cast<uintmax_t>(limitGB * 1024.0 * 1024.0 * 1024.0);
    std::sort(dirs.begin(), dirs.end(), [](const Dir& a, const Dir& b) { return a.time < b.time; });
    for (const auto& d : dirs) {
        if (total <= limit) break;
        if (d.path == keep) continue;
        fs::remove_all(d.path, ec);
        total -= d.bytes;
    }
}


}  // namespace

class CorridorKey2Effect;
static std::mutex gInstancesMutex;
static std::set<CorridorKey2Effect*> gInstances;   // live nodes, to spot copied IDs

class CorridorKey2Effect : public OFX::ImageEffect {
public:
    explicit CorridorKey2Effect(OfxImageEffectHandle handle)
        : ImageEffect(handle),
          dst_(fetchClip(kOfxImageEffectOutputClipName)),
          src_(fetchClip(kOfxImageEffectSimpleSourceClipName)),
          status_(fetchStringParam(kStatus)),
          output_(fetchChoiceParam(kOutput)),
          despill_(fetchDoubleParam(kDespill)),
          screen_(fetchChoiceParam(kScreen)),
          inputIs_(fetchChoiceParam(kInputIs)),
          window_(fetchChoiceParam(kWindow)),
          precision_(fetchChoiceParam(kPrecision)),
          modelFolder_(fetchStringParam(kModelFolder)),
          pythonPath_(fetchStringParam(kPythonPath)),
          nodeId_(fetchStringParam(kNodeId)),
          refreshTick_(fetchIntParam(kRefreshTick)),
          hint_(getContext() == OFX::eContextGeneral ? fetchClip(kHintClip) : nullptr),
          hintFrom_(fetchChoiceParam(kHintFrom)),
          hintFrames_(fetchChoiceParam(kHintFrames)),
          hintStrength_(fetchDoubleParam(kHintStrength)),
          rangeMode_(fetchChoiceParam(kRangeMode)),
          firstFrame_(fetchIntParam(kFirstFrame)),
          lastFrame_(fetchIntParam(kLastFrame)),
          cacheLimit_(fetchDoubleParam(kCacheLimit)),
          cacheFolder_(fetchStringParam(kCacheFolder)),
          installModel_(fetchPushButtonParam(kInstallModel)) {
        // A new node gets its ID now, so Process never has to write it before
        // reading frames (see startProcess).
        if (!isNodeId(nodeId())) nodeId_->setValue(newNodeId());
        // Install Model only shows while CorridorKey isn't installed.
        installModel_->setIsSecret(modelInstalled());
        int mode = 0;
        rangeMode_->getValue(mode);
        firstFrame_->setEnabled(mode == 1);
        lastFrame_->setEnabled(mode == 1);
        std::lock_guard<std::mutex> lock(gInstancesMutex);
        gInstances.insert(this);
    }

    ~CorridorKey2Effect() override {
        {
            std::lock_guard<std::mutex> lock(gInstancesMutex);
            gInstances.erase(this);
        }
        std::lock_guard<std::mutex> lock(jobMutex_);
        if (job_) job_->detach();
    }

    std::string nodeId() {
        std::string id;
        nodeId_->getValue(id);
        return id;
    }

    void render(const OFX::RenderArguments& args) override;
    void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override;
    void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override;

private:
    // The hint actually used: the Hint input only counts when connected (the
    // Edit page has none).
    ck2::HintFrom effectiveHint() {
        int from = 0;
        hintFrom_->getValue(from);
        auto h = static_cast<ck2::HintFrom>(from);
        if (h == ck2::HintFrom::HintInput && !(hint_ && hint_->isConnected())) return ck2::HintFrom::None;
        return h;
    }
    uint32_t currentSettings(double t) {
        int in = 0, win = 0, prec = 0, frames = 0;
        inputIs_->getValueAtTime(t, in);
        window_->getValueAtTime(t, win);
        precision_->getValueAtTime(t, prec);
        hintFrames_->getValueAtTime(t, frames);
        return ck2::settingsHash(static_cast<ck2::InputSpace>(in), kWindowChoices[win], prec, effectiveHint(),
                                 frames == 0, hintStrength_->getValueAtTime(t));
    }
    // The frames Process covers for the whole shot (without First/Last Frame).
    bool wholeShotRange(long& first, long& last) {
        double t1 = 0, t2 = 0;
        try { timeLineGetBounds(t1, t2); } catch (const std::exception&) { t1 = t2 = 0; }
        OfxRangeD clip{0, -1};
        try { clip = src_->getFrameRange(); } catch (const std::exception&) { return false; }
        return ck2::processRange(clip.min, clip.max, t1, t2, false, 0, 0, first, last);
    }
    // First/Last Frame are only used (and only enabled) with Frames: First to Last.
    void updateRangeControls(bool fillIfEmpty) {
        int mode = 0;
        rangeMode_->getValue(mode);
        firstFrame_->setEnabled(mode == 1);
        lastFrame_->setEnabled(mode == 1);
        long first = 0, last = -1;
        if (!wholeShotRange(first, last)) return;
        firstFrame_->setDisplayRange(static_cast<int>(first), static_cast<int>(last));
        lastFrame_->setDisplayRange(static_cast<int>(first), static_cast<int>(last));
        int ff = 0, lf = 0;
        firstFrame_->getValue(ff);
        lastFrame_->getValue(lf);
        // Fresh (both 0) or outside the shot: start from the whole shot.
        if (fillIfEmpty && mode == 1 &&
            ((ff == 0 && lf == 0) || ff > lf || lf < first || ff > last)) {
            firstFrame_->setValue(static_cast<int>(first));
            lastFrame_->setValue(static_cast<int>(last));
        }
    }
    bool modelInstalled() {
        return fs::exists(fs::u8path(pythonPath())) &&
               fs::exists(fs::u8path(modelFolderPath()) / "weights" / "corridorkey_v2.safetensors");
    }
    fs::path cacheFolder() {
        std::string f;
        cacheFolder_->getValue(f);
        return fs::u8path(expandHome(f.empty() ? defaultCacheFolder() : f)).lexically_normal();
    }
    fs::path nodeCacheDir() {
        std::string id;
        nodeId_->getValue(id);
        return isNodeId(id) ? cacheFolder() / "mattes" / id : fs::path();
    }
    void startProcess();
    void setStatus(const std::string& s) { status_->setValue(s); }

    OFX::Clip* dst_;
    OFX::Clip* src_;
    OFX::StringParam* status_;
    OFX::ChoiceParam* output_;
    OFX::DoubleParam* despill_;
    OFX::ChoiceParam* screen_;
    OFX::ChoiceParam* inputIs_;
    OFX::ChoiceParam* window_;
    OFX::ChoiceParam* precision_;
    OFX::StringParam* modelFolder_;
    OFX::StringParam* pythonPath_;
    OFX::StringParam* nodeId_;
    OFX::IntParam* refreshTick_;
    OFX::Clip* hint_;
    OFX::ChoiceParam* hintFrom_;
    OFX::ChoiceParam* hintFrames_;
    OFX::DoubleParam* hintStrength_;
    OFX::ChoiceParam* rangeMode_;
    OFX::IntParam* firstFrame_;
    OFX::IntParam* lastFrame_;
    OFX::DoubleParam* cacheLimit_;
    OFX::StringParam* cacheFolder_;
    OFX::PushButtonParam* installModel_;
    // The cached result being drawn; kept between renders so a 4K frame's
    // 140 MB isn't allocated every frame (render is instance-safe: one at a time).
    ck2::ImageRGBA cached_;

    std::mutex jobMutex_;
    std::shared_ptr<ck2::Job> job_;
    std::chrono::steady_clock::time_point lastClearAll_{};
};

// --- Process: hand the frames to the worker ---------------------------------

void CorridorKey2Effect::startProcess() {
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        if (job_ && job_->running()) {
            job_->cancel();
            setStatus("Stopping...");
            return;
        }
    }
    // IMPORTANT (Resolve's Fusion page): change no parameter of this node until
    // every frame has been read. A setValue during the button press makes
    // Fusion defer a notification, and fetchImage then waits forever (0.06 to
    // 0.12 froze this way). LTX Alpha Gen also reads first and writes after.
    //
    // A copied node arrives with the original's ID: it gets its own, written
    // after the frames are read.
    std::string id = nodeId();
    bool shared = false;
    {
        std::lock_guard<std::mutex> lock(gInstancesMutex);
        for (auto* other : gInstances)
            if (other != this && isNodeId(id) && other->nodeId() == id) shared = true;
    }
    const bool writeId = !isNodeId(id) || shared;
    if (writeId) id = newNodeId();

    const std::string python = pythonPath();       // this Mac's, never the comp's
    const std::string model = modelFolderPath();
    fs::path worker = bundleResources() / "worker" / "ck2_worker.py";
    if (!fs::exists(fs::u8path(model) / "corridorkey") || !fs::exists(fs::u8path(python))) {
        installModel_->setIsSecret(false);
        setStatus("Error: CorridorKey isn't installed. Use Setup > Install Model");
        return;
    }
    if (!fs::exists(fs::u8path(model) / "weights" / "corridorkey_v2.safetensors")) {
        installModel_->setIsSecret(false);
        setStatus("Error: the weights are missing. Use Setup > Install Model");
        return;
    }
    if (!fs::exists(worker)) { setStatus("Error: worker missing from the plugin"); return; }

    double t1 = 0, t2 = 0;
    try { timeLineGetBounds(t1, t2); } catch (const std::exception&) { t1 = t2 = 0; }
    const OfxRangeD clip = src_->getFrameRange();
    int mode = 0, ff = 0, lf = 0;
    rangeMode_->getValue(mode);
    firstFrame_->getValue(ff);
    lastFrame_->getValue(lf);
    long first = 0, last = -1;
    const bool any = ck2::processRange(clip.min, clip.max, t1, t2, mode == 1, ff, lf, first, last);
    plog("process: clip [" + std::to_string(clip.min) + ", " + std::to_string(clip.max) + "], timeline [" +
         std::to_string(t1) + ", " + std::to_string(t2) + "] -> frames " + std::to_string(first) + " to " +
         std::to_string(last));
    if (!any) {
        setStatus("Error: no frames to process (the clip has frames " +
                  std::to_string(static_cast<long>(std::ceil(clip.min - 1e-6))) + " to " +
                  std::to_string(static_cast<long>(std::floor(clip.max + 1e-6))) + ", the comp " +
                  std::to_string(std::lround(t1)) + " to " + std::to_string(std::lround(t2)) + ")");
        return;
    }

    const fs::path root = cacheFolder();
    if (!makePrivateDirs(root, "")) {
        setStatus("Error: the Cache Folder isn't available: " + root.generic_u8string());
        return;
    }
    fs::path staging = root / "staging" / id;
    std::error_code ec;
    fs::remove_all(staging, ec);
    makePrivateDirs(root, fs::path("staging") / id / "frames");
    const ck2::HintFrom hintFrom = effectiveHint();
    int hintFramesChoice = 0;
    hintFrames_->getValue(hintFramesChoice);
    const bool hintEvery = hintFramesChoice == 0;
    const double hintStrength = hintStrength_->getValueAtTime(t1);
    if (hintFrom != ck2::HintFrom::None) makePrivateDirs(root, fs::path("staging") / id / "hint");
    fs::path cacheDir = root / "mattes" / id;
    makePrivateDirs(root, fs::path("mattes") / id);
    makePrivateDirs(localRoot(), "logs");
    pruneCache(root / "mattes", cacheDir, cacheLimit_->getValue());
    fs::last_write_time(cacheDir, fs::file_time_type::clock::now(), ec);

    // Settings that change the matte are taken at the first frame.
    const uint32_t settings = currentSettings(t1);
    int in = 0, win = 0, prec = 0;
    inputIs_->getValue(in);
    window_->getValue(win);
    precision_->getValue(prec);

    {
        // The last frame first: a range the host can't deliver fails at once.
        plog("process: fetching the last frame, " + std::to_string(last));
        std::unique_ptr<OFX::Image> probe(src_->fetchImage(static_cast<double>(last)));
        plog(std::string("process: last frame ") + (probe ? "received" : "missing"));
        if (!probe) { setStatus("Error: no source picture at frame " + std::to_string(last)); return; }
    }
    // The host's progress bar (with Cancel) while the rest are read.
    struct Progress {
        OFX::ImageEffect* fx;
        explicit Progress(OFX::ImageEffect* e) : fx(e) { fx->progressStart("CorridorKey 2: reading frames", "ck2read"); }
        ~Progress() { fx->progressEnd(); }
    } progress(this);
    std::vector<std::string> names;
    int width = -1, height = -1;
    for (long f = first; f <= last; ++f) {
        if (!progressUpdate(static_cast<double>(f - first) / static_cast<double>(last - first + 1))) {
            setStatus("Cancelled");
            return;
        }
        std::unique_ptr<OFX::Image> img(src_->fetchImage(static_cast<double>(f)));
        if (!img) { setStatus("Error: no source picture at frame " + std::to_string(f)); return; }
        if (img->getPixelDepth() != OFX::eBitDepthFloat ||
            img->getPixelComponents() != OFX::ePixelComponentRGBA) {
            setStatus("Error: source must be RGBA float"); return;
        }
        const OfxRectI b = img->getBounds();
        const int w = b.x2 - b.x1, h = b.y2 - b.y1;
        if (width < 0) { width = w; height = h; }
        else if (w != width || h != height) {
            setStatus("Error: frame " + std::to_string(f) + " has a different size"); return;
        }
        // Top row first: start at the top row and step down.
        const float* top = static_cast<const float*>(img->getPixelAddress(b.x1, b.y2 - 1));
        const long stride = -static_cast<long>(img->getRowBytes() / static_cast<int>(sizeof(float)));
        char name[32];
        std::snprintf(name, sizeof name, "%07ld.exr", f - first);
        std::string err;
        // CorridorKey works in linear sRGB/Rec.709: ACEScg, ACEScct and camera
        // log go over to it first (curve to linear, primaries to Rec.709).
        bool ok;
        const ck2::SpaceConversion& cv = ck2::conversion(static_cast<ck2::InputSpace>(in));
        if (cv.matrix || cv.curve != ck2::Curve::Linear) {
            std::vector<float> conv(static_cast<size_t>(w) * h * 4);
            ck2::parallelBands(h, 16, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < w; ++x) {
                        const float* sp = top + stride * y + x * 4;
                        float* dp = &conv[(static_cast<size_t>(y) * w + x) * 4];
                        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
                        ck2::toModelSpace(cv, dp[0], dp[1], dp[2]);
                    }
            });
            ok = ck2::writeExr((staging / "frames" / name).generic_u8string(), conv.data(), w, h, w * 4L, err);
            setFileMode(staging / "frames" / name, root);
        } else {
            ok = ck2::writeExr((staging / "frames" / name).generic_u8string(), top, w, h, stride, err);
            setFileMode(staging / "frames" / name, root);
        }
        if (!ok) {
            setStatus("Error: " + err); return;
        }
        names.push_back(ck2::cacheFileName(f, settings));
        ck2::writeThumb((cacheDir / (names.back() + ".thumb")).generic_u8string(), ck2::makeThumb(top, w, h, stride));
        setFileMode(cacheDir / (names.back() + ".thumb"), root);

        // Hint: one grey value per pixel, written as R=G=B.
        if (hintFrom != ck2::HintFrom::None && (hintEvery || f == first)) {
            std::unique_ptr<OFX::Image> himg;
            OfxRectI hb{0, 0, 0, 0};
            int hcomp = 0;
            if (hintFrom == ck2::HintFrom::HintInput) {
                himg.reset(hint_->fetchImage(static_cast<double>(f)));
                if (himg) {
                    hb = himg->getBounds();
                    hcomp = himg->getPixelComponents() == OFX::ePixelComponentAlpha ? 1 : 4;
                    if (himg->getPixelDepth() != OFX::eBitDepthFloat) {
                        setStatus("Error: the Hint input must be float"); return;
                    }
                }
            }
            std::vector<float> hpx(static_cast<size_t>(w) * h * 4, 0.0f);
            for (int y = 0; y < h; ++y) {
                const int py = b.y2 - 1 - y;                 // top row first
                for (int x = 0; x < w; ++x) {
                    const int px = b.x1 + x;
                    float v = 0.0f;
                    if (hintFrom == ck2::HintFrom::SourceAlpha) {
                        v = static_cast<const float*>(img->getPixelAddress(px, py))[3];
                    } else if (himg && px >= hb.x1 && px < hb.x2 && py >= hb.y1 && py < hb.y2) {
                        const float* q = static_cast<const float*>(himg->getPixelAddress(px, py));
                        v = hcomp == 1 ? q[0] : q[3];
                    }
                    v = std::fmin(std::fmax(v, 0.0f), 1.0f);
                    float* o = &hpx[(static_cast<size_t>(y) * w + x) * 4];
                    o[0] = o[1] = o[2] = v; o[3] = 1.0f;
                }
            }
            if (!ck2::writeExr((staging / "hint" / name).generic_u8string(), hpx.data(), w, h, w * 4L, err)) {
                setStatus("Error: " + err); return;
            }
            setFileMode(staging / "hint" / name, root);
        }
    }

    if (writeId) nodeId_->setValue(id);      // only now: every frame has been read
    plog("process: read " + std::to_string(names.size()) + " frames");

    ck2::JobSpec spec;
    spec.modelDir = model;
    spec.framesDir = (staging / "frames").generic_u8string();
    spec.workDir = (staging / "work").generic_u8string();
    spec.outputDir = cacheDir.generic_u8string();
    spec.hintDir = (staging / "hint").generic_u8string();
    spec.inputLinear = ck2::inputIsLinear(static_cast<ck2::InputSpace>(in));   // converted to linear Rec.709
    spec.window = kWindowChoices[win];
    spec.precision = kPrecisionChoices[prec];
    spec.hintFrom = hintFrom;
    spec.hintEveryFrame = hintEvery;
    spec.hintStrength = hintStrength;
    spec.outputNames = names;
    spec.privateFiles = keepPrivate(root);
    fs::path jobPath = staging / "job.json";
    { std::ofstream(jobPath) << ck2::jobJson(spec); }
    setMode(jobPath, 0600);

    // Updates come from the worker's reader thread.
    // The Edit page (Filter context in Resolve) doesn't redraw by itself when the
    // job ends; only moving the playhead does (LTX found the same).
    const bool editPage = getContext() == OFX::eContextFilter;
    auto onUpdate = [this, staging, editPage](const ck2::JobUpdate& u) {
        status_->setValue(u.ok && editPage ? u.status + " - move the playhead to see it" : u.status);
        if (u.finished) {
            std::error_code e;
            fs::remove_all(staging, e);              // the frames handed over are no longer needed
            int tick = 0;
            refreshTick_->getValue(tick);
            refreshTick_->setValue(tick + 1);        // redraw with the new mattes
        }
    };
    std::string err;
    auto job = ck2::Job::start(python, worker.generic_u8string(), jobPath.generic_u8string(),
                               (localRoot() / "logs" / "worker.log").generic_u8string(), onUpdate, err);
    if (!job) { setStatus("Error: " + err); return; }
    {
        std::lock_guard<std::mutex> lock(jobMutex_);
        if (job_) job_->detach();
        job_ = job;
    }
    setStatus("Starting " + std::to_string(names.size()) + " frames...");
}

void CorridorKey2Effect::changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) {
    if (args.reason != OFX::eChangeUserEdit) return;
    if (name == kProcess) {
        startProcess();
    } else if (name == kInstallModel) {
#ifdef _WIN32
        const fs::path cmd = bundleResources() / "setup" / "ck2_setup.ps1";
        if (!fs::exists(cmd)) { setStatus("Error: setup missing from the plugin"); return; }
        wchar_t systemDir[32768];
        const auto length = GetSystemDirectoryW(systemDir, 32768);
        if (!length || length >= 32768) { setStatus("Error: Windows PowerShell was not found"); return; }
        const auto exe = fs::path(systemDir) / "WindowsPowerShell/v1.0/powershell.exe";
        const std::wstring args = L"-NoProfile -NoExit -ExecutionPolicy Bypass -File \"" + cmd.native() + L"\"";
        const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(),
                                                              cmd.parent_path().c_str(), SW_SHOWNORMAL));
        setStatus(rc > 32 ? "Setup opened in PowerShell" : "Error: could not open PowerShell");
#else
        // Opens the setup in Terminal, where it can ask for the licence and show progress.
        fs::path cmd = bundleResources() / "setup" / "Install CorridorKey Model.command";
        if (!fs::exists(cmd)) { setStatus("Error: setup missing from the plugin"); return; }
        std::string open = "/usr/bin/open", a = "-a", term = "Terminal", c = cmd.generic_u8string();
        char* argv[] = {open.data(), a.data(), term.data(), c.data(), nullptr};
        pid_t pid;
        if (posix_spawn(&pid, open.c_str(), nullptr, nullptr, argv, environ) == 0) {
            int st;
            waitpid(pid, &st, 0);
            setStatus("Setup opened in Terminal");
        } else {
            setStatus("Error: could not open Terminal");
        }
#endif
    } else if (name == kClearCache) {
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            if (job_ && job_->running()) { setStatus("Stop Process first"); return; }
        }
        fs::path dir = nodeCacheDir();
        std::error_code ec;
        if (dir.empty() || !fs::exists(dir, ec)) { setStatus("Nothing to delete"); return; }
        fs::remove_all(dir, ec);
        setStatus("This node's cache deleted");
        int tick = 0;
        refreshTick_->getValue(tick);
        refreshTick_->setValue(tick + 1);
    } else if (name == kClearAllCache) {
        // No Yes/No dialogs in Resolve: a second press within 5 seconds confirms.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastClearAll_ > std::chrono::seconds(5)) {
            lastClearAll_ = now;
            setStatus("Press Delete All Cache again within 5 seconds to delete every node's cache");
            return;
        }
        lastClearAll_ = {};
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            if (job_ && job_->running()) { setStatus("Stop Process first"); return; }
        }
        // Only the plugin's own folders: mattes/<16 hex digits> and staging/<...>.
        const fs::path root = cacheFolder();
        std::error_code ec;
        int removed = 0;
        for (const char* sub : {"mattes", "staging"}) {
            for (auto& e : fs::directory_iterator(root / sub, ec)) {
                const std::string n = e.path().filename().generic_u8string();
                if (n.size() == 16 && n.find_first_not_of("0123456789abcdef") == std::string::npos &&
                    e.is_directory(ec)) {
                    fs::remove_all(e.path(), ec);
                    if (std::string(sub) == "mattes") ++removed;
                }
            }
        }
        plog("delete all cache: " + std::to_string(removed) + " nodes in " + root.generic_u8string());
        setStatus("Deleted the cache of " + std::to_string(removed) + " node(s)");
        int tick = 0;
        refreshTick_->getValue(tick);
        refreshTick_->setValue(tick + 1);
    } else if (name == kRangeMode) {
        updateRangeControls(true);
    } else if (name == kRefresh) {
        int tick = 0;
        refreshTick_->getValue(tick);
        refreshTick_->setValue(tick + 1);
    }
}

void CorridorKey2Effect::getClipPreferences(OFX::ClipPreferencesSetter& prefs) {
    int out = 0;
    output_->getValue(out);
    prefs.setOutputPremultiplication(
        static_cast<ck2::Output>(out) == ck2::Output::Premultiplied ? OFX::eImagePreMultiplied
        : static_cast<ck2::Output>(out) == ck2::Output::MatteOnly ? OFX::eImageOpaque
        : OFX::eImageUnPreMultiplied);
}

// --- Render: draw from the cache ------------------------------------------

void CorridorKey2Effect::render(const OFX::RenderArguments& args) {
    std::unique_ptr<OFX::Image> dst(dst_->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(src_->fetchImage(args.time));
    if (!dst) OFX::throwSuiteStatusException(kOfxStatFailed);
    if (dst->getPixelDepth() != OFX::eBitDepthFloat ||
        dst->getPixelComponents() != OFX::ePixelComponentRGBA)
        OFX::throwSuiteStatusException(kOfxStatErrFormat);

    const OfxRectI rw = args.renderWindow;
    const OfxRectI sb = src ? src->getBounds() : OfxRectI{0, 0, 0, 0};

    // The cached result for this frame, if Process has made one.
    ck2::ImageRGBA& cached = cached_;
    bool have = false, changed = false;
    fs::path dir = nodeCacheDir();
    if (!dir.empty() && src) {
        fs::path file = dir / ck2::cacheFileName(std::lround(args.time), currentSettings(args.time));
        std::string err;
        const bool exists = fs::exists(file);
        const bool read = exists && ck2::readExrRGBA(file.generic_u8string(), cached, err);
        have = read && cached.width == sb.x2 - sb.x1 && cached.height == sb.y2 - sb.y1;
        // Same picture as at Process? (a slipped clip or a new grade upstream is not)
        std::vector<float> stored;
        double diff = 0.0;
        if (have && ck2::readThumb(file.generic_u8string() + ".thumb", stored)) {
            const float* top = static_cast<const float*>(src->getPixelAddress(sb.x1, sb.y2 - 1));
            const long stride = -static_cast<long>(src->getRowBytes() / static_cast<int>(sizeof(float)));
            diff = ck2::thumbDiff(stored, ck2::makeThumb(top, sb.x2 - sb.x1, sb.y2 - sb.y1, stride));
            changed = diff > ck2::kThumbChanged;
        }
        // A processed frame drawn as stripes: say why (troubleshooting).
        if (exists && (!have || changed)) {
            char why[256];
            if (!read)
                std::snprintf(why, sizeof why, "could not read the result (%s)", err.c_str());
            else if (!have)
                std::snprintf(why, sizeof why, "size %dx%d, result %dx%d", sb.x2 - sb.x1, sb.y2 - sb.y1,
                              cached.width, cached.height);
            else
                std::snprintf(why, sizeof why, "picture changed by %.4f (limit %.2f)", diff, ck2::kThumbChanged);
            plog("render: time " + std::to_string(args.time) + " scale " +
                 std::to_string(args.renderScale.x) + ": stripes, " + why);
        }
    }

    ck2::Look look;
    int out = 0, scr = 0, in = 0;
    output_->getValueAtTime(args.time, out);
    screen_->getValueAtTime(args.time, scr);
    inputIs_->getValueAtTime(args.time, in);
    look.output = static_cast<ck2::Output>(out);
    look.despill = static_cast<float>(despill_->getValueAtTime(args.time));
    look.screen = static_cast<ck2::Screen>(scr);
    look.input = static_cast<ck2::InputSpace>(in);

    const int border = std::max(4, (sb.x2 - sb.x1) / 200);
    const bool useCache = have && !changed;
    // Source rows by pointer (one getPixelAddress per frame, not per pixel).
    const float* srcBase = src ? static_cast<const float*>(src->getPixelAddress(sb.x1, sb.y1)) : nullptr;
    const long srcStride = src ? src->getRowBytes() / static_cast<int>(sizeof(float)) : 0;
    if (abort()) return;
    // Bands of rows on every core: about 6 ms for a 4K frame instead of 50.
    ck2::parallelBands(rw.y2 - rw.y1, 16, [&](int b0, int b1) {
        for (int y = rw.y1 + b0; y < rw.y1 + b1; ++y) {
            float* d = static_cast<float*>(dst->getPixelAddress(rw.x1, y));
            if (!d) continue;
            const bool rowIn = srcBase && y >= sb.y1 && y < sb.y2;
            const float* srow = rowIn ? srcBase + static_cast<long>(y - sb.y1) * srcStride : nullptr;
            const float* crow = rowIn && useCache
                ? &cached.pixels[static_cast<size_t>(sb.y2 - 1 - y) * cached.width * 4] : nullptr;
            for (int x = rw.x1; x < rw.x2; ++x, d += 4) {
                if (!rowIn || x < sb.x1 || x >= sb.x2) {
                    d[0] = d[1] = d[2] = d[3] = 0.0f;
                    continue;
                }
                const float* s = srow + static_cast<long>(x - sb.x1) * 4;
                if (crow) {
                    const float* c = crow + static_cast<size_t>(x - sb.x1) * 4;
                    ck2::composePixel(look, c, c[3], s, d);
                    continue;
                }
                // Not processed (red frame) or picture changed since Process
                // (yellow frame): the source, dimmed in diagonal stripes.
                const bool edge = x < sb.x1 + border || x >= sb.x2 - border ||
                                  y < sb.y1 + border || y >= sb.y2 - border;
                const bool stripe = (((x - sb.x1) + (y - sb.y1)) / 48) % 2 == 0;
                if (edge) { d[0] = 1.0f; d[1] = changed ? 0.85f : 0.0f; d[2] = 0.0f; d[3] = 1.0f; }
                else {
                    const float k = stripe ? 0.45f : 1.0f;
                    d[0] = s[0] * k; d[1] = s[1] * k; d[2] = s[2] * k; d[3] = 1.0f;
                }
            }
        }
    });
}

// --- Description ------------------------------------------------------------

mDeclarePluginFactory(CorridorKey2Factory, {}, {});

void CorridorKey2Factory::describe(OFX::ImageEffectDescriptor& desc) {
    desc.setLabels("CorridorKey 2", "CorridorKey 2", "CorridorKey 2");
    desc.setPluginGrouping("Creative Video Tips");
    desc.setPluginDescription(
        "CorridorKey 2 for Resolve (unofficial). Keys green and blue screens with "
        "CorridorKey 2.0 by Corridor Digital. Resolve OFX by Creative Video Tips.");
    // Filter (Edit page) and General (Fusion page, with the Hint input). The
    // 0.06-0.12 Process freezes were setValue before fetchImage, not the
    // General context (0.11-0.13 offered Filter only while that was found).
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedContext(OFX::eContextGeneral);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    // Process reads every frame of the shot from a button press. Fusion only
    // delivers frames other than the one being rendered to clips declared
    // with temporal access; without it fetchImage waits forever (0.06, 0.07).
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
}

namespace {

OFX::ChoiceParamDescriptor* choice(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                   OFX::GroupParamDescriptor* group, const char* name,
                                   const char* label, const char* hint,
                                   std::initializer_list<const char*> options, int def) {
    auto* p = desc.defineChoiceParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    for (const char* o : options) p->appendOption(o);
    p->setDefault(def);
    p->setAnimates(false);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

OFX::GroupParamDescriptor* group(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                 const char* name, const char* label, bool open) {
    auto* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    if (page) page->addChild(*g);
    return g;
}

}  // namespace

void CorridorKey2Factory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) {
    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->setTemporalClipAccess(true);
    src->setSupportsTiles(false);
    src->setIsMask(false);

    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    OFX::PageParamDescriptor* page = desc.definePageParam("Controls");

    // Top, before Process: what the footage is, and how many frames are keyed
    // together. Single controls, so no section of their own.
    choice(desc, page, nullptr, kInputIs, "Input Is",
           "What the picture coming into the node is. Rec.709: pictures as they look on a "
           "monitor (most timelines). sRGB Linear: linear light with sRGB/Rec.709 primaries. "
           "ACEScg: linear ACES (AP1), such as CG renders or an ACES Fusion page. "
           "DWG Linear: what the Fusion page gets in a DaVinci colour-managed project. "
           "DWG / DaVinci Intermediate: a DaVinci colour-managed timeline. "
           "The camera logs are for clips still in their log: pick the camera's log and "
           "gamut. DJI D-Log M fits by Thatcher Freeman. CorridorKey keys in linear "
           "Rec.709, and the output goes back to the space picked here. Changing it "
           "needs a new Process.",
           {"Rec.709", "sRGB Linear", "ACEScg", "DWG Linear", "DWG / DaVinci Intermediate",
            "Sony S-Log3 / S-Gamut3.Cine", "Blackmagic Film Gen 5", "ACEScct",
            "ARRI LogC3 (EI 800)", "ARRI LogC4", "RED Log3G10 / REDWideGamutRGB",
            "Canon Log 2 / Cinema Gamut", "Canon Log 3 / Cinema Gamut", "Apple Log / Rec.2020",
            "Apple Log 2 / Apple Gamut", "DJI D-Log M (Pocket 3)", "DJI D-Log M (Action 5)",
            "DJI D-Log / D-Gamut", "Panasonic V-Log / V-Gamut"}, 0);
    choice(desc, page, nullptr, kWindow, "Temporal Chunks",
           "How many frames CorridorKey keys together; chunks overlap by half and are "
           "blended, which keeps the matte steady from frame to frame. Longer chunks: "
           "steadier on motion, more memory (HD: 16 about 24 GB, 24 about 31 GB, 40 about "
           "46 GB, 80 about 82 GB). Auto picks the longest this Mac's memory allows. "
           "Changing it needs a new Process.",
           {"Auto", "8", "16", "24", "40", "80"}, 0);

    auto* process = desc.definePushButtonParam(kProcess);
    process->setLabels("Process", "Process", "Process");
    process->setHint("Keys every frame of the shot with CorridorKey and stores the results. "
                     "Press again while it runs to stop.");
    page->addChild(*process);

    auto* status = desc.defineStringParam(kStatus);
    status->setAnimates(false);              // never keyframed
    status->setLabels("Status", "Status", "Status");
    // Resolve and Fusion draw a label param with its name, not its value, so
    // Status (whose text changes) is a greyed-out text field, as in LTX.
    status->setStringType(OFX::eStringTypeSingleLine);
    status->setEnabled(false);
    status->setDefault("Not processed yet");
    status->setIsPersistant(true);           // the last result is still there after reopening
    status->setEvaluateOnChange(false);
    status->setCanUndo(false);
    page->addChild(*status);

    auto* gOut = group(desc, page, "groupOutput", "Output", true);
    choice(desc, page, gOut, kOutput, "Output",
           "Premultiplied: the cut-out foreground with its matte, ready to merge. "
           "Matte Only: the matte in black and white. "
           "Unpremultiplied: the same cleaned, despilled foreground with the matte as alpha, "
           "not multiplied by it; grade it, then premultiply. "
           "Source + Matte (Unpremultiplied): your picture untouched (no despill) with the "
           "matte as alpha, for your own spill and colour work. For the two unpremultiplied "
           "outputs, premultiply before a Merge or set the Merge to Subtractive.",
           {"Premultiplied", "Matte Only", "Unpremultiplied", "Source + Matte (Unpremultiplied)"}, 0);
    auto* despill = desc.defineDoubleParam(kDespill);
    despill->setAnimates(false);              // never keyframed
    despill->setLabels("Despill", "Despill", "Despill");
    despill->setHint("Removes the screen colour that spills onto the foreground. "
                     "0 is off; lower it for green or blue objects you want to keep.");
    despill->setDefault(0.5);
    despill->setRange(0.0, 1.0);
    despill->setDisplayRange(0.0, 1.0);
    despill->setParent(*gOut);
    page->addChild(*despill);
    choice(desc, page, gOut, kScreen, "Screen", "The screen colour, for despill.",
           {"Green", "Blue"}, 0);

    if (context == OFX::eContextGeneral) {
        OFX::ClipDescriptor* hint = desc.defineClip(kHintClip);
        hint->addSupportedComponent(OFX::ePixelComponentRGBA);
        hint->addSupportedComponent(OFX::ePixelComponentAlpha);
        hint->setOptional(true);
        hint->setIsMask(true);
        hint->setSupportsTiles(false);
        hint->setTemporalClipAccess(true);
    }
    auto* gHint = group(desc, page, "groupHint", "Hint", false);
    choice(desc, page, gHint, kHintFrom, "Hint From",
           "A rough matte of the subject (Magic Mask, roto, even a garbage box) that tells "
           "CorridorKey what to keep. Hint Input: the node's Hint input (Fusion page). "
           "Source Alpha: the alpha of the picture coming in (also on the Edit page). "
           "Needs a new Process.",
           {"None", "Hint Input", "Source Alpha"}, 1);
    choice(desc, page, gHint, kHintFrames, "Hint Frames",
           "Every Frame: use the hint on all frames (best for moving subjects). "
           "First Frame Only: CorridorKey carries it through the shot itself.",
           {"Every Frame", "First Frame Only"}, 0);
    auto* hs = desc.defineDoubleParam(kHintStrength);
    hs->setLabels("Hint Strength", "Hint Strength", "Hint Strength");
    hs->setHint("How much CorridorKey trusts the hint. 0.3 to 0.5 for rough mattes and garbage "
                "boxes: it removes what's outside and keeps its own edges inside. Above 0.9 only "
                "for an accurate matte, because the result follows the hint's edges.");
    hs->setDefault(0.5);
    hs->setRange(0.0, 1.0);
    hs->setDisplayRange(0.0, 1.0);
    hs->setAnimates(false);
    hs->setParent(*gHint);
    page->addChild(*hs);

    auto* gRange = group(desc, page, "groupRange", "Range", false);
    choice(desc, page, gRange, kRangeMode, "Frames",
           "Whole Shot: every frame of the clip or comp. First to Last: only those frames. "
           "On the Edit page frames count from 0 at the start of the clip.",
           {"Whole Shot", "First to Last"}, 0);
    for (auto [pname, label] : {std::pair{kFirstFrame, "First Frame"}, std::pair{kLastFrame, "Last Frame"}}) {
        auto* fp = desc.defineIntParam(pname);
        fp->setLabels(label, label, label);
        fp->setHint("Used when Frames is First to Last. Switching Frames to First to Last fills "
                    "in the shot's own first and last frame, so you can narrow it down.");
        fp->setDefault(0);
        fp->setRange(-1000000, 10000000);
        fp->setDisplayRange(0, 1000);
        fp->setAnimates(false);
        fp->setEvaluateOnChange(false);
        fp->setEnabled(false);               // until Frames is First to Last
        fp->setParent(*gRange);
        page->addChild(*fp);
    }

    auto* gCache = group(desc, page, "groupCache", "Cache", false);
    auto* folder = desc.defineStringParam(kCacheFolder);
    folder->setAnimates(false);              // never keyframed
    folder->setLabels("Cache Folder", "Cache Folder", "Cache Folder");
    folder->setHint("Where the stored results go (about 2.5 MB per HD frame). A fast drive with "
                    "room is best; a shared network folder lets other Macs on the same project "
                    "use the results. Changing it needs a new Process. New nodes start with the "
                    "folder set in ~/.config/cvt-corridorkey2/settings.json, or ~/.cache/cvt-corridorkey2.");
    folder->setStringType(OFX::eStringTypeDirectoryPath);
    folder->setDefault(defaultCacheFolder());
    folder->setEvaluateOnChange(true);
    folder->setParent(*gCache);
    page->addChild(*folder);
    auto* limit = desc.defineDoubleParam(kCacheLimit);
    limit->setLabels("Cache Limit (GB)", "Cache Limit (GB)", "Cache Limit (GB)");
    limit->setHint("Stored results of all CorridorKey 2 nodes together. When Process goes over "
                   "it, the results used longest ago are removed. About 2.5 MB per HD frame.");
    limit->setDefault(100.0);
    limit->setRange(1.0, 10000.0);
    limit->setDisplayRange(10.0, 500.0);
    limit->setAnimates(false);
    limit->setEvaluateOnChange(false);
    limit->setParent(*gCache);
    page->addChild(*limit);
    auto* refresh = desc.definePushButtonParam(kRefresh);
    refresh->setLabels("Refresh", "Refresh", "Refresh");
    refresh->setHint("Redraws this node from its stored results, if the viewer hasn't caught up "
                     "after a Process. Nothing is keyed again or deleted.");
    refresh->setParent(*gCache);
    page->addChild(*refresh);
    auto* clear = desc.definePushButtonParam(kClearCache);
    clear->setLabels("Delete Cache", "Delete Cache", "Delete Cache");
    clear->setHint("Deletes this node's stored results (other nodes keep theirs). Its frames "
                   "show stripes until the next Process.");
    clear->setParent(*gCache);
    page->addChild(*clear);
    auto* clearAll = desc.definePushButtonParam(kClearAllCache);
    clearAll->setLabels("Delete All Cache", "Delete All Cache", "Delete All Cache");
    clearAll->setHint("Deletes the stored results of every CorridorKey 2 node in this Cache "
                      "Folder, in every project. Press twice within 5 seconds to confirm.");
    clearAll->setParent(*gCache);
    page->addChild(*clearAll);

    // The version goes in the section's title: a label line shows its name on
    // both sides in Resolve and Fusion, so it can't show anything once.
    const std::string setupTitle = std::string("Setup \u00b7 Version ") + ck2::kVersion;
    auto* gSetup = group(desc, page, "groupSetup", setupTitle.c_str(), false);
    gSetup->setHint("CorridorKey 2 for Resolve " + std::string(ck2::kVersion) +
                    ": OFX by Creative Video Tips, model by Corridor Digital. An unofficial plugin; "
                    "CC BY-NC-SA 4.0 with the CorridorKey terms. DJI D-Log M conversions "
                    "by Thatcher Freeman (github.com/thatcherfreeman/dwg-transforms).");
    // Precision is picked for each Mac (fp16 on M1 to M4, bf16 on M5 and later);
    // kept as a hidden param so saved comps and the cache names still match.
    choice(desc, nullptr, nullptr, kPrecision, "Precision", "", {"Auto", "bf16", "fp16"}, 0)
        ->setIsSecret(true);
    auto* model = desc.defineStringParam(kModelFolder);
    model->setAnimates(false);              // never keyframed
    model->setLabels("Model Folder", "Model Folder", "Model Folder");
    model->setHint("Where CorridorKey 2.0 and its weights are on this computer (Install Model puts "
                   "them in ~/CorridorKey). For safety this is never taken from a comp; to use "
                   "another folder, set \"model_folder\" in ~/.config/cvt-corridorkey2/settings.json.");
    model->setStringType(OFX::eStringTypeDirectoryPath);
    model->setDefault(settingsValue("model_folder", kDefaultModel));
    model->setIsPersistant(false);           // shown only; Process uses this Mac's setting
    model->setEnabled(false);
    model->setEvaluateOnChange(false);
    model->setParent(*gSetup);
    page->addChild(*model);
    auto* python = desc.defineStringParam(kPythonPath);
    python->setAnimates(false);              // never keyframed
    python->setLabels("Python", "Python", "Python");
    python->setHint("The Python with CorridorKey's packages on this computer. For safety this is "
                    "never taken from a comp; to use another, set \"python\" in "
                    "~/.config/cvt-corridorkey2/settings.json.");
    python->setStringType(OFX::eStringTypeFilePath);
    python->setDefault(settingsValue("python", kDefaultPython));
    python->setIsPersistant(false);           // shown only; Process uses this Mac's setting
    python->setEnabled(false);
    python->setEvaluateOnChange(false);
    python->setParent(*gSetup);
    page->addChild(*python);
    auto* install = desc.definePushButtonParam(kInstallModel);
    install->setLabels("Install Model", "Install Model", "Install Model");
    install->setHint("Opens a setup window to download CorridorKey 2.0 and its weights "
                     "(about 5 GB) in ~/CorridorKey. Also repairs an existing install.");
    install->setParent(*gSetup);
    page->addChild(*install);

    auto* nodeId = desc.defineStringParam(kNodeId);
    nodeId->setAnimates(false);              // never keyframed
    nodeId->setLabels("Node ID", "Node ID", "Node ID");
    nodeId->setIsSecret(true);
    nodeId->setEvaluateOnChange(false);
    nodeId->setCanUndo(false);

    auto* tick = desc.defineIntParam(kRefreshTick);
    tick->setLabels("Refresh Tick", "Refresh Tick", "Refresh Tick");
    tick->setIsSecret(true);
    tick->setDefault(0);
    tick->setAnimates(false);
    tick->setCanUndo(false);
    tick->setIsPersistant(false);
}

OFX::ImageEffect* CorridorKey2Factory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
    return new CorridorKey2Effect(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
    static CorridorKey2Factory factory(kPluginID, ck2::kVersionMajor, ck2::kVersionMinor);
    ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
