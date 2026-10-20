// ANGLE/D3D11 can bind optimized uniform blocks beyond its 16-entry texture SRV
// cache (StateManager11::setShaderResourceInternal). Keep these Filament blocks
// as constant buffers by splitting off the last element. std140 offsets, array
// stride, total block size and all values remain identical.
const FILAMENT_ARRAY_BLOCKS = new Map([
  ['ShadowUniforms', 'ShadowData'],
  ['FroxelsUniforms', 'uvec4'],
  ['FroxelRecordUniforms', 'uvec4'],
  ['LightsUniforms', 'mat4']
]);
const preparedContexts = new WeakSet();

function constantValue(source, token, depth = 0) {
  if (/^\d+$/.test(token)) return Number(token);
  if (depth > 4) return null;
  const definition = new RegExp(`^\\s*#define\\s+${token}\\s+(\\w+)\\s*$`, 'm').exec(source)
    ?? new RegExp(`\\bconst\\s+(?:int|uint)\\s+${token}\\s*=\\s*(\\w+)\\s*;`).exec(source);
  return definition ? constantValue(source, definition[1], depth + 1) : null;
}

export function rewriteFilamentUniformArrays(source) {
  if (typeof source !== 'string') return source;
  const arrays = [], definitions = [];
  const rewritten = source.replace(
    /layout\s*\(\s*std140\s*\)\s*uniform\s+(\w+)\s*\{([^{}]+)\}\s*(\w+)\s*;/g,
    (original, block, body, instance) => {
      const field = /^\s*((?:(?:lowp|mediump|highp)\s+)?(\w+))\s+(\w+)\s*\[\s*(\w+)\s*\]\s*;\s*$/.exec(body);
      if (!field || FILAMENT_ARRAY_BLOCKS.get(block) !== field[2]) return original;
      const [, type, , member, count] = field;
      const size = constantValue(source, count);
      if (size === null || size < 50) return original;
      const load = `aquaFilamentLoad${block}`, last = 'aquaFilamentLast';
      arrays.push({instance, member, count, load});
      definitions.push(`${type} ${load}(int aquaIndex) {\n`
        + `    if (aquaIndex == ${count} - 1) return ${instance}.${last};\n`
        + `    return ${instance}.${member}[aquaIndex];\n}`);
      return `layout(std140) uniform ${block} {\n`
        + `    ${type} ${member}[${count} - 1];\n    ${type} ${last};\n} ${instance};\n`
        + `${type} ${load}(int aquaIndex);`;
    }
  );
  if (!arrays.length) return source;
  let result = rewritten;
  for (const {instance, member, count, load} of arrays) {
    result = result.replace(new RegExp(`\\b${instance}\\s*\\.\\s*${member}\\s*\\.\\s*length\\s*\\(\\s*\\)`, 'g'), count);
    const access = new RegExp(`\\b${instance}\\s*\\.\\s*${member}\\s*\\[`, 'g');
    let match, offset = 0, updated = '';
    while ((match = access.exec(result))) {
      let end = access.lastIndex, depth = 1;
      for (; end < result.length && depth; end++) {
        if (result[end] === '[') depth++;
        else if (result[end] === ']') depth--;
      }
      if (depth) break; // Leave malformed shader diagnostics to the WebGL compiler.
      const index = result.slice(access.lastIndex, end - 1);
      updated += result.slice(offset, match.index) + `${load}(int(${index}))`;
      offset = end; access.lastIndex = end;
    }
    result = updated + result.slice(offset);
  }
  // Append definitions after rewriting so their direct loads are not rewritten.
  return result + '\n' + definitions.join('\n') + '\n';
}

export function prepareFilamentWebglContext(context) {
  if (preparedContexts.has(context)) return;
  preparedContexts.add(context);
  // Other backends do not have the D3D11 binding bug. Leave their optimizer
  // path alone; if renderer details are unavailable, use the safe layout.
  let renderer = '';
  try {
    const info = context.getExtension?.('WEBGL_debug_renderer_info');
    if (info) renderer = String(context.getParameter(info.UNMASKED_RENDERER_WEBGL));
  } catch {}
  if (/Vulkan|Metal|OpenGL|SwiftShader/i.test(renderer) && !/Direct3D/i.test(renderer)) return;
  const shaderSource = context.shaderSource.bind(context);
  context.shaderSource = (shader, source) => shaderSource(shader, rewriteFilamentUniformArrays(source));
}
