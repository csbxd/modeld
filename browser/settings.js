export const defaults={autoEnable:false,bufferSeconds:12,backSeconds:60,cacheMiB:256,qp:18};
export function preferences(value={}){
  const result={...defaults,...value};
  for(const [name,min,max]of [['bufferSeconds',4,60],['backSeconds',0,300],['cacheMiB',32,1024],['qp',0,40]]){
    if(!Number.isInteger(result[name])||result[name]<min||result[name]>max)throw Error('无效配置：'+name);
  }
  result.autoEnable=Boolean(result.autoEnable);return result;
}
