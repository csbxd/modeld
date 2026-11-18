import fs from'node:fs';import assert from'node:assert/strict';import{setTimeout as sleep}from'node:timers/promises';import{Cdp}from'./cdp.mjs';
const directory='/home/bxd/.cache/modeld/browser-development';
const c=await Cdp.profile(directory+'/chrome');
const targets=(await c.send('Target.getTargets')).targetInfos;
const page=targets.find(t=>t.url.includes('bilibili.com/video/'));
const session=await c.attach(page.targetId);
await c.send('Page.enable',{},session);await c.send('Runtime.enable',{},session);
const errors=[];c.listeners.add(event=>{if(event.sessionId===session&&event.method==='Runtime.exceptionThrown')errors.push(event.params.exceptionDetails.exception?.description||event.params.exceptionDetails.text);});
async function evaluate(fn){return c.evaluate(session,'('+fn.toString()+')()');}
async function until(fn,label,limit=90000){const end=performance.now()+limit;while(performance.now()<end){const value=await fn();if(value)return value;await sleep(250);}throw Error('Timed out: '+label);}
const snapshot=()=>{const v=document.querySelector('video');const root=document.querySelector('#modeld-browser');let current;
try{current={time:window.player.getCurrentTime(),duration:window.player.getDuration(),paused:window.player.isPaused()};}catch(error){current={error:error.message};}
return {ui:root?{state:root.dataset.state,label:root.shadowRoot.querySelector('#state').textContent}:null,video:v?{time:v.currentTime,duration:v.duration,paused:v.paused,width:v.videoWidth,height:v.videoHeight,ready:v.readyState,quality:{totalVideoFrames:v.getVideoPlaybackQuality().totalVideoFrames,droppedVideoFrames:v.getVideoPlaybackQuality().droppedVideoFrames},buffer:Array.from({length:v.buffered.length},(_,i)=>[v.buffered.start(i),v.buffered.end(i)]),error:v.error?.message}:null,player:current};};
const report={test:'real Bilibili extension playback'};
try{
 await c.send('Page.bringToFront',{},session);
 const origin=await c.evaluate(session,'performance.timeOrigin');
 await c.send('Page.reload',{},session);
 await until(()=>c.evaluate(session,'performance.timeOrigin!=='+origin+'&&document.readyState==="complete"').catch(()=>false),'new document');
 await until(()=>evaluate(()=>!!document.querySelector('#modeld-browser')&&document.querySelector('video')?.videoWidth>0),'site and extension ready');
 report.original=await evaluate(snapshot);
 console.log('original',report.original.video.width,report.original.video.height);
 await evaluate(()=>document.querySelector('#modeld-browser').shadowRoot.querySelector('#toggle').click());
 await until(async()=>{const s=await evaluate(snapshot);if(s.ui?.state==='off'&&s.ui.label!=='未开启')throw Error(s.ui.label);return s.ui?.state==='on';},'interpolated output');
 report.started=await evaluate(snapshot);console.log('started',report.started);
 const start=report.started.video.time,frames=report.started.video.quality.totalVideoFrames;
 if(report.started.video.paused)await evaluate(()=>window.player.play());
 await sleep(8000);
 report.playing=await evaluate(snapshot);
 report.advance=report.playing.video.time-start;report.decoded_frames=report.playing.video.quality.totalVideoFrames-frames;
 assert(report.advance>6);assert(report.decoded_frames>300);
 await evaluate(()=>window.player.pause());await sleep(300);
 assert((await evaluate(snapshot)).video.paused);
 await evaluate(()=>window.player.seek(120));
 await until(async()=>Math.abs((await evaluate(snapshot)).video.time-120)<0.2&&(await evaluate(snapshot)).video.ready>=3,'native Bilibili seek');
 report.seek=await evaluate(snapshot);
 await evaluate(()=>window.player.play());await sleep(2000);
 assert((await evaluate(snapshot)).video.time>121);
 await evaluate(()=>document.querySelector('#modeld-browser').shadowRoot.querySelector('#toggle').click());
 await until(async()=>{const value=await evaluate(snapshot);return value.ui?.state==='off'&&value.video?.ready>=3;},'original source restored');
 report.restored=await evaluate(snapshot);
 assert(report.restored.video.width>0);
 assert(await evaluate(()=>!window[Symbol.for('org.csbxd.modeld.page')].active()&&!!window.player.__core().player.getSource()));
 // The native player may resume automatic quality selection on restoration.
 report.passed=true;
}catch(error){report.error=error.message;report.current=await evaluate(snapshot).catch(()=>null);console.error(error.message);}
finally{
 report.errors=errors.slice(-15);
 const image=await c.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false},session).catch(()=>null);
 if(image)fs.writeFileSync(directory+'/playback.png',Buffer.from(image.data,'base64'));
 fs.writeFileSync(directory+'/browser-report.json',JSON.stringify(report,null,2)+'\n');console.log(JSON.stringify(report,null,2));c.close();
}
if(!report.passed)process.exitCode=1;
