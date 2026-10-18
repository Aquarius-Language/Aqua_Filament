// Aqua_Filament web adapter (WebGL 2 via the official filament.js 1.78.0 WebAssembly build).
// Implements the same JSON command protocol as the native Windows/Linux library, exported through
// the AquariusLang DynamicLibrary ES-module factory ABI 1 (`aqua_library_abi`, `aqua_create_library`).
// `Filament` and `FILAMENT_WASM_BASE64` are provided by the bundle prologue (see web/build.mjs).

const VERSION = '1.0.0', FILAMENT_VERSION = '1.78.0';
const encoder = new TextEncoder(), decoder = new TextDecoder();
const fail = message => { throw new Error(message); };
const num = (o, k, d) => (o && o[k] !== undefined && o[k] !== null) ? (typeof o[k] === 'boolean' ? (o[k] ? 1 : 0) : (typeof o[k] === 'number' ? o[k] : fail(`Expected a number for '${k}' / 欄位必須是數字`))) : d;
const flag = (o, k, d) => (o && o[k] !== undefined && o[k] !== null) ? !!o[k] : d;
const has = (o, k) => o && o[k] !== undefined && o[k] !== null;
const text = (o, k, d = '') => (o && typeof o[k] === 'string') ? o[k] : d;
const vec = (o, k, d) => {
  if (!has(o, k)) return d;
  const v = o[k];
  if (!Array.isArray(v) || v.some(x => typeof x !== 'number')) fail(`Expected a number array for '${k}' / 欄位必須是數字陣列`);
  return v;
};
const idOf = (o, k = 'id') => Number.isInteger(o?.[k]) ? o[k] : fail(`Missing integer '${k}' / 缺少整數代碼`);
const normalize3 = v => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
const DEG = Math.PI / 180;

// ---------------------------------------------------------------------------------------------
// Math (column-major 4x4)
// ---------------------------------------------------------------------------------------------
function quatAxis(axis, angle) { const s = Math.sin(angle / 2); return [axis[0] * s, axis[1] * s, axis[2] * s, Math.cos(angle / 2)]; }
function quatMul(a, b) {
  return [a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]];
}
function euler(e) { // yaw (Y), then pitch (X), then roll (Z) — identical to the native library
  return quatMul(quatMul(quatAxis([0, 1, 0], e[1] * DEG), quatAxis([1, 0, 0], e[0] * DEG)), quatAxis([0, 0, 1], e[2] * DEG));
}
function compose(t, q, s) {
  const [x, y, z, w] = q;
  const m = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0,
    2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
    2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0, t[0], t[1], t[2], 1];
  for (let i = 0; i < 3; i++) { m[i] *= s[0]; m[4 + i] *= s[1]; m[8 + i] *= s[2]; }
  return m;
}

// ---------------------------------------------------------------------------------------------
// Built-in material parameter types (JS has no parameter introspection; Filament aborts on mismatch)
// ---------------------------------------------------------------------------------------------
const LIT = {baseColor: 4, metallic: 1, roughness: 1, reflectance: 1, clearCoat: 1, clearCoatRoughness: 1, occlusion: 1,
  normalScale: 1, emissive: 3, emissiveIntensity: 1, uvScale: 2, baseColorMap: 's', normalMap: 's', ormMap: 's'};
const BUILTIN_PARAMS = {
  lit: LIT, lit_transparent: LIT,
  refraction: {baseColor: 3, roughness: 1, metallic: 1, clearCoat: 1, clearCoatRoughness: 1, absorption: 3, thickness: 1,
    ior: 1, transmission: 1, emissive: 3, emissiveIntensity: 1},
  shadow: {strength: 1},
  unlit: {baseColor: 4, intensity: 1, uvScale: 2, baseColorMap: 's'}
};
const BUILTIN_DEFAULTS = {
  lit: {baseColor: [0.8, 0.8, 0.8, 1], metallic: 0, roughness: 0.5, reflectance: 0.5, clearCoat: 0, clearCoatRoughness: 0.1,
    occlusion: 1, normalScale: 1, emissive: [0, 0, 0], emissiveIntensity: 0, uvScale: [1, 1]},
  refraction: {baseColor: [1, 1, 1], roughness: 0.05, metallic: 0, clearCoat: 0, clearCoatRoughness: 0.1, absorption: [0, 0, 0],
    thickness: 0.5, ior: 1.5, transmission: 1, emissive: [0, 0, 0], emissiveIntensity: 0},
  shadow: {strength: 0.5},
  unlit: {baseColor: [1, 1, 1, 1], intensity: 1, uvScale: [1, 1]}
};
BUILTIN_DEFAULTS.lit_transparent = BUILTIN_DEFAULTS.lit;

// ---------------------------------------------------------------------------------------------
// Package assets: the website stores every asset as asset-<sha256>.bin, mapped by program.json.
// ---------------------------------------------------------------------------------------------
let manifestPromise = null;
function manifest() {
  if (!manifestPromise) {
    manifestPromise = fetch(new URL('program.json', import.meta.url)).then(r => r.ok ? r.json() : fail('program.json missing'))
      .then(m => { const fold = new Map(); for (const [k, v] of Object.entries(m.assets || {})) fold.set(k.toUpperCase(), v); return fold; });
  }
  return manifestPromise;
}
function packageKey(path, appRoot) {
  let p = String(path).replaceAll('\\', '/');
  if (appRoot && appRoot !== '/' && p.toLowerCase().startsWith(appRoot.toLowerCase() + '/')) p = p.slice(appRoot.length + 1);
  const out = [];
  for (const part of p.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') { if (!out.length) fail('Path escapes the package / 路徑超出套件'); out.pop(); } else out.push(part);
  }
  return out.join('/');
}
async function readAsset(path, appRoot) {
  const key = packageKey(path, appRoot);
  const map = await manifest();
  const file = map.get(key.toUpperCase());
  if (!file) fail(`File not found / 找不到檔案: ${path}`);
  const response = await fetch(new URL(file, import.meta.url));
  if (!response.ok) fail(`Cannot download / 無法下載: ${path}`);
  return new Uint8Array(await response.arrayBuffer());
}

// ---------------------------------------------------------------------------------------------
// Filament module bootstrap (wasm embedded in the bundle)
// ---------------------------------------------------------------------------------------------
let filamentReady = null;
function loadFilament() {
  if (!filamentReady) {
    filamentReady = (async () => {
      const binary = Uint8Array.from(atob(FILAMENT_WASM_BASE64), c => c.charCodeAt(0));
      // __aquaInit (added by web/build.mjs) instantiates the module and installs Filament's JS extensions.
      Filament = await Filament.__aquaInit(binary);
      return Filament;
    })();
  }
  return filamentReady;
}

