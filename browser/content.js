(()=>{
  if(globalThis.__modeldBrowserContent)return;
  globalThis.__modeldBrowserContent=true;
  const call=async(type,values={})=>{
    const reply=await chrome.runtime.sendMessage({type,...values});
    if(!reply?.ok)throw Error(reply?.error||'扩展连接失败');return reply.value;
  };
  const bytes=value=>{const text=atob(value.base64),result=new Uint8Array(text.length);for(let i=0;i<text.length;i++)result[i]=text.charCodeAt(i);return result;};
  const routeKey=()=>location.pathname+'?p='+(new URLSearchParams(location.search).get('p')||'1');
  const video=()=>document.querySelector('.bpx-player-video-wrap video,video,bwp-video');
  let player=null,epoch=0,preferences=null,starting=false,phase='off',error='',originalRoute='';
  const root=document.createElement('div');root.id='modeld-browser';
  const shadow=root.attachShadow({mode:'open'});
  shadow.innerHTML='<style>:host{position:absolute;right:18px;top:16px;z-index:10020;display:block;font:12px/1.4 system-ui,sans-serif;color:#fff}section{display:flex;align-items:center;gap:7px;background:#15252ae8;box-shadow:0 3px 18px #0004;border:1px solid #ffffff24;border-radius:9px;padding:5px 7px;backdrop-filter:blur(12px)}button{font:inherit;cursor:pointer;color:inherit;border:0;border-radius:6px;background:transparent;padding:5px 7px}button:hover{background:#ffffff20}button.active{background:#168367}small{max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#c8d9d5}.error{color:#ffbca9}button:focus-visible{outline:2px solid #64e4b8}</style><section><button id="toggle" title="Alt+Shift+I">插帧 60 FPS</button><small id="state">未开启</small><button id="settings" title="插帧设置">⚙</button></section>';
  const button=shadow.querySelector('#toggle'),label=shadow.querySelector('#state');
  function render(){
    button.classList.toggle('active',phase==='on'||phase==='buffering');
    button.textContent=starting?'取消准备':phase==='on'||phase==='buffering'?'关闭插帧':'插帧 60 FPS';
    label.textContent=error||({off:'未开启',opening:'准备中',on:'60 fps',buffering:'缓冲中'}[phase]||phase);
    label.classList.toggle('error',!!error);label.title=error;
    root.dataset.state=phase;
  }
  function mount(){const container=document.querySelector('.bpx-player-container,#bilibili-player,.bpx-player-primary-area');if(container&&!root.isConnected)container.append(root);}
  function hasRange(v,time){for(let i=0;i<v.buffered.length;i++)if(v.buffered.start(i)<=time+0.08&&v.buffered.end(i)>time+0.02)return i;return -1;}
  class Playback{
    constructor(opened){
      this.created=opened.created;this.preferences=opened.preferences;this.media=this.created.manifest.media;
      this.video=video();this.source=new MediaSource();this.url=URL.createObjectURL(this.source);
      this.running=true;this.loaded=new Set();this.generation=0;this.serial=Promise.resolve();this.busy=false;
      this.cleanups=[];this.events=[];this.initializing=true;this.lastPosition=0;
    }
    failure(error){if(this.running&&player===this)fail(error);}
    listen(type,handler){this.video.addEventListener(type,handler);this.events.push([type,handler]);}
    mutate(work){
      const promise=this.serial.then(async()=>{
        if(!this.running)return;
        if(this.buffer?.updating)await new Promise((resolve,reject)=>{this.buffer.addEventListener('updateend',resolve,{once:true});this.buffer.addEventListener('error',()=>reject(Error('视频缓冲写入失败')),{once:true});});
        if(!this.running)return;
        return work();
      });
      this.serial=promise.catch(()=>{});return promise;
    }
    append(data){
      return this.mutate(()=>new Promise((resolve,reject)=>{
        const done=()=>{this.buffer.removeEventListener('error',fail);resolve();};
        const fail=()=>{this.buffer.removeEventListener('updateend',done);reject(Error('浏览器解码缓冲出错'));};
        this.buffer.addEventListener('updateend',done,{once:true});this.buffer.addEventListener('error',fail,{once:true});
        try{this.buffer.appendBuffer(data);}catch(error){this.buffer.removeEventListener('updateend',done);this.buffer.removeEventListener('error',fail);reject(error);}
      }));
    }
    async begin(){
      if(!MediaSource.isTypeSupported(this.created.mime_type))throw Error('这台浏览器不能解码插帧输出');
      const opened=new Promise(resolve=>this.source.addEventListener('sourceopen',resolve,{once:true}));
      const saved=await call('page.takeover',{value:{url:this.url,duration:this.media.duration_seconds}});
      await opened;if(!this.running)return;
      this.buffer=this.source.addSourceBuffer(this.created.mime_type);
      await this.append(bytes(await call('media.init')));
      if(!this.running)return;
      this.source.duration=this.media.duration_seconds;
      this.initialTarget=Math.min(saved.position,this.media.duration_seconds-0.05);
      this.video.currentTime=this.initialTarget;
      this.listen('seeking',()=>{if(this.initializing||!this.running)return;this.generation++;this.loaded.clear();this.pump().catch(error=>this.failure(error));});
      this.listen('play',()=>call('session.pause',{paused:false}).catch(()=>{}));
      this.listen('pause',()=>call('session.pause',{paused:true}).catch(()=>{}));
      this.listen('error',()=>{if(this.running)fail(Error('视频解码失败，请关闭后重试'));});
      await this.fill(this.initialTarget,Math.min(2,this.media.segment_count));
      if(!this.running)return;
      this.initializing=false;
      const range=hasRange(this.video,this.initialTarget);
      if(range>=0&&this.video.currentTime<this.video.buffered.start(range))this.video.currentTime=this.video.buffered.start(range)+0.001;
      if(!saved.paused)await this.video.play().catch(()=>{});
      await call('session.pause',{paused:this.video.paused});
      this.timer=setInterval(()=>this.pump().catch(error=>this.failure(error)),300);
      this.statusTimer=setInterval(()=>{
        if(!this.running)return;
        const range=hasRange(this.video,this.video.currentTime);
        phase=!this.video.paused&&range<0?'buffering':'on';render();
        root.dataset.position=String(this.video.currentTime);
        root.dataset.width=String(this.video.videoWidth);root.dataset.height=String(this.video.videoHeight);
        root.dataset.frames=String(this.video.getVideoPlaybackQuality?.().totalVideoFrames||0);
      },500);
      phase='on';render();this.pump().catch(error=>this.failure(error));
    }
    async fill(time,count){
      const start=Math.max(0,Math.floor(time/this.media.segment_seconds)),generation=this.generation;
      for(let i=start;i<Math.min(this.media.segment_count,start+count);i++){
        if(!this.running||generation!==this.generation)return;
        if(this.loaded.has(i))continue;
        const data=await call('media.segment',{index:i});
        if(!this.running||generation!==this.generation)return;
        await this.append(bytes(data));this.loaded.add(i);
      }
    }
    async pump(){
      if(!this.running||this.busy||!this.buffer||this.source.readyState!=='open')return;
      this.busy=true;
      try{
        const position=this.video.currentTime,range=hasRange(this.video,position);
        const end=range>=0?this.video.buffered.end(range):position;
        if(end-position<this.preferences.bufferSeconds){
          let index=Math.floor((end+0.025)/this.media.segment_seconds);
          if(range<0)index=Math.floor(position/this.media.segment_seconds);
          await this.fill(index*this.media.segment_seconds,2);
        }
        const trim=position-this.preferences.backSeconds;
        if(trim>0&&this.video.buffered.length&&this.video.buffered.start(0)<trim-5)
          await this.mutate(()=>new Promise((resolve,reject)=>{this.buffer.addEventListener('updateend',resolve,{once:true});try{this.buffer.remove(0,trim);}catch(error){reject(error);}}));
      }finally{this.busy=false;}
    }
    async stop(){
      this.running=false;this.generation++;
      clearInterval(this.timer);clearInterval(this.statusTimer);
      for(const [type,handler]of this.events)this.video.removeEventListener(type,handler);
      if(this.buffer?.updating)try{this.buffer.abort();}catch{}
      await call('session.stop').catch(()=>{});
      await call('page.restore').catch(()=>{});
      URL.revokeObjectURL(this.url);
    }
  }
  async function stop(){
    epoch++;starting=false;
    const current=player;player=null;
    if(current)await current.stop();else await call('session.stop').catch(()=>{});
    phase='off';render();
  }
  async function fail(cause){
    const message=cause?.message||String(cause);
    await stop();error=message;render();
  }
  async function toggle(){
    if(starting||player){await stop();return;}
    error='';phase='opening';starting=true;render();const current=++epoch;
    try{
      const opened=await call('session.open');
      if(current!==epoch)return;
      player=new Playback(opened);
      originalRoute=routeKey();
      await player.begin();
      if(current!==epoch){await stop();return;}
      starting=false;render();
    }catch(cause){if(current===epoch)await fail(cause);}
  }
  button.addEventListener('click',()=>toggle());
  shadow.querySelector('#settings').addEventListener('click',()=>chrome.runtime.sendMessage({type:'options.open'}));
  chrome.runtime.onMessage.addListener((message,sender,reply)=>{
    if(message.type==='modeld.toggle'){toggle();reply({ok:true});}
    if(message.type==='modeld.failure')fail(Error(message.error));
  });
  document.addEventListener('keydown',event=>{
    if(event.altKey&&event.shiftKey&&event.code==='KeyI'&&!event.target.closest('input,textarea,[contenteditable="true"]')){event.preventDefault();toggle();}
  });
  setInterval(()=>{
    mount();
    if(player&&(!player.video.isConnected||routeKey()!==originalRoute))stop();
  },1000);
  window.addEventListener('modeld.native-source-change',()=>stop());
  window.addEventListener('pagehide',()=>call('session.stop').catch(()=>{}));
  mount();render();
  call('config.get').then(value=>{
    preferences=value.preferences;
    if(!value.service.configured){label.textContent='请先配置服务';return;}
    if(preferences.autoEnable)setTimeout(()=>toggle(),2500);
  }).catch(cause=>{error='本机组件未连接，点击设置';render();});
})();
