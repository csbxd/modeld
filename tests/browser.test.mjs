import assert from'node:assert/strict';
import{test}from'node:test';
import{preferences,defaults}from'../browser/settings.js';
import{spawn}from'node:child_process';
import fs from'node:fs';
import os from'node:os';
import path from'node:path';
import{Native}from'../tools/browser/native.mjs';
test('preferences validate values and are manual by default',()=>{
 assert.equal(defaults.autoEnable,false);
 assert.equal(preferences({bufferSeconds:8,qp:16}).cacheMiB,256);
 for(const value of [{bufferSeconds:0},{cacheMiB:8192},{qp:-1},{backSeconds:-1}])
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
test('native messaging rejects oversized and truncated input without hanging',async()=>{
 const child=spawn(process.env.MODELD_BROWSER_HOST||'./build-browser/modeld-browser-host',[],{stdio:['pipe','pipe','pipe']});
 const header=Buffer.alloc(4);header.writeUInt32LE(4*1024*1024);
 child.stdin.end(header);await new Promise(resolve=>child.once('exit',resolve));
 assert.equal(child.exitCode,0);
});
