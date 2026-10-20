# Filament WebGL compatibility fix — 2026-10-21

The shared **Aqua_Filament 1.0.2** web runtime fixes the context loss reproduced on Intel Iris Xe through Chrome's default ANGLE/Direct3D 11 backend. The rebuilt showcase renders all five scenes with the browser's default backend. Its launcher no longer forces Vulkan.

## Cause and change

The browser GPU process stopped with a native bounds failure, followed by the misleading empty `base_lit_masked` link error. The captured Chrome 155.0.8059.84 GPU-process dump stops at `chrome.dll + 0x6032dc1` with `0xC000001D`. The instructions and arguments match ANGLE's `StateManager11::setShaderResourceInternal`: fragment shader, resource slot 16, and a shader-resource cache containing 16 entries.

ANGLE can translate a std140 block containing a single large array into a Direct3D StructuredBuffer, using shader-resource slots above the texture slots. That path hit the resource-cache bounds check here. Capturing and compiling the shader pair independently did not reproduce a GLSL link error; the reported link failure followed the GPU-process failure.

The library keeps these four blocks as constant buffers by declaring the last array element as a second field in the same std140 block:

| Block | Original array | Original and patched block bytes |
| --- | --- | ---: |
| FroxelsUniforms | uvec4[2048] | 32,768 |
| FroxelRecordUniforms | uvec4[4096] | 65,536 |
| LightsUniforms | mat4[255] | 16,320 |
| ShadowUniforms | ShadowData[128] | 16,384 |

Indexed reads use an accessor that returns the final field for the last index. Array length expressions retain the original count. Data offsets, strides, buffer sizes and contents remain compatible with Filament's existing CPU uploads. No scene, light, shadow or effect was removed.

The adapter installs this transformation only on its own WebGL context, before Filament creates shaders. Direct3D and contexts whose renderer details are unavailable receive the compatibility path. Known Vulkan, Metal, OpenGL and SwiftShader contexts retain their original optimizer path. No browser settings or global WebGL prototypes are changed. Native Windows/Linux library binaries are unchanged.

The implementation is in the sibling library's `web/src/shader-compat.mjs`; `web/build.mjs` includes it in `runtimes/web/aqua_filament.mjs`. The web adapter and package version are 1.0.2. The showcase now requires >=1.0.2 and its web exports and Windows executable were rebuilt.

## Validation

All runs used the final generated runtime. Five scenes were captured in every run: Bistro night street, parking-garage materials, street materials, Flight Helmet, and refraction.

| Browser run | Actual renderer | Scene captures | Context losses |
| --- | --- | ---: | ---: |
| Chrome, default, shader disk cache disabled | Intel Iris Xe, ANGLE/Direct3D 11 | 5 | 0 |
| Edge, default, shader disk cache disabled | NVIDIA RTX 4070 Laptop, ANGLE/Direct3D 11 | 5 | 0 |
| Chrome, Vulkan diagnostic run | NVIDIA RTX 4070 Laptop, Vulkan | 5 | 0 |
| Chrome, software diagnostic run | SwiftShader, Vulkan | 5 | 0 |
| Chrome, repeated default run | Intel Iris Xe, ANGLE/Direct3D 11 | 5 | 0 |

All 25 captures passed completion-marker, runtime-error and context-loss checks. The Intel Bistro and Flight Helmet images were also visually inspected.

Six source tests passed. The GPU regression compiled both the original and transformed declarations, compared all four block sizes, uploaded data in the original CPU layout, and verified seven indices per block through rendered pixel values. It covers the first, middle, penultimate and last element, with 28 reads per browser/backend configuration. The first four matrix configurations passed **112 pixel checks** in total.

The current compiler also passed all five native scenes in both Wasm-hosted and standalone Windows EXE execution. The extended compatibility report still contains 16 passed checks across its seven projects. A final independent run against the main compatibility web export passed on default Intel Direct3D with five captures and no context loss.

Both exported web modules match the shared library runtime byte for byte. Its SHA-256 is:

```text
33a930f4c4f08aa671eaf838460d3cb0acf4fa341ec149227817af532dc40794
```

Evidence in the Aquarius checkout:

- `.web-build/filament-intel/matrix/results.json`: all five browser runs and actual renderers.
- `.web-build/filament-intel/matrix/<run>/scenes.json`, logs and five PNGs per run.
- `.web-build/filament-intel/matrix/<run>/uniform-layout.json`: GPU buffer sizes and tested indices for the first four runs.
- `.web-build/filament-intel/native-final.log` and `.web-build/compatibility/extended-results.json`: native results.
- `.web-build/filament-intel/default-export-final.log` and `.web-build/compatibility/browser-results.json`: independent default Intel browser result.
- `.web-build/filament-intel/crash-dump/gpu.dmp`, the GPU crash log and retained ANGLE source files: original diagnosis.

The ONNX delivery's `reports/filament-webgl/` contains portable JSON evidence and the default Intel captures. Its compatibility report and `VALIDATION.md` have been refreshed to remove the obsolete unresolved-Intel limitation.

## Updating existing projects

Require Aqua_Filament >=1.0.2 in the application manifest, then update the dependency lock and rebuild:

```powershell
aqua bottle update --project . --target web
aqua build . --target web -o dist/web
```

The compatibility implementation is included automatically in web exports that resolve this library version. Existing exports must be rebuilt; the installed browser does not need modification.

Library checks, from `Aqua_Filament`:

```powershell
node --test web/src/shader-compat.test.mjs
node tests/web/verify-uniform-layout.mjs
```

The GPU test needs Playwright; `AQUARIUS_PLAYWRIGHT` can name its absolute `index.mjs` path. It defaults to the adjacent Aquarius checkout's browser dependency. `WEBGL_BROWSER=chrome` or `msedge` selects the installed browser. Leave `WEBGL_ANGLE` unset for default-backend verification; optional diagnostic values are `vulkan` and `swiftshader`.

From the showcase, `node tests/verify_web.cjs` verifies all five scenes. Set `NODE_PATH` to the Playwright installation if necessary. From the Aquarius checkout, `scripts/test-extended-projects.ps1 -ProjectNames filament` runs the native checks, followed by `node scripts/test-compatibility-browser.mjs filament` for the exported browser scenario.

These checks cover the installed Windows browsers, the two GPUs above and software rendering. They do not establish compatibility with every GPU, driver, browser or operating system. The showcase still has substantial asset, memory and GPU requirements, particularly for Bistro.