// ---------------------------------------------------------------------------------------------
// Geometry (same shapes as the native library)
// ---------------------------------------------------------------------------------------------
function makeShape(c) {
  const shape = text(c, 'shape', 'box').toLowerCase();
  const P = [], N = [], U = [], I = [];
  const quad = (o, u, v, n, s) => {
    const b = P.length / 3;
    P.push(...o, o[0] + u[0], o[1] + u[1], o[2] + u[2], o[0] + u[0] + v[0], o[1] + u[1] + v[1], o[2] + u[2] + v[2], o[0] + v[0], o[1] + v[1], o[2] + v[2]);
    for (let i = 0; i < 4; i++) N.push(...n);
    U.push(0, 0, s[0], 0, s[0], s[1], 0, s[1]);
    I.push(b, b + 1, b + 2, b, b + 2, b + 3);
  };
  if (shape === 'box' || shape === 'cube') {
    const sz = vec(c, 'size', [1, 1, 1]), s = [sz[0] / 2, sz[1] / 2, sz[2] / 2], one = [1, 1];
    quad([-s[0], -s[1], s[2]], [2 * s[0], 0, 0], [0, 2 * s[1], 0], [0, 0, 1], one);
    quad([s[0], -s[1], -s[2]], [-2 * s[0], 0, 0], [0, 2 * s[1], 0], [0, 0, -1], one);
    quad([s[0], -s[1], s[2]], [0, 0, -2 * s[2]], [0, 2 * s[1], 0], [1, 0, 0], one);
    quad([-s[0], -s[1], -s[2]], [0, 0, 2 * s[2]], [0, 2 * s[1], 0], [-1, 0, 0], one);
    quad([-s[0], s[1], s[2]], [2 * s[0], 0, 0], [0, 0, -2 * s[2]], [0, 1, 0], one);
    quad([-s[0], -s[1], -s[2]], [2 * s[0], 0, 0], [0, 0, 2 * s[2]], [0, -1, 0], one);
  } else if (shape === 'plane') {
    const w = num(c, 'width', 10), d = num(c, 'depth', 10), t = num(c, 'uvTiles', 1);
    quad([-w / 2, 0, d / 2], [w, 0, 0], [0, 0, -d], [0, 1, 0], [t, t]);
  } else if (shape === 'sphere') {
    const r = num(c, 'radius', 0.5), seg = Math.min(512, Math.max(3, num(c, 'segments', 48) | 0)), rings = Math.min(512, Math.max(2, num(c, 'rings', seg / 2) | 0));
    for (let y = 0; y <= rings; y++) {
      const v = y / rings, phi = v * Math.PI;
      for (let x = 0; x <= seg; x++) {
        const u = x / seg, th = u * 2 * Math.PI, n = [-Math.cos(th) * Math.sin(phi), Math.cos(phi), Math.sin(th) * Math.sin(phi)];
        P.push(n[0] * r, n[1] * r, n[2] * r); N.push(...n); U.push(u, 1 - v);
      }
    }
    for (let y = 0; y < rings; y++) for (let x = 0; x < seg; x++) { const a = y * (seg + 1) + x, b = a + seg + 1; I.push(a, b, a + 1, b, b + 1, a + 1); }
  } else if (shape === 'cylinder' || shape === 'cone') {
    const r = num(c, 'radius', 0.5), h = num(c, 'height', 1), top = shape === 'cone' ? 0 : num(c, 'topRadius', r);
    const seg = Math.min(512, Math.max(3, num(c, 'segments', 48) | 0)), slope = (r - top) / h;
    for (let x = 0; x <= seg; x++) {
      const u = x / seg, a = u * 2 * Math.PI, cx = Math.cos(a), sz = Math.sin(a), n = normalize3([cx, slope, sz]);
      P.push(cx * r, -h / 2, sz * r, cx * top, h / 2, sz * top); N.push(...n, ...n); U.push(u, 0, u, 1);
    }
    for (let x = 0; x < seg; x++) { const a = x * 2; I.push(a, a + 1, a + 2, a + 1, a + 3, a + 2); }
    for (let cap = 0; cap < 2; cap++) {
      const y = cap ? h / 2 : -h / 2, rr = cap ? top : r;
      if (rr <= 0) continue;
      const n = [0, cap ? 1 : -1, 0], center = P.length / 3;
      P.push(0, y, 0); N.push(...n); U.push(0.5, 0.5);
      for (let x = 0; x <= seg; x++) { const a = x / seg * 2 * Math.PI; P.push(Math.cos(a) * rr, y, Math.sin(a) * rr); N.push(...n); U.push(0.5 + 0.5 * Math.cos(a), 0.5 + 0.5 * Math.sin(a)); }
      for (let x = 0; x < seg; x++) { const a = center + 1 + x; if (cap) I.push(center, a + 1, a); else I.push(center, a, a + 1); }
    }
  } else if (shape === 'torus') {
    const R = num(c, 'radius', 0.5), r = num(c, 'tube', 0.15), seg = Math.min(512, Math.max(3, num(c, 'segments', 64) | 0)), sides = Math.min(256, Math.max(3, num(c, 'sides', 24) | 0));
    for (let i = 0; i <= seg; i++) {
      const u = i / seg * 2 * Math.PI;
      for (let j = 0; j <= sides; j++) {
        const v = j / sides * 2 * Math.PI, n = [Math.cos(u) * Math.cos(v), Math.sin(v), Math.sin(u) * Math.cos(v)];
        P.push(Math.cos(u) * R + n[0] * r, n[1] * r, Math.sin(u) * R + n[2] * r); N.push(...n); U.push(i / seg * 4, j / sides);
      }
    }
    for (let i = 0; i < seg; i++) for (let j = 0; j < sides; j++) { const a = i * (sides + 1) + j, b = a + sides + 1; I.push(a, a + 1, b, b, a + 1, b + 1); }
  } else fail(`Unknown shape / 未知的形狀: ${shape}`);
  if (has(c, 'uvScale')) { const s = vec(c, 'uvScale', [1, 1]); for (let i = 0; i < U.length; i += 2) { U[i] *= s[0]; U[i + 1] *= s[1]; } }
  return {positions: P, normals: N, uvs: U, indices: I};
}

// ---------------------------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------------------------
const KEY_NAMES = {' ': 'space', Enter: 'enter', Escape: 'escape', Tab: 'tab', Backspace: 'backspace', Delete: 'delete',
  ArrowLeft: 'left', ArrowRight: 'right', ArrowUp: 'up', ArrowDown: 'down', Home: 'home', End: 'end', PageUp: 'pageup',
  PageDown: 'pagedown', Shift: 'shift', Control: 'control', Alt: 'alt'};
function keyName(e) {
  if (KEY_NAMES[e.key]) return KEY_NAMES[e.key];
  if (/^F([1-9]|1[0-2])$/.test(e.key)) return e.key.toLowerCase();
  if (/^Key[A-Z]$/.test(e.code)) return e.code.slice(3).toLowerCase();
  if (/^Digit[0-9]$/.test(e.code)) return e.code.slice(5);
  if (/^Numpad[0-9]$/.test(e.code)) return e.code.slice(6);
  return ['-', '=', ',', '.', '/', '[', ']'].includes(e.key) ? e.key : null;
}

class Runtime {
  constructor() { this.objects = new Map(); this.events = []; this.builtins = new Map(); this.queue = Promise.resolve(); }

  call(bytes) {
    // Commands are serialized: each batch waits for the previous one.
    const run = this.queue.then(() => this.execute(JSON.parse(decoder.decode(bytes))));
    this.queue = run.catch(() => {});
    return run.then(r => encoder.encode(JSON.stringify(r)));
  }

  async execute(request) {
    const cmds = request?.cmds;
    if (!Array.isArray(cmds)) return {ok: false, error: 'Expected {"cmds":[...]} / 需要指令陣列'};
    const results = [];
    let index = 0;
    try {
      for (; index < cmds.length; index++) {
        const r = await this.dispatch(cmds[index]);
        if (r !== undefined && r !== null) results.push(r);
      }
      return {ok: true, results};
    } catch (e) {
      return {ok: false, error: String(e?.message ?? e), index, op: cmds[index]?.op ?? '?', results};
    }
  }

  async dispatch(c) {
    if (!c || typeof c !== 'object') fail('Each command must be an object / 每個指令必須是物件');
    const op = c.op;
    if (op === 'init') return this.init(c);
    if (op === 'info') return {version: VERSION, filament: FILAMENT_VERSION, platform: 'web', open: !!this.engine};
    if (!this.engine) fail(`Call Open / 開啟視窗 first (op: ${op})`);
    switch (op) {
      case 'frame': return this.frame(c);
      case 'events': { const events = this.events; this.events = []; return {events}; }
      case 'title': document.title = text(c, 'text'); return null;
      case 'close': this.shutdown(); return null;
      case 'camera': this.eye = vec(c, 'eye', this.eye); this.target = vec(c, 'target', this.target); this.up = vec(c, 'up', this.up);
        if (this.manipulator && flag(c, 'syncControl', true)) this.rebuildManipulator(); return null;
      case 'lens': return this.lens(c);
      case 'exposure':
        if (has(c, 'ev100')) this.camera.setExposure(1, Math.pow(2, -num(c, 'ev100', 15)), 100);
        else this.camera.setExposure(num(c, 'aperture', 16), num(c, 'shutter', 1 / 125), num(c, 'iso', 100));
        return null;
      case 'cameraControl': return this.cameraControl(c);
      case 'cameraState': return {eye: this.eye, target: this.target, up: this.up, fov: this.fov, near: this.near, far: this.far, width: this.width, height: this.height};
      case 'view': return this.viewOptions(c);
      case 'environment': return this.environment(c);
      case 'skybox': return this.skybox(c);
      case 'light': return this.light(c, true);
      case 'setLight': return this.light(c, false);
      case 'material': return this.material(c);
      case 'instance': return this.instance(c);
      case 'params': { const o = this.get(idOf(c)); if (o.kind !== 'instance') fail('Expected a material instance / 需要材質實例'); this.setParams(o, c.params || {}); return null; }
      case 'texture': return this.texture(c);
      case 'shape': return this.shape(c);
      case 'geometry': return this.geometry(c);
      case 'mesh': return this.mesh(c);
      case 'model': return this.model(c);
      case 'modelInfo': return this.modelInfo(idOf(c), this.model3(c));
      case 'animate': return this.animate(c);
      case 'emissive': return this.emissive(c);
      case 'group': { const id = idOf(c); const o = this.create(id, 'entity'); o.entity = Filament.EntityManager.get().create(); this.engine.getTransformManager().create(o.entity); this.applyTransform(o.entity, c); return null; }
      case 'transform': return this.transform(c);
      case 'parent': return this.parent(c);
      case 'visible': return this.visible(c);
      case 'shadows': return this.shadows(c);
      case 'setMaterial': return this.setMaterial(c);
      case 'remove': this.remove(idOf(c)); return null;
      case 'clear': this.clear(flag(c, 'keepEnvironment', false)); return null;
      case 'screenshot': return this.screenshot(c);
      case 'stats': return this.stats();
      default: fail(`Unknown command / 未知的指令: ${op}`);
    }
  }

