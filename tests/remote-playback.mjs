// node tests/remote-playback.mjs PLUGIN CONFIG.json VIDEO REPORT.json
// Optional MODELD_MPV_TEST_COOKIES selects an existing yt-dlp browser-cookie profile.
// MODELD_MODELCTL selects the matching 2.0 CLI before installation.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import assert from 'node:assert/strict';
import {spawn,spawnSync,execFileSync} from 'node:child_process';
import {setTimeout as sleep} from 'node:timers/promises';
const [pluginFile,configFile,video,reportFile]=process.argv.slice(2);
if(!reportFile)throw Error('Expected PLUGIN CONFIG.json VIDEO REPORT.json');
const config=JSON.parse(fs.readFileSync(configFile,'utf8'));
const root=fs.mkdtempSync(path.join(os.tmpdir(),'modeld-remote-'));
const socket=path.join(root,'mpv.sock'),configuration=path.join(root,'config');
fs.mkdirSync(path.join(configuration,'modeld'),{recursive:true,mode:0o700});
fs.writeFileSync(path.join(configuration,'modeld/client.json'),JSON.stringify(config),{mode:0o600});
const logFile=reportFile+'.mpv.log',log=fs.openSync(logFile,'w',0o600);
const options=['--no-config','--load-scripts=no','--script='+path.resolve(pluginFile),
    '--input-ipc-server='+socket,'--vo=null','--ao=null','--pause','--keep-open=yes','--idle=yes',
    '--cache=yes','--demuxer-max-bytes=128MiB','--demuxer-max-back-bytes=128MiB','--demuxer-readahead-secs=30',
    '--start='+(video.startsWith('http')?30:2),'--ytdl-format=bestvideo[height<=1080][fps<60]+bestaudio/best[height<=1080][fps<60]'];
