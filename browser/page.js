(()=>{
  const key=Symbol.for('org.csbxd.modeld.page');
  if(window[key])return;
  let active=null;
  const video=()=>window.player?.mediaElement?.()||document.querySelector('video,bwp-video');
  const allowed=value=>{
    try{const u=new URL(value);return ['http:','https:'].includes(u.protocol)&&(/\.(bilivideo\.com|bilivideo\.cn)$/).test(u.hostname);}catch{return false;}
  };
  const file=value=>{try{return new URL(value).pathname.split('/').pop();}catch{return '';}};
  function urls(selected,entries){
    if(!allowed(selected))throw Error('尚未取得当前媒体地址，请先播放几秒');
    const candidates=[];
    const add=value=>{if(typeof value==='string'&&allowed(value)&&file(value)===file(selected)&&!candidates.includes(value))candidates.push(value);};
    for(const entry of entries||[]){
      add(entry.baseUrl);
      for(const backup of entry.backupUrl||[])add(backup);
    }
    candidates.sort((a,b)=>{
      const rank=value=>{const u=new URL(value);return (u.protocol==='https:'?0:2)+(u.hostname.startsWith('upos-')?0:1);};
      return rank(a)-rank(b);
    });
    const result=candidates.filter(value=>value!==selected);result.push(selected);return result.slice(0,9);
  }
  function probe(){
    if(active)return active.source;
    const p=window.player,k=p?.__core?.(),v=video(),dash=k?.getCorePlayer?.();
    if(!p||!k||!v||!dash||typeof k.getCurrentPlayURLFor!=='function')throw Error('当前页面播放器尚未准备好');
    const manifest=dash.getSource?.()||{};
    const selectedVideo=k.getCurrentPlayURLFor('video'),selectedAudio=k.getCurrentPlayURLFor('audio');
    const info=p.getManifest?.()||{};
    const videoUrls=urls(selectedVideo,manifest.video);
    let audio=null;
    if(selectedAudio){
      const audioEntries=manifest.audio||[];
      const exact=audioEntries.find(entry=>file(entry.baseUrl)===file(selectedAudio));
      // The browser transport uses the AAC track list; FLAC/Dolby are separate lists.
      const candidate=exact?.baseUrl||audioEntries[0]?.baseUrl;
      if(candidate)audio=urls(exact?selectedAudio:candidate,audioEntries);
    }
    if(v.videoWidth>0&&(Math.max(v.videoWidth,v.videoHeight)>1920||Math.min(v.videoWidth,v.videoHeight)>1080))
      throw Error('请在 B 站选择 1080p 或以下画质');
    return {video:videoUrls,audio,width:v.videoWidth,height:v.videoHeight,
      position:v.currentTime,duration:v.duration,paused:v.paused,bvid:info.bvid,cid:info.cid,p:info.p,
      user_agent:navigator.userAgent,title:document.title};
  }
  async function takeover({url,duration}){
    if(active)throw Error('插帧已开启');
    if(typeof url!=='string'||!url.startsWith('blob:'+location.origin+'/'))throw Error('无效的网页媒体地址');
    const source=probe(),p=window.player,k=p.__core(),original=k.player,v=video();
    const originalSource=original.getSource();
    const paused=v.paused,time=v.currentTime,rate=v.playbackRate,volume=v.volume,muted=v.muted;
    p.pause();
    original.attachSource(null);
    const ahead=()=>{for(let i=0;i<v.buffered.length;i++)if(v.buffered.start(i)<=v.currentTime+0.1&&v.buffered.end(i)>v.currentTime)return v.buffered.end(i)-v.currentTime;return 0;};
    const replacements={
      play:()=>v.play(),pause:()=>v.pause(),isPaused:()=>v.paused,isSeeking:()=>v.seeking,
      time:()=>v.currentTime,duration:()=>Number.isFinite(v.duration)?v.duration:duration,
      seek:t=>{v.currentTime=Math.max(0,Math.min(duration,Number(t)));},
      getVideoElement:()=>v,getBufferLength:ahead,isReady:()=>true,isDynamic:()=>false,
      setQualityFor:(...args)=>{restore().then(()=>{original.setQualityFor(...args);window.dispatchEvent(new Event('modeld.native-source-change'));});},
      setVolume:n=>{v.volume=n;},getVolume:()=>v.volume,setMute:n=>{v.muted=n;},isMuted:()=>v.muted,
      setPlaybackRate:n=>{v.playbackRate=n;},getPlaybackRate:()=>v.playbackRate
    };
    k.player=new Proxy(original,{get:(target,name)=>name in replacements?replacements[name]:Reflect.get(target,name)});
    active={source,k,original,originalSource,v,p};
    v.src=url;v.playbackRate=rate;v.volume=volume;v.muted=muted;
    return {position:time,paused,rate,volume,muted};
  }
  async function restore(){
    if(!active)return;
    const state=active;active=null;
    const {v,k,original,originalSource}=state;
    if(!v.isConnected||window.player?.__core?.()!==k){try{original.reset?.();}catch{}return;}
    const time=v.currentTime,paused=v.paused,rate=v.playbackRate,volume=v.volume,muted=v.muted;
    v.pause();v.removeAttribute('src');v.load();k.player=original;
    original.attachSource(originalSource);
    const ready=()=>{
      if(v.readyState<1)return false;
      try{v.currentTime=time;v.playbackRate=rate;v.volume=volume;v.muted=muted;if(!paused)v.play().catch(()=>{});}catch{}
      return true;
    };
    if(!ready())await new Promise(resolve=>{
      const done=()=>{clearTimeout(timer);v.removeEventListener('loadedmetadata',done);ready();resolve();};
      const timer=setTimeout(done,8000);v.addEventListener('loadedmetadata',done,{once:true});
    });
  }
  Object.defineProperty(window,key,{value:{probe,takeover,restore,active:()=>!!active},configurable:false});
})();