  async init(c) {
    if (this.engine) fail('Filament window is already open / 視窗已開啟');
    await loadFilament();
    this.appRoot = String(c.appRoot || '').replaceAll('\\', '/').replace(/\/+$/, '');
    this.materialDir = text(c, 'materials', '/bottles/Aqua_Filament/materials');
    const host = document.getElementById('surfaces') || document.body;
    const canvas = document.createElement('canvas');
    canvas.tabIndex = 0;
    canvas.style.cssText = 'display:block;width:100%;height:100%;outline:none;touch-action:none';
    host.appendChild(canvas);
    this.canvas = canvas;
    if (c.title) document.title = c.title;
    // preserveDrawingBuffer keeps screenshots possible; pass {"preserveDrawingBuffer": false} to Open to opt out.
    this.engine = Filament.Engine.create(canvas, {antialias: false, alpha: false, preserveDrawingBuffer: flag(c, 'preserveDrawingBuffer', true)});
    this.swapChain = this.engine.createSwapChain();
    this.renderer = this.engine.createRenderer();
    this.scene = this.engine.createScene();
    this.view = this.engine.createView();
    this.cameraEntity = Filament.EntityManager.get().create();
    this.camera = this.engine.createCamera(this.cameraEntity);
    this.view.setCamera(this.camera);
    this.view.setScene(this.scene);
    this.camera.setExposure(16, 1 / 125, 100);
    this.fov = num(c, 'fov', 45); this.near = num(c, 'near', 0.05); this.far = num(c, 'far', 1000); this.fovHorizontal = false;
    this.eye = [0, 1.5, 5]; this.target = [0, 0.5, 0]; this.up = [0, 1, 0];
    this.renderer.setClearOptions({clearColor: [0.03, 0.04, 0.06, 1], clear: true});
    this.skyboxObject = Filament.Skybox.Builder().color([0.03, 0.04, 0.06, 1]).build(this.engine);
    this.scene.setSkybox(this.skyboxObject);
    this.view.setAntiAliasing(Filament.View$AntiAliasing.FXAA);
    this.view.setShadowType(Filament.View$ShadowType.PCF);
    this.white = this.solid([255, 255, 255, 255], true);
    this.flatNormal = this.solid([128, 128, 255, 255], false);
    this.defaultOrm = this.solid([255, 255, 255, 255], false);
    this.assetLoader = this.engine.createAssetLoader();
    this.resourceLoader = new Filament.gltfio$ResourceLoader(this.engine, true);
    this.stb = new Filament.gltfio$StbProvider(this.engine);
    this.ktx2 = new Filament.gltfio$Ktx2Provider(this.engine);
    this.resourceLoader.addStbProvider('image/png', this.stb);
    this.resourceLoader.addStbProvider('image/jpeg', this.stb);
    this.resourceLoader.addKtx2Provider('image/ktx2', this.ktx2);
    this.toneMapping = 'aces'; this.grade = {exposure: 0, contrast: 1, saturation: 1, vibrance: 1, whiteBalance: 0};
    this.loading = new Map();
    this.start = this.last = this.fpsWindow = performance.now();
    this.frames = 0; this.fpsFrames = 0; this.fps = 0;
    this.attachInput();
    this.resize(true);
    return {backend: 'webgl2', width: this.width, height: this.height, resourceRoot: '', version: VERSION, filament: FILAMENT_VERSION};
  }

  solid(rgba, srgb) {
    const F = Filament;
    const t = F.Texture.Builder().width(1).height(1).levels(1).sampler(F.Texture$Sampler.SAMPLER_2D)
      .format(srgb ? F.Texture$InternalFormat.SRGB8_A8 : F.Texture$InternalFormat.RGBA8).build(this.engine);
    t.setImage(this.engine, 0, F.PixelBuffer(new Uint8Array(rgba), F.PixelDataFormat.RGBA, F.PixelDataType.UBYTE));
    return t;
  }

  attachInput() {
    const c = this.canvas, push = e => { this.events.push(e); if (this.events.length > 4096) this.events.splice(0, 2048); };
    const pos = e => { const r = c.getBoundingClientRect(), s = c.width / Math.max(1, r.width); return [Math.round((e.clientX - r.left) * s), Math.round((e.clientY - r.top) * s)]; };
    const button = b => b === 0 ? 0 : b === 2 ? 1 : 2;
    this.listeners = [
      [c, 'pointerdown', e => { c.focus(); c.setPointerCapture?.(e.pointerId); const [x, y] = pos(e); push({type: 'mouse', action: 'down', button: button(e.button), x, y}); }],
      [c, 'pointermove', e => { const [x, y] = pos(e); push({type: 'mouse', action: 'move', x, y}); }],
      [c, 'pointerup', e => { const [x, y] = pos(e); push({type: 'mouse', action: 'up', button: button(e.button), x, y}); }],
      [c, 'wheel', e => { e.preventDefault(); const [x, y] = pos(e); push({type: 'mouse', action: 'wheel', x, y, delta: e.deltaY < 0 ? 1 : -1}); }],
      [c, 'contextmenu', e => e.preventDefault()],
      [window, 'keydown', e => { if (e.repeat) return; const k = keyName(e); if (k) { push({type: 'key', key: k, down: true}); if (e.target === c || e.target === document.body) e.preventDefault(); } }],
      [window, 'keyup', e => { const k = keyName(e); if (k) push({type: 'key', key: k, down: false}); }],
      [window, 'blur', () => push({type: 'focus', focused: false})],
      [window, 'focus', () => push({type: 'focus', focused: true})]
    ];
    for (const [t, n, f] of this.listeners) t.addEventListener(n, f, {passive: n !== 'wheel'});
  }

  resize(force = false) {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(16, Math.round(this.canvas.clientWidth * dpr)), h = Math.max(16, Math.round(this.canvas.clientHeight * dpr));
    if (!force && w === this.width && h === this.height) return false;
    this.canvas.width = w; this.canvas.height = h; this.width = w; this.height = h;
    this.view.setViewport([0, 0, w, h]);
    this.applyProjection();
    this.manipulator?.setViewport(w, h);
    return true;
  }

  applyProjection() {
    this.camera.setProjectionFov(this.fov, this.width / Math.max(1, this.height), this.near, this.far,
      this.fovHorizontal ? Filament.Camera$Fov.HORIZONTAL : Filament.Camera$Fov.VERTICAL);
  }

  lens(c) {
    this.fov = num(c, 'fov', this.fov); this.near = num(c, 'near', this.near); this.far = num(c, 'far', this.far);
    if (has(c, 'fovDirection')) this.fovHorizontal = text(c, 'fovDirection').toLowerCase() === 'horizontal';
    if (this.fov <= 0 || this.fov >= 180 || this.near <= 0 || this.far <= this.near) fail('Invalid lens / 鏡頭參數無效');
    this.applyProjection();
    return null;
  }

  cameraControl(c) {
    const mode = text(c, 'mode', 'orbit').toLowerCase();
    this.control = c;
    if (mode === 'none' || mode === 'off') { this.manipulator?.delete?.(); this.manipulator = null; return null; }
    if (!['orbit', 'map', 'fly', 'free'].includes(mode)) fail(`Unknown camera control / 未知的相機控制模式: ${mode}`);
    if (has(c, 'eye')) this.eye = vec(c, 'eye', this.eye);
    if (has(c, 'target')) this.target = vec(c, 'target', this.target);
    this.wheelScale = num(c, 'wheelScale', 1);
    this.rebuildManipulator();
    return null;
  }

