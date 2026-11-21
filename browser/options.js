const $=id=>document.getElementById(id);
async function send(type,value={}){const response=await chrome.runtime.sendMessage({type,...value});if(!response?.ok)throw Error(response?.error||'扩展连接失败');return response.value;}
function result(id,message,error=false){$(id).textContent=message;$(id).className=error?'error':'success';}
async function load(){
  try{
    const value=await send('config.get');
    $('endpoint').value=value.service.endpoint;$('connect').value=value.service.connect_address;
    $('auto').checked=value.preferences.autoEnable;$('buffer').value=value.preferences.bufferSeconds;
    $('back').value=value.preferences.backSeconds;$('cache').value=value.preferences.cacheMiB;
    $('codec').value=value.preferences.videoCodec;
    $('qp').value=value.preferences.qp;
    $('av1-target-bitrate').value=value.preferences.av1TargetBitrateMbps;
    $('av1-max-bitrate').value=value.preferences.av1MaxBitrateMbps;
    result('connection',value.service.configured?'本机配置已就绪':'填写服务地址和密钥后保存');
  }catch(error){result('connection','本机组件未连接：'+error.message,true);}
}
$('settings').addEventListener('submit',async event=>{
  event.preventDefault();
  try{
    await send('config.save',{service:{endpoint:$('endpoint').value.trim(),connect_address:$('connect').value.trim(),token:$('token').value.trim()},
      preferences:{
        autoEnable:$('auto').checked,bufferSeconds:Number($('buffer').value),
        backSeconds:Number($('back').value),cacheMiB:Number($('cache').value),
        videoCodec:$('codec').value,qp:Number($('qp').value),
        av1TargetBitrateMbps:Number($('av1-target-bitrate').value),
        av1MaxBitrateMbps:Number($('av1-max-bitrate').value)
      }});
    $('token').value='';result('result','已保存，下次开启插帧时生效');
  }catch(error){result('result',error.message,true);}
});
$('test').addEventListener('click',async()=>{
  result('connection','正在连接…');
  try{
    const value=await send('service.test');
    const codecs=value.browser.av1_vbr?.target_bitrate_mbps_range?'H.264 / AV1 VBR':'H.264';
    result('connection','已连接 modeld '+value.version+' · '+codecs+' · 60 fps');
  }
  catch(error){result('connection',error.message,true);}
});
load();
