// Aqua_Filament native engine for AquariusLang (Windows / Linux).
//
// DynamicLibrary ABI 1:  int32_t fl_call(const uint8_t* in, int32_t n, uint8_t* out, int32_t cap)
// The input is a JSON batch {"cmds":[{"op":...}, ...]}; the output is
// {"ok":true,"results":[...]} or {"ok":false,"error":"...","index":i}.
//
// A dedicated render thread owns the OS window, the Filament Engine and every Filament object,
// so the library is independent of which host thread executes Aquarius code. Calls block until
// the render thread has executed the whole batch.

#include "window.h"

#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <camutils/Manipulator.h>
#include <filameshio/MeshReader.h>
#include <geometry/SurfaceOrientation.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/FilamentInstance.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <gltfio/materials/uberarchive.h>
#include <image/Ktx1Bundle.h>
#include <ktxreader/Ktx1Reader.h>
#include <ktxreader/Ktx2Reader.h>
#include <math/mat3.h>
#include <math/mat4.h>
#include <math/norm.h>
#include <math/quat.h>
#include <math/vec3.h>
#include <math/vec4.h>
#include <utils/EntityManager.h>
#include <utils/NameComponentManager.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define AQUA_EXPORT extern "C" __declspec(dllexport)
#else
#include <dlfcn.h>
#define AQUA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using namespace filament;
using namespace filament::math;
using utils::Entity;
using utils::EntityManager;
using Json = nlohmann::json;

namespace aquafl {

static constexpr const char* kVersion = "1.0.0";
static constexpr double kPi = 3.14159265358979323846;
static constexpr const char* kFilamentVersion = "1.78.0";

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] static void fail(const std::string& message) { throw Failure(message); }

// ------------------------------------------------------------------------------------------------
// JSON helpers
// ------------------------------------------------------------------------------------------------

static double num(const Json& j, const char* key, double fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>() ? 1.0 : 0.0;
    if (!it->is_number()) fail(std::string("Expected a number for '") + key + "' / 欄位必須是數字");
    return it->get<double>();
}

static bool flag(const Json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    fail(std::string("Expected a boolean for '") + key + "' / 欄位必須是布林值");
}

static std::string text(const Json& j, const char* key, const std::string& fallback = {}) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_string()) fail(std::string("Expected text for '") + key + "' / 欄位必須是文字");
    return it->get<std::string>();
}

static bool has(const Json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && !it->is_null();
}

static std::vector<double> numbers(const Json& v, size_t minCount, const char* key) {
    if (!v.is_array()) fail(std::string("Expected a number array for '") + key + "' / 欄位必須是數字陣列");
    std::vector<double> out;
    out.reserve(v.size());
    for (auto& e : v) {
        if (!e.is_number()) fail(std::string("Expected numbers in '") + key + "' / 陣列只能包含數字");
        out.push_back(e.get<double>());
    }
    if (out.size() < minCount) fail(std::string("Too few values in '") + key + "' / 陣列元素不足");
    return out;
}

static float3 vec3(const Json& j, const char* key, float3 fallback) {
    if (!has(j, key)) return fallback;
    auto v = numbers(j.at(key), 3, key);
    return {float(v[0]), float(v[1]), float(v[2])};
}

static float4 vec4(const Json& j, const char* key, float4 fallback) {
    if (!has(j, key)) return fallback;
    auto v = numbers(j.at(key), 3, key);
    return {float(v[0]), float(v[1]), float(v[2]), v.size() > 3 ? float(v[3]) : fallback.w};
}

static int64_t idOf(const Json& j, const char* key = "id") {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) fail(std::string("Missing integer '") + key + "' / 缺少整數代碼");
    return it->get<int64_t>();
}

static Json arr3(float3 v) { return Json::array({v.x, v.y, v.z}); }

// ------------------------------------------------------------------------------------------------
// Files (UTF-8 paths, wide APIs on Windows so non-ASCII temp folders work)
// ------------------------------------------------------------------------------------------------

#if defined(_WIN32)
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static FILE* openFile(const std::string& path, const char* mode) {
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
}
#else
static FILE* openFile(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
#endif

static bool fileExists(const std::string& path) {
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f = openFile(path, "rb");
    if (!f) fail("Cannot open file / 無法開啟檔案: " + path);
    std::vector<uint8_t> data;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize((size_t)size);
        if (fread(data.data(), 1, (size_t)size, f) != (size_t)size) {
            fclose(f);
            fail("Cannot read file / 無法讀取檔案: " + path);
        }
    }
    fclose(f);
    return data;
}

static std::string normalizeSlashes(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    return p;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Lexically removes "." and ".." segments (keeps a leading "/" or drive prefix).
static std::string collapsePath(const std::string& path) {
    std::string p = normalizeSlashes(path);
    std::string prefix;
    if (p.size() > 1 && p[1] == ':') { prefix = p.substr(0, 2); p = p.substr(2); }
    bool absolute = !p.empty() && p[0] == '/';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        std::string part = p.substr(start, end - start);
        if (part == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else if (!absolute) parts.push_back(part); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        start = end + 1;
    }
    std::string out = prefix + (absolute ? "/" : "");
    for (size_t i = 0; i < parts.size(); i++) out += (i ? "/" : "") + parts[i];
    return out;
}

static std::string directoryOf(const std::string& path) {
    auto p = normalizeSlashes(path);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

static std::string extensionOf(const std::string& path) {
    auto dot = path.find_last_of('.');
    auto slash = normalizeSlashes(path).find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot + 1));
}

static std::string modulePath() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&modulePath), &module);
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(module, buffer.data(), (DWORD)buffer.size());
    buffer.resize(n);
    return normalizeSlashes(narrow(buffer));
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&modulePath), &info) && info.dli_fname) return info.dli_fname;
    return {};
#endif
}

// ------------------------------------------------------------------------------------------------
// Scene objects
// ------------------------------------------------------------------------------------------------

enum class Kind { Entity, Light, Shape, Mesh, Model, Material, MaterialInstance, Texture };

struct Object {
    Kind kind = Kind::Entity;
    Entity entity;                       // transform / renderable / light entity
    VertexBuffer* vb = nullptr;          // owned geometry (shapes, meshes)
    IndexBuffer* ib = nullptr;
    gltfio::FilamentAsset* asset = nullptr;
    Material* material = nullptr;
    MaterialInstance* instance = nullptr;
    Texture* texture = nullptr;
    bool ownsMaterial = true;
    bool loading = false;                // gltf textures still decoding
    std::vector<float> emissiveBase;     // gltf emissive strengths captured at load
    std::shared_ptr<std::vector<uint8_t>> source; // CPU bytes referenced by pending GPU uploads
};

struct Job {
    Json request;
    Json response;
    bool done = false;
};

struct PendingScreenshot {
    std::string path;
};

class Runtime {
public:
    ~Runtime() { stop(); }

    Json submit(Json&& request) {
        auto job = std::make_shared<Job>();
        job->request = std::move(request);
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (!mThread.joinable()) {
                if (!mStopped) startLocked();
            }
            if (mStopped) {
                return Json{{"ok", false}, {"error", "Filament runtime has stopped / Filament 執行環境已停止"}};
            }
            mJobs.push_back(job);
        }
        mWake.notify_all();
        std::unique_lock<std::mutex> lock(mMutex);
        mDone.wait(lock, [&] { return job->done; });
        return std::move(job->response);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mQuit = true;
        }
        mWake.notify_all();
        if (mThread.joinable()) mThread.join();
        std::lock_guard<std::mutex> lock(mMutex);
        mQuit = false;
        mStopped = false;
    }