  rebuildManipulator() {
    const c = this.control || {}, mode = text(c, 'mode', 'orbit').toLowerCase(), F = Filament;
    const b = new F.Camutils$Manipulator$Builder();
    b.viewport(this.width, this.height).targetPosition(...this.target).upVector(...this.up).orbitHomePosition(...this.eye)
      .zoomSpeed(num(c, 'zoomSpeed', 0.05)).fovDegrees(this.fov).farPlane(this.far);
    const orbit = num(c, 'orbitSpeed', 0.01);
    b.orbitSpeed(orbit, orbit);
    this.manipulator?.delete?.();
    if (mode === 'fly' || mode === 'free') {
      const d = normalize3([this.target[0] - this.eye[0], this.target[1] - this.eye[1], this.target[2] - this.eye[2]]);
      const pan = num(c, 'panSpeed', 0.004);
      b.flightStartPosition(...this.eye).flightStartOrientation(Math.asin(Math.max(-1, Math.min(1, d[1]))), Math.atan2(-d[0], -d[2]))
        .flightMaxMoveSpeed(num(c, 'speed', 6)).flightMoveDamping(num(c, 'damping', 15)).flightPanSpeed(pan, pan);
      this.manipulator = b.build(F.Camutils$Mode.FREE_FLIGHT);
    } else {
      this.manipulator = b.build(mode === 'map' ? F.Camutils$Mode.MAP : F.Camutils$Mode.ORBIT);
    }
  }

  handleInput(e) {
    const m = this.manipulator;
    if (!m) return;
    if (e.type === 'mouse') {
      const x = e.x | 0, y = (this.height - (e.y | 0)) | 0;
      if (e.action === 'down') { m.grabBegin(x, y, e.button !== 0); this.grabbing = true; }
      else if (e.action === 'move' && this.grabbing) m.grabUpdate(x, y);
      else if (e.action === 'up' && this.grabbing) { m.grabEnd(); this.grabbing = false; }
      else if (e.action === 'wheel') m.scroll(x, y, -e.delta * (this.wheelScale || 1));
    } else if (e.type === 'key') {
      const K = Filament.Camutils$Key, map = {w: K.FORWARD, up: K.FORWARD, s: K.BACKWARD, down: K.BACKWARD, a: K.LEFT, left: K.LEFT, d: K.RIGHT, right: K.RIGHT, e: K.UP, q: K.DOWN};
      const k = map[e.key];
      if (k !== undefined) { if (e.down) m.keyDown(k); else m.keyUp(k); }
    } else if (e.type === 'focus' && !e.focused && this.grabbing) { m.grabEnd(); this.grabbing = false; }
  }

  async frame(c) {
    await new Promise(r => requestAnimationFrame(r));
    const events = this.events; this.events = [];
    for (const e of events) this.handleInput(e);
    if (this.resize()) events.push({type: 'resize', width: this.width, height: this.height});
    const now = performance.now(), dt = (now - this.last) / 1000;
    this.last = now;
    if (this.manipulator) {
      this.manipulator.update(Math.min(dt, 0.1));
      const eye = [0, 0, 0], target = [0, 0, 0], up = [0, 0, 0];
      this.manipulator.getLookAt(eye, target, up);
      this.eye = eye; this.target = target; this.up = up;
    }
    this.camera.lookAt(this.eye, this.target, this.up);
    this.updateLoading();
    this.renderer.render(this.swapChain, this.view);
    if (this.pendingShot) {
      this.pendingShot = false;
      const url = this.canvas.toDataURL('image/png');
      globalThis.__aquaFilamentScreenshot = url;
      (globalThis.__aquaFilamentScreenshots ??= []).push({path: this.shotPath, url});
    }
    this.frames++; this.fpsFrames++;
    if (now - this.fpsWindow >= 500) { this.fps = this.fpsFrames * 1000 / (now - this.fpsWindow); this.fpsFrames = 0; this.fpsWindow = now; }
    return {events, width: this.width, height: this.height, time: (now - this.start) / 1000, dt, frame: this.frames, fps: this.fps,
      rendered: true, open: true, loading: this.loading.size};
  }

  quality(q) { const Q = Filament.View$QualityLevel; return ({low: Q.LOW, high: Q.HIGH, ultra: Q.ULTRA})[String(q || '').toLowerCase()] ?? Q.MEDIUM; }

  viewOptions(c) {
    const v = this.view, F = Filament, on = (k) => typeof c[k] === 'object' ? flag(c[k], 'enabled', true) : flag(c, k, false);
    if (has(c, 'postProcessing')) v.setPostProcessingEnabled(flag(c, 'postProcessing', true));
    if (has(c, 'antiAliasing')) v.setAntiAliasing(['none', 'off'].includes(text(c, 'antiAliasing').toLowerCase()) ? F.View$AntiAliasing.NONE : F.View$AntiAliasing.FXAA);
    if (has(c, 'msaa')) { const s = num(c, 'msaa', 0) | 0; v.setMultiSampleAntiAliasingOptions({enabled: s > 1, sampleCount: Math.max(1, Math.min(8, s))}); }
    if (has(c, 'taa')) { const t = typeof c.taa === 'object' ? c.taa : {}; v.setTemporalAntiAliasingOptions({enabled: on('taa'), feedback: num(t, 'feedback', 0.12), filterWidth: num(t, 'filterWidth', 1)}); }
    if (has(c, 'bloom')) { const t = typeof c.bloom === 'object' ? c.bloom : {}; v.setBloomOptions({enabled: on('bloom'), strength: num(t, 'strength', 0.1), levels: num(t, 'levels', 6), threshold: flag(t, 'threshold', true), highlight: num(t, 'highlight', 1000), lensFlare: flag(t, 'lensFlare', false), starburst: flag(t, 'starburst', true), quality: this.quality(t.quality)}); }
    if (has(c, 'ssao')) { const t = typeof c.ssao === 'object' ? c.ssao : {}; v.setAmbientOcclusionOptions({enabled: on('ssao'), radius: num(t, 'radius', 0.3), intensity: num(t, 'intensity', 1), power: num(t, 'power', 1), quality: this.quality(t.quality || 'low'), bentNormals: flag(t, 'bentNormals', false)}); }
    if (has(c, 'ssr')) { const t = typeof c.ssr === 'object' ? c.ssr : {}; v.setScreenSpaceReflectionsOptions({enabled: on('ssr'), thickness: num(t, 'thickness', 0.1), maxDistance: num(t, 'maxDistance', 3), bias: num(t, 'bias', 0.01), stride: num(t, 'stride', 2)}); }
    if (has(c, 'dof')) {
      const t = typeof c.dof === 'object' ? c.dof : {};
      v.setDepthOfFieldOptions({enabled: on('dof'), cocScale: num(t, 'cocScale', 1), maxApertureDiameter: num(t, 'maxAperture', 0.01), nativeResolution: flag(t, 'nativeResolution', false)});
      if (has(t, 'focusDistance')) this.camera.setFocusDistance(num(t, 'focusDistance', 1));
    }
    if (has(c, 'fog')) { const t = typeof c.fog === 'object' ? c.fog : {}; v.setFogOptions({enabled: on('fog'), distance: num(t, 'distance', 0), density: num(t, 'density', 0.1), height: num(t, 'height', 0), heightFalloff: num(t, 'heightFalloff', 1), maximumOpacity: num(t, 'maximumOpacity', 1), color: vec(t, 'color', [1, 1, 1]), fogColorFromIbl: flag(t, 'fromIbl', false), inScatteringSize: num(t, 'inScatteringSize', -1), inScatteringStart: num(t, 'inScatteringStart', 0)}); }
    if (has(c, 'vignette')) { const t = typeof c.vignette === 'object' ? c.vignette : {}; v.setVignetteOptions({enabled: on('vignette'), midPoint: num(t, 'midPoint', 0.5), roundness: num(t, 'roundness', 0.5), feather: num(t, 'feather', 0.5)}); }
    if (has(c, 'shadowType')) {
      const s = text(c, 'shadowType').toLowerCase(), T = F.View$ShadowType;
      v.setShadowType(s === 'vsm' ? T.VSM : s === 'dpcf' ? T.DPCF : s === 'pcss' ? T.PCSS : T.PCF);
      if (s === 'vsm') v.setVsmShadowOptions({anisotropy: 0, mipmapping: false, highPrecision: false, lightBleedReduction: 0.2});
    }
    if (has(c, 'softShadows')) { const t = typeof c.softShadows === 'object' ? c.softShadows : {}; v.setSoftShadowOptions({penumbraScale: num(t, 'penumbraScale', 1), penumbraRatioScale: num(t, 'penumbraRatioScale', 1)}); }
    if (has(c, 'shadows')) v.setShadowingEnabled(flag(c, 'shadows', true));
    if (has(c, 'dynamicResolution')) { const t = typeof c.dynamicResolution === 'object' ? c.dynamicResolution : {}; v.setDynamicResolutionOptions({enabled: on('dynamicResolution'), minScale: [num(t, 'minScale', 0.5), num(t, 'minScale', 0.5)], maxScale: [num(t, 'maxScale', 1), num(t, 'maxScale', 1)], quality: this.quality(t.quality)}); }
    if (['toneMapper', 'exposureBias', 'contrast', 'saturation', 'vibrance', 'whiteBalance'].some(k => has(c, k))) {
      if (has(c, 'toneMapper')) this.toneMapping = text(c, 'toneMapper').toLowerCase();
      if (has(c, 'exposureBias')) this.grade.exposure = num(c, 'exposureBias', 0);
      for (const k of ['contrast', 'saturation', 'vibrance', 'whiteBalance']) if (has(c, k)) this.grade[k] = num(c, k, 1);
      const TM = F.ColorGrading$ToneMapping;
      // filament.js exposes LINEAR/ACES_LEGACY/ACES/FILMIC; AgX and PBR Neutral fall back to ACES on the web.
      const tm = {linear: TM.LINEAR, aces_legacy: TM.ACES_LEGACY, acesLegacy: TM.ACES_LEGACY, filmic: TM.FILMIC}[this.toneMapping] ?? TM.ACES;
      const grading = F.ColorGrading.Builder().toneMapping(tm).exposure(this.grade.exposure).contrast(this.grade.contrast)
        .saturation(this.grade.saturation).vibrance(this.grade.vibrance).whiteBalance(this.grade.whiteBalance, 0).build(this.engine);
      v.setColorGrading(grading);
      if (this.colorGrading) this.engine.destroyColorGrading?.(this.colorGrading);
      this.colorGrading = grading;
    }
    if (has(c, 'clearColor')) { const col = vec(c, 'clearColor', [0, 0, 0, 1]); this.renderer.setClearOptions({clearColor: [col[0], col[1], col[2], col[3] ?? 1], clear: true}); }
    return null;
  }

