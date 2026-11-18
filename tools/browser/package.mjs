import fs from'node:fs';
import path from'node:path';
import os from'node:os';
import{createHash,createPrivateKey,createPublicKey}from'node:crypto';
import{spawnSync}from'node:child_process';
const [keyFile,outputDirectory]=process.argv.slice(2);
if(!outputDirectory)throw Error('Usage: package.mjs PRIVATE_KEY OUTPUT_DIRECTORY');
const source=path.resolve('browser'),manifest=JSON.parse(fs.readFileSync(path.join(source,'manifest.json')));
const key=createPrivateKey(fs.readFileSync(keyFile)),publicKey=createPublicKey(key).export({format:'der',type:'spki'});
if(manifest.key!==publicKey.toString('base64'))throw Error('Signing key does not match the extension manifest');
const id=[...createHash('sha256').update(publicKey).digest().subarray(0,16)].map(n=>String.fromCharCode(97+(n>>4))+String.fromCharCode(97+(n&15))).join('');
const profile=fs.mkdtempSync(path.join(os.tmpdir(),'modeld-pack-'));
try{
 const result=spawnSync(process.env.MODELD_CHROME||'/opt/google/chrome/chrome',
  ['--pack-extension='+source,'--pack-extension-key='+path.resolve(keyFile),'--user-data-dir='+profile,'--no-message-box'],{encoding:'utf8'});
 if(result.status!==0)throw Error('Chrome packaging failed: '+result.stderr);
 fs.mkdirSync(outputDirectory,{recursive:true});
 const name='modeld-browser-'+manifest.version+'.crx';
 const output=path.join(outputDirectory,name);fs.copyFileSync(source+'.crx',output);fs.chmodSync(output,0o644);
 const url='https://github.com/csbxd/modeld/releases/download/v'+manifest.version+'/'+name;
 fs.writeFileSync(path.join(outputDirectory,'browser-update.xml'),
  '<?xml version="1.0" encoding="UTF-8"?>\n<gupdate xmlns="http://www.google.com/update2/response" protocol="2.0"><app appid="'+id+'"><updatecheck codebase="'+url+'" version="'+manifest.version+'" /></app></gupdate>\n');
 console.log(JSON.stringify({id,version:manifest.version,sha256:createHash('sha256').update(fs.readFileSync(output)).digest('hex'),file:output}));
}finally{fs.rmSync(profile,{recursive:true,force:true});}