private:
    void startLocked() {
        pinModule();
        mThread = std::thread([this] { threadMain(); });
    }

    // The render thread executes code from this module, so the host must never unmap it while
    // the thread may be alive (e.g. when an Aquarius program ends without calling Close).
    static void pinModule() {
#if defined(_WIN32)
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&modulePath), &module);
#else
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&modulePath), &info) && info.dli_fname)
            dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
    }

    void threadMain() {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait_for(lock, std::chrono::milliseconds(mWindow ? 8 : 100),
                        [&] { return mQuit || !mJobs.empty(); });
                if (mQuit && mJobs.empty()) break;
                if (!mJobs.empty()) {
                    job = mJobs.front();
                    mJobs.pop_front();
                }
            }
            if (job) {
                execute(*job);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    job->done = true;
                }
                mDone.notify_all();
            } else if (mWindow) {
                // Keep the window responsive while the application is busy elsewhere.
                mWindow->pump(mPendingEvents);
                if (mPendingEvents.size() > 4096) mPendingEvents.erase(mPendingEvents.begin(), mPendingEvents.begin() + 2048);
                updateLoadingAssets();
            }
        }
        shutdown();
        std::lock_guard<std::mutex> lock(mMutex);
        mStopped = true;
        for (auto& j : mJobs) {
            j->response = Json{{"ok", false}, {"error", "Filament runtime has stopped / Filament 執行環境已停止"}};
            j->done = true;
        }
        mJobs.clear();
        mDone.notify_all();
    }

    void execute(Job& job) {
        Json results = Json::array();
        const Json& cmds = job.request.contains("cmds") ? job.request["cmds"] : Json::array();
        if (!cmds.is_array()) {
            job.response = Json{{"ok", false}, {"error", "Expected {\"cmds\":[...]} / 需要指令陣列"}};
            return;
        }
        size_t index = 0;
        try {
            for (; index < cmds.size(); ++index) {
                const Json& cmd = cmds[index];
                if (!cmd.is_object()) fail("Each command must be an object / 每個指令必須是物件");
                Json r = dispatch(cmd);
                if (!r.is_null()) results.push_back(std::move(r));
            }
            job.response = Json{{"ok", true}, {"results", std::move(results)}};
        } catch (const std::exception& e) {
            std::string op = index < cmds.size() && cmds[index].is_object() ? text(cmds[index], "op", "?") : "?";
            job.response = Json{{"ok", false}, {"error", std::string(e.what())}, {"index", index}, {"op", op},
                    {"results", std::move(results)}};
        }
    }

    // --------------------------------------------------------------------------------------------
    // Dispatch
    // --------------------------------------------------------------------------------------------

    Json dispatch(const Json& c) {
        const std::string op = text(c, "op");
        if (op == "init") return cmdInit(c);
        if (op == "info") return cmdInfo();
        if (!mEngine) fail("Call Open / 開啟視窗 first (op: " + op + ")");
        if (op == "frame") return cmdFrame(c);
        if (op == "events") return cmdEvents();
        if (op == "title") { if (mWindow) mWindow->setTitle(text(c, "text")); return nullptr; }
        if (op == "close") { shutdownScene(); return nullptr; }
        if (op == "camera") return cmdCamera(c);
        if (op == "lens") return cmdLens(c);
        if (op == "exposure") return cmdExposure(c);
        if (op == "cameraControl") return cmdCameraControl(c);
        if (op == "cameraState") return cmdCameraState();
        if (op == "view") return cmdView(c);
        if (op == "environment") return cmdEnvironment(c);
        if (op == "skybox") return cmdSkybox(c);
        if (op == "light") return cmdLight(c, true);
        if (op == "setLight") return cmdLight(c, false);
        if (op == "material") return cmdMaterial(c);
        if (op == "instance") return cmdInstance(c);
        if (op == "params") return cmdParams(c);
        if (op == "texture") return cmdTexture(c);
        if (op == "shape") return cmdShape(c);
        if (op == "geometry") return cmdGeometry(c);
        if (op == "mesh") return cmdMesh(c);
        if (op == "model") return cmdModel(c);
        if (op == "modelInfo") return cmdModelInfo(c);
        if (op == "animate") return cmdAnimate(c);
        if (op == "emissive") return cmdEmissive(c);
        if (op == "group") return cmdGroup(c);
        if (op == "transform") return cmdTransform(c);
        if (op == "parent") return cmdParent(c);
        if (op == "visible") return cmdVisible(c);
        if (op == "shadows") return cmdShadows(c);
        if (op == "setMaterial") return cmdSetMaterial(c);
        if (op == "remove") { removeObject(idOf(c)); return nullptr; }
        if (op == "clear") { clearScene(flag(c, "keepEnvironment", false)); return nullptr; }
        if (op == "screenshot") return cmdScreenshot(c);
        if (op == "stats") return cmdStats();
        fail("Unknown command / 未知的指令: " + op);
    }

    // --------------------------------------------------------------------------------------------
    // Initialization
    // --------------------------------------------------------------------------------------------

    Json cmdInfo() {
        return Json{{"version", kVersion}, {"filament", kFilamentVersion},
#if defined(_WIN32)
                {"platform", "windows"},
#else
                {"platform", "linux"},
#endif
                {"open", mEngine != nullptr}};
    }

    Json cmdInit(const Json& c) {
        if (mEngine) fail("Filament window is already open / 視窗已開啟");
        mAppRoot = collapsePath(text(c, "appRoot"));
        while (!mAppRoot.empty() && mAppRoot.back() == '/') mAppRoot.pop_back();
        mResourceRoot.clear();
        std::string key = lower(normalizeSlashes(text(c, "libraryKey")));
        std::string self = normalizeSlashes(modulePath());
        if (!key.empty() && key.front() == '/') key.erase(0, 1);
        if (!key.empty() && lower(self).size() > key.size() &&
                lower(self).compare(lower(self).size() - key.size(), key.size(), key) == 0) {
            mResourceRoot = self.substr(0, self.size() - key.size());
            while (!mResourceRoot.empty() && mResourceRoot.back() == '/') mResourceRoot.pop_back();
        }
        mMaterialDir = text(c, "materials", "/bottles/Aqua_Filament/materials");

        const int width = (int)num(c, "width", 1280), height = (int)num(c, "height", 720);
        if (width < 16 || height < 16 || width > 16384 || height > 16384) fail("Invalid window size / 視窗尺寸無效");
        mHeadless = flag(c, "headless", false);
        mMaxFps = num(c, "maxFps", 0.0);
        std::string error;
        if (!mHeadless) {
            mWindow.reset(createNativeWindow());
            if (!mWindow->create(text(c, "title", "Aqua_Filament"), width, height, flag(c, "resizable", true),
                        flag(c, "visible", true), error)) {
                mWindow.reset();
                fail(error);
            }
        }
        std::string backend = lower(text(c, "backend", "default"));
        Engine::Backend b = Engine::Backend::DEFAULT;
        if (backend == "opengl" || backend == "gl") b = Engine::Backend::OPENGL;
        else if (backend == "vulkan") b = Engine::Backend::VULKAN;
        else if (backend != "default") fail("Unknown backend / 未知的後端: " + backend);
        // OpenGL is the default everywhere: it needs no extra runtime (vulkan-1.dll / libvulkan) and is the
        // backend validated by the showcase tests. {"backend":"vulkan"} opts into Vulkan.
        if (b == Engine::Backend::DEFAULT) b = Engine::Backend::OPENGL;
        mEngine = Engine::Builder().backend(b).build();
        if (!mEngine) {
            mWindow.reset();
            fail("Filament engine creation failed (GPU driver?) / Filament 引擎建立失敗");
        }
        if (mHeadless) {
            mSwapChain = mEngine->createSwapChain((uint32_t)width, (uint32_t)height, SwapChain::CONFIG_READABLE);
            mWidth = width;
            mHeight = height;
        } else {
            mSwapChain = mEngine->createSwapChain(mWindow->nativeHandle(), SwapChain::CONFIG_READABLE);
            mWindow->size(mWidth, mHeight);
        }
        mRenderer = mEngine->createRenderer();
        mScene = mEngine->createScene();
        mView = mEngine->createView();
        mCameraEntity = EntityManager::get().create();
        mCamera = mEngine->createCamera(mCameraEntity);
        mView->setScene(mScene);
        mView->setCamera(mCamera);
        mCamera->setExposure(16.0f, 1.0f / 125.0f, 100.0f);
        mFov = num(c, "fov", 45.0);
        mNear = num(c, "near", 0.05);
        mFar = num(c, "far", 1000.0);
        mEye = {0, 1.5f, 5};
        mTarget = {0, 0.5f, 0};
        mUp = {0, 1, 0};
        applyViewport();
        mView->setAntiAliasing(View::AntiAliasing::FXAA);
        mView->setShadowType(View::ShadowType::PCF);
        Renderer::ClearOptions clear;
        clear.clearColor = {0.03, 0.04, 0.06, 1.0};
        clear.clear = true;
        clear.discard = true;
        mRenderer->setClearOptions(clear);
        mSkybox = Skybox::Builder().color({0.03f, 0.04f, 0.06f, 1.0f}).build(*mEngine);
        mScene->setSkybox(mSkybox);

        // Neutral textures bound to unused optional samplers of the built-in materials.
        mWhite = solidTexture(255, 255, 255, 255, true);
        mFlatNormal = solidTexture(128, 128, 255, 255, false);
        mDefaultOrm = solidTexture(255, 255, 255, 255, false);

        // glTF support (prebuilt ubershader archive + KTX2/PNG/JPEG decoders).
        mNames = new utils::NameComponentManager(EntityManager::get());
        mMaterialProvider = gltfio::createUbershaderProvider(mEngine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
        gltfio::AssetConfiguration config{};
        config.engine = mEngine;
        config.materials = mMaterialProvider;
        config.names = mNames;
        mAssetLoader = gltfio::AssetLoader::create(config);
        gltfio::ResourceConfiguration rc{};
        rc.engine = mEngine;
        rc.normalizeSkinningWeights = true;
        mResourceLoader = new gltfio::ResourceLoader(rc);
        mStbDecoder = gltfio::createStbProvider(mEngine);
        mKtxDecoder = gltfio::createKtx2Provider(mEngine);
        mResourceLoader->addTextureProvider("image/png", mStbDecoder);
        mResourceLoader->addTextureProvider("image/jpeg", mStbDecoder);
        mResourceLoader->addTextureProvider("image/ktx2", mKtxDecoder);

        mStart = std::chrono::steady_clock::now();
        mLastFrame = mStart;
        mFpsWindow = mStart;
        const char* names[] = {"noop", "opengl", "vulkan", "metal", "webgpu"};
        int backendIndex = (int)mEngine->getBackend();
        return Json{{"backend", backendIndex >= 0 && backendIndex < 5 ? names[backendIndex] : "unknown"},
                {"width", mWidth}, {"height", mHeight}, {"resourceRoot", mResourceRoot},
                {"version", kVersion}, {"filament", kFilamentVersion}};
    }

    Texture* solidTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb) {
        Texture* t = Texture::Builder().width(1).height(1).levels(1)
                .format(srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
                .sampler(Texture::Sampler::SAMPLER_2D).build(*mEngine);
        auto* px = new uint8_t[4]{r, g, b, a};
        t->setImage(*mEngine, 0, Texture::PixelBufferDescriptor(px, 4, Texture::Format::RGBA, Texture::Type::UBYTE,
                [](void* p, size_t, void*) { delete[] static_cast<uint8_t*>(p); }));
        return t;
    }

    void applyViewport() {
        mView->setViewport({0, 0, (uint32_t)std::max(1, mWidth), (uint32_t)std::max(1, mHeight)});
        double aspect = double(std::max(1, mWidth)) / double(std::max(1, mHeight));
        mCamera->setProjection(mFov, aspect, mNear, mFar,
                mFovHorizontal ? Camera::Fov::HORIZONTAL : Camera::Fov::VERTICAL);
        if (mManipulator) mManipulator->setViewport(mWidth, mHeight);
    }

    // --------------------------------------------------------------------------------------------
    // Path resolution
    // --------------------------------------------------------------------------------------------

    std::string resolve(const std::string& raw) {
        if (raw.empty()) fail("Empty path / 路徑為空");
        std::string p = collapsePath(raw);
        std::vector<std::string> candidates;
        std::string relative;
        if (!mAppRoot.empty() && lower(p).rfind(lower(mAppRoot) + "/", 0) == 0) relative = p.substr(mAppRoot.size() + 1);
        else if (p.rfind("/bottles/", 0) == 0) relative = p.substr(1);
        else if (!(p.size() > 1 && (p[0] == '/' || p[1] == ':'))) relative = p;
        if (!relative.empty() && !mResourceRoot.empty()) candidates.push_back(mResourceRoot + "/" + relative);
        if (!relative.empty() && !mAppRoot.empty()) candidates.push_back(mAppRoot + "/" + relative);
        candidates.push_back(p);
        for (auto& candidate : candidates) {
            if (fileExists(candidate)) return candidate;
        }
        fail("File not found / 找不到檔案: " + raw);
    }

    // --------------------------------------------------------------------------------------------
    // Frame loop
    // --------------------------------------------------------------------------------------------

    Json cmdEvents() {
        if (mWindow) mWindow->pump(mPendingEvents);
        Json events = Json::array();
        for (auto& e : mPendingEvents) events.push_back(std::move(e));
        mPendingEvents.clear();
        return Json{{"events", std::move(events)}};
    }

    Json cmdFrame(const Json& c) {
        if (mWindow) mWindow->pump(mPendingEvents);
        Json events = Json::array();
        for (auto& e : mPendingEvents) {
            handleInput(e);
            events.push_back(std::move(e));
        }
        mPendingEvents.clear();
        if (mWindow) {
            int w = 0, h = 0;
            mWindow->size(w, h);
            if (w > 0 && h > 0 && (w != mWidth || h != mHeight)) {
                mWidth = w;
                mHeight = h;
                applyViewport();
            }
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - mLastFrame).count();
        mLastFrame = now;
        if (mManipulator) {
            mManipulator->update((float)std::min(dt, 0.1));
            float3 eye, target, up;
            mManipulator->getLookAt(&eye, &target, &up);
            mEye = eye; mTarget = target; mUp = up;
        }
        mCamera->lookAt(double3(mEye), double3(mTarget), double3(mUp));
        updateLoadingAssets();
        bool rendered = false;
        if (mRenderer->beginFrame(mSwapChain)) {
            mRenderer->render(mView);
            if (!mScreenshots.empty()) captureScreenshots();
            mRenderer->endFrame();
            rendered = true;
        }
        if (!mScreenshots.empty() && !rendered) {
            // A skipped frame cannot be read back: force one more frame for pending screenshots.
            mEngine->flushAndWait();
            if (mRenderer->beginFrame(mSwapChain)) {
                mRenderer->render(mView);
                captureScreenshots();
                mRenderer->endFrame();
            }
        }
        if (flag(c, "wait", false) || mHeadless) mEngine->flushAndWait();
        mFrames++;
        mFpsFrames++;
        double windowSeconds = std::chrono::duration<double>(now - mFpsWindow).count();
        if (windowSeconds >= 0.5) {
            mFps = mFpsFrames / windowSeconds;
            mFpsFrames = 0;
            mFpsWindow = now;
        }
        if (mMaxFps > 0) {
            auto budget = std::chrono::duration<double>(1.0 / mMaxFps);
            auto elapsed = std::chrono::steady_clock::now() - now;
            if (elapsed < budget) std::this_thread::sleep_for(budget - elapsed);
        }
        return Json{{"events", std::move(events)}, {"width", mWidth}, {"height", mHeight},
                {"time", std::chrono::duration<double>(now - mStart).count()}, {"dt", dt},
                {"frame", mFrames}, {"fps", mFps}, {"rendered", rendered},
                {"open", !(mWindow && mWindow->closeRequested())}, {"loading", mLoadingCount}};
    }

    void handleInput(const Json& e) {
        if (!mManipulator) return;
        const std::string type = text(e, "type");
        if (type == "mouse") {
            const std::string action = text(e, "action");
            int x = (int)num(e, "x", 0), y = mHeight - (int)num(e, "y", 0); // camutils uses bottom-left origin
            if (action == "down") {
                int button = (int)num(e, "button", 0);
                mManipulator->grabBegin(x, y, button != 0);
                mGrabbing = true;
            } else if (action == "move" && mGrabbing) {
                mManipulator->grabUpdate(x, y);
            } else if (action == "up" && mGrabbing) {
                mManipulator->grabEnd();
                mGrabbing = false;
            } else if (action == "wheel") {
                mManipulator->scroll(x, y, (float)(-num(e, "delta", 0) * mWheelScale));
            }
        } else if (type == "key") {
            const std::string key = text(e, "key");
            bool down = flag(e, "down", false);
            using Key = camutils::Manipulator<float>::Key;
            Key k = Key::COUNT;
            if (key == "w" || key == "up") k = Key::FORWARD;
            else if (key == "s" || key == "down") k = Key::BACKWARD;
            else if (key == "a" || key == "left") k = Key::LEFT;
            else if (key == "d" || key == "right") k = Key::RIGHT;
            else if (key == "e") k = Key::UP;
            else if (key == "q") k = Key::DOWN;
            if (k != Key::COUNT) {
                if (down) mManipulator->keyDown(k); else mManipulator->keyUp(k);
            }
        } else if (type == "focus" && !flag(e, "focused", true) && mGrabbing) {
            mManipulator->grabEnd();
            mGrabbing = false;
        }
    }

    // --------------------------------------------------------------------------------------------
    // Camera & view
    // --------------------------------------------------------------------------------------------

    Json cmdCamera(const Json& c) {
        mEye = vec3(c, "eye", mEye);
        mTarget = vec3(c, "target", mTarget);
        mUp = vec3(c, "up", mUp);
        if (mManipulator && flag(c, "syncControl", true)) rebuildManipulator();
        return nullptr;
    }

    Json cmdLens(const Json& c) {
        mFov = num(c, "fov", mFov);
        mNear = num(c, "near", mNear);
        mFar = num(c, "far", mFar);
        if (has(c, "fovDirection")) mFovHorizontal = lower(text(c, "fovDirection")) == "horizontal";
        if (mFov <= 0 || mFov >= 180 || mNear <= 0 || mFar <= mNear) fail("Invalid lens / 鏡頭參數無效");
        applyViewport();
        return nullptr;
    }

    Json cmdExposure(const Json& c) {
        if (has(c, "ev100")) {
            // EV100 = log2(N^2 / t * 100 / S) with N=1, S=100 -> t = 2^-EV100
            double ev = num(c, "ev100", 15);
            mCamera->setExposure(1.0f, (float)std::pow(2.0, -ev), 100.0f);
        } else {
            mCamera->setExposure((float)num(c, "aperture", 16), (float)num(c, "shutter", 1.0 / 125.0),
                    (float)num(c, "iso", 100));
        }
        return nullptr;
    }

    Json cmdCameraControl(const Json& c) {
        std::string mode = lower(text(c, "mode", "orbit"));
        mControl = c;
        if (mode == "none" || mode == "off") {
            delete mManipulator;
            mManipulator = nullptr;
            return nullptr;
        }
        if (mode != "orbit" && mode != "map" && mode != "fly" && mode != "free") fail("Unknown camera control / 未知的相機控制模式: " + mode);
        if (has(c, "eye")) mEye = vec3(c, "eye", mEye);
        if (has(c, "target")) mTarget = vec3(c, "target", mTarget);
        mWheelScale = num(c, "wheelScale", 1.0);
        rebuildManipulator();
        return nullptr;
    }

    void rebuildManipulator() {
        using M = camutils::Manipulator<float>;
        std::string mode = lower(text(mControl, "mode", "orbit"));
        M::Builder b;
        b.viewport(std::max(1, mWidth), std::max(1, mHeight))
                .targetPosition(mTarget.x, mTarget.y, mTarget.z)
                .upVector(mUp.x, mUp.y, mUp.z)
                .orbitHomePosition(mEye.x, mEye.y, mEye.z)
                .zoomSpeed((float)num(mControl, "zoomSpeed", 0.05))
                .fovDegrees((float)mFov)
                .farPlane((float)mFar);
        double orbit = num(mControl, "orbitSpeed", 0.01);
        b.orbitSpeed((float)orbit, (float)orbit);
        delete mManipulator;
        if (mode == "fly" || mode == "free") {
            float3 d = normalize(mTarget - mEye);
            float pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
            float yaw = std::atan2(-d.x, -d.z);
            b.flightStartPosition(mEye.x, mEye.y, mEye.z)
                    .flightStartOrientation(pitch, yaw)
                    .flightMaxMoveSpeed((float)num(mControl, "speed", 6.0))
                    .flightMoveDamping((float)num(mControl, "damping", 15.0));
            double pan = num(mControl, "panSpeed", 0.004);
            b.flightPanSpeed((float)pan, (float)pan);
            mManipulator = b.build(camutils::Mode::FREE_FLIGHT);
        } else if (mode == "map") {
            mManipulator = b.build(camutils::Mode::MAP);
        } else {
            mManipulator = b.build(camutils::Mode::ORBIT);
        }
    }

    Json cmdCameraState() {
        return Json{{"eye", arr3(mEye)}, {"target", arr3(mTarget)}, {"up", arr3(mUp)}, {"fov", mFov},
                {"near", mNear}, {"far", mFar}, {"width", mWidth}, {"height", mHeight}};
    }

    static View::QualityLevel quality(const std::string& q) {
        auto s = lower(q);
        if (s == "low") return View::QualityLevel::LOW;
        if (s == "high") return View::QualityLevel::HIGH;
        if (s == "ultra") return View::QualityLevel::ULTRA;
        return View::QualityLevel::MEDIUM;
    }

    Json cmdView(const Json& c) {
        if (has(c, "postProcessing")) mView->setPostProcessingEnabled(flag(c, "postProcessing", true));
        if (has(c, "antiAliasing")) {
            auto aa = lower(text(c, "antiAliasing"));
            mView->setAntiAliasing(aa == "none" || aa == "off" ? View::AntiAliasing::NONE : View::AntiAliasing::FXAA);
        }
        if (has(c, "msaa")) {
            View::MultiSampleAntiAliasingOptions o;
            int samples = (int)num(c, "msaa", 0);
            o.enabled = samples > 1;
            o.sampleCount = (uint8_t)std::clamp(samples, 1, 8);
            mView->setMultiSampleAntiAliasingOptions(o);
        }
        if (has(c, "taa")) {
            View::TemporalAntiAliasingOptions o;
            const Json& t = c["taa"];
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "taa", false);
            if (t.is_object()) {
                o.feedback = (float)num(t, "feedback", o.feedback);
                o.filterWidth = (float)num(t, "filterWidth", o.filterWidth);
                o.sharpness = (float)num(t, "sharpness", o.sharpness);
                o.upscaling = (float)num(t, "upscaling", o.upscaling);
            }
            mView->setTemporalAntiAliasingOptions(o);
        }
        if (has(c, "bloom")) {
            const Json& t = c["bloom"];
            View::BloomOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "bloom", false);
            if (t.is_object()) {
                o.strength = (float)num(t, "strength", o.strength);
                o.levels = (uint8_t)num(t, "levels", o.levels);
                o.threshold = flag(t, "threshold", o.threshold);
                o.highlight = (float)num(t, "highlight", o.highlight);
                o.lensFlare = flag(t, "lensFlare", o.lensFlare);
                o.starburst = flag(t, "starburst", o.starburst);
                o.quality = quality(text(t, "quality", "medium"));
                if (lower(text(t, "blendMode", "add")) == "interpolate") o.blendMode = View::BloomOptions::BlendMode::INTERPOLATE;
            }
            mView->setBloomOptions(o);
        }
        if (has(c, "ssao")) {
            const Json& t = c["ssao"];
            View::AmbientOcclusionOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "ssao", false);
            if (t.is_object()) {
                o.radius = (float)num(t, "radius", o.radius);
                o.intensity = (float)num(t, "intensity", o.intensity);
                o.power = (float)num(t, "power", o.power);
                o.quality = quality(text(t, "quality", "low"));
                o.bentNormals = flag(t, "bentNormals", false);
                if (has(t, "contactShadows")) {
                    o.ssct.enabled = flag(t, "contactShadows", false);
                }
            }
            mView->setAmbientOcclusionOptions(o);
        }
        if (has(c, "ssr")) {
            const Json& t = c["ssr"];
            View::ScreenSpaceReflectionsOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "ssr", false);
            if (t.is_object()) {
                o.thickness = (float)num(t, "thickness", o.thickness);
                o.maxDistance = (float)num(t, "maxDistance", o.maxDistance);
                o.bias = (float)num(t, "bias", o.bias);
                o.stride = (float)num(t, "stride", o.stride);
            }
            mView->setScreenSpaceReflectionsOptions(o);
        }
        if (has(c, "dof")) {
            const Json& t = c["dof"];
            View::DepthOfFieldOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "dof", false);
            if (t.is_object()) {
                o.cocScale = (float)num(t, "cocScale", o.cocScale);
                o.maxApertureDiameter = (float)num(t, "maxAperture", o.maxApertureDiameter);
                o.nativeResolution = flag(t, "nativeResolution", false);
                if (has(t, "focusDistance")) mFocusDistance = (float)num(t, "focusDistance", 1.0);
            }
            mView->setDepthOfFieldOptions(o);
            if (mFocusDistance > 0) mCamera->setFocusDistance(mFocusDistance);
        }
        if (has(c, "fog")) {
            const Json& t = c["fog"];
            View::FogOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "fog", false);
            if (t.is_object()) {
                o.distance = (float)num(t, "distance", o.distance);
                o.density = (float)num(t, "density", o.density);
                o.height = (float)num(t, "height", o.height);
                o.heightFalloff = (float)num(t, "heightFalloff", o.heightFalloff);
                o.maximumOpacity = (float)num(t, "maximumOpacity", o.maximumOpacity);
                float3 col = vec3(t, "color", {o.color.r, o.color.g, o.color.b});
                o.color = {col.x, col.y, col.z};
                o.fogColorFromIbl = flag(t, "fromIbl", o.fogColorFromIbl);
                o.inScatteringSize = (float)num(t, "inScatteringSize", o.inScatteringSize);
                o.inScatteringStart = (float)num(t, "inScatteringStart", o.inScatteringStart);
            }
            mView->setFogOptions(o);
        }
        if (has(c, "vignette")) {
            const Json& t = c["vignette"];
            View::VignetteOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "vignette", false);
            if (t.is_object()) {
                o.midPoint = (float)num(t, "midPoint", o.midPoint);
                o.roundness = (float)num(t, "roundness", o.roundness);
                o.feather = (float)num(t, "feather", o.feather);
            }
            mView->setVignetteOptions(o);
        }
        if (has(c, "shadowType")) {
            auto s = lower(text(c, "shadowType"));
            mView->setShadowType(s == "vsm" ? View::ShadowType::VSM : s == "dpcf" ? View::ShadowType::DPCF
                    : s == "pcss" ? View::ShadowType::PCSS : s == "pcfd" ? View::ShadowType::PCFd : View::ShadowType::PCF);
            if (s == "vsm") {
                View::VsmShadowOptions o;
                o.anisotropy = 0;
                o.mipmapping = false;
                o.highPrecision = false;
                o.lightBleedReduction = 0.2f;
                mView->setVsmShadowOptions(o);
            }
        }
        if (has(c, "softShadows")) {
            const Json& t = c["softShadows"];
            View::SoftShadowOptions o;
            if (t.is_object()) {
                o.penumbraScale = (float)num(t, "penumbraScale", o.penumbraScale);
                o.penumbraRatioScale = (float)num(t, "penumbraRatioScale", o.penumbraRatioScale);
            }
            mView->setSoftShadowOptions(o);
        }
        if (has(c, "shadows")) mView->setShadowingEnabled(flag(c, "shadows", true));
        if (has(c, "screenSpaceRefraction")) mView->setScreenSpaceRefractionEnabled(flag(c, "screenSpaceRefraction", true));
        if (has(c, "dynamicResolution")) {
            const Json& t = c["dynamicResolution"];
            View::DynamicResolutionOptions o;
            o.enabled = t.is_object() ? flag(t, "enabled", true) : flag(c, "dynamicResolution", false);
            if (t.is_object()) {
                o.minScale = {(float)num(t, "minScale", 0.5), (float)num(t, "minScale", 0.5)};
                o.maxScale = {(float)num(t, "maxScale", 1.0), (float)num(t, "maxScale", 1.0)};
                o.quality = quality(text(t, "quality", "medium"));
            }
            mView->setDynamicResolutionOptions(o);
        }
        if (has(c, "renderQuality")) {
            View::RenderQuality q;
            q.hdrColorBuffer = quality(text(c, "renderQuality", "high"));
            mView->setRenderQuality(q);
        }
        if (has(c, "toneMapper") || has(c, "exposureBias") || has(c, "contrast") || has(c, "saturation") ||
                has(c, "vibrance") || has(c, "whiteBalance")) {
            if (has(c, "toneMapper")) mToneMapper = lower(text(c, "toneMapper"));
            if (has(c, "exposureBias")) mGradeExposure = (float)num(c, "exposureBias", 0);
            if (has(c, "contrast")) mGradeContrast = (float)num(c, "contrast", 1);
            if (has(c, "saturation")) mGradeSaturation = (float)num(c, "saturation", 1);
            if (has(c, "vibrance")) mGradeVibrance = (float)num(c, "vibrance", 1);
            if (has(c, "whiteBalance")) mGradeTemperature = (float)num(c, "whiteBalance", 0);
            rebuildColorGrading();
        }
        if (has(c, "clearColor")) {
            float4 col = vec4(c, "clearColor", {0, 0, 0, 1});
            Renderer::ClearOptions clear;
            clear.clearColor = {col.x, col.y, col.z, col.w};
            clear.clear = true;
            clear.discard = true;
            mRenderer->setClearOptions(clear);
        }
        return nullptr;
    }

    void rebuildColorGrading() {
        ToneMapper* mapper = nullptr;
        if (mToneMapper == "linear") mapper = new LinearToneMapper();
        else if (mToneMapper == "aces") mapper = new ACESToneMapper();
        else if (mToneMapper == "aces_legacy" || mToneMapper == "acesLegacy") mapper = new ACESLegacyToneMapper();
        else if (mToneMapper == "filmic") mapper = new FilmicToneMapper();
        else if (mToneMapper == "pbrneutral" || mToneMapper == "neutral") mapper = new PBRNeutralToneMapper();
        else mapper = new AgxToneMapper();
        ColorGrading* grading = ColorGrading::Builder()
                .toneMapper(mapper)
                .exposure(mGradeExposure)
                .contrast(mGradeContrast)
                .saturation(mGradeSaturation)
                .vibrance(mGradeVibrance)
                .whiteBalance(mGradeTemperature, 0.0f)
                .build(*mEngine);
        delete mapper;
        mView->setColorGrading(grading);
        if (mColorGrading) mEngine->destroy(mColorGrading);
        mColorGrading = grading;
    }

    // --------------------------------------------------------------------------------------------
    // Environment
    // --------------------------------------------------------------------------------------------

    Texture* loadKtx1(const std::string& path, float3* sh) {
        auto data = readFile(resolve(path));
        auto* bundle = new image::Ktx1Bundle(data.data(), (uint32_t)data.size());
        if (sh && !bundle->getSphericalHarmonics(sh)) sh[0] = float3(-1.0f);
        Texture* t = ktxreader::Ktx1Reader::createTexture(mEngine, bundle, false);
        if (!t) fail("Invalid KTX texture / 無效的 KTX 紋理: " + path);
        return t;
    }

    void destroyEnvironment() {
        if (mIndirectLight) { mScene->setIndirectLight(nullptr); mEngine->destroy(mIndirectLight); mIndirectLight = nullptr; }
        if (mIblTexture) { mEngine->destroy(mIblTexture); mIblTexture = nullptr; }
        if (mSkyTexture) { mEngine->destroy(mSkyTexture); mSkyTexture = nullptr; }
    }

    Json cmdEnvironment(const Json& c) {
        float intensity = (float)num(c, "intensity", 30000.0);
        if (has(c, "ibl")) {
            std::string ibl = text(c, "ibl");
            float3 sh[9];
            sh[0] = float3(-1.0f);
            destroyEnvironment();
            if (extensionOf(ibl) == "hdr") {
                fail("Equirectangular .hdr environments must be converted with cmgen to .ktx / 請先以 cmgen 轉換 .hdr 為 .ktx");
            }
            mIblTexture = loadKtx1(ibl, sh);
            IndirectLight::Builder builder;
            builder.reflections(mIblTexture).intensity(intensity);
            if (sh[0].x != -1.0f) builder.irradiance(3, sh);
            mIndirectLight = builder.build(*mEngine);
            mScene->setIndirectLight(mIndirectLight);
            if (has(c, "skybox")) {
                mSkyTexture = loadKtx1(text(c, "skybox"), nullptr);
                Skybox* sky = Skybox::Builder().environment(mSkyTexture).showSun(flag(c, "showSun", false)).build(*mEngine);
                replaceSkybox(sky);
            }
        } else if (mIndirectLight) {
            mIndirectLight->setIntensity(intensity);
        }
        if (mIndirectLight && has(c, "rotation")) {
            float angle = (float)(num(c, "rotation", 0) * kPi / 180.0);
            mIndirectLight->setRotation(mat3f::rotation(angle, float3{0, 1, 0}));
            mEnvRotation = angle;
        }
        if (has(c, "enabled") && !flag(c, "enabled", true)) {
            mScene->setIndirectLight(nullptr);
        } else if (mIndirectLight) {
            mScene->setIndirectLight(mIndirectLight);
        }
        return nullptr;
    }

    void replaceSkybox(Skybox* sky) {
        mScene->setSkybox(sky);
        if (mSkybox) mEngine->destroy(mSkybox);
        mSkybox = sky;
    }

    Json cmdSkybox(const Json& c) {
        if (has(c, "color")) {
            float4 col = vec4(c, "color", {0, 0, 0, 1});
            replaceSkybox(Skybox::Builder().color(col).build(*mEngine));
        } else if (has(c, "path")) {
            if (mSkyTexture) { mEngine->destroy(mSkyTexture); mSkyTexture = nullptr; }
            mSkyTexture = loadKtx1(text(c, "path"), nullptr);
            replaceSkybox(Skybox::Builder().environment(mSkyTexture).showSun(flag(c, "showSun", false))
                    .intensity((float)num(c, "intensity", 30000.0)).build(*mEngine));
        } else if (has(c, "visible")) {
            mScene->setSkybox(flag(c, "visible", true) ? mSkybox : nullptr);
        }
        return nullptr;
    }

    // --------------------------------------------------------------------------------------------
    // Object registry
    // --------------------------------------------------------------------------------------------

    Object& create(int64_t id, Kind kind) {
        if (mObjects.count(id)) fail("Object id already in use / 物件代碼已使用: " + std::to_string(id));
        Object& o = mObjects[id];
        o.kind = kind;
        return o;
    }

    Object& get(int64_t id) {
        auto it = mObjects.find(id);
        if (it == mObjects.end()) fail("Unknown object id / 未知的物件代碼: " + std::to_string(id));
        return it->second;
    }

    Object& getEntity(int64_t id) {
        Object& o = get(id);
        if (o.kind == Kind::Material || o.kind == Kind::MaterialInstance || o.kind == Kind::Texture)
            fail("Object is not a scene entity / 物件不是場景實體: " + std::to_string(id));
        return o;
    }

    MaterialInstance* instanceOf(const Json& c, const char* key = "material") {
        if (!has(c, key)) return defaultInstance();
        Object& o = get(c.at(key).get<int64_t>());
        if (o.kind != Kind::MaterialInstance) fail("Expected a material instance / 需要材質實例");
        return o.instance;
    }

    Entity entityOf(Object& o) {
        if (o.kind == Kind::Model) return o.asset->getRoot();
        return o.entity;
    }

    void removeObject(int64_t id) {
        auto it = mObjects.find(id);
        if (it == mObjects.end()) return;
        Object& o = it->second;
        auto& em = EntityManager::get();
        switch (o.kind) {
            case Kind::Model:
                mScene->removeEntities(o.asset->getEntities(), o.asset->getEntityCount());
                if (o.loading) { mResourceLoader->asyncCancelLoad(); mLoadingCount = std::max(0, mLoadingCount - 1); }
                mAssetLoader->destroyAsset(o.asset);
                break;
            case Kind::Material:
                for (auto& [k, other] : mObjects) {
                    if (other.kind == Kind::MaterialInstance && other.material == o.material) {
                        fail("Material still has instances / 材質仍有實例，請先移除實例");
                    }
                }
                if (o.ownsMaterial) mEngine->destroy(o.material);
                break;
            case Kind::MaterialInstance:
                mEngine->destroy(o.instance);
                break;
            case Kind::Texture:
                mEngine->destroy(o.texture);
                break;
            default:
                mScene->remove(o.entity);
                mEngine->destroy(o.entity);
                if (o.source) mEngine->flushAndWait(); // uploads must finish before the bytes go away
                if (o.vb) mEngine->destroy(o.vb);
                if (o.ib) mEngine->destroy(o.ib);
                em.destroy(o.entity);
                break;
        }
        mObjects.erase(it);
    }

    void clearScene(bool keepEnvironment) {
        // Entities first, then instances, materials and textures.
        std::vector<int64_t> order[4];
        for (auto& [id, o] : mObjects) {
            int rank = o.kind == Kind::MaterialInstance ? 1 : o.kind == Kind::Material ? 2 : o.kind == Kind::Texture ? 3 : 0;
            order[rank].push_back(id);
        }
        for (auto& ids : order) for (auto id : ids) removeObject(id);
        if (!keepEnvironment) {
            destroyEnvironment();
            replaceSkybox(Skybox::Builder().color({0.03f, 0.04f, 0.06f, 1.0f}).build(*mEngine));
        }
    }

    // --------------------------------------------------------------------------------------------
    // Lights
    // --------------------------------------------------------------------------------------------

    Json cmdLight(const Json& c, bool creating) {
        int64_t id = idOf(c);
        auto& lm = mEngine->getLightManager();
        if (creating) {
            std::string type = lower(text(c, "type", "point"));
            LightManager::Type t = LightManager::Type::POINT;
            if (type == "sun") t = LightManager::Type::SUN;
            else if (type == "directional") t = LightManager::Type::DIRECTIONAL;
            else if (type == "spot") t = LightManager::Type::FOCUSED_SPOT;
            else if (type == "softspot" || type == "spot_soft") t = LightManager::Type::SPOT;
            else if (type != "point") fail("Unknown light type / 未知的光源類型: " + type);
            Object& o = create(id, Kind::Light);
            o.entity = EntityManager::get().create();
            LightManager::Builder b(t);
            float3 color = vec3(c, "color", {1, 1, 1});
            b.color(LinearColor{color.x, color.y, color.z});
            bool directional = t == LightManager::Type::SUN || t == LightManager::Type::DIRECTIONAL;
            float intensity = (float)num(c, "intensity", directional ? 100000.0 : 10000.0);
            if (lower(text(c, "unit", directional ? "lux" : "lumen")) == "candela" && !directional) b.intensityCandela(intensity);
            else b.intensity(intensity);
            b.direction(normalize(vec3(c, "direction", {0, -1, 0})));
            b.position(vec3(c, "position", {0, 0, 0}));
            b.falloff((float)num(c, "falloff", 10.0));
            if (t == LightManager::Type::FOCUSED_SPOT || t == LightManager::Type::SPOT) {
                float inner = (float)(num(c, "inner", 25.0) * kPi / 180.0);
                float outer = (float)(num(c, "outer", 35.0) * kPi / 180.0);
                b.spotLightCone(std::min(inner, outer), outer);
            }
            if (t == LightManager::Type::SUN) {
                b.sunAngularRadius((float)num(c, "sunAngularRadius", 1.9));
                b.sunHaloSize((float)num(c, "sunHaloSize", 10.0));
                b.sunHaloFalloff((float)num(c, "sunHaloFalloff", 80.0));
            }
            bool shadows = flag(c, "castShadows", false);
            b.castShadows(shadows);
            if (shadows || has(c, "shadow")) b.shadowOptions(shadowOptions(c.contains("shadow") ? c["shadow"] : Json::object(), directional));
            b.lightChannel(0, true);
            b.build(*mEngine, o.entity);
            mScene->addEntity(o.entity);
            return nullptr;
        }
        Object& o = get(id);
        if (o.kind != Kind::Light) fail("Object is not a light / 物件不是光源");
        auto li = lm.getInstance(o.entity);
        if (has(c, "color")) { float3 col = vec3(c, "color", {1, 1, 1}); lm.setColor(li, {col.x, col.y, col.z}); }
        if (has(c, "intensity")) {
            float v = (float)num(c, "intensity", 1);
            if (lower(text(c, "unit", "")) == "candela") lm.setIntensityCandela(li, v); else lm.setIntensity(li, v);
        }
        if (has(c, "position")) lm.setPosition(li, vec3(c, "position", {}));
        if (has(c, "direction")) lm.setDirection(li, normalize(vec3(c, "direction", {0, -1, 0})));
        if (has(c, "falloff")) lm.setFalloff(li, (float)num(c, "falloff", 10));
        if (has(c, "inner") || has(c, "outer")) {
            float inner = (float)(num(c, "inner", 25.0) * kPi / 180.0);
            float outer = (float)(num(c, "outer", 35.0) * kPi / 180.0);
            lm.setSpotLightCone(li, std::min(inner, outer), outer);
        }
        if (has(c, "castShadows")) lm.setShadowCaster(li, flag(c, "castShadows", false));
        if (has(c, "enabled")) {
            bool on = flag(c, "enabled", true);
            if (on) mScene->addEntity(o.entity); else mScene->remove(o.entity);
        }
        return nullptr;
    }

    LightManager::ShadowOptions shadowOptions(const Json& s, bool directional) {
        LightManager::ShadowOptions o;
        o.mapSize = (uint32_t)num(s, "mapSize", directional ? 2048 : 1024);
        o.shadowCascades = (uint8_t)std::clamp((int)num(s, "cascades", directional ? 3 : 1), 1, 4);
        o.constantBias = (float)num(s, "constantBias", o.constantBias);
        o.normalBias = (float)num(s, "normalBias", o.normalBias);
        o.shadowFar = (float)num(s, "far", 0);
        o.stable = flag(s, "stable", false);
        o.lispsm = flag(s, "lispsm", true);
        o.screenSpaceContactShadows = flag(s, "contactShadows", false);
        o.vsm.blurWidth = (float)num(s, "blur", 0);
        o.shadowBulbRadius = (float)num(s, "bulbRadius", o.shadowBulbRadius);
        return o;
    }

    // --------------------------------------------------------------------------------------------
    // Materials and textures
    // --------------------------------------------------------------------------------------------

    Material* builtinMaterial(const std::string& name) {
        auto it = mBuiltins.find(name);
        if (it != mBuiltins.end()) return it->second;
        static const char* known[] = {"lit", "lit_transparent", "refraction", "shadow", "unlit"};
        if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return name == k; }) == std::end(known))
            fail("Unknown built-in material / 未知的內建材質: " + name);
        auto data = readFile(resolve(mMaterialDir + "/" + name + ".filamat"));
        Material* m = Material::Builder().package(data.data(), data.size()).build(*mEngine);
        if (!m) fail("Invalid material package / 無效的材質檔: " + name);
        mBuiltins[name] = m;
        return m;
    }

    MaterialInstance* defaultInstance() {
        if (!mDefaultInstance) {
            mDefaultInstance = builtinMaterial("lit")->createInstance();
            applyDefaults(mDefaultInstance);
        }
        return mDefaultInstance;
    }

    TextureSampler defaultSampler() {
        TextureSampler s(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR, TextureSampler::MagFilter::LINEAR,
                TextureSampler::WrapMode::REPEAT);
        s.setAnisotropy(8.0f);
        return s;
    }

    static bool paramInfo(const Material* m, const std::string& name, Material::ParameterInfo& out) {
        std::vector<Material::ParameterInfo> infos(m->getParameterCount());
        m->getParameters(infos.data(), infos.size());
        for (auto& p : infos) {
            if (name == p.name) { out = p; return true; }
        }
        return false;
    }

    // Type-checked uniform assignment: Filament aborts on mismatched parameter types.
    static void setUniform(MaterialInstance* mi, const std::string& name, const std::vector<double>& v) {
        Material::ParameterInfo info{};
        if (!paramInfo(mi->getMaterial(), name, info)) fail("Unknown material parameter / 未知的材質參數: " + name);
        if (info.isSampler || info.isSubpass) fail("Parameter needs a texture / 參數需要紋理: " + name);
        if (info.count > 1) fail("Array parameters are not supported / 不支援陣列參數: " + name);
        auto at = [&](size_t i, double d) { return float(i < v.size() ? v[i] : d); };
        using T = backend::UniformType;
        switch (info.type) {
            case T::FLOAT: mi->setParameter(name.c_str(), at(0, 0)); break;
            case T::FLOAT2: mi->setParameter(name.c_str(), float2{at(0, 0), at(1, 0)}); break;
            case T::FLOAT3: mi->setParameter(name.c_str(), float3{at(0, 0), at(1, 0), at(2, 0)}); break;
            case T::FLOAT4: mi->setParameter(name.c_str(), float4{at(0, 0), at(1, 0), at(2, 0), at(3, 1)}); break;
            case T::INT: mi->setParameter(name.c_str(), (int32_t)at(0, 0)); break;
            case T::UINT: mi->setParameter(name.c_str(), (uint32_t)std::max(0.0f, at(0, 0))); break;
            case T::BOOL: mi->setParameter(name.c_str(), at(0, 0) != 0.0f); break;
            default: fail("Unsupported parameter type / 不支援的參數型別: " + name);
        }
    }

    void setTextureParam(MaterialInstance* mi, const std::string& name, Texture* t, TextureSampler s) {
        Material::ParameterInfo info{};
        if (!paramInfo(mi->getMaterial(), name, info)) fail("Unknown material parameter / 未知的材質參數: " + name);
        if (!info.isSampler) fail("Parameter is not a texture / 參數不是紋理: " + name);
        mi->setParameter(name.c_str(), t, s);
    }

    void applyDefaults(MaterialInstance* mi) {
        const Material* m = mi->getMaterial();
        auto set = [&](const char* n, std::vector<double> v) { if (m->hasParameter(n)) setUniform(mi, n, v); };
        auto tex = [&](const char* n, Texture* t) { if (m->hasParameter(n)) setTextureParam(mi, n, t, defaultSampler()); };
        bool refraction = std::string(m->getName()) == "AquaRefraction";
        set("baseColor", refraction ? std::vector<double>{1, 1, 1} : std::vector<double>{0.8, 0.8, 0.8, 1});
        set("metallic", {0});
        set("roughness", {refraction ? 0.05 : 0.5});
        set("reflectance", {0.5});
        set("clearCoat", {0});
        set("clearCoatRoughness", {0.1});
        set("occlusion", {1});
        set("normalScale", {1});
        set("emissive", {0, 0, 0});
        set("emissiveIntensity", {0});
        set("uvScale", {1, 1});
        set("absorption", {0, 0, 0});
        set("thickness", {0.5});
        set("ior", {1.5});
        set("transmission", {1});
        set("strength", {0.5});
        set("intensity", {1});
        tex("baseColorMap", mWhite);
        tex("normalMap", mFlatNormal);
        tex("ormMap", mDefaultOrm);
    }

    Json cmdMaterial(const Json& c) {
        int64_t id = idOf(c);
        std::string path = text(c, "path");
        auto data = readFile(resolve(path));
        Material* m = Material::Builder().package(data.data(), data.size()).build(*mEngine);
        if (!m) fail("Invalid material package / 無效的材質檔: " + path);
        Object& o = create(id, Kind::Material);
        o.material = m;
        Json params = Json::array();
        std::vector<Material::ParameterInfo> infos(m->getParameterCount());
        m->getParameters(infos.data(), infos.size());
        for (auto& p : infos) params.push_back(p.name);
        return Json{{"id", id}, {"name", m->getName()}, {"parameters", params}};
    }

    Json cmdInstance(const Json& c) {
        int64_t id = idOf(c);
        Material* m = nullptr;
        const Json& mat = c.contains("material") ? c["material"] : Json("lit");
        if (mat.is_string()) m = builtinMaterial(mat.get<std::string>());
        else if (mat.is_number_integer()) {
            Object& mo = get(mat.get<int64_t>());
            if (mo.kind != Kind::Material) fail("Expected a material / 需要材質");
            m = mo.material;
        } else fail("Expected a material name or id / 需要材質名稱或代碼");
        Object& o = create(id, Kind::MaterialInstance);
        o.material = m;
        o.instance = m->createInstance();
        applyDefaults(o.instance);
        if (has(c, "params")) setParams(o, c["params"]);
        if (flag(c, "doubleSided", false)) o.instance->setCullingMode(MaterialInstance::CullingMode::NONE);
        if (has(c, "culling")) {
            auto cull = lower(text(c, "culling"));
            o.instance->setCullingMode(cull == "none" ? MaterialInstance::CullingMode::NONE
                    : cull == "front" ? MaterialInstance::CullingMode::FRONT : MaterialInstance::CullingMode::BACK);
        }
        return nullptr;
    }

    Json cmdParams(const Json& c) {
        Object& o = get(idOf(c));
        if (o.kind != Kind::MaterialInstance) fail("Expected a material instance / 需要材質實例");
        setParams(o, c.contains("params") ? c["params"] : Json::object());
        return nullptr;
    }

    void setParams(Object& o, const Json& params) {
        if (!params.is_object()) fail("Material parameters must be a hash / 材質參數必須是雜湊表");
        MaterialInstance* mi = o.instance;
        for (auto& [name, value] : params.items()) {
            if (value.is_object() && value.contains("texture")) {
                Object& t = get(value["texture"].get<int64_t>());
                if (t.kind != Kind::Texture) fail("Expected a texture / 需要紋理");
                TextureSampler s = defaultSampler();
                if (lower(text(value, "wrap", "repeat")) == "clamp") {
                    s.setWrapModeS(TextureSampler::WrapMode::CLAMP_TO_EDGE);
                    s.setWrapModeT(TextureSampler::WrapMode::CLAMP_TO_EDGE);
                }
                if (lower(text(value, "filter", "linear")) == "nearest") {
                    s.setMinFilter(TextureSampler::MinFilter::NEAREST);
                    s.setMagFilter(TextureSampler::MagFilter::NEAREST);
                }
                setTextureParam(mi, name, t.texture, s);
            } else if (value.is_number()) {
                setUniform(mi, name, {value.get<double>()});
            } else if (value.is_boolean()) {
                setUniform(mi, name, {value.get<bool>() ? 1.0 : 0.0});
            } else if (value.is_array()) {
                setUniform(mi, name, numbers(value, 1, name.c_str()));
            } else {
                fail("Unsupported parameter value / 不支援的參數值: " + name);
            }
        }
    }

    Json cmdTexture(const Json& c) {
        int64_t id = idOf(c);
        std::string path = text(c, "path");
        bool srgb = flag(c, "srgb", true);
        std::string file = resolve(path);
        std::string ext = extensionOf(file);
        auto data = readFile(file);
        Texture* t = nullptr;
        if (ext == "ktx2") {
            ktxreader::Ktx2Reader reader(*mEngine, true);
            using F = Texture::InternalFormat;
            if (srgb) {
                for (F f : {F::DXT5_SRGBA, F::DXT1_SRGBA, F::ETC2_EAC_SRGBA8, F::SRGB8_ALPHA8_ASTC_4x4}) reader.requestFormat(f);
                reader.requestFormat(F::SRGB8_A8);
            } else {
                for (F f : {F::DXT5_RGBA, F::DXT1_RGBA, F::ETC2_EAC_RGBA8, F::RGBA_ASTC_4x4}) reader.requestFormat(f);
                reader.requestFormat(F::RGBA8);
            }
            t = reader.load(data.data(), data.size(),
                    srgb ? ktxreader::Ktx2Reader::TransferFunction::sRGB : ktxreader::Ktx2Reader::TransferFunction::LINEAR);
        } else if (ext == "ktx") {
            auto* bundle = new image::Ktx1Bundle(data.data(), (uint32_t)data.size());
            t = ktxreader::Ktx1Reader::createTexture(mEngine, bundle, srgb);
        } else {
            int w = 0, h = 0, n = 0;
            stbi_uc* pixels = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &n, 4);
            if (!pixels) fail("Cannot decode image / 無法解碼影像: " + path);
            int levels = 1;
            if (flag(c, "mipmaps", true)) while ((std::max(w, h) >> levels) > 0) levels++;
            t = Texture::Builder().width((uint32_t)w).height((uint32_t)h).levels((uint8_t)levels)
                    .usage(Texture::Usage::DEFAULT | (levels > 1 ? Texture::Usage::GEN_MIPMAPPABLE : Texture::Usage::NONE))
                    .format(srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
                    .sampler(Texture::Sampler::SAMPLER_2D).build(*mEngine);
            t->setImage(*mEngine, 0, Texture::PixelBufferDescriptor(pixels, size_t(w) * h * 4, Texture::Format::RGBA,
                    Texture::Type::UBYTE, [](void* p, size_t, void*) { stbi_image_free(p); }));
            if (levels > 1) t->generateMipmaps(*mEngine);
        }
        if (!t) fail("Cannot load texture / 無法載入紋理: " + path);
        Object& o = create(id, Kind::Texture);
        o.texture = t;
        return Json{{"id", id}, {"width", t->getWidth()}, {"height", t->getHeight()}, {"levels", t->getLevels()}};
    }

    // --------------------------------------------------------------------------------------------
    // Geometry
    // --------------------------------------------------------------------------------------------

    struct MeshData {
        std::vector<float3> positions;
        std::vector<float3> normals;
        std::vector<float2> uvs;
        std::vector<uint32_t> indices;
    };

    static void addQuad(MeshData& m, float3 o, float3 u, float3 v, float3 n, float2 uvScale) {
        uint32_t base = (uint32_t)m.positions.size();
        m.positions.insert(m.positions.end(), {o, o + u, o + u + v, o + v});
        m.normals.insert(m.normals.end(), {n, n, n, n});
        m.uvs.insert(m.uvs.end(), {float2{0, 0}, float2{uvScale.x, 0}, float2{uvScale.x, uvScale.y}, float2{0, uvScale.y}});
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    MeshData makeShape(const Json& c) {
        std::string shape = lower(text(c, "shape", "box"));
        MeshData m;
        const double pi = kPi;
        if (shape == "box" || shape == "cube") {
            float3 s = vec3(c, "size", {1, 1, 1}) * 0.5f;
            float2 uv = {1, 1};
            addQuad(m, {-s.x, -s.y, s.z}, {2 * s.x, 0, 0}, {0, 2 * s.y, 0}, {0, 0, 1}, uv);
            addQuad(m, {s.x, -s.y, -s.z}, {-2 * s.x, 0, 0}, {0, 2 * s.y, 0}, {0, 0, -1}, uv);
            addQuad(m, {s.x, -s.y, s.z}, {0, 0, -2 * s.z}, {0, 2 * s.y, 0}, {1, 0, 0}, uv);
            addQuad(m, {-s.x, -s.y, -s.z}, {0, 0, 2 * s.z}, {0, 2 * s.y, 0}, {-1, 0, 0}, uv);
            addQuad(m, {-s.x, s.y, s.z}, {2 * s.x, 0, 0}, {0, 0, -2 * s.z}, {0, 1, 0}, uv);
            addQuad(m, {-s.x, -s.y, -s.z}, {2 * s.x, 0, 0}, {0, 0, 2 * s.z}, {0, -1, 0}, uv);
        } else if (shape == "plane") {
            float w = (float)num(c, "width", 10), d = (float)num(c, "depth", 10);
            float tile = (float)num(c, "uvTiles", 1);
            addQuad(m, {-w / 2, 0, d / 2}, {w, 0, 0}, {0, 0, -d}, {0, 1, 0}, {tile, tile});
        } else if (shape == "sphere") {
            float r = (float)num(c, "radius", 0.5);
            int seg = std::clamp((int)num(c, "segments", 48), 3, 512), rings = std::clamp((int)num(c, "rings", seg / 2), 2, 512);
            for (int y = 0; y <= rings; y++) {
                double v = double(y) / rings, phi = v * pi;
                for (int x = 0; x <= seg; x++) {
                    double u = double(x) / seg, theta = u * 2 * pi;
                    float3 n = {float(-std::cos(theta) * std::sin(phi)), float(std::cos(phi)), float(std::sin(theta) * std::sin(phi))};
                    m.positions.push_back(n * r);
                    m.normals.push_back(n);
                    m.uvs.push_back({float(u), float(1 - v)});
                }
            }
            for (int y = 0; y < rings; y++) for (int x = 0; x < seg; x++) {
                uint32_t a = y * (seg + 1) + x, b = a + seg + 1;
                m.indices.insert(m.indices.end(), {a, b, a + 1, b, b + 1, a + 1});
            }
        } else if (shape == "cylinder" || shape == "cone") {
            float r = (float)num(c, "radius", 0.5), h = (float)num(c, "height", 1);
            float top = shape == "cone" ? 0.0f : (float)num(c, "topRadius", r);
            int seg = std::clamp((int)num(c, "segments", 48), 3, 512);
            float slope = (r - top) / h;
            for (int x = 0; x <= seg; x++) {
                double u = double(x) / seg, a = u * 2 * pi;
                float cx = (float)std::cos(a), sz = (float)std::sin(a);
                float3 n = normalize(float3{cx, slope, sz});
                m.positions.push_back({cx * r, -h / 2, sz * r});
                m.positions.push_back({cx * top, h / 2, sz * top});
                m.normals.push_back(n);
                m.normals.push_back(n);
                m.uvs.push_back({float(u), 0});
                m.uvs.push_back({float(u), 1});
            }
            for (int x = 0; x < seg; x++) {
                uint32_t a = x * 2;
                m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
            }
            for (int capIndex = 0; capIndex < 2; capIndex++) {
                float y = capIndex ? h / 2 : -h / 2, rr = capIndex ? top : r;
                if (rr <= 0) continue;
                float3 n = {0, capIndex ? 1.0f : -1.0f, 0};
                uint32_t center = (uint32_t)m.positions.size();
                m.positions.push_back({0, y, 0});
                m.normals.push_back(n);
                m.uvs.push_back({0.5f, 0.5f});
                for (int x = 0; x <= seg; x++) {
                    double a = double(x) / seg * 2 * pi;
                    m.positions.push_back({float(std::cos(a)) * rr, y, float(std::sin(a)) * rr});
                    m.normals.push_back(n);
                    m.uvs.push_back({0.5f + 0.5f * float(std::cos(a)), 0.5f + 0.5f * float(std::sin(a))});
                }
                for (int x = 0; x < seg; x++) {
                    uint32_t a = center + 1 + x;
                    if (capIndex) m.indices.insert(m.indices.end(), {center, a + 1, a});
                    else m.indices.insert(m.indices.end(), {center, a, a + 1});
                }
            }
        } else if (shape == "torus") {
            float R = (float)num(c, "radius", 0.5), r = (float)num(c, "tube", 0.15);
            int seg = std::clamp((int)num(c, "segments", 64), 3, 512), sides = std::clamp((int)num(c, "sides", 24), 3, 256);
            for (int i = 0; i <= seg; i++) {
                double u = double(i) / seg * 2 * pi;
                for (int j = 0; j <= sides; j++) {
                    double v = double(j) / sides * 2 * pi;
                    float3 center = {float(std::cos(u)) * R, 0, float(std::sin(u)) * R};
                    float3 n = {float(std::cos(u) * std::cos(v)), float(std::sin(v)), float(std::sin(u) * std::cos(v))};
                    m.positions.push_back(center + n * r);
                    m.normals.push_back(n);
                    m.uvs.push_back({float(i) / seg * 4.0f, float(j) / sides});
                }
            }
            for (int i = 0; i < seg; i++) for (int j = 0; j < sides; j++) {
                uint32_t a = i * (sides + 1) + j, b = a + sides + 1;
                m.indices.insert(m.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
            }
        } else {
            fail("Unknown shape / 未知的形狀: " + shape);
        }
        if (has(c, "uvScale")) {
            auto s = numbers(c["uvScale"], 2, "uvScale");
            for (auto& uv : m.uvs) uv = uv * float2{(float)s[0], (float)s[1]};
        }
        return m;
    }

    void buildRenderable(Object& o, MeshData& m, MaterialInstance* mi, const Json& c) {
        size_t n = m.positions.size();
        if (n == 0 || m.indices.empty()) fail("Empty geometry / 幾何資料為空");
        if (m.normals.size() != n) fail("Normals count mismatch / 法線數量不符");
        if (m.uvs.empty()) m.uvs.assign(n, float2{0, 0});
        if (m.uvs.size() != n) fail("UV count mismatch / UV 數量不符");
        for (uint32_t i : m.indices) if (i >= n) fail("Index out of range / 索引超出範圍");
        std::vector<float4> tangents(n);
        auto* so = geometry::SurfaceOrientation::Builder()
                .vertexCount(n)
                .positions(m.positions.data())
                .normals(m.normals.data())
                .uvs(m.uvs.data())
                .triangleCount(m.indices.size() / 3)
                .triangles(reinterpret_cast<const uint3*>(m.indices.data()))
                .build();
        so->getQuats(reinterpret_cast<quatf*>(tangents.data()), n, sizeof(float4));
        delete so;
        struct Vertex { float3 p; float4 t; float2 uv; };
        auto* vertices = new Vertex[n];
        Box box;
        float3 lo(1e30f), hi(-1e30f);
        for (size_t i = 0; i < n; i++) {
            vertices[i] = {m.positions[i], tangents[i], m.uvs[i]};
            lo = min(lo, m.positions[i]);
            hi = max(hi, m.positions[i]);
        }
        box.set(lo, hi);
        auto* indices = new uint32_t[m.indices.size()];
        std::copy(m.indices.begin(), m.indices.end(), indices);
        o.vb = VertexBuffer::Builder().vertexCount((uint32_t)n).bufferCount(1)
                .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, offsetof(Vertex, p), sizeof(Vertex))
                .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, t), sizeof(Vertex))
                .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex))
                .build(*mEngine);
        o.vb->setBufferAt(*mEngine, 0, VertexBuffer::BufferDescriptor(vertices, n * sizeof(Vertex),
                [](void* p, size_t, void*) { delete[] static_cast<Vertex*>(p); }));
        o.ib = IndexBuffer::Builder().indexCount((uint32_t)m.indices.size()).bufferType(IndexBuffer::IndexType::UINT).build(*mEngine);
        o.ib->setBuffer(*mEngine, IndexBuffer::BufferDescriptor(indices, m.indices.size() * sizeof(uint32_t),
                [](void* p, size_t, void*) { delete[] static_cast<uint32_t*>(p); }));
        o.entity = EntityManager::get().create();
        RenderableManager::Builder(1)
                .boundingBox(box)
                .material(0, mi)
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, o.vb, o.ib)
                .castShadows(flag(c, "castShadows", true))
                .receiveShadows(flag(c, "receiveShadows", true))
                .culling(true)
                .build(*mEngine, o.entity);
        mScene->addEntity(o.entity);
        applyTransform(o.entity, c);
    }

    Json cmdShape(const Json& c) {
        int64_t id = idOf(c);
        MaterialInstance* mi = instanceOf(c);
        MeshData m = makeShape(c);
        Object& o = create(id, Kind::Shape);
        buildRenderable(o, m, mi, c);
        return nullptr;
    }

    Json cmdGeometry(const Json& c) {
        int64_t id = idOf(c);
        MaterialInstance* mi = instanceOf(c);
        MeshData m;
        auto p = numbers(c.at("positions"), 9, "positions");
        auto idx = numbers(c.at("indices"), 3, "indices");
        if (p.size() % 3 || idx.size() % 3) fail("positions/indices must be multiples of 3 / 位置與索引數量須為 3 的倍數");
        for (size_t i = 0; i < p.size(); i += 3) m.positions.push_back({(float)p[i], (float)p[i + 1], (float)p[i + 2]});
        for (double v : idx) m.indices.push_back((uint32_t)v);
        if (has(c, "normals")) {
            auto nn = numbers(c["normals"], 3, "normals");
            for (size_t i = 0; i + 2 < nn.size(); i += 3) m.normals.push_back(normalize(float3{(float)nn[i], (float)nn[i + 1], (float)nn[i + 2]}));
        } else {
            m.normals.assign(m.positions.size(), float3{0});
            for (size_t i = 0; i < m.indices.size(); i += 3) {
                float3 a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]], cc = m.positions[m.indices[i + 2]];
                float3 fn = cross(b - a, cc - a);
                for (int k = 0; k < 3; k++) m.normals[m.indices[i + k]] += fn;
            }
            for (auto& nrm : m.normals) nrm = length(nrm) > 0 ? normalize(nrm) : float3{0, 1, 0};
        }
        if (has(c, "uvs")) {
            auto uv = numbers(c["uvs"], 2, "uvs");
            for (size_t i = 0; i + 1 < uv.size(); i += 2) m.uvs.push_back({(float)uv[i], (float)uv[i + 1]});
        }
        Object& o = create(id, Kind::Shape);
        buildRenderable(o, m, mi, c);
        return nullptr;
    }

    Json cmdMesh(const Json& c) {
        int64_t id = idOf(c);
        MaterialInstance* mi = instanceOf(c);
        std::string path = text(c, "path");
        // MeshReader invokes the destructor once per GPU buffer, so the source bytes are owned by
        // the object instead and released when it is removed.
        auto data = std::make_shared<std::vector<uint8_t>>(readFile(resolve(path)));
        auto mesh = filamesh::MeshReader::loadMeshFromBuffer(mEngine, data->data(), data->size(),
                [](void*, size_t, void*) {}, nullptr, mi);
        if (mesh.renderable.isNull()) fail("Invalid .filamesh / 無效的網格檔: " + path);
        Object& o = create(id, Kind::Mesh);
        o.entity = mesh.renderable;
        o.source = data;
        o.vb = mesh.vertexBuffer;
        o.ib = mesh.indexBuffer;
        auto& rm = mEngine->getRenderableManager();
        auto ri = rm.getInstance(o.entity);
        rm.setCastShadows(ri, flag(c, "castShadows", true));
        rm.setReceiveShadows(ri, flag(c, "receiveShadows", true));
        mScene->addEntity(o.entity);
        applyTransform(o.entity, c);
        Box b = rm.getAxisAlignedBoundingBox(ri);
        return Json{{"id", id}, {"min", arr3(b.getMin())}, {"max", arr3(b.getMax())}};
    }

    // --------------------------------------------------------------------------------------------
    // glTF models
    // --------------------------------------------------------------------------------------------

    Json cmdModel(const Json& c) {
        int64_t id = idOf(c);
        std::string path = text(c, "path");
        std::string file = resolve(path);
        auto data = readFile(file);
        gltfio::FilamentAsset* asset = mAssetLoader->createAsset(data.data(), (uint32_t)data.size());
        if (!asset) fail("Invalid glTF / 無效的 glTF 模型: " + path);
        std::string dir = directoryOf(file);
        const char* const* uris = asset->getResourceUris();
        for (size_t i = 0; i < asset->getResourceUriCount(); i++) {
            std::string uri = uris[i];
            if (uri.rfind("data:", 0) == 0) continue;
            std::string decoded;
            for (size_t k = 0; k < uri.size(); k++) {  // percent-decoding
                if (uri[k] == '%' && k + 2 < uri.size()) { decoded += (char)std::stoi(uri.substr(k + 1, 2), nullptr, 16); k += 2; }
                else decoded += uri[k];
            }
            std::string resourcePath = dir.empty() ? decoded : dir + "/" + decoded;
            auto* bytes = new std::vector<uint8_t>(readFile(resourcePath));
            mResourceLoader->addResourceData(uri.c_str(), gltfio::ResourceLoader::BufferDescriptor(bytes->data(), bytes->size(),
                    [](void*, size_t, void* user) { delete static_cast<std::vector<uint8_t>*>(user); }, bytes));
        }
        // Desktop gltfio reads external .bin buffers through cgltf's fopen(), relative to gltfPath.
        mGltfPathStorage = cgltfPath(file);
        gltfio::ResourceConfiguration rc{};
        rc.engine = mEngine;
        rc.gltfPath = mGltfPathStorage.c_str();
        rc.normalizeSkinningWeights = true;
        mResourceLoader->setConfiguration(rc);
        bool ok = mResourceLoader->asyncBeginLoad(asset);
        if (!ok) {
            mAssetLoader->destroyAsset(asset);
            mResourceLoader->evictResourceData();
            fail("Cannot load glTF resources / 無法載入 glTF 資源: " + path);
        }
        Object& o = create(id, Kind::Model);
        o.asset = asset;
        o.loading = true;
        mLoadingCount++;
        mLoadingModel = id;
        auto& rm = mEngine->getRenderableManager();
        bool cast = flag(c, "castShadows", true), receive = flag(c, "receiveShadows", true);
        for (size_t i = 0; i < asset->getRenderableEntityCount(); i++) {
            auto ri = rm.getInstance(asset->getRenderableEntities()[i]);
            rm.setCastShadows(ri, cast);
            rm.setReceiveShadows(ri, receive);
        }
        if (!flag(c, "lights", true)) {
            for (size_t i = 0; i < asset->getLightEntityCount(); i++) mEngine->getLightManager().destroy(asset->getLightEntities()[i]);
        }
        mScene->addEntities(asset->getEntities(), asset->getEntityCount());
        applyTransform(asset->getRoot(), c);
        if (flag(c, "wait", true)) {
            // Finish texture decoding now so the first rendered frame is complete.
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (mResourceLoader->asyncGetLoadProgress() < 1.0f && std::chrono::steady_clock::now() < deadline) {
                mResourceLoader->asyncUpdateLoad();
                if (mWindow) mWindow->pump(mPendingEvents);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            finishLoading();
        }
        return modelInfo(id, o);
    }

    // cgltf uses narrow fopen(); on Windows a non-ASCII path (e.g. a user profile temp folder) is
    // converted to its ASCII 8.3 short form.
    static std::string cgltfPath(const std::string& path) {
#if defined(_WIN32)
        bool ascii = std::all_of(path.begin(), path.end(), [](char ch) { return (unsigned char)ch < 128; });
        if (!ascii) {
            std::wstring wide = widen(path);
            DWORD n = GetShortPathNameW(wide.c_str(), nullptr, 0);
            if (n) {
                std::wstring shortPath(n, L'\0');
                n = GetShortPathNameW(wide.c_str(), shortPath.data(), n);
                shortPath.resize(n);
                std::string narrowPath = narrow(shortPath);
                if (std::all_of(narrowPath.begin(), narrowPath.end(), [](char ch) { return (unsigned char)ch < 128; }))
                    return narrowPath;
            }
        }
#endif
        return path;
    }

    void updateLoadingAssets() {
        if (mLoadingModel < 0) return;
        mResourceLoader->asyncUpdateLoad();
        if (mResourceLoader->asyncGetLoadProgress() >= 1.0f) finishLoading();
    }

    void finishLoading() {
        if (mLoadingModel < 0) return;
        mResourceLoader->asyncUpdateLoad();
        auto it = mObjects.find(mLoadingModel);
        if (it != mObjects.end() && it->second.loading) {
            Object& o = it->second;
            o.loading = false;
            mLoadingCount = std::max(0, mLoadingCount - 1);
            auto* instance = o.asset->getInstance();
            MaterialInstance* const* mis = instance->getMaterialInstances();
            o.emissiveBase.clear();
            for (size_t i = 0; i < instance->getMaterialInstanceCount(); i++) {
                o.emissiveBase.push_back(mis[i]->getMaterial()->hasParameter("emissiveStrength")
                        ? mis[i]->getParameter<float>("emissiveStrength") : 1.0f);
            }
            o.asset->releaseSourceData();
        }
        mResourceLoader->evictResourceData();
        mLoadingModel = -1;
    }

    Json modelInfo(int64_t id, Object& o) {
        auto* asset = o.asset;
        Aabb box = asset->getBoundingBox();
        Json animations = Json::array();
        if (auto* animator = asset->getInstance()->getAnimator()) {
            for (size_t i = 0; i < animator->getAnimationCount(); i++) {
                const char* name = animator->getAnimationName(i);
                animations.push_back(Json{{"name", name ? name : ""}, {"duration", animator->getAnimationDuration(i)}});
            }
        }
        Json materials = Json::array();
        auto* instance = asset->getInstance();
        for (size_t i = 0; i < instance->getMaterialInstanceCount(); i++) {
            const char* name = instance->getMaterialInstances()[i]->getName();
            materials.push_back(name ? name : "");
        }
        return Json{{"id", id}, {"min", arr3(box.min)}, {"max", arr3(box.max)},
                {"center", arr3((box.min + box.max) * 0.5f)}, {"size", arr3(box.max - box.min)},
                {"entities", asset->getEntityCount()}, {"renderables", asset->getRenderableEntityCount()},
                {"lights", asset->getLightEntityCount()}, {"animations", animations}, {"materials", materials},
                {"loading", o.loading}};
    }

    Object& model(const Json& c) {
        Object& o = get(idOf(c));
        if (o.kind != Kind::Model) fail("Object is not a glTF model / 物件不是 glTF 模型");
        return o;
    }

    Json cmdModelInfo(const Json& c) {
        Object& o = model(c);
        return modelInfo(idOf(c), o);
    }

    Json cmdAnimate(const Json& c) {
        Object& o = model(c);
        auto* animator = o.asset->getInstance()->getAnimator();
        if (!animator) return nullptr;
        double time = num(c, "time", 0);
        if (has(c, "animation")) {
            int index = (int)num(c, "animation", 0);
            if (index >= 0 && (size_t)index < animator->getAnimationCount()) animator->applyAnimation((size_t)index, (float)time);
        } else {
            for (size_t i = 0; i < animator->getAnimationCount(); i++) animator->applyAnimation(i, (float)time);
        }
        animator->updateBoneMatrices();
        return nullptr;
    }

    Json cmdEmissive(const Json& c) {
        Object& o = model(c);
        if (o.loading) finishLoading();
        float scale = (float)num(c, "scale", 1.0);
        auto* instance = o.asset->getInstance();
        MaterialInstance* const* mis = instance->getMaterialInstances();
        for (size_t i = 0; i < instance->getMaterialInstanceCount() && i < o.emissiveBase.size(); i++) {
            if (mis[i]->getMaterial()->hasParameter("emissiveStrength")) mis[i]->setParameter("emissiveStrength", o.emissiveBase[i] * scale);
        }
        return nullptr;
    }

    // --------------------------------------------------------------------------------------------
    // Transforms
    // --------------------------------------------------------------------------------------------

    Json cmdGroup(const Json& c) {
        int64_t id = idOf(c);
        Object& o = create(id, Kind::Entity);
        o.entity = EntityManager::get().create();
        mEngine->getTransformManager().create(o.entity);
        applyTransform(o.entity, c);
        return nullptr;
    }

    static mat4f composeTransform(float3 t, quatf q, float3 s) {
        return mat4f::translation(t) * mat4f(mat3f(q)) * mat4f::scaling(s);
    }

    static quatf eulerDegrees(float3 e) {
        const float k = float(kPi / 180.0);
        quatf qx = quatf::fromAxisAngle(float3{1, 0, 0}, e.x * k);
        quatf qy = quatf::fromAxisAngle(float3{0, 1, 0}, e.y * k);
        quatf qz = quatf::fromAxisAngle(float3{0, 0, 1}, e.z * k);
        return qy * qx * qz; // yaw, then pitch, then roll
    }

    void applyTransform(Entity e, const Json& c) {
        bool any = has(c, "position") || has(c, "rotation") || has(c, "quaternion") || has(c, "scale") || has(c, "matrix");
        if (!any) return;
        auto& tm = mEngine->getTransformManager();
        if (!tm.hasComponent(e)) tm.create(e);
        auto ti = tm.getInstance(e);
        if (has(c, "matrix")) {
            auto m = numbers(c["matrix"], 16, "matrix");
            mat4f mat;
            for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) mat[col][row] = (float)m[col * 4 + row];
            tm.setTransform(ti, mat);
            return;
        }
        float3 t = vec3(c, "position", {0, 0, 0});
        quatf q = {1, 0, 0, 0};
        if (has(c, "quaternion")) {
            auto v = numbers(c["quaternion"], 4, "quaternion");
            q = normalize(quatf{(float)v[3], (float)v[0], (float)v[1], (float)v[2]});
        } else if (has(c, "rotation")) {
            q = eulerDegrees(vec3(c, "rotation", {0, 0, 0}));
        }
        float3 s{1, 1, 1};
        if (has(c, "scale")) {
            const Json& sv = c["scale"];
            if (sv.is_number()) s = float3(float(sv.get<double>()));
            else s = vec3(c, "scale", {1, 1, 1});
        }
        tm.setTransform(ti, composeTransform(t, q, s));
    }

    Json cmdTransform(const Json& c) {
        Object& o = getEntity(idOf(c));
        if (o.kind == Kind::Light) {
            auto& lm = mEngine->getLightManager();
            auto li = lm.getInstance(o.entity);
            if (has(c, "position")) lm.setPosition(li, vec3(c, "position", {}));
            if (has(c, "direction")) lm.setDirection(li, normalize(vec3(c, "direction", {0, -1, 0})));
            return nullptr;
        }
        applyTransform(entityOf(o), c);
        return nullptr;
    }

    Json cmdParent(const Json& c) {
        Object& child = getEntity(idOf(c));
        auto& tm = mEngine->getTransformManager();
        Entity ce = entityOf(child);
        if (!tm.hasComponent(ce)) tm.create(ce);
        if (!has(c, "parent")) { tm.setParent(tm.getInstance(ce), {}); return nullptr; }
        Object& parent = getEntity(idOf(c, "parent"));
        Entity pe = entityOf(parent);
        if (!tm.hasComponent(pe)) tm.create(pe);
        tm.setParent(tm.getInstance(ce), tm.getInstance(pe));
        return nullptr;
    }

    void forEachRenderable(Object& o, const std::function<void(Entity)>& fn) {
        if (o.kind == Kind::Model) {
            for (size_t i = 0; i < o.asset->getRenderableEntityCount(); i++) fn(o.asset->getRenderableEntities()[i]);
        } else if (o.kind == Kind::Shape || o.kind == Kind::Mesh) {
            fn(o.entity);
        }
    }

    Json cmdVisible(const Json& c) {
        Object& o = getEntity(idOf(c));
        bool visible = flag(c, "visible", true);
        if (o.kind == Kind::Model) {
            if (visible) mScene->addEntities(o.asset->getEntities(), o.asset->getEntityCount());
            else mScene->removeEntities(o.asset->getEntities(), o.asset->getEntityCount());
        } else {
            if (visible) mScene->addEntity(o.entity); else mScene->remove(o.entity);
        }
        return nullptr;
    }

    Json cmdShadows(const Json& c) {
        Object& o = getEntity(idOf(c));
        auto& rm = mEngine->getRenderableManager();
        forEachRenderable(o, [&](Entity e) {
            auto ri = rm.getInstance(e);
            if (has(c, "cast")) rm.setCastShadows(ri, flag(c, "cast", true));
            if (has(c, "receive")) rm.setReceiveShadows(ri, flag(c, "receive", true));
        });
        return nullptr;
    }

    Json cmdSetMaterial(const Json& c) {
        Object& o = getEntity(idOf(c));
        MaterialInstance* mi = instanceOf(c);
        auto& rm = mEngine->getRenderableManager();
        forEachRenderable(o, [&](Entity e) {
            auto ri = rm.getInstance(e);
            for (size_t p = 0; p < rm.getPrimitiveCount(ri); p++) rm.setMaterialInstanceAt(ri, p, mi);
        });
        return nullptr;
    }

    // --------------------------------------------------------------------------------------------
    // Screenshots & stats
    // --------------------------------------------------------------------------------------------

    Json cmdScreenshot(const Json& c) {
        std::string path = text(c, "path");
        if (path.empty()) fail("Screenshot needs a path / 截圖需要路徑");
        mScreenshots.push_back({path});
        // Render immediately so the call returns after the file exists.
        Json frame = cmdFrame(Json::object());
        mEngine->flushAndWait();
        if (!mScreenshots.empty()) fail("Screenshot failed / 截圖失敗");
        if (!mScreenshotError.empty()) { std::string e = mScreenshotError; mScreenshotError.clear(); fail(e); }
        return Json{{"path", path}, {"width", mWidth}, {"height", mHeight}};
    }

    struct Readback {
        Runtime* self;
        std::string path;
        int width, height;
    };

    void captureScreenshots() {
        for (auto& shot : mScreenshots) {
            size_t size = size_t(mWidth) * mHeight * 4;
            auto* pixels = new uint8_t[size];
            auto* rb = new Readback{this, shot.path, mWidth, mHeight};
            mRenderer->readPixels(0, 0, (uint32_t)mWidth, (uint32_t)mHeight,
                    backend::PixelBufferDescriptor(pixels, size, backend::PixelDataFormat::RGBA, backend::PixelDataType::UBYTE,
                            [](void* buffer, size_t, void* user) {
                                auto* r = static_cast<Readback*>(user);
                                auto* px = static_cast<uint8_t*>(buffer);
                                // Filament reads bottom-up.
                                stbi_flip_vertically_on_write(0);
                                bool ok;
#if defined(_WIN32)
                                FILE* f = _wfopen(widen(r->path).c_str(), L"wb");
                                ok = f != nullptr;
                                if (f) {
                                    struct Ctx { FILE* f; } ctx{f};
                                    ok = stbi_write_png_to_func([](void* ctx, void* data, int size) {
                                        fwrite(data, 1, (size_t)size, static_cast<Ctx*>(ctx)->f);
                                    }, &ctx, r->width, r->height, 4, px, r->width * 4) != 0;
                                    fclose(f);
                                }
#else
                                ok = stbi_write_png(r->path.c_str(), r->width, r->height, 4, px, r->width * 4) != 0;
#endif
                                if (!ok) r->self->mScreenshotError = "Cannot write screenshot / 無法寫入截圖: " + r->path;
                                delete[] px;
                                delete r;
                            }, rb));
        }
        mScreenshots.clear();
    }

    Json cmdStats() {
        size_t counts[8] = {};
        for (auto& [id, o] : mObjects) counts[(int)o.kind]++;
        return Json{{"fps", mFps}, {"frames", mFrames}, {"objects", mObjects.size()}, {"lights", counts[(int)Kind::Light]},
                {"models", counts[(int)Kind::Model]}, {"shapes", counts[(int)Kind::Shape]}, {"meshes", counts[(int)Kind::Mesh]},
                {"textures", counts[(int)Kind::Texture]}, {"width", mWidth}, {"height", mHeight}, {"loading", mLoadingCount}};
    }

    // --------------------------------------------------------------------------------------------
    // Teardown
    // --------------------------------------------------------------------------------------------

    void shutdownScene() {
        if (!mEngine) return;
        clearScene(false);
        delete mManipulator;
        mManipulator = nullptr;
        if (mDefaultInstance) { mEngine->destroy(mDefaultInstance); mDefaultInstance = nullptr; }
        for (auto& [name, m] : mBuiltins) mEngine->destroy(m);
        mBuiltins.clear();
        if (mColorGrading) { mEngine->destroy(mColorGrading); mColorGrading = nullptr; }
        mScene->setSkybox(nullptr);
        if (mSkybox) { mEngine->destroy(mSkybox); mSkybox = nullptr; }
        destroyEnvironment();
        mEngine->destroy(mWhite);
        mEngine->destroy(mFlatNormal);
        mEngine->destroy(mDefaultOrm);
        delete mResourceLoader;
        mResourceLoader = nullptr;
        delete mStbDecoder;
        delete mKtxDecoder;
        mStbDecoder = mKtxDecoder = nullptr;
        gltfio::AssetLoader::destroy(&mAssetLoader);
        mMaterialProvider->destroyMaterials();
        delete mMaterialProvider;
        mMaterialProvider = nullptr;
        delete mNames;
        mNames = nullptr;
        mEngine->destroyCameraComponent(mCameraEntity);
        EntityManager::get().destroy(mCameraEntity);
        mEngine->destroy(mView);
        mEngine->destroy(mScene);
        mEngine->destroy(mRenderer);
        mEngine->destroy(mSwapChain);
        Engine::destroy(&mEngine);
        mEngine = nullptr;
        mWindow.reset();
        mPendingEvents.clear();
    }

    void shutdown() { shutdownScene(); }

    // Threading state
    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mWake, mDone;
    std::deque<std::shared_ptr<Job>> mJobs;
    bool mQuit = false, mStopped = false;

