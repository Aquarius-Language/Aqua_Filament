import assert from 'node:assert/strict';
import {test} from 'node:test';
import {rewriteFilamentUniformArrays,prepareFilamentWebglContext} from './shader-compat.mjs';

const shader = `#version 300 es
#define SPIRV_CROSS_CONSTANT_ID_4 2048
const int CONFIG_FROXEL_BUFFER_HEIGHT = SPIRV_CROSS_CONSTANT_ID_4;
layout(std140) uniform FroxelsUniforms { highp uvec4 a[CONFIG_FROXEL_BUFFER_HEIGHT]; } froxelsUniforms;
void main() { uvec4 x = froxelsUniforms.a[indices[int(other[2])]]; int n = froxelsUniforms.a.length(); }
`;
test('large known blocks retain std140 storage and route every indexed read',()=>{
  const result=rewriteFilamentUniformArrays(shader);
  assert.match(result,/a\[CONFIG_FROXEL_BUFFER_HEIGHT - 1\]/);
  assert.match(result,/highp uvec4 aquaFilamentLast;/);
  assert.match(result,/aquaFilamentLoadFroxelsUniforms\(int\(indices\[int\(other\[2\]\)\]\)\)/);
  assert.match(result,/int n = CONFIG_FROXEL_BUFFER_HEIGHT/);
  assert.match(result,/return froxelsUniforms.a\[aquaIndex\];/);
  assert.equal(rewriteFilamentUniformArrays(result),result);
});
test('all Filament blocks, boundary indices and ordinary i variables are handled',()=>{
  for(const [block,type,count]of [['ShadowUniforms','ShadowData',128],['LightsUniforms','mat4',255],['FroxelRecordUniforms','uvec4',4096]]){
    const result=rewriteFilamentUniformArrays(`layout(std140) uniform ${block} { ${type} a[${count}]; } data; void main(){ ${type} first=data.a[0],last=data.a[${count-1}],middle=data.a[i]; }`);
    for(const index of ['0',String(count-1),'i'])assert.ok(result.includes(`aquaFilamentLoad${block}(int(${index}))`));
    assert.ok(result.includes(`if (aquaIndex == ${count} - 1) return data.aquaFilamentLast`));
  }
});
test('unrelated, small and unknown layouts are preserved',()=>{
  for(const source of [null,'layout(std140) uniform ObjectUniforms { PerRenderableData a[64]; } objectUniforms;',
    'layout(std140) uniform UserData { vec4 a[128]; } user;',
    'layout(std140) uniform LightsUniforms { mat4 a[1]; } lights;',
    'layout(std140) uniform LightsUniforms { mat4 a[UNKNOWN]; } lights;',
    'layout(std140) uniform LightsUniforms { mat4 a[64]; vec4 existing; } lights;'])assert.equal(rewriteFilamentUniformArrays(source),source);
});
test('compatibility is installed once on the owned context only',()=>{
  const calls=[];const other={shaderSource(){}};const untouched=other.shaderSource;
  const context={shaderSource(...args){assert.equal(this,context);calls.push(args);}};
  prepareFilamentWebglContext(context);const installed=context.shaderSource;
  prepareFilamentWebglContext(context);assert.equal(context.shaderSource,installed);
  context.shaderSource('shader',shader);assert.equal(calls.length,1);
  assert.match(calls[0][1],/aquaFilamentLoad/);assert.equal(other.shaderSource,untouched);
});
test('known non-D3D backends retain their native optimizer path',()=>{
  for(const renderer of ['ANGLE NVIDIA Vulkan','Apple Metal','Mesa OpenGL','Google SwiftShader']){
    const context={shaderSource(){},getExtension(){return {UNMASKED_RENDERER_WEBGL:1};},getParameter(){return renderer;}};
    const original=context.shaderSource;prepareFilamentWebglContext(context);assert.equal(context.shaderSource,original);
  }
});
test('Direct3D and privacy-restricted renderer information both get the fix',()=>{
  for(const renderer of ['ANGLE Intel Direct3D11','ANGLE NVIDIA Direct3D11',null]){
    const context={shaderSource(){},getExtension(){return renderer?{UNMASKED_RENDERER_WEBGL:1}:null;},getParameter(){return renderer;}};
    const original=context.shaderSource;prepareFilamentWebglContext(context);assert.notEqual(context.shaderSource,original);
  }
});
