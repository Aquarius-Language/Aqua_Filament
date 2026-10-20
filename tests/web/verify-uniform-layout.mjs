import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {mkdir,writeFile} from 'node:fs/promises';
import {pathToFileURL,fileURLToPath} from 'node:url';
import path from 'node:path';
const root=process.env.FILAMENT_ROOT??path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const {rewriteFilamentUniformArrays}=await import(pathToFileURL(path.join(root,'web/src/shader-compat.mjs')));
const playwright=process.env.AQUARIUS_PLAYWRIGHT??path.resolve(root,'../AquariusLangTW/AquariusWebCompiler/node_modules/playwright/index.mjs');
const {chromium}=await import(pathToFileURL(playwright));
const cases=[
  {block:'FroxelsUniforms',type:'uvec4',count:2048,stride:16,expression:'vec4(data.a[selected]) / 255.0',kind:'uint'},
  {block:'FroxelRecordUniforms',type:'uvec4',count:4096,stride:16,expression:'vec4(data.a[selected]) / 255.0',kind:'uint'},
  {block:'LightsUniforms',type:'mat4',count:255,stride:64,expression:'data.a[selected][2]',kind:'matrix'},
  {block:'ShadowUniforms',type:'ShadowData',count:128,stride:128,kind:'shadow',expression:'vec4(data.a[selected].normalBias, data.a[selected].projectionParam, float(data.a[selected].evsm4_layer) / 255.0)',prefix:`struct ShadowData {
    highp mat4 lightFromWorldMatrix; highp vec4 lightFromWorldZ; highp vec4 scissorNormalized;
    highp vec2 normalBias; highp float projectionParam; highp float wsOneOverTexelSizeAtOneMeter;
    highp uint maxMipLevel_maxSearchRadius; highp uint bulbRadius_vsmExponent;
    highp uint maxPenumbraRatio_penumbraRatioScale; highp uint evsm4_layer;
  };`}
].map(c=>{
  const source=`#version 300 es
precision highp float; precision highp int;
${c.prefix??''}
layout(std140) uniform ${c.block} { ${c.type} a[${c.count}]; } data;
uniform int selected;
out vec4 color;
void main(){color=${c.expression};}`;
  return {...c,source,rewritten:rewriteFilamentUniformArrays(source)};
});
const server=createServer((req,res)=>res.end('<!doctype html><title>Filament uniform layout verification</title>'));
await new Promise(r=>server.listen(0,'127.0.0.1',r));
const browser=await chromium.launch({channel:process.env.WEBGL_BROWSER??'chrome',headless:true,args:['--disable-gpu-shader-disk-cache',...(process.env.WEBGL_ANGLE?[`--use-angle=${process.env.WEBGL_ANGLE}`]:[])]});
let report;
try{
  const page=await browser.newPage();await page.goto(`http://127.0.0.1:${server.address().port}/`);
  report=await page.evaluate(cases=>{
    const canvas=document.createElement('canvas');canvas.width=canvas.height=1;
    const gl=canvas.getContext('webgl2',{antialias:false,preserveDrawingBuffer:true});
    if(!gl)throw Error('WebGL 2 unavailable');
    const debug=gl.getExtension('WEBGL_debug_renderer_info');
    const renderer=debug?gl.getParameter(debug.UNMASKED_RENDERER_WEBGL):'unavailable';
    const check=(condition,message)=>{if(!condition)throw Error(message);};
    const vertex=`#version 300 es
    void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0,1);}`;
    const compile=(source,type)=>{const s=gl.createShader(type);gl.shaderSource(s,source);gl.compileShader(s);check(gl.getShaderParameter(s,gl.COMPILE_STATUS),gl.getShaderInfoLog(s));return s;};
    const program=source=>{
      const vs=compile(vertex,gl.VERTEX_SHADER),fs=compile(source,gl.FRAGMENT_SHADER),p=gl.createProgram();
      gl.attachShader(p,vs);gl.attachShader(p,fs);gl.linkProgram(p);
      check(gl.getProgramParameter(p,gl.LINK_STATUS),gl.getProgramInfoLog(p));gl.deleteShader(vs);gl.deleteShader(fs);return p;
    };
    const results=[];
    gl.bindVertexArray(gl.createVertexArray());gl.viewport(0,0,1,1);
    for(const c of cases){
      const original=program(c.source),patched=program(c.rewritten),required=c.count*c.stride;
      const originalIndex=gl.getUniformBlockIndex(original,c.block),patchedIndex=gl.getUniformBlockIndex(patched,c.block);
      const before=gl.getActiveUniformBlockParameter(original,originalIndex,gl.UNIFORM_BLOCK_DATA_SIZE);
      const after=gl.getActiveUniformBlockParameter(patched,patchedIndex,gl.UNIFORM_BLOCK_DATA_SIZE);
      check(before===required&&after===required,`${c.block}: buffer size ${before} / ${after}, expected ${required}`);
      gl.deleteProgram(original); // Never draw the original program: it reproduces the ANGLE crash.
      const bytes=new ArrayBuffer(required),ints=new Uint32Array(bytes),floats=new Float32Array(bytes);
      const expected=i=>[0,1,2,3].map(j=>(i*7+j*31+17)%251);
      for(let i=0;i<c.count;i++){
        const values=expected(i),offset=i*c.stride/4;
        if(c.kind==='uint')values.forEach((v,j)=>ints[offset+j]=v);
        else if(c.kind==='matrix')values.forEach((v,j)=>floats[offset+8+j]=v/255);
        else{floats[offset+24]=values[0]/255;floats[offset+25]=values[1]/255;floats[offset+26]=values[2]/255;ints[offset+31]=values[3];}
      }
      const buffer=gl.createBuffer();gl.bindBuffer(gl.UNIFORM_BUFFER,buffer);gl.bufferData(gl.UNIFORM_BUFFER,bytes,gl.STATIC_DRAW);
      gl.bindBufferBase(gl.UNIFORM_BUFFER,0,buffer);gl.uniformBlockBinding(patched,patchedIndex,0);gl.useProgram(patched);
      const selected=gl.getUniformLocation(patched,'selected');const indices=[0,1,31,32,Math.floor(c.count/2),c.count-2,c.count-1];
      for(const i of indices){
        gl.uniform1i(selected,i);gl.drawArrays(gl.TRIANGLES,0,3);const pixel=new Uint8Array(4);gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);
        const values=expected(i);check(values.every((v,j)=>Math.abs(v-pixel[j])<=1),`${c.block}[${i}]: got ${pixel}, expected ${values}`);
        check(gl.getError()===gl.NO_ERROR&&!gl.isContextLost(),`${c.block}: graphics error or context loss`);
      }
      results.push({block:c.block,passed:true,beforeBytes:before,afterBytes:after,indices});
      gl.deleteBuffer(buffer);gl.deleteProgram(patched);
    }
    return {passed:true,renderer,results,pixelChecks:results.reduce((sum,r)=>sum+r.indices.length,0)};
  },cases);
  if(process.env.REQUIRE_RENDERER)assert.match(report.renderer,new RegExp(process.env.REQUIRE_RENDERER,'i'));
  console.log(`PASS ${report.pixelChecks} real GPU boundary reads and all four original/patched buffer sizes: ${report.renderer}`);
}finally{
  await browser.close();server.closeAllConnections();await new Promise(r=>server.close(r));
  if(report){const output=process.env.UNIFORM_REPORT??path.join(root,'tests/artifacts/uniform-layout.json');await mkdir(path.dirname(output),{recursive:true});await writeFile(output,JSON.stringify(report,null,2)+'\n');}
}
