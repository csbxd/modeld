let current;
async function send(type,value={}){const r=await chrome.runtime.sendMessage({type,...value});if(!r?.ok)throw Error(r?.error||'连接失败');return r.value;}
async function refresh(){
  [current]=await chrome.tabs.query({active:true,currentWindow:true});
  if(!current?.url?.includes('bilibili.com/')){document.getElementById('status').textContent='打开一个 B 站视频页后使用';document.getElementById('toggle').disabled=true;return;}
  const state=await send('tab.status',{tabId:current.id});
  document.getElementById('status').textContent=state.error||(state.active?'插帧已开启 · RIFE Heavy FP32':'使用远端 GPU，在原播放器内补到 60 fps');
  document.getElementById('toggle').textContent=state.active?'关闭插帧':'开启插帧';
}
document.getElementById('toggle').onclick=async()=>{try{await send('tab.toggle',{tabId:current.id});window.close();}catch(error){document.getElementById('status').textContent=error.message;}};
document.getElementById('options').onclick=()=>chrome.runtime.openOptionsPage();
refresh().catch(error=>{document.getElementById('status').textContent=error.message;});