  async environment(c) {
    const intensity = num(c, 'intensity', 30000);
    if (has(c, 'ibl')) {
      if (/\.hdr$/i.test(c.ibl)) fail('Equirectangular .hdr environments must be converted with cmgen to .ktx / 請先以 cmgen 轉換 .hdr 為 .ktx');
      this.destroyEnvironment();
      const ibl = await readAsset(c.ibl, this.appRoot);
      this.indirectLight = this.engine.createIblFromKtx1(ibl);
      this.iblTexture = this.indirectLight.getReflectionsTexture();
      this.indirectLight.setIntensity(intensity);
      this.scene.setIndirectLight(this.indirectLight);
      if (has(c, 'skybox')) {
        const sky = await readAsset(c.skybox, this.appRoot);
        this.skyTexture = this.engine.createTextureFromKtx1(sky);
        this.replaceSkybox(Filament.Skybox.Builder().environment(this.skyTexture).showSun(flag(c, 'showSun', false)).build(this.engine));
      }
    } else if (this.indirectLight) this.indirectLight.setIntensity(intensity);
    if (this.indirectLight && has(c, 'rotation')) {
      const a = num(c, 'rotation', 0) * DEG, cs = Math.cos(a), sn = Math.sin(a);
      this.indirectLight.setRotation([cs, 0, -sn, 0, 1, 0, sn, 0, cs]);
    }
    if (has(c, 'enabled') && !flag(c, 'enabled', true)) this.scene.setIndirectLight(null);
    else if (this.indirectLight) this.scene.setIndirectLight(this.indirectLight);
    return null;
  }

  destroyEnvironment() {
    if (this.indirectLight) { this.scene.setIndirectLight(null); this.engine.destroyIndirectLight(this.indirectLight); this.indirectLight = null; }
    if (this.iblTexture) { this.engine.destroyTexture(this.iblTexture); this.iblTexture = null; }
    if (this.skyTexture) { this.engine.destroyTexture(this.skyTexture); this.skyTexture = null; }
  }

  replaceSkybox(sky) {
    this.scene.setSkybox(sky);
    if (this.skyboxObject) this.engine.destroySkybox(this.skyboxObject);
    this.skyboxObject = sky;
  }

  async skybox(c) {
    if (has(c, 'color')) { const col = vec(c, 'color', [0, 0, 0, 1]); this.replaceSkybox(Filament.Skybox.Builder().color([col[0], col[1], col[2], col[3] ?? 1]).build(this.engine)); }
    else if (has(c, 'path')) {
      if (this.skyTexture) { this.engine.destroyTexture(this.skyTexture); this.skyTexture = null; }
      this.skyTexture = this.engine.createTextureFromKtx1(await readAsset(c.path, this.appRoot));
      this.replaceSkybox(Filament.Skybox.Builder().environment(this.skyTexture).showSun(flag(c, 'showSun', false)).build(this.engine));
    } else if (has(c, 'visible')) this.scene.setSkybox(flag(c, 'visible', true) ? this.skyboxObject : null);
    return null;
  }

  create(id, kind) {
    if (this.objects.has(id)) fail(`Object id already in use / 物件代碼已使用: ${id}`);
    const o = {kind};
    this.objects.set(id, o);
    return o;
  }
  get(id) { const o = this.objects.get(id); if (!o) fail(`Unknown object id / 未知的物件代碼: ${id}`); return o; }
  getEntity(id) { const o = this.get(id); if (['material', 'instance', 'texture'].includes(o.kind)) fail(`Object is not a scene entity / 物件不是場景實體: ${id}`); return o; }
  entityOf(o) { return o.kind === 'model' ? o.asset.getRoot() : o.entity; }

  async builtinMaterial(name) {
    if (this.builtins.has(name)) return this.builtins.get(name);
    if (!BUILTIN_PARAMS[name]) fail(`Unknown built-in material / 未知的內建材質: ${name}`);
    const m = this.engine.createMaterial(await readAsset(`${this.materialDir}/${name}.filamat`, this.appRoot));
    m.__params = BUILTIN_PARAMS[name]; m.__defaults = BUILTIN_DEFAULTS[name];
    this.builtins.set(name, m);
    return m;
  }

  sampler(o = {}) {
    const F = Filament;
    const clamp = text(o, 'wrap', 'repeat').toLowerCase() === 'clamp', nearest = text(o, 'filter', 'linear').toLowerCase() === 'nearest';
    const s = new F.TextureSampler(nearest ? F.MinFilter.NEAREST : F.MinFilter.LINEAR_MIPMAP_LINEAR, nearest ? F.MagFilter.NEAREST : F.MagFilter.LINEAR,
      clamp ? F.WrapMode.CLAMP_TO_EDGE : F.WrapMode.REPEAT);
    s.setAnisotropy(8);
    return s;
  }

  setUniform(mi, types, name, value) {
    const t = types?.[name] ?? (Array.isArray(value) ? value.length : 1);
    if (types && types[name] === undefined) fail(`Unknown material parameter / 未知的材質參數: ${name}`);
    if (t === 's') fail(`Parameter needs a texture / 參數需要紋理: ${name}`);
    const v = Array.isArray(value) ? value : [value];
    const at = (i, d) => v[i] ?? d;
    if (t === 1) mi.setFloatParameter(name, at(0, 0));
    else if (t === 2) mi.setFloat2Parameter(name, [at(0, 0), at(1, 0)]);
    else if (t === 3) mi.setFloat3Parameter(name, [at(0, 0), at(1, 0), at(2, 0)]);
    else mi.setFloat4Parameter(name, [at(0, 0), at(1, 0), at(2, 0), at(3, 1)]);
  }

  applyDefaults(o) {
    const d = o.material.__defaults, t = o.material.__params;
    if (!d) return;
    for (const [k, v] of Object.entries(d)) this.setUniform(o.instance, t, k, v);
    const tex = {baseColorMap: this.white, normalMap: this.flatNormal, ormMap: this.defaultOrm};
    for (const [k, v] of Object.entries(tex)) if (t[k] === 's') o.instance.setTextureParameter(k, v, this.sampler());
  }

  setParams(o, params) {
    if (!params || typeof params !== 'object' || Array.isArray(params)) fail('Material parameters must be a hash / 材質參數必須是雜湊表');
    const types = o.material.__params || o.material.__userTypes || null;
    for (const [name, value] of Object.entries(params)) {
      if (value && typeof value === 'object' && !Array.isArray(value) && value.texture !== undefined) {
        const t = this.get(value.texture);
        if (t.kind !== 'texture') fail('Expected a texture / 需要紋理');
        if (types && types[name] !== 's' && types[name] !== undefined) fail(`Parameter is not a texture / 參數不是紋理: ${name}`);
        o.instance.setTextureParameter(name, t.texture, this.sampler(value));
      } else if (value && typeof value === 'object' && !Array.isArray(value) && value.value !== undefined) {
        const map = {float: 1, float2: 2, float3: 3, float4: 4};
        this.setUniform(o.instance, {[name]: map[value.type] ?? (Array.isArray(value.value) ? value.value.length : 1)}, name, value.value);
      } else if (typeof value === 'number' || typeof value === 'boolean' || Array.isArray(value)) {
        this.setUniform(o.instance, types, name, typeof value === 'boolean' ? (value ? 1 : 0) : value);
      } else fail(`Unsupported parameter value / 不支援的參數值: ${name}`);
    }
  }

