export const defaults = {
  autoEnable: false, bufferSeconds: 12, backSeconds: 60, cacheMiB: 256,
  videoCodec: 'av1', qp: 18, av1TargetBitrateMbps: 12, av1MaxBitrateMbps: 18
};
export function preferences(value={}){
  const result={...defaults};
  for(const key of Object.keys(result))if(Object.hasOwn(value??{},key))result[key]=value[key];
  if(Object.hasOwn(value??{},'av1MaxBitrateMbps')&&!Object.hasOwn(value,'av1TargetBitrateMbps')){
    result.av1TargetBitrateMbps=Math.max(1,Math.round(result.av1MaxBitrateMbps*2/3));
  }
  if(!['h264','av1'].includes(result.videoCodec))throw Error('请选择 H.264 或 AV1');
  const ranges = [
    ['bufferSeconds', 4, Number.MAX_SAFE_INTEGER], ['backSeconds', 0, Number.MAX_SAFE_INTEGER],
    ['cacheMiB', 32, 1024], ['qp', 0, 40],
    ['av1MaxBitrateMbps', 1, 20], ['av1TargetBitrateMbps', 1, 20]
  ];
  for(const [name,min,max]of ranges){
    if(!Number.isSafeInteger(result[name])||result[name]<min||result[name]>max)throw Error('无效配置：'+name);
  }
  if(result.av1TargetBitrateMbps>result.av1MaxBitrateMbps)throw Error('AV1 目标码率不能超过峰值码率');
  result.autoEnable=Boolean(result.autoEnable);return result;
}
