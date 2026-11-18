import fs from'node:fs';
import{Native}from'./native.mjs';
const directory='/home/bxd/.cache/modeld/browser-development';
const input=JSON.parse(fs.readFileSync(directory+'/bilibili-input.json'));
const native=new Native('/home/bxd/src/modeld/build-browser/modeld-browser-host',directory+'/client.json');
let session;
try{
 console.log(await native.call('health'));
 session=await native.call('open',{video:input.video.urls,audio:input.audio.urls,user_agent:input.video.headers['User-Agent'],qp:18});
 fs.writeFileSync(directory+'/browser-session.json',JSON.stringify(session),{mode:0o600});
 console.log({mime:session.mime_type,media:session.manifest.media});
 const init=await native.call('init',{session:session.id});
 fs.writeFileSync(directory+'/init.mp4',init);
 for(const index of [0,1,4]){
   const data=await native.call('segment',{session:session.id,index});
   fs.writeFileSync(directory+'/'+index+'.m4s',data);
   fs.writeFileSync(directory+'/'+index+'.mp4',Buffer.concat([init,data]));
   console.log({segment:index,bytes:data.length});
 }
}catch(error){console.error(error.message);process.exitCode=1;}
finally{if(session)await native.call('close',{session:session.id});native.close();}