  async material(c) {
    const id = idOf(c);
    const m = this.engine.createMaterial(await readAsset(text(c, 'path'), this.appRoot));
    if (c.types && typeof c.types === 'object') m.__userTypes = c.types;
    const o = this.create(id, 'material'); o.material = m;
    return {id, name: m.getName(), parameters: []};
  }

  async instance(c) {
    const id = idOf(c);
    let m;
    const mat = c.material ?? 'lit';
    if (typeof mat === 'string') m = await this.builtinMaterial(mat);
    else if (Number.isInteger(mat)) { const mo = this.get(mat); if (mo.kind !== 'material') fail('Expected a material / 需要材質'); m = mo.material; }
    else fail('Expected a material name or id / 需要材質名稱或代碼');
    const o = this.create(id, 'instance');
    o.material = m; o.instance = m.createInstance();
    this.applyDefaults(o);
    if (c.params) this.setParams(o, c.params);
    if (flag(c, 'doubleSided', false)) o.instance.setCullingMode(Filament.CullingMode.NONE);
    if (has(c, 'culling')) { const s = text(c, 'culling').toLowerCase(); o.instance.setCullingMode(s === 'none' ? Filament.CullingMode.NONE : s === 'front' ? Filament.CullingMode.FRONT : Filament.CullingMode.BACK); }
    return null;
  }

  async defaultInstance() {
    if (!this.defaultMi) { const m = await this.builtinMaterial('lit'); this.defaultMi = {material: m, instance: m.createInstance()}; this.applyDefaults(this.defaultMi); }
    return this.defaultMi.instance;
  }

  async instanceOf(c, key = 'material') {
    if (!has(c, key)) return this.defaultInstance();
    const o = this.get(c[key]);
    if (o.kind !== 'instance') fail('Expected a material instance / 需要材質實例');
    return o.instance;
  }

  async texture(c) {
    const id = idOf(c), path = text(c, 'path'), srgb = flag(c, 'srgb', true), F = Filament;
    const bytes = await readAsset(path, this.appRoot);
    let t;
    if (/\.ktx2$/i.test(path)) {
      const reader = new F.Ktx2Reader(this.engine, true), IF = F.Texture$InternalFormat;
      const prefer = srgb ? [IF.DXT5_SRGBA, IF.DXT1_SRGBA, IF.ETC2_EAC_SRGBA8, IF.SRGB8_ALPHA8_ASTC_4x4, IF.SRGB8_A8] : [IF.DXT5_RGBA, IF.DXT1_RGBA, IF.ETC2_EAC_RGBA8, IF.RGBA_ASTC_4x4, IF.RGBA8];
      for (const f of prefer) reader.requestFormat(f);
      const buffer = F.Buffer(bytes);
      t = reader.load(buffer, srgb ? F.Ktx2Reader$TransferFunction.sRGB : F.Ktx2Reader$TransferFunction.LINEAR);
      buffer.delete(); reader.delete();
    } else if (/\.ktx$/i.test(path)) t = this.engine.createTextureFromKtx1(bytes, {srgb});
    else if (/\.jpe?g$/i.test(path)) t = this.engine.createTextureFromJpeg(bytes, {srgb, nomips: !flag(c, 'mipmaps', true)});
    else t = this.engine.createTextureFromPng(bytes, {srgb, nomips: !flag(c, 'mipmaps', true)});
    if (!t) fail(`Cannot load texture / 無法載入紋理: ${path}`);
    const o = this.create(id, 'texture'); o.texture = t;
    return {id, width: t.getWidth(this.engine, 0), height: t.getHeight(this.engine, 0), levels: t.getLevels(this.engine)};
  }

  buildRenderable(o, m, mi, c) {
    const F = Filament, n = m.positions.length / 3;
    if (!n || !m.indices.length) fail('Empty geometry / 幾何資料為空');
    if (m.normals.length !== m.positions.length) fail('Normals count mismatch / 法線數量不符');
    const uvs = m.uvs.length ? m.uvs : new Array(n * 2).fill(0);
    if (uvs.length !== n * 2) fail('UV count mismatch / UV 數量不符');
    for (const i of m.indices) if (i >= n || i < 0) fail('Index out of range / 索引超出範圍');
    // The JS SurfaceOrientation builder helpers copy data into the wasm heap and do not chain.
    const sob = new F.SurfaceOrientation$Builder();
    sob.vertexCount(n);
    sob.normals(new Float32Array(m.normals));
    sob.uvs(new Float32Array(uvs));
    sob.positions(new Float32Array(m.positions));
    sob.triangleCount(m.indices.length / 3);
    sob.triangles32(new Uint32Array(m.indices));
    const so = sob.build();
    const quats = so.getQuatsFloat4(n); so.delete();
    const stride = 9, data = new Float32Array(n * stride), lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (let i = 0; i < n; i++) {
      for (let k = 0; k < 3; k++) { const p = m.positions[i * 3 + k]; data[i * stride + k] = p; lo[k] = Math.min(lo[k], p); hi[k] = Math.max(hi[k], p); }
      for (let k = 0; k < 4; k++) data[i * stride + 3 + k] = quats[i * 4 + k];
      data[i * stride + 7] = uvs[i * 2]; data[i * stride + 8] = uvs[i * 2 + 1];
    }
    const VA = F.VertexAttribute, AT = F.VertexBuffer$AttributeType;
    o.vb = F.VertexBuffer.Builder().vertexCount(n).bufferCount(1)
      .attribute(VA.POSITION, 0, AT.FLOAT3, 0, stride * 4).attribute(VA.TANGENTS, 0, AT.FLOAT4, 12, stride * 4)
      .attribute(VA.UV0, 0, AT.FLOAT2, 28, stride * 4).build(this.engine);
    o.vb.setBufferAt(this.engine, 0, data);
    o.ib = F.IndexBuffer.Builder().indexCount(m.indices.length).bufferType(F.IndexBuffer$IndexType.UINT).build(this.engine);
    o.ib.setBuffer(this.engine, new Uint32Array(m.indices));
    o.entity = F.EntityManager.get().create();
    F.RenderableManager.Builder(1)
      .boundingBox({center: [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2], halfExtent: [(hi[0] - lo[0]) / 2, (hi[1] - lo[1]) / 2, (hi[2] - lo[2]) / 2]})
      .material(0, mi).geometry(0, F.RenderableManager$PrimitiveType.TRIANGLES, o.vb, o.ib)
      .castShadows(flag(c, 'castShadows', true)).receiveShadows(flag(c, 'receiveShadows', true)).culling(true).build(this.engine, o.entity);
    this.scene.addEntity(o.entity);
    this.applyTransform(o.entity, c);
  }

  register(id, kind, build) {
    // Objects are registered only after Filament objects were created successfully.
    if (this.objects.has(id)) fail(`Object id already in use / 物件代碼已使用: ${id}`);
    const o = {kind};
    build(o);
    this.objects.set(id, o);
    return o;
  }

  async shape(c) { const id = idOf(c), mi = await this.instanceOf(c), m = makeShape(c); this.register(id, 'shape', o => this.buildRenderable(o, m, mi, c)); return null; }

