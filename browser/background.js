import{preferences}from'./settings.js';
const host='org.csbxd.modeld.browser';
let port=null,next=0;
const pending=new Map(),tabs=new Map();
async function native(action,values={}){
  if(!port){
    port=chrome.runtime.connectNative(host);
    port.onMessage.addListener(message=>{
      const entry=pending.get(message.id);if(!entry)return;
      if(!message.ok){pending.delete(message.id);clearTimeout(entry.timer);entry.reject(Object.assign(Error(message.error?.message||'本机连接失败'),message.error));return;}
      if('data'in message){
        entry.parts.push(message.data);
        if(!message.done)return;
        pending.delete(message.id);clearTimeout(entry.timer);
        entry.resolve({base64:entry.parts.join(''),bytes:message.bytes});
      }else{pending.delete(message.id);clearTimeout(entry.timer);entry.resolve(message.result);}
    });
    port.onDisconnect.addListener(()=>{
      const error=chrome.runtime.lastError?.message||'本机插帧服务连接已断开';port=null;
      for(const entry of pending.values()){clearTimeout(entry.timer);entry.reject(Error(error));}
      pending.clear();
      for(const [id,state]of tabs){state.error=error;chrome.tabs.sendMessage(id,{type:'modeld.failure',error}).catch(()=>{});}
    });
  }
  const id=++next;
  return new Promise((resolve,reject)=>{
    const timer=setTimeout(()=>{pending.delete(id);reject(Error('远端处理超时，请重试'));},120000);
    pending.set(id,{resolve,reject,timer,parts:[]});port.postMessage({id,action,...values});
  });
}
const extensionPage=sender=>sender.url?.startsWith(chrome.runtime.getURL(''));
async function page(tabId,operation,parameters){
  await chrome.scripting.executeScript({target:{tabId},world:'MAIN',files:['page.js']});
  const result=await chrome.scripting.executeScript({target:{tabId},world:'MAIN',
    func:async(operation,parameters)=>{
      try{return {ok:true,value:await window[Symbol.for('org.csbxd.modeld.page')][operation](parameters)};}
      catch(error){return {ok:false,error:error.message};}
    },args:[operation,parameters??null]});
  const value=result[0]?.result;
  if(!value?.ok)throw Error(value?.error||'无法操作 B 站播放器');return value.value;
}
async function stop(tabId){
  const state=tabs.get(tabId);if(!state)return;
  tabs.delete(tabId);clearInterval(state.lease);
  if(state.created)await native('close',{session:state.created.id}).catch(()=>{});
  state.cache?.clear();chrome.action.setBadgeText({tabId,text:''}).catch(()=>{});
}
async function route(message,sender){
  const tabId=sender.tab?.id;
  if(message.type==='options.open'){await chrome.runtime.openOptionsPage();return null;}
  if(message.type==='config.get'){
    const local=await chrome.storage.local.get('preferences');
    return {service:await native('config_get'),preferences:preferences(local.preferences)};
  }
  if(message.type==='config.save'){
    if(!extensionPage(sender))throw Error('仅设置页可以修改服务配置');
    const pref=preferences(message.preferences);
    const service=await native('config_save',{value:message.service});
    await chrome.storage.local.set({preferences:pref});return {service,preferences:pref};
  }
  if(message.type==='service.test')return native('health');
  if(message.type==='tab.status'){
    const state=tabs.get(message.tabId||tabId);
    return {active:!!state?.created,error:state?.error||null};
  }
  if(message.type==='tab.toggle'){
    if(!extensionPage(sender))throw Error('无效操作');
    const tab=await chrome.tabs.get(message.tabId);
    return chrome.tabs.sendMessage(tab.id,{type:'modeld.toggle'});
  }
  if(!Number.isInteger(tabId)||!/^https:\/\/(?:www|m)\.bilibili\.com\//.test(sender.url||''))throw Error('仅 B 站视频页可以请求插帧');
  if(message.type==='page.probe')return page(tabId,'probe');
  if(message.type==='page.takeover')return page(tabId,'takeover',message.value);
  if(message.type==='page.restore')return page(tabId,'restore');
  if(message.type==='session.stop'){await stop(tabId);return null;}
  if(message.type==='session.open'){
    await stop(tabId);
    const state={cache:new Map(),cacheBytes:0,clock:0,paused:true,error:null};tabs.set(tabId,state);
    const local=await chrome.storage.local.get('preferences'),pref=preferences(local.preferences);
    const source=await page(tabId,'probe');
    const output={
      video_codec:pref.videoCodec,qp:pref.videoCodec==='av1'?pref.av1Qp:pref.qp
    };
    let created;const deadline=Date.now()+45000;
    for(;;){
      if(tabs.get(tabId)!==state)throw Error('已取消插帧');
      try{created=await native('open',{video:source.video,audio:source.audio,user_agent:source.user_agent,...output});break;}
      catch(error){if(error.code!=='session_limit'||!error.retryable||Date.now()>=deadline)throw error;await new Promise(resolve=>setTimeout(resolve,250));}
    }
    if(tabs.get(tabId)!==state){await native('close',{session:created.id});throw Error('已取消插帧');}
    state.created=created;state.preferences=pref;
    const renew=()=>native('lease',{session:created.id,paused:state.paused})
      .then(()=>{state.error=null;},error=>{state.error=error.message;});
    state.lease=setInterval(renew,10000);
    await renew();
    chrome.action.setBadgeText({tabId,text:'60'});chrome.action.setBadgeBackgroundColor({tabId,color:'#1b8f71'});
    return {created,preferences:pref,source:{width:source.width,height:source.height,bvid:source.bvid,cid:source.cid}};
  }
  const state=tabs.get(tabId);if(!state?.created)throw Error('插帧会话已关闭');
  if(message.type==='session.pause'){state.paused=Boolean(message.paused);await native('lease',{session:state.created.id,paused:state.paused});return null;}
  if(message.type==='media.init')return native('init',{session:state.created.id});
  if(message.type==='media.segment'){
    const index=message.index;
    if(!Number.isInteger(index)||index<0||index>=state.created.manifest.media.segment_count)throw Error('无效分段位置');
    let entry=state.cache.get(index);
    if(entry){entry.used=++state.clock;return entry.promise;}
    entry={used:++state.clock,bytes:0,promise:null};
    entry.promise=native('segment',{session:state.created.id,index}).then(value=>{
      entry.bytes=value.bytes;state.cacheBytes+=value.bytes;
      while(state.cacheBytes>state.preferences.cacheMiB*1048576){
        const candidates=[...state.cache.entries()].filter(([,e])=>e.bytes>0).sort((a,b)=>a[1].used-b[1].used);
        if(!candidates.length)break;const [key,e]=candidates[0];state.cache.delete(key);state.cacheBytes-=e.bytes;
      }
      return value;
    }).catch(error=>{state.cache.delete(index);throw error;});
    state.cache.set(index,entry);return entry.promise;
  }
  throw Error('未知请求');
}
chrome.runtime.onMessage.addListener((message,sender,reply)=>{
  route(message,sender).then(value=>reply({ok:true,value})).catch(error=>reply({ok:false,error:error.message}));
  return true;
});
chrome.tabs.onRemoved.addListener(tabId=>stop(tabId));
chrome.runtime.onInstalled.addListener(async()=>{
  const values=await chrome.storage.local.get('preferences');
  if(!values.preferences)await chrome.storage.local.set({preferences:preferences()});
  await chrome.storage.local.setAccessLevel({accessLevel:'TRUSTED_CONTEXTS'});
  const existing=await chrome.tabs.query({url:['https://www.bilibili.com/video/*','https://www.bilibili.com/bangumi/play/*']});
  for(const tab of existing)chrome.scripting.executeScript({target:{tabId:tab.id},files:['content.js']}).catch(()=>{});
});
