// Real GPU regression. Use a private test API with one session.
// probes requires a 3-second source timeout; media uses the normal 30-second budget.
// node tests/native-api.mjs API TOKEN_FILE FIXTURE_DIRECTORY REPORT.json [media|probes]
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {randomBytes,createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {setTimeout as sleep} from 'node:timers/promises';
const [api,tokenFile,directory,reportFile,phase='media']=process.argv.slice(2);
if(!reportFile)throw Error('Expected API TOKEN_FILE FIXTURE_DIRECTORY REPORT.json');
assert(['media','probes'].includes(phase));
const token=fs.readFileSync(tokenFile,'utf8').trim();
const auth={Authorization:'Bearer '+token,'Content-Type':'application/json'};
const model='rife-4.26-heavy-fp32';
function command(binary,args,encoding='utf8'){
    const result=spawnSync(binary,args,{encoding,maxBuffer:32*1024*1024});
    if(result.status!==0)throw Error(result.stderr.toString());
    return result.stdout;
}
const original=path.join(directory,'source.mkv');
command('ffmpeg',['-v','error','-y','-i',original,'-map','0:v','-an','-c','copy',path.join(directory,'video.mp4')]);
command('ffmpeg',['-v','error','-y','-i',original,'-vn','-sn','-c:a','aac','-b:a','192k',path.join(directory,'audio.m4a')]);
const address=Object.values(os.networkInterfaces()).flat().find(a=>a.family==='IPv4'&&a.address.startsWith('100.'))?.address;
assert(address,'NetBird source address is required');
const cap=randomBytes(16).toString('hex');
const files={source:original,video:path.join(directory,'video.mp4'),audio:path.join(directory,'audio.m4a'),
    portrait:path.join(directory,'portrait.mp4'),landscape:path.join(directory,'landscape.mp4')};
let audioFailures=0,primaryFailed=false,primaryPaused=false,primaryFast=false,backupReads=0;
const primarySockets=new Set();
const fixture=http.createServer((req,res)=>{
    if(!req.url.startsWith('/'+cap+'/'))return res.writeHead(404).end();
    const key=req.url.slice(cap.length+2);
    const primary=key==='primary';
    if(primary&&primaryFailed)return res.writeHead(503).end();
    if(key==='fail-audio'){audioFailures++;return res.writeHead(503).end();}
    if(key==='drip'){
        const bytes=fs.readFileSync(original);let i=0;
        res.writeHead(200,{'Content-Length':bytes.length});
        const timer=setInterval(()=>{if(i<bytes.length)res.write(bytes.subarray(i,i+1));else res.end();i++;},100);
        res.on('close',()=>clearInterval(timer));
        return;
    }
    const file=primary||key==='backup'?original:files[key];if(!file)return res.writeHead(404).end();
    if(key==='backup')backupReads++;
    const size=fs.statSync(file).size,range=req.headers.range?.match(/^bytes=(\d+)-(\d*)$/);
    let start=0,end=size-1;
    if(range){
        start=Number(range[1]);end=range[2]?Math.min(Number(range[2]),size-1):size-1;
        if(start>=size)return res.writeHead(416,{'Content-Range':'bytes */'+size}).end();
        res.statusCode=206;res.setHeader('Content-Range','bytes '+start+'-'+end+'/'+size);
    }
    res.setHeader('Accept-Ranges','bytes');res.setHeader('Content-Length',end-start+1);
    if(primary&&!primaryFast){
        const socket=req.socket;primarySockets.add(socket);socket.once('close',()=>primarySockets.delete(socket));
        const bytes=fs.readFileSync(file);let offset=start;
        const timer=setInterval(()=>{
            if(primaryPaused)return;
            if(offset>end){res.end();clearInterval(timer);return;}
            const next=Math.min(offset+4096,end+1);res.write(bytes.subarray(offset,next));offset=next;
        },10);
        res.on('close',()=>clearInterval(timer));
    }else fs.createReadStream(file,{start,end}).pipe(res);
});
await new Promise(resolve=>fixture.listen(0,address,resolve));
const source=key=>'http://'+address+':'+fixture.address().port+'/'+cap+'/'+key;
const payload=key=>({model,video:{urls:[source(key)]}});
async function request(route,method='GET',body,timeout=60000){
    const response=await fetch(api+route,{method,headers:auth,
        body:body&&JSON.stringify(body),signal:AbortSignal.timeout(timeout)});
    return {status:response.status,body:response.status===204?null:await response.json()};
}
async function close(id){await request('/api/v2/sessions/'+id,'DELETE');}
async function open(input){
    const end=performance.now()+5000;
    for(;;){
        const value=await request('/api/v2/sessions','POST',input);
        if(value.status!==429||performance.now()>=end)return value;
        await sleep(100);
    }
}
function packetHash(file){
    const data=JSON.parse(command('ffprobe',['-v','error','-select_streams','a:0','-show_packets','-show_data_hash','sha256',
        '-show_entries','packet=data_hash','-of','json',file]));
    return createHash('sha256').update(data.packets.map(p=>p.data_hash).join('\n')).digest('hex');
}
function pcm(file){return command('ffmpeg',['-v','error','-i',file,'-map','0:a:0','-f','f32le','-'],null);}
const report={version:'2.0.0',phase,backend:'real TensorRT-RTX/CUDA/NVENC',cases:[]};
let session;
try{
    assert.equal((await request('/healthz')).body.protocol,2);
    assert.equal((await request('/v1/models')).status,404);
    if(phase==='probes'){
    const started=performance.now();
    const failure=await request('/api/v2/sessions','POST',payload('drip'),10000);
    report.source_deadline={status:failure.status,code:failure.body.error?.code,
        seconds:(performance.now()-started)/1000};
    assert.equal(failure.status,504);assert.equal(failure.body.error.code,'source_timeout');
    assert(report.source_deadline.seconds>=2.5&&report.source_deadline.seconds<6);
    const pending=http.request(api+'/api/v2/sessions',{method:'POST',headers:auth});
    pending.on('error',()=>{});pending.end(JSON.stringify(payload('drip')));
    await sleep(400);
    const canceled=performance.now();pending.destroy();
    let next;
    do{
        await sleep(100);next=await request('/api/v2/sessions','POST',payload('source'));
    }while(next.status===429&&performance.now()-canceled<1800);
    if(next.status===201)session=next.body.id;
    report.canceled_probe={next_status:next.status,seconds:(performance.now()-canceled)/1000};
    assert.equal(next.status,201);assert(report.canceled_probe.seconds<2);
    await close(session);session=null;
    const stalled=await open(payload('primary'));
    assert.equal(stalled.status,201);session=stalled.body.id;
    await request('/api/v2/sessions/'+session+'/lease','PUT',{paused:true});
    primaryPaused=true;
    let interrupted=-1;
    for(let i=0;i<stalled.body.manifest.media.segment_count;i++){
        const part=await fetch(api+stalled.body.manifest.segment_template.replace('{index}',i));
        if(part.status===504){assert.equal((await part.json()).error.code,'source_timeout');interrupted=i;break;}
        assert.equal(part.status,200);await part.arrayBuffer();
    }
    assert(interrupted>=0,'paused source must time out during a segment');
    primaryPaused=false;primaryFast=true;
    const resumed=await fetch(api+stalled.body.manifest.segment_template.replace('{index}',interrupted));
    assert.equal(resumed.status,200);
    const firstHeader=Buffer.from(await(await fetch(api+stalled.body.manifest.init)).arrayBuffer());
    const resumedFile=path.join(directory,'resumed.mkv');
    fs.writeFileSync(resumedFile,Buffer.concat([firstHeader,Buffer.from(await resumed.arrayBuffer())]));
    await close(session);session=null;
    const clean=await open(payload('source'));
    assert.equal(clean.status,201);session=clean.body.id;
    const fresh=await fetch(api+clean.body.manifest.segment_template.replace('{index}',interrupted));
    assert.equal(fresh.status,200);
    const cleanHeader=Buffer.from(await(await fetch(api+clean.body.manifest.init)).arrayBuffer());
    const cleanFile=path.join(directory,'clean.mkv');
    fs.writeFileSync(cleanFile,Buffer.concat([cleanHeader,Buffer.from(await fresh.arrayBuffer())]));
    const videoHash=file=>command('ffmpeg',['-v','error','-i',file,'-map','0:v:0','-f','hash','-hash','sha256','-']).trim();
    assert.equal(videoHash(resumedFile),videoHash(cleanFile));
    report.midsegment_timeout_recovery={segment:interrupted,retried_pixels_match_clean_render:true};
    await close(session);session=null;
    }
    if(phase==='media'){
    for(const mode of ['combined','separate_audio','landscape','portrait']){
        const local=path.join(directory,mode);fs.mkdirSync(local,{recursive:true});
        const input=payload(mode==='combined'?'source':mode==='separate_audio'?'video':mode);
        if(mode==='separate_audio')input.audio={urls:[source('fail-audio'),source('audio')]};
        const result=await open(input);
        assert.equal(result.status,201,JSON.stringify(result.body));session=result.body.id;
        const manifest=result.body.manifest,media=manifest.media;
        assert.equal(manifest.schema,2);assert.equal(media.output_fps,60);
        const header=Buffer.from(await(await fetch(api+manifest.init)).arrayBuffer());
        fs.writeFileSync(path.join(local,'init.mkv'),header);
        const chunks=[header],retries=[];
        for(let i=0;i<media.segment_count;i++){
            let response;
            for(let attempt=0;attempt<3;attempt++){
                response=await fetch(api+manifest.segment_template.replace('{index}',i),{signal:AbortSignal.timeout(90000)});
                if(response.ok)break;
                const error=await response.json();
                retries.push({index:i,status:response.status,code:error.error.code});
                assert(error.error.retryable);
            }
            assert.equal(response.status,200);
            const bytes=Buffer.from(await response.arrayBuffer());
            fs.writeFileSync(path.join(local,i+'.mkv'),bytes);chunks.push(bytes);
        }
        const combined=path.join(local,'all.mkv');fs.writeFileSync(combined,Buffer.concat(chunks));
        const probe=JSON.parse(command('ffprobe',['-v','error','-count_frames','-show_entries',
            'stream=codec_type,codec_name,width,height,r_frame_rate,nb_read_frames','-of','json',combined]));
        const video=probe.streams.find(s=>s.codec_type==='video');
        assert.equal(Number(video.nb_read_frames),Math.ceil(media.duration_seconds*60));
        assert.equal(video.r_frame_rate,'60/1');
        const checked={mode,frames:Number(video.nb_read_frames),width:video.width,height:video.height,
            rate:video.r_frame_rate,tracks:media.tracks,retries};
        if(mode==='combined'||mode==='separate_audio'){
            const inputAudio=mode==='combined'?original:path.join(directory,'audio.m4a');
            assert.equal(packetHash(combined),packetHash(inputAudio));
            const a=pcm(inputAudio),b=pcm(combined);
            assert(a.subarray(0,b.length).equals(b));
            checked.audio_packets_identical=true;checked.audio_samples_identical=true;
            checked.trimmed_tail_padding_samples=(a.length-b.length)/4;
            if(mode==='separate_audio')assert(audioFailures>0);
        }else if(mode==='portrait'){
            assert.equal(video.width,1080);assert.equal(video.height,1920);
        }else{
            assert.equal(video.width,1920);assert.equal(video.height,1080);
        }
        report.cases.push(checked);await close(session);session=null;
    }
    report.audio_backup_failures_observed=audioFailures;
    const recovery=payload('primary');recovery.video.urls.push(source('backup'));
    const opened=await open(recovery);
    assert.equal(opened.status,201);session=opened.body.id;
    primaryFailed=true;for(const socket of primarySockets)socket.destroy();
    const response=await fetch(api+opened.body.manifest.segment_template.replace('{index}',5),{signal:AbortSignal.timeout(60000)});
    assert.equal(response.status,200);
    const header=Buffer.from(await(await fetch(api+opened.body.manifest.init)).arrayBuffer());
    const recovered=path.join(directory,'recovered.mkv');
    fs.writeFileSync(recovered,Buffer.concat([header,Buffer.from(await response.arrayBuffer())]));
    const frames=JSON.parse(command('ffprobe',['-v','error','-select_streams','v:0','-count_frames','-show_entries','stream=nb_read_frames','-of','json',recovered]));
    assert.equal(Number(frames.streams[0].nb_read_frames),120);assert(backupReads>0);
    report.interrupted_source_recovery={backup_reads:backupReads,frames:120};
    await close(session);session=null;
    }
    report.passed=true;
}finally{
    if(session)await close(session);
    fixture.closeAllConnections();await new Promise(resolve=>fixture.close(resolve));
    fs.writeFileSync(reportFile,JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report,null,2));
}