  async geometry(c) {
    const id = idOf(c), mi = await this.instanceOf(c);
    const p = vec(c, 'positions', []), idx = vec(c, 'indices', []);
    if (p.length < 9 || idx.length < 3 || p.length % 3 || idx.length % 3) fail('positions/indices must be multiples of 3 / 位置與索引數量須為 3 的倍數');
    let normals = has(c, 'normals') ? vec(c, 'normals', []) : null;
    if (!normals) {
      normals = new Array(p.length).fill(0);
      for (let i = 0; i < idx.length; i += 3) {
        const [a, b, d] = [idx[i] * 3, idx[i + 1] * 3, idx[i + 2] * 3];
        const u = [p[b] - p[a], p[b + 1] - p[a + 1], p[b + 2] - p[a + 2]], v = [p[d] - p[a], p[d + 1] - p[a + 1], p[d + 2] - p[a + 2]];
        const fn = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]];
        for (const j of [a, b, d]) for (let k = 0; k < 3; k++) normals[j + k] += fn[k];
      }
    }
    for (let i = 0; i < normals.length; i += 3) { const n = Math.hypot(normals[i], normals[i + 1], normals[i + 2]); if (n > 0) for (let k = 0; k < 3; k++) normals[i + k] /= n; else normals[i + 1] = 1; }
    this.register(id, 'shape', o => this.buildRenderable(o, {positions: p, normals, uvs: has(c, 'uvs') ? vec(c, 'uvs', []) : [], indices: idx}, mi, c));
    return null;
  }

  async mesh(c) {
    const id = idOf(c), mi = await this.instanceOf(c), path = text(c, 'path');
    const mesh = this.engine.loadFilamesh(await readAsset(path, this.appRoot), mi, {});
    const o = this.create(id, 'mesh');
    o.entity = mesh.renderable; o.vb = mesh.vertexBuffer; o.ib = mesh.indexBuffer;
    const rm = this.engine.getRenderableManager(), ri = rm.getInstance(o.entity);
    rm.setCastShadows(ri, flag(c, 'castShadows', true)); rm.setReceiveShadows(ri, flag(c, 'receiveShadows', true));
    ri.delete();
    this.scene.addEntity(o.entity);
    this.applyTransform(o.entity, c);
    return {id};
  }

  async model(c) {
    const id = idOf(c), path = text(c, 'path'), F = Filament;
    const bytes = await readAsset(path, this.appRoot);
    const asset = this.assetLoader.createAsset(bytes);
    if (!asset) fail(`Invalid glTF / 無效的 glTF 模型: ${path}`);
    const key = packageKey(path, this.appRoot), dir = key.includes('/') ? key.slice(0, key.lastIndexOf('/') + 1) : '';
    const uris = asset.getResourceUris();
    const datas = await Promise.all(uris.map(u => u.startsWith('data:') ? null : readAsset(dir + decodeURIComponent(u), '')));
    uris.forEach((u, i) => { if (datas[i]) { const b = F.Buffer(datas[i]); this.resourceLoader.addResourceData(u, b); b.delete(); } });
    // Emissive strengths by material name for the `emissive` command (filament.js cannot read parameters).
    let strengths = {};
    try {
      const json = /\.glb$/i.test(path) ? null : JSON.parse(decoder.decode(bytes));
      for (const m of json?.materials ?? []) strengths[m.name ?? ''] = m.extensions?.KHR_materials_emissive_strength?.emissiveStrength ?? 1;
    } catch { strengths = {}; }
    if (!this.resourceLoader.asyncBeginLoad(asset)) { this.assetLoader.destroyAsset(asset); fail(`Cannot load glTF resources / 無法載入 glTF 資源: ${path}`); }
    const o = this.create(id, 'model');
    o.asset = asset; o.strengths = strengths; o.loading = true;
    this.loading.set(id, o);
    const rm = this.engine.getRenderableManager();
    for (const e of asset.getRenderableEntities()) { const ri = rm.getInstance(e); rm.setCastShadows(ri, flag(c, 'castShadows', true)); rm.setReceiveShadows(ri, flag(c, 'receiveShadows', true)); ri.delete(); }
    if (!flag(c, 'lights', true)) for (const e of asset.getLightEntities()) this.engine.getLightManager().destroy(e);
    this.scene.addEntities(asset.getEntities());
    this.applyTransform(asset.getRoot(), c);
    if (flag(c, 'wait', true)) {
      const deadline = performance.now() + 120000;
      while (this.resourceLoader.asyncGetLoadProgress() < 1 && performance.now() < deadline) {
        this.resourceLoader.asyncUpdateLoad();
        await new Promise(r => setTimeout(r, 1));
      }
      this.finishLoading(id, o);
    }
    return this.modelInfo(id, o);
  }

  updateLoading() {
    if (!this.loading.size) return;
    this.resourceLoader.asyncUpdateLoad();
    if (this.resourceLoader.asyncGetLoadProgress() >= 1) for (const [id, o] of this.loading) this.finishLoading(id, o);
  }

  finishLoading(id, o) {
    if (!o.loading) return;
    this.resourceLoader.asyncUpdateLoad();
    o.loading = false;
    this.loading.delete(id);
    o.asset.releaseSourceData();
  }

  modelInfo(id, o) {
    const box = o.asset.getBoundingBox(), inst = o.asset.getInstance(), animator = inst.getAnimator();
    const animations = [];
    if (animator) for (let i = 0; i < animator.getAnimationCount(); i++) animations.push({name: animator.getAnimationName(i) || '', duration: animator.getAnimationDuration(i)});
    const mis = inst.getMaterialInstances(), materials = [];
    for (let i = 0; i < mis.size(); i++) materials.push(mis.get(i).getName() || '');
    const c3 = k => (box.min[k] + box.max[k]) / 2;
    return {id, min: box.min, max: box.max, center: [c3(0), c3(1), c3(2)], size: [box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]],
      entities: o.asset.getEntities().length, renderables: o.asset.getRenderableEntities().length, lights: o.asset.getLightEntities().length,
      animations, materials, loading: !!o.loading};
  }

  model3(c) { const o = this.get(idOf(c)); if (o.kind !== 'model') fail('Object is not a glTF model / 物件不是 glTF 模型'); return o; }

  animate(c) {
    const o = this.model3(c), animator = o.asset.getInstance().getAnimator();
    if (!animator) return null;
    const time = num(c, 'time', 0);
    if (has(c, 'animation')) { const i = num(c, 'animation', 0) | 0; if (i >= 0 && i < animator.getAnimationCount()) animator.applyAnimation(i, time); }
    else for (let i = 0; i < animator.getAnimationCount(); i++) animator.applyAnimation(i, time);
    animator.updateBoneMatrices();
    return null;
  }

  emissive(c) {
    const o = this.model3(c), scale = num(c, 'scale', 1), mis = o.asset.getInstance().getMaterialInstances();
    for (let i = 0; i < mis.size(); i++) {
      const mi = mis.get(i), base = o.strengths[mi.getName() || ''];
      if (base !== undefined) mi.setFloatParameter('emissiveStrength', base * scale);
    }
    return null;
  }

  applyTransform(entity, c) {
    if (!['position', 'rotation', 'quaternion', 'scale', 'matrix'].some(k => has(c, k))) return;
    const tm = this.engine.getTransformManager();
    if (!tm.hasComponent(entity)) tm.create(entity);
    const inst = tm.getInstance(entity);
    let m;
    if (has(c, 'matrix')) { m = vec(c, 'matrix', []); if (m.length < 16) fail("Too few values in 'matrix' / 陣列元素不足"); }
    else {
      const t = vec(c, 'position', [0, 0, 0]);
      let q = [0, 0, 0, 1];
      if (has(c, 'quaternion')) { const v = vec(c, 'quaternion', q), l = Math.hypot(...v) || 1; q = v.map(x => x / l); }
      else if (has(c, 'rotation')) q = euler(vec(c, 'rotation', [0, 0, 0]));
      const s = typeof c.scale === 'number' ? [c.scale, c.scale, c.scale] : vec(c, 'scale', [1, 1, 1]);
      m = compose(t, q, s);
    }
    tm.setTransform(inst, m);
    inst.delete();
  }

  transform(c) {
    const o = this.getEntity(idOf(c));
    if (o.kind === 'light') {
      const lm = this.engine.getLightManager(), li = lm.getInstance(o.entity);
      if (has(c, 'position')) lm.setPosition(li, vec(c, 'position', [0, 0, 0]));
      if (has(c, 'direction')) lm.setDirection(li, normalize3(vec(c, 'direction', [0, -1, 0])));
      li.delete();
      return null;
    }
    this.applyTransform(this.entityOf(o), c);
    return null;
  }

  parent(c) {
    const tm = this.engine.getTransformManager(), ce = this.entityOf(this.getEntity(idOf(c)));
    if (!tm.hasComponent(ce)) tm.create(ce);
    const ci = tm.getInstance(ce);
    if (!has(c, 'parent')) { tm.setParent(ci, null); ci.delete(); return null; }
    const pe = this.entityOf(this.getEntity(idOf(c, 'parent')));
    if (!tm.hasComponent(pe)) tm.create(pe);
    const pi = tm.getInstance(pe);
    tm.setParent(ci, pi); ci.delete(); pi.delete();
    return null;
  }

  renderables(o) { return o.kind === 'model' ? o.asset.getRenderableEntities() : (o.kind === 'shape' || o.kind === 'mesh') ? [o.entity] : []; }

  visible(c) {
    const o = this.getEntity(idOf(c)), on = flag(c, 'visible', true);
    if (o.kind === 'model') { if (on) this.scene.addEntities(o.asset.getEntities()); else this.scene.removeEntities(o.asset.getEntities()); }
    else if (on) this.scene.addEntity(o.entity); else this.scene.remove(o.entity);
    return null;
  }

  shadows(c) {
    const rm = this.engine.getRenderableManager();
    for (const e of this.renderables(this.getEntity(idOf(c)))) {
      const ri = rm.getInstance(e);
      if (has(c, 'cast')) rm.setCastShadows(ri, flag(c, 'cast', true));
      if (has(c, 'receive')) rm.setReceiveShadows(ri, flag(c, 'receive', true));
      ri.delete();
    }
    return null;
  }

  async setMaterial(c) {
    const o = this.getEntity(idOf(c)), mi = await this.instanceOf(c), rm = this.engine.getRenderableManager();
    for (const e of this.renderables(o)) {
      const ri = rm.getInstance(e);
      for (let p = 0; p < rm.getPrimitiveCount(ri); p++) rm.setMaterialInstanceAt(ri, p, mi);
      ri.delete();
    }
    return null;
  }

  light(c, creating) {
    const id = idOf(c), F = Filament, lm = this.engine.getLightManager();
    if (creating) {
      const type = text(c, 'type', 'point').toLowerCase(), T = F.LightManager$Type;
      const t = {sun: T.SUN, directional: T.DIRECTIONAL, spot: T.FOCUSED_SPOT, softspot: T.SPOT, spot_soft: T.SPOT, point: T.POINT}[type];
      if (t === undefined) fail(`Unknown light type / 未知的光源類型: ${type}`);
      const o = this.create(id, 'light');
      o.entity = F.EntityManager.get().create();
      const directional = t === T.SUN || t === T.DIRECTIONAL;
      let intensity = num(c, 'intensity', directional ? 100000 : 10000);
      if (!directional && text(c, 'unit', 'lumen').toLowerCase() === 'candela') intensity *= 4 * Math.PI;
      const b = F.LightManager.Builder(t).color(vec(c, 'color', [1, 1, 1])).intensity(intensity)
        .direction(normalize3(vec(c, 'direction', [0, -1, 0]))).position(vec(c, 'position', [0, 0, 0])).falloff(num(c, 'falloff', 10));
      if (t === T.FOCUSED_SPOT || t === T.SPOT) { const inner = num(c, 'inner', 25) * DEG, outer = num(c, 'outer', 35) * DEG; b.spotLightCone(Math.min(inner, outer), outer); }
      if (t === T.SUN) b.sunAngularRadius(num(c, 'sunAngularRadius', 1.9)).sunHaloSize(num(c, 'sunHaloSize', 10)).sunHaloFalloff(num(c, 'sunHaloFalloff', 80));
      const shadows = flag(c, 'castShadows', false);
      b.castShadows(shadows);
      if (shadows || has(c, 'shadow')) {
        const s = c.shadow || {};
        b.shadowOptions({mapSize: num(s, 'mapSize', directional ? 2048 : 1024), shadowCascades: Math.max(1, Math.min(4, num(s, 'cascades', directional ? 3 : 1))),
          constantBias: num(s, 'constantBias', 0.001), normalBias: num(s, 'normalBias', 1), shadowFar: num(s, 'far', 0), stable: flag(s, 'stable', false),
          lispsm: flag(s, 'lispsm', true), screenSpaceContactShadows: flag(s, 'contactShadows', false), shadowBulbRadius: num(s, 'bulbRadius', 0.02)});
      }
      b.build(this.engine, o.entity);
      this.scene.addEntity(o.entity);
      return null;
    }
    const o = this.get(id);
    if (o.kind !== 'light') fail('Object is not a light / 物件不是光源');
    const li = lm.getInstance(o.entity);
    if (has(c, 'color')) lm.setColor(li, vec(c, 'color', [1, 1, 1]));
    if (has(c, 'intensity')) { let v = num(c, 'intensity', 1); if (text(c, 'unit', '').toLowerCase() === 'candela') v *= 4 * Math.PI; lm.setIntensity(li, v); }
    if (has(c, 'position')) lm.setPosition(li, vec(c, 'position', [0, 0, 0]));
    if (has(c, 'direction')) lm.setDirection(li, normalize3(vec(c, 'direction', [0, -1, 0])));
    if (has(c, 'falloff')) lm.setFalloff(li, num(c, 'falloff', 10));
    if (has(c, 'inner') || has(c, 'outer')) { const inner = num(c, 'inner', 25) * DEG, outer = num(c, 'outer', 35) * DEG; lm.setSpotLightCone(li, Math.min(inner, outer), outer); }
    if (has(c, 'castShadows')) lm.setShadowCaster(li, flag(c, 'castShadows', false));
    if (has(c, 'enabled')) { if (flag(c, 'enabled', true)) this.scene.addEntity(o.entity); else this.scene.remove(o.entity); }
    li.delete();
    return null;
  }

  remove(id) {
    const o = this.objects.get(id);
    if (!o) return;
    const e = this.engine;
    if (o.kind === 'model') { this.scene.removeEntities(o.asset.getEntities()); this.loading.delete(id); this.assetLoader.destroyAsset(o.asset); }
    else if (o.kind === 'material') {
      for (const other of this.objects.values()) if (other.kind === 'instance' && other.material === o.material) fail('Material still has instances / 材質仍有實例，請先移除實例');
      e.destroyMaterial(o.material);
    } else if (o.kind === 'instance') e.destroyMaterialInstance(o.instance);
    else if (o.kind === 'texture') e.destroyTexture(o.texture);
    else {
      this.scene.remove(o.entity);
      e.destroyEntity(o.entity);
      if (o.vb) e.destroyVertexBuffer(o.vb);
      if (o.ib) e.destroyIndexBuffer(o.ib);
      Filament.EntityManager.get().destroy(o.entity);
    }
    this.objects.delete(id);
  }

  clear(keepEnvironment) {
    const rank = o => o.kind === 'instance' ? 1 : o.kind === 'material' ? 2 : o.kind === 'texture' ? 3 : 0;
    const ids = [...this.objects.entries()].sort((a, b) => rank(a[1]) - rank(b[1])).map(([id]) => id);
    for (const id of ids) this.remove(id);
    if (!keepEnvironment) { this.destroyEnvironment(); this.replaceSkybox(Filament.Skybox.Builder().color([0.03, 0.04, 0.06, 1]).build(this.engine)); }
  }

  async screenshot(c) {
    // Browsers cannot write files: the PNG is rendered, kept as a data URL and exposed for tests/tools.
    // The canvas keeps its drawing buffer (preserveDrawingBuffer), so the PNG is taken right after render().
    this.pendingShot = true;
    this.shotPath = text(c, 'path');
    await this.frame({});
    return {path: text(c, 'path'), width: this.width, height: this.height, web: true};
  }

  stats() {
    const count = k => [...this.objects.values()].filter(o => o.kind === k).length;
    return {fps: this.fps, frames: this.frames, objects: this.objects.size, lights: count('light'), models: count('model'), shapes: count('shape'),
      meshes: count('mesh'), textures: count('texture'), width: this.width, height: this.height, loading: this.loading.size};
  }

  shutdown() {
    if (!this.engine) return;
    this.clear(false);
    for (const m of this.builtins.values()) this.engine.destroyMaterial(m);
    this.builtins.clear();
    if (this.defaultMi) { this.engine.destroyMaterialInstance(this.defaultMi.instance); this.defaultMi = null; }
    for (const [t, n, f] of this.listeners || []) t.removeEventListener(n, f);
    this.canvas?.remove();
    this.engine.destroySwapChain(this.swapChain);
    this.engine.destroyRenderer(this.renderer);
    this.engine.destroyView(this.view);
    this.engine.destroyScene(this.scene);
    this.engine.destroyCameraComponent(this.cameraEntity);
    Filament.Engine.destroy(this.engine);
    this.engine = null;
  }
}

export function aqua_library_abi() { return 1; }

export function aqua_create_library() {
  const runtime = new Runtime();
  return {
    fl_call: bytes => runtime.call(bytes),
    fl_fail: bytes => { throw new Error(`Aqua_Filament: ${decoder.decode(bytes)}`); },
    fl_shutdown: async () => { await runtime.queue; runtime.shutdown(); return encoder.encode('{"ok":true}'); },
    aqua_dispose: async () => { await runtime.queue; runtime.shutdown(); }
  };
}