public:
    std::string mScreenshotError;

private:
    // Render-thread state
    std::unique_ptr<NativeWindow> mWindow;
    std::vector<Json> mPendingEvents;
    Engine* mEngine = nullptr;
    SwapChain* mSwapChain = nullptr;
    Renderer* mRenderer = nullptr;
    Scene* mScene = nullptr;
    View* mView = nullptr;
    Camera* mCamera = nullptr;
    Entity mCameraEntity;
    Skybox* mSkybox = nullptr;
    IndirectLight* mIndirectLight = nullptr;
    Texture* mIblTexture = nullptr;
    Texture* mSkyTexture = nullptr;
    ColorGrading* mColorGrading = nullptr;
    Texture* mWhite = nullptr;
    Texture* mFlatNormal = nullptr;
    Texture* mDefaultOrm = nullptr;
    MaterialInstance* mDefaultInstance = nullptr;
    std::map<std::string, Material*> mBuiltins;
    utils::NameComponentManager* mNames = nullptr;
    gltfio::MaterialProvider* mMaterialProvider = nullptr;
    gltfio::AssetLoader* mAssetLoader = nullptr;
    gltfio::ResourceLoader* mResourceLoader = nullptr;
    gltfio::TextureProvider* mStbDecoder = nullptr;
    gltfio::TextureProvider* mKtxDecoder = nullptr;
    camutils::Manipulator<float>* mManipulator = nullptr;
    Json mControl = Json::object();
    bool mGrabbing = false;
    double mWheelScale = 1.0;
    std::unordered_map<int64_t, Object> mObjects;
    std::vector<PendingScreenshot> mScreenshots;
    std::string mAppRoot, mResourceRoot, mMaterialDir, mGltfPathStorage;
    std::string mToneMapper = "agx";
    float mGradeExposure = 0, mGradeContrast = 1, mGradeSaturation = 1, mGradeVibrance = 1, mGradeTemperature = 0;
    float mFocusDistance = 0, mEnvRotation = 0;
    float3 mEye, mTarget, mUp;
    double mFov = 45, mNear = 0.05, mFar = 1000;
    bool mFovHorizontal = false, mHeadless = false;
    int mWidth = 0, mHeight = 0;
    int64_t mLoadingModel = -1;
    int mLoadingCount = 0;
    double mMaxFps = 0, mFps = 0;
    uint64_t mFrames = 0;
    int mFpsFrames = 0;
    std::chrono::steady_clock::time_point mStart, mLastFrame, mFpsWindow;
};

