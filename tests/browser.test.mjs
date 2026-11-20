import assert from'node:assert/strict';
import{test}from'node:test';
import{preferences,defaults}from'../browser/settings.js';
import{spawn}from'node:child_process';
import{once}from'node:events';
import{createServer}from'node:http';
import fs from'node:fs';
import os from'node:os';
import path from'node:path';
import{Native}from'../tools/browser/native.mjs';
test('preferences validate values and are manual by default',()=>{
 assert.equal(defaults.autoEnable,false);
 const prior=preferences({qp:16});
 assert.equal(prior.videoCodec,'h264');
 assert.equal(prior.qp,16);
 assert.equal(prior.av1Qp,36);
 const av1=preferences({...prior,videoCodec:'av1',av1Qp:45});
 assert.equal(av1.qp,16);
 assert.equal(av1.av1Qp,45);
 assert.equal(preferences({bufferSeconds:8,qp:16}).cacheMiB,256);
 for(const value of [{bufferSeconds:0},{cacheMiB:8192},{qp:-1},{backSeconds:-1},{videoCodec:'hevc'},{av1Qp:256},{av1Qp:1.5}])
  assert.throws(()=>preferences(value));
});
test('buffer durations accept large safe integers and survive storage serialization',()=>{
 for(const seconds of [3600,86400,365*86400,Number.MAX_SAFE_INTEGER]){
  const saved=JSON.parse(JSON.stringify(preferences({bufferSeconds:seconds,backSeconds:seconds})));
  assert.equal(preferences(saved).bufferSeconds,seconds);
  assert.equal(preferences(saved).backSeconds,seconds);
 }
 for(const name of ['bufferSeconds','backSeconds'])
  for(const seconds of [NaN,Infinity,-Infinity,1.5,Number.MAX_SAFE_INTEGER+1,'3600',null])
   assert.throws(()=>preferences({[name]:seconds}));
});
test('native host never exposes the credential and rejects foreign media hosts',async()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'modeld-host-'));
 const token=path.join(directory,'token'),config=path.join(directory,'client.json');
 fs.writeFileSync(token,'test-only-credential-should-never-appear-in-response',{mode:0o600});
 fs.writeFileSync(config,JSON.stringify({endpoint:'http://127.0.0.1:1',token_file:token}),{mode:0o600});
 const native=new Native(process.env.MODELD_BROWSER_HOST||'./build-browser/modeld-browser-host',config);
 try{
  const result=await native.call('config_get');
  assert.equal(result.configured,true);assert(!JSON.stringify(result).includes('test-only-credential'));
  await assert.rejects(native.call('open',{video:['https://foreign.example/private'],audio:null}),/Bilibili media host/);
 }finally{native.close();await new Promise(resolve=>native.child.once('exit',resolve));fs.rmSync(directory,{recursive:true,force:true});}
});
test('native host negotiates AV1 and sends H.264 requests to older servers',async()=>{
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'modeld-codec-'));
 const token=path.join(directory,'token'),config=path.join(directory,'client.json');
 const requests=[];
 let advertisesAv1=false;
 const server=createServer(async(request,response)=>{
  response.setHeader('content-type','application/json');
  if(request.method==='GET'){
   const capability={video_codec:'h264'};
   if(advertisesAv1)capability.video_codecs=['h264','av1'];
   response.end(JSON.stringify(capability));
   return;
  }
  let input='';
  for await(const part of request)input+=part;
  requests.push(JSON.parse(input));
  response.statusCode=422;
  response.end(JSON.stringify({error:{code:'invalid_request',message:'Test source',retryable:false}}));
 });
 server.listen(0,'127.0.0.1');
 await once(server,'listening');
 fs.writeFileSync(token,'test-only-credential-not-a-live-secret',{mode:0o600});
 fs.writeFileSync(config,JSON.stringify({endpoint:'http://127.0.0.1:'+server.address().port,token_file:token}),{mode:0o600});
 const native=new Native(process.env.MODELD_BROWSER_HOST||'./build-browser/modeld-browser-host',config);
 const video=['https://upos-test.bilivideo.com/video.m4s'];
 try{
  await assert.rejects(native.call('open',{video,video_codec:'av1',qp:45}),/未提供 AV1/);
  assert.equal(requests.length,0);
  await assert.rejects(native.call('open',{video,video_codec:'h264',qp:18}),/Test source/);
  assert.equal('video_codec' in requests[0],false);
  assert.equal(requests[0].qp,18);
  advertisesAv1=true;
  await assert.rejects(native.call('open',{video,video_codec:'av1',qp:45}),/Test source/);
  assert.equal(requests[1].video_codec,'av1');
  assert.equal(requests[1].qp,45);
 }finally{
  const exited=once(native.child,'exit');
  native.close();
  await exited;
  await new Promise(resolve=>server.close(resolve));
  fs.rmSync(directory,{recursive:true,force:true});
 }
});
test('native messaging rejects oversized and truncated input without hanging',async()=>{
 const child=spawn(process.env.MODELD_BROWSER_HOST||'./build-browser/modeld-browser-host',[],{stdio:['pipe','pipe','pipe']});
 const header=Buffer.alloc(4);header.writeUInt32LE(4*1024*1024);
 child.stdin.end(header);await new Promise(resolve=>child.once('exit',resolve));
 assert.equal(child.exitCode,0);
});