if(process.env.MODELD_MPV_TEST_COOKIES)options.push('--ytdl-raw-options=cookies-from-browser='+process.env.MODELD_MPV_TEST_COOKIES);
const child=spawn('mpv',[...options,video],{env:{...process.env,XDG_CONFIG_HOME:configuration,XDG_STATE_HOME:path.join(root,'state')},stdio:['ignore',log,log]});
let ipc,input='',sequence=0;const pending=new Map();
function command(command){
    return new Promise((resolve,reject)=>{
        const request_id=++sequence;
        const timer=setTimeout(()=>{pending.delete(request_id);reject(Error('mpv IPC timed out'));},5000);
        pending.set(request_id,response=>{clearTimeout(timer);response.error==='success'?resolve(response.data):reject(Error(response.error));});
        ipc.write(JSON.stringify({command,request_id})+'\n');
    });
}
const get=name=>command(['get_property',name]);
async function until(check,description,timeout=90000){
    const end=performance.now()+timeout;
    while(performance.now()<end){
        try{if(await check())return;}catch(e){if(e.message!=='property unavailable')throw e;}
        await sleep(50);
    }
    throw Error('Timed out: '+description);
}
function status(){
    const result=spawnSync(process.env.MODELD_MODELCTL||'modelctl',['--config',configFile,'status'],{encoding:'utf8',timeout:10000});
    if(result.status!==0)throw Error('Cannot query the matching API with modelctl');
    return JSON.parse(result.stdout);
}
function network(){
    const output=execFileSync('ss',['-ntipH'],{encoding:'utf8'}).split('\n'),result=new Map();
    for(let i=0;i<output.length;i++){
        const line=output[i];if(!line.includes('pid='+child.pid+','))continue;
        const peer=line.trim().split(/\s+/)[4],fd=line.match(/fd=(\d+)/)?.[1];
        const local=peer.startsWith('127.0.0.1:'),apiPeer=peer.startsWith((config.connect_address||new URL(config.endpoint).hostname)+':');
        result.set(fd,{kind:local?'loopback':apiPeer?'modeld':'other_https',
            bytes:Number((output[i+1]||'').match(/bytes_received:(\d+)/)?.[1]||0)});
    }
    return result;
}
const report={version:'2.0.0',headless:true};
try{
    report.status_before=status();assert.equal(report.status_before.protocol,2);
    await until(()=>fs.existsSync(socket),'IPC socket');
    ipc=net.connect(socket);await new Promise(resolve=>ipc.once('connect',resolve));
    ipc.on('data',bytes=>{
        input+=bytes;
        while(input.includes('\n')){
            const end=input.indexOf('\n'),response=JSON.parse(input.slice(0,end));input=input.slice(end+1);
            if(pending.has(response.request_id)){pending.get(response.request_id)(response);pending.delete(response.request_id);}
        }
    });
    await until(async()=>Boolean(await get('video-params')),'source decoding');
    const originals=await get('track-list'),originalCodec=originals.find(t=>t.type==='video'&&t.selected).codec;
    const sourceName=await get('path'),opened=await get('stream-open-filename'),seekCache=await get('demuxer-seekable-cache');
    report.source={codec:originalCodec,width:(await get('video-params')).w,height:(await get('video-params')).h};
    await command(['loadfile',video,'append']);
    const entries=async()=>(await get('playlist')).map(({id,filename})=>({id,filename}));
    const playlist=await entries();
    const processed=async()=>(await get('track-list')).some(t=>t.type==='video'&&t.selected&&t.codec==='hevc'&&!t.external);
    await command(['script-message','toggle']);
    await until(processed,'processed main media');
    assert((await get('stream-open-filename')).endsWith('/timeline.edl'));
    assert.equal(await get('path'),sourceName);assert.equal(await get('pause'),true);
    assert.equal(await get('demuxer-seekable-cache'),seekCache);assert.deepEqual(await entries(),playlist);
    await until(async()=>(await get('demuxer-cache-state'))['cache-end']>=Math.min((await get('duration'))-0.2,(await get('time-pos'))+10),'paused forward buffering');
    const params=await get('video-params');
    report.output={width:params.w,height:params.h,fps:await get('container-fps')};
    assert.equal(report.output.width,report.source.width);assert.equal(report.output.height,report.source.height);
    assert(Math.abs(report.output.fps-60)<0.1);
    report.tracks=(await get('track-list')).map(({type,codec,external,selected})=>({type,codec,external,selected}));
    assert(report.tracks.filter(t=>t.type==='audio'||t.type==='video').every(t=>!t.external));
    const before=network();await sleep(2000);const after=network(),traffic={};
    for(const [fd,s]of before){
        if(!traffic[s.kind])traffic[s.kind]={connections:0,received_bytes:0};
        traffic[s.kind].connections++;
        if(after.has(fd))traffic[s.kind].received_bytes+=after.get(fd).bytes-s.bytes;
    }
    report.network_sample={seconds:2,...traffic};
    const start=await get('time-pos'),wall=performance.now();report.samples=[];
    await command(['set_property','pause',false]);
    for(let i=0;i<8;i++){await sleep(1000);report.samples.push({position:await get('time-pos'),avsync:await get('avsync'),cache_wait:await get('paused-for-cache')});}
    report.clock={wall_seconds:(performance.now()-wall)/1000,advance_seconds:(await get('time-pos'))-start,
        max_abs_avsync:Math.max(...report.samples.map(s=>Math.abs(s.avsync)))};
    assert(report.clock.advance_seconds>=6);assert(report.clock.max_abs_avsync<0.1);
    await command(['set_property','pause',true]);
    const duration=await get('duration'),far=Math.min(120,duration/2);
    for(const target of [far,start]){
        await command(['seek',target,'absolute+exact']);
        await until(async()=>!await get('seeking')&&Math.abs((await get('time-pos'))-target)<0.2,'seek '+target);
        await sleep(250);assert(Math.abs((await get('time-pos'))-target)<0.2);
    }
    report.seeks=[far,start];
    await command(['script-message','toggle']);
    await until(async()=>(await get('stream-open-filename'))===opened,'original source restored');
    await until(async()=>(await get('track-list')).some(t=>t.type==='video'&&t.selected&&t.codec===originalCodec),'original codec restored');
    assert.deepEqual(await entries(),playlist);
    await command(['script-message','toggle']);await until(processed,'processed media restored');
    await until(async()=>!await get('seeking')&&Math.abs((await get('time-pos'))-start)<0.2,'processed position restored');
    assert.equal(await get('pause'),true);assert.deepEqual(await entries(),playlist);
    report.original_restored=true;report.playlist_preserved=true;report.user_pause_preserved=true;
    report.status_during=status();report.passed=true;
}catch(error){report.error=error.message;throw error;}
finally{
    if(ipc){try{await command(['quit']);}catch{}ipc.destroy();}
    if(child.exitCode===null)child.kill('SIGTERM');
    await new Promise(resolve=>child.exitCode===null?child.once('exit',resolve):resolve());
    fs.closeSync(log);
    const sourceLog=path.join(root,'state/modeld/mpv.log');
    if(fs.existsSync(sourceLog)){fs.copyFileSync(sourceLog,reportFile+'.events.log');fs.chmodSync(reportFile+'.events.log',0o600);}
    fs.rmSync(root,{recursive:true,force:true});
    fs.writeFileSync(reportFile,JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify({passed:report.passed,error:report.error,source:report.source,clock:report.clock,network:report.network_sample},null,2));
}
