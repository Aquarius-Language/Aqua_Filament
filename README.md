# Aqua_Filament 1.0.1 — 星泉 Filament 即時算繪函式庫

## !!目前尚不穩定!!

用 [AquariusLang 星泉](https://github.com/Aquarius-Language/AquariusLangTW) 驅動 Google
[Filament 1.78.0](https://github.com/google/filament) 實體算繪（PBR）引擎的第三方 Bottle 函式庫。
每個函式都有 **繁體中文** 與 **English** 兩個名稱，兩者指向同一個函式物件。

A third-party AquariusLang Bottle library for Google's Filament 1.78.0 physically based renderer. Every
function has a 繁體中文 name and an English name that share one function object.

| 平台 / Platform | 實作 / Implementation | 圖形 API |
| --- | --- | --- |
| Windows x64 | `runtimes/windows/aqua_filament.dll`：原生視窗＋專屬算繪執行緒，靜態連結官方 Filament 1.78.0 函式庫（附 VC++ 執行階段 DLL） | OpenGL 4.1（預設）／Vulkan |
| Linux x64 | `runtimes/linux/libaqua_filament.so`：Xlib 視窗（Xorg／XWayland）＋專屬算繪執行緒；靜態連結 libc++；glibc 2.35 以上 | OpenGL（預設）／Vulkan |
| Web | `runtimes/web/aqua_filament.mjs`：官方 `filament.js`＋`filament.wasm` 1.78.0（已內嵌）＋ JSON 指令轉接層 | WebGL 2 |

三種平台都使用同一套 JSON 指令協定（`fl_call`），因此星泉程式碼完全相同。
All three runtimes implement the same JSON command protocol (`fl_call`), so Aquarius code is identical on
every platform.

尚未支援的平台：**Android**，因為星泉的 Android 主機目前沒有把 `SurfaceView` 交給 `DynamicLibrary` 原生
函式庫；**macOS**，因為需要 Apple SDK 建置 `.dylib`。在這兩個平台上呼叫 `開啟視窗` 會明確回報錯誤。

## 安裝 / Install

```toml
# 你的 aqua.toml / your project's aqua.toml
[dependencies]
Aqua_Filament = { path = "../Aqua_Filament", version = ">= 1.0.0, < 2.0.0" }
# or: aqua bottle install https://github.com/OWNER/Aqua_Filament.git --rev v1.0.1 --version "1.0.1" --project .
```

```aquarius
變數 濾 = 匯入("/bottles/Aqua_Filament/main.aqua");
變數 無值 = 濾.無值;
濾.開啟視窗({"標題":"Hello Filament", "寬度":1280, "高度":720});
濾.設定相機([0, 1.5d, 4], [0, 0.5d, 0], [0, 1, 0]);
濾.軌道控制({"目標":[0, 0.5d, 0]});
濾.加入太陽光([-0.5d, -1, -0.3d], [1, 0.95d, 0.9d], 100000, 無值);
變數 紅 = 濾.標準材質({"基礎色":[0.9d, 0.15d, 0.1d, 1], "粗糙度":0.3d, "清漆":1});
變數 球 = 濾.加入球體(0.6d, 紅, {"位置":[0, 0.6d, 0]});
濾.加入平面(10, 10, 濾.標準材質({"基礎色":[0.6d, 0.6d, 0.6d, 1]}), 無值);
濾.設定畫面({"泛光":{"強度":0.1d}, "環境遮蔽":真, "色調映射":"ACES"});
迴圈 (變數 i = 0; 濾.視窗開啟中(); i++) {
    變數 幀 = 濾.繪製畫面();
    濾.設定旋轉(球, [0, 幀["時間"] * 45, 0]);
    如果 (濾.按鍵剛按下("escape")) { 中斷; }
}
濾.關閉視窗();
```

```powershell
aqua build . --target windows -o dist\app.exe     # Windows
aqua build . --target linux -o dist/app           # Linux
aqua build . --target web -o dist\web             # 網頁：以 localhost 或 HTTPS 提供 / serve over localhost or HTTPS
```

大多數呼叫只會把指令放進佇列；`繪製畫面` / `Frame` 會一次送出整批指令並算繪一幀。需要回傳值的函式
（`載入模型`、`統計` 等）會立即送出。物件代碼由星泉端配置，所以建立物件不必等待原生端回應。指令錯誤會
印出雙語訊息，並以 DynamicLibrary 錯誤停止程式。

Most calls only queue commands. `Frame` / `繪製畫面` sends the batch and renders one frame. Calls that return
data (`LoadModel`, `Stats`, …) flush immediately. Object ids are allocated on the Aquarius side, so creating
objects never waits for the native side. When a command fails, the library prints a bilingual message and the
DynamicLibrary call then stops the program.

## API

參數說明：`[x,y,z]` 是數字陣列；選項雜湊表的鍵可以用中文或英文（例如 `"位置"` 或 `"position"`）；
不需要的參數請傳 `濾.無值` / `Null`。角度單位是度。

### 視窗與迴圈 / Window and loop

| 繁體中文 | English | 參數 | 說明 |
| --- | --- | --- | --- |
| 開啟視窗 | Open | 選項 | 鍵：標題 title、寬度 width、高度 height、後端 backend（`"OpenGL"`/`"Vulkan"`）、最高幀率 maxFps、無視窗 headless、視野 fov、近平面 near、遠平面 far、保留繪圖緩衝 preserveDrawingBuffer（網頁，預設 真）、應用程式根目錄 appRoot。回傳 {後端, 寬度, 高度} |
| 視窗開啟中 | IsOpen | — | 視窗開啟中且使用者尚未要求關閉 |
| 繪製畫面 | Frame | — | 送出佇列、算繪一幀；回傳 {事件 events, 寬度, 高度, 時間 time, 間隔 dt, 幀率 fps, 幀數 frame, 開啟中 open, 載入中 loading} |
| 送出 | Flush | — | 立即送出佇列，回傳結果陣列 |
| 關閉視窗 | Close | — | 釋放 GPU 資源並結束算繪執行緒 |
| 設定標題 | SetTitle | 文字 | 視窗標題（網頁為分頁標題） |
| 按鍵按下中 / 按鍵剛按下 / 按鍵剛放開 | IsKeyDown / KeyPressed / KeyReleased | 鍵名 | 鍵名為小寫：`"a"`…`"z"`、`"0"`…`"9"`、`"space"`、`"enter"`、`"escape"`、`"left"`、`"f1"`… |
| 滑鼠狀態 | Mouse | — | {x, y, 左鍵/left, 右鍵/right, 中鍵/middle, 滾輪/wheel} |
| 上一幀資訊 | LastFrame | — | 最近一次 `繪製畫面` 的結果 |
| 統計 | Stats | — | {幀率 fps, 物件數 objects, 光源數 lights, 模型數 models, …} |
| 截圖 | Screenshot | 路徑 | 桌面輸出 PNG 檔；網頁存成 `globalThis.__aquaFilamentScreenshot`（data URL） |
| 資訊 | Info | — | 版本、平台 |
| 是否支援 | IsSupported | — | 目前平台是否支援 |

### 相機 / Camera

| 繁體中文 | English | 參數 | 說明 |
| --- | --- | --- | --- |
| 設定相機 / 看向 | SetCamera / LookAt | 眼睛, 目標, 上方 / 眼睛, 目標 | 世界座標，單位公尺 |
| 設定鏡頭 | SetLens | 視野角度, 近平面, 遠平面 | 垂直視野（度） |
| 設定曝光 / 設定曝光值 | SetExposure / SetEV100 | 光圈, 快門, 感光度 / EV100 | 實體相機曝光，日光約 f/16、1/125、ISO 100 |
| 軌道控制 / 飛行控制 / 地圖控制 | OrbitControl / FlyControl / MapControl | 選項 | 原生端 camutils 操作器：拖曳旋轉、右鍵平移、滾輪縮放；飛行模式使用 WASD/QE。鍵：目標、眼睛、旋轉速度、縮放速度、速度、平移速度、阻尼、滾輪倍率 |
| 停用相機控制 | DisableCameraControl | — | 改由程式控制相機 |
| 相機狀態 | CameraState | — | {眼睛, 目標, 上方, 視野} |

### 畫面與後處理 / View and post-processing

`設定畫面(選項)` / `SetView(options)`。子選項可以給 `真`/`假`，也可以給雜湊表：

| 鍵 | key | 子鍵（中／英） |
| --- | --- | --- |
| 泛光 | bloom | 強度 strength、層數 levels、閾值 threshold、高光 highlight、鏡頭光暈 lensFlare、星芒 starburst、品質 quality |
| 環境遮蔽 | ssao | 半徑 radius、強度 intensity、次方 power、品質 quality、彎曲法線 bentNormals |
| 螢幕空間反射 | ssr | 厚度 thickness、最大距離 maxDistance、偏移 bias、步距 stride |
| 景深 | dof | 模糊圈縮放 cocScale、最大光圈 maxAperture、對焦距離 focusDistance |
| 霧 | fog | 距離 distance、密度 density、高度 height、高度衰減 heightFalloff、顏色 color、取自環境光 fromIbl |
| 暈影 | vignette | 中點 midPoint、圓度 roundness、羽化 feather |
| 時間抗鋸齒 | taa | 回饋 feedback、濾波寬度 filterWidth、銳利度 sharpness |
| 多重取樣 | msaa | 取樣數（數字） |
| 抗鋸齒 | antiAliasing | `"FXAA"` / `"無"` |
| 陰影類型 | shadowType | `"PCF"`、`"VSM"`、`"DPCF"`、`"PCSS"` |
| 色調映射 | toneMapper | `"ACES"`、`"電影"` filmic、`"線性"`、`"AgX"`、`"中性"`（網頁版 filament.js 只有 ACES／Filmic／Linear，其餘以 ACES 取代） |
| 曝光補償、對比、飽和度、自然飽和度、白平衡 | exposureBias, contrast, saturation, vibrance, whiteBalance | 數字 |
| 動態解析度 | dynamicResolution | 最小縮放 minScale、最大縮放 maxScale |
| 螢幕空間折射、陰影、後處理 | screenSpaceRefraction, shadows, postProcessing | 真／假 |

簡便函式：`設定色調映射/SetToneMapper`、`設定陰影類型/SetShadowType`、`設定泛光/SetBloom(強度)`、`設定清除色/SetClearColor`。

### 環境光與光源 / Environment and lights

| 繁體中文 | English | 參數 | 說明 |
| --- | --- | --- | --- |
| 載入環境光 | LoadEnvironment | 環境光路徑, 天空盒路徑, 強度 | Filament `cmgen` 產生的 `*_ibl.ktx`／`*_skybox.ktx`；天空盒可傳 無值。日光曝光下強度約 30000 |
| 設定環境光強度 / 旋轉環境光 / 啟用環境光 | SetEnvironmentIntensity / RotateEnvironment / SetEnvironmentEnabled | 數值 / 度 / 布林 | |
| 設定天空顏色 / 載入天空盒 / 顯示天空盒 | SetSkyColor / LoadSkybox / ShowSkybox | 顏色 / 路徑 / 布林 | |
| 加入太陽光 | AddSun | 方向, 顏色, 照度(lux), 選項 | 預設投射陰影；選項可含 陰影 {貼圖尺寸, 串接層數, 常數偏移, 法線偏移, 接觸陰影} |
| 加入平行光 | AddDirectionalLight | 方向, 顏色, 照度, 選項 | |
| 加入點光源 | AddPointLight | 位置, 顏色, 流明, 衰減距離, 選項 | |
| 加入聚光燈 | AddSpotLight | 位置, 方向, 顏色, 流明, 外角(度), 衰減距離, 選項 | 內角預設為外角的 0.8 倍；選項：內角、投射陰影、單位（`"燭光"`） |
| 設定光源 | SetLight | 光源, 選項 | 顏色、強度、位置、方向、衰減距離、內角、外角、投射陰影、啟用 |
| 移動光源 | MoveLight | 光源, 位置, 方向 | |

### 材質與紋理 / Materials and textures

| 繁體中文 | English | 參數 | 說明 |
| --- | --- | --- | --- |
| 標準材質 | StandardMaterial | 參數 | 基礎色 baseColor、金屬度 metallic、粗糙度 roughness、反射率 reflectance、清漆 clearCoat、清漆粗糙度、遮蔽 occlusion、法線強度 normalScale、自發光 emissive、自發光強度、UV縮放 uvScale、基礎色貼圖、法線貼圖、ORM貼圖（R=AO, G=粗糙度, B=金屬度） |
| 貼圖材質 | TexturedMaterial | 基礎色路徑, 法線路徑, ORM路徑, 參數 | 一次載入三張紋理並建立標準材質 |
| 透明材質 | TransparentMaterial | 參數 | 同標準材質；基礎色 alpha 為不透明度 |
| 折射材質 | RefractiveMaterial | 參數 | 螢幕空間折射：基礎色、粗糙度、吸收 absorption、厚度 thickness、折射率 ior、穿透率 transmission、清漆 |
| 陰影接收材質 | ShadowCatcher | 強度 | 只顯示陰影的透明地面 |
| 無光照材質 | UnlitMaterial | 參數 | 基礎色 × 亮度 intensity（可產生泛光） |
| 建立材質實例 | CreateMaterialInstance | 材質, 參數 | 材質：內建名稱 `"lit"`、`"lit_transparent"`、`"refraction"`、`"shadow"`、`"unlit"`，或 `載入材質` 的代碼 |
| 載入材質 | LoadMaterial | 路徑 | 自訂 `matc` 編譯的 `.filamat` |
| 設定材質參數 / 設定自訂材質參數 | SetMaterialParams / SetCustomMaterialParams | 材質實例, 參數 | |
| 載入紋理 | LoadTexture | 路徑, srgb | `.ktx2`（BasisU）、`.ktx`、`.png`、`.jpg`；顏色紋理 srgb=真 |
| 紋理參數 | TextureParam | 紋理, 選項 | 包覆 wrap（`"重複"`/`"夾取"`）、濾波 filter（`"最近"`） |

### 幾何與模型 / Geometry and models

變換雜湊表的鍵：位置 position、旋轉 rotation（度，依序為 Y→X→Z）、縮放 scale（數字或陣列）、四元數 quaternion、投射陰影 castShadows、接收陰影 receiveShadows。

| 繁體中文 | English | 參數 |
| --- | --- | --- |
| 加入方塊 / 球體 / 平面 / 圓柱 / 圓錐 / 圓環 | AddBox / AddSphere / AddPlane / AddCylinder / AddCone / AddTorus | 尺寸… , 材質實例, 變換 |
| 加入形狀 | AddShape | 形狀名稱, 參數（含分段、UV縮放…）, 材質實例, 變換 |
| 加入幾何 | AddGeometry | 位置陣列, 索引陣列, 法線陣列, UV陣列, 材質實例, 變換 |
| 載入網格 | LoadMesh | `.filamesh` 路徑, 材質實例, 變換 |
| 載入模型 | LoadModel | glTF/GLB 路徑, 選項（變換鍵＋等待載入 wait、載入光源 lights）→ {代碼 id, 中心 center, 尺寸 size, 最小, 最大, 動畫 animations, 材質 materials} |
| 模型資訊 / 播放動畫 / 設定模型自發光 | ModelInfo / Animate / SetModelEmissive | 模型 / 模型, 秒數, 索引 / 模型, 倍率 |
| 加入群組 / 設定父物件 | AddGroup / SetParent | 變換 / 物件, 父物件 |
| 設定變換 / 設定位置 / 設定旋轉 / 設定縮放 | SetTransform / SetPosition / SetRotation / SetScale | 物件, 值 |
| 設定可見 / 設定陰影 / 套用材質 / 移除 / 清除場景 | SetVisible / SetShadows / ApplyMaterial / Remove / ClearScene | |

glTF 支援 Filament gltfio 的全部功能：KTX2/BasisU、WebP、meshopt、Draco（桌面版）、蒙皮、形變、動畫、
`KHR_lights_punctual`、`KHR_materials_*`（clearcoat、transmission、volume、sheen、emissive_strength…）。

## 架構 / Architecture

```
星泉程式 ─ main.aqua（雙語 API、指令佇列、雙語選項轉換）
   │  DynamicLibrary.Call("fl_call", JSON {"cmds":[…]})
   ├─ Windows/Linux：aqua_filament.dll/.so → 請求交給專屬算繪執行緒 → 原生視窗＋Filament Engine
   └─ Web：aqua_filament.mjs → filament.js（WebGL 2）＋ <canvas>（#surfaces）
```

原生端由專屬執行緒持有視窗與 Filament 的所有物件，所以不論星泉從哪一條主機執行緒呼叫都安全。
星泉程式忙碌時，這條執行緒仍會繼續處理視窗訊息，視窗不會停止回應。模組會固定在記憶體中，主機卸載
函式庫時不會出問題。資產路徑可以是 `目前工作目錄 + "/assets/…"`、相對於應用程式根目錄的路徑，或
`/bottles/…`。

- **桌面：** 原生端從主機解開資產的暫存資料夾直接讀檔，支援含中文等非 ASCII 字元的使用者路徑。
- **網頁：** 依 `program.json` 對應的 `asset-*.bin` 下載資產。

The native runtime keeps the window and every Filament object on one dedicated thread, so it does not matter
which host thread runs Aquarius code. That thread keeps pumping window messages while the program is busy, and
the module is pinned in memory so the host can safely unload the library. Asset paths can be
`currWorkingDir + "/assets/…"`, paths relative to the application root, or `/bottles/…`.

- **Desktop:** files are read straight from the host's extracted asset folder, including user profile paths
  with non-ASCII characters.
- **Web:** assets are fetched as the `asset-*.bin` files listed in `program.json`.

## 建置原生執行階段 / Building the runtimes

| 目標 | 指令 |
| --- | --- |
| Windows（在 Windows 上，VS 2022+） | `.\native\build\build-windows.ps1 -FilamentDir C:\…\filament-v1.78.0-windows` |
| Windows（在 Linux 上交叉編譯） | `FILAMENT_WINDOWS=… MSVC=…/VC/Tools/MSVC/14.xx WINSDK=…/Windows\ Kits/10 native/build/build-windows-cross.sh`（clang-cl＋lld-link） |
| Linux | `FILAMENT_LINUX=…/filament-v1.78.0-linux/filament LIBCXX=…/llvm-18 native/build/build-linux.sh` |
| Web | `node web/build.mjs …/filament-v1.78.0-web` |
| 內建材質 | `matc -a opengl -a vulkan -p all -o materials/lit.filamat native/materials/lit.mat`（共五個） |

官方 Linux 版 Filament 以 libc++ 與 glibc 2.38 的標頭建置。`native/src/compat_glibc.c` 補上
`__isoc23_*` 與 `fmod` 的本地實作，因此產生的 `.so` 在 Ubuntu 22.04（glibc 2.35）也能載入。

## 測試 / Tests

- `tests/smoke/`：建立場景、算繪 20 幀、截圖並印出 `AQUA_FILAMENT_SMOKE_OK`。
- `tests/native_driver.py`：以與主機相同的 ABI 直接驅動原生函式庫。
- `tests/verify_camera.py`：以像素驗證 Windows DPI 不感知主機的視野、置中與三種控制模式；需要 Pillow。

已驗證：Linux（Xvfb＋Mesa llvmpipe）上的原生 `.so` 直接測試與完整星泉程式；Chromium（SwiftShader
WebGL 2）上的網頁版。Windows 1.0.1 已在 250% 顯示縮放下驗證固定、軌道與飛行相機。
WGL 驅動執行緒與視窗執行緒使用相同 DPI 模式，避免只顯示視口一角。

## 授權 / License

Aqua_Filament 的程式碼採 Apache-2.0。第三方元件的授權見 `licenses/`，包括 Filament、abseil、basisu、
cgltf、draco、meshoptimizer、stb、zstd、smol-v、robin-map、dawn、spirv-cross、perfetto、nlohmann/json、
LLVM libc++，以及隨附的 Microsoft VC++ 可再發行元件。