static Runtime& runtime() {
    static Runtime* instance = new Runtime(); // intentionally leaked: torn down by fl_shutdown
    return *instance;
}

} // namespace aquafl

AQUA_EXPORT int32_t aqua_library_abi(void) { return 1; }

AQUA_EXPORT int32_t fl_call(const uint8_t* input, int32_t length, uint8_t* output, int32_t capacity) {
    using namespace aquafl;
    std::string response;
    try {
        Json request = Json::parse(input, input + std::max(0, length));
        response = runtime().submit(std::move(request)).dump(-1, ' ', false, Json::error_handler_t::replace);
    } catch (const std::exception& e) {
        response = Json{{"ok", false}, {"error", std::string("Invalid request / 無效的請求: ") + e.what()}}.dump();
    }
    if ((int64_t)response.size() > capacity) {
        response = Json{{"ok", false}, {"error", "Response exceeds 1 MiB / 回應超過 1 MiB"}}.dump();
        if ((int64_t)response.size() > capacity) return -2;
    }
    std::memcpy(output, response.data(), response.size());
    return (int32_t)response.size();
}

// Raises a host-side error carrying the caller's message (used by the Aquarius wrapper after it printed
// the diagnostic): returning a negative value makes DynamicLibrary fail the call and stop the program.
AQUA_EXPORT int32_t fl_fail(const uint8_t*, int32_t, uint8_t*, int32_t) { return -1; }

// Optional: stops the render thread and releases the GPU (also used by tests).
AQUA_EXPORT int32_t fl_shutdown(const uint8_t*, int32_t, uint8_t* output, int32_t capacity) {
    aquafl::runtime().stop();
    const char ok[] = "{\"ok\":true}";
    if (capacity < (int32_t)sizeof(ok) - 1) return -2;
    std::memcpy(output, ok, sizeof(ok) - 1);
    return (int32_t)sizeof(ok) - 1;
}
