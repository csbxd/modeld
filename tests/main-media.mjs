// Playback regression using real, previously rendered 12-second RIFE fixtures:
// node tests/main-media.mjs MODELDSO SOURCE.mkv SEGMENT_DIR REPORT.json
// SEGMENT_DIR contains init.mkv and 0.mkv through 5.mkv (Matroska clusters).
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import assert from 'node:assert/strict';
import {randomBytes} from 'node:crypto';
import {spawn} from 'node:child_process';
import {setTimeout as sleep} from 'node:timers/promises';

const [plugin, source, segments, reportPath] = process.argv.slice(2).map(p => path.resolve(p));
if (!reportPath) throw Error('Usage: main-media.mjs MODELDSO SOURCE.mkv SEGMENT_DIR REPORT.json');
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'modeld-main-'));
const socket = path.join(root, 'mpv.sock');
const sidecar = path.join(root, 'external.ass');
fs.writeFileSync(sidecar, `[Script Info]
ScriptType: v4.00+
PlayResX: 320
PlayResY: 180
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,18,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:09.00,Default,,0,0,0,,External snapshot
`);
const cleanupScript = path.join(root, 'subtitle-cleanup.lua');
fs.writeFileSync(cleanupScript, `mp.register_event('end-file', function() os.remove(${JSON.stringify(sidecar)}) end)`);
let release;
const gate = new Promise(resolve => { release = resolve; });
const requests = [];
const server = http.createServer(async (req, res) => {
    req.resume();
    const id='f'.repeat(32),base='/api/v2/sessions/'+id;
    if(req.method==='POST'&&req.url==='/api/v2/sessions'){
        res.setHeader('Content-Type','application/json');
        res.writeHead(201).end(JSON.stringify({id,lease_seconds:120,manifest:{schema:2,
            init:base+'/media/init',segment_template:base+'/media/segments/{index}',media:{
                width:320,height:180,duration_seconds:12,source_fps:30,output_fps:60,segment_seconds:2,segment_count:6,
                tracks:[{kind:'video',id:1,source:'video',source_index:0,codec:'hevc',language:'',title:''},
                    {kind:'audio',id:1,source:'video',source_index:1,codec:'flac',language:'',title:''},
                    {kind:'subtitle',id:1,source:'video',source_index:2,codec:'ass',language:'',title:''}]}}}));
    }else if(req.url==='/healthz'){res.end('{"status":"ready","protocol":2}');
    }else if(req.method==='DELETE'){res.writeHead(204).end();
    }else if(req.url===base+'/lease'){res.end('{}');
    }else if(req.url===base+'/media/init'){res.end(fs.readFileSync(path.join(segments,'init.mkv')));
    }else if(new RegExp('^'+base+'/media/segments/[0-5]$').test(req.url)){
        const index=Number(req.url.slice(req.url.lastIndexOf('/')+1));requests.push(index);
        if(index>=2)await gate;
        res.end(fs.readFileSync(path.join(segments,index+'.mkv')));
    }else res.writeHead(404).end();
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const config = path.join(root, 'config');
fs.mkdirSync(path.join(config, 'modeld'), {recursive: true, mode: 0o700});
const token = path.join(root, 'token');
fs.writeFileSync(token, randomBytes(32).toString('hex'), {mode: 0o600});
fs.writeFileSync(path.join(config, 'modeld/client.json'), JSON.stringify({
    endpoint: `http://127.0.0.1:${server.address().port}`, token_file: token,
    source_bind: '127.0.0.1'
}), {mode: 0o600});
const log = fs.openSync(path.join(root, 'mpv.stderr'), 'w', 0o600);
const player = spawn('mpv', ['--no-config', '--load-scripts=no', `--script=${plugin}`,
    `--script=${cleanupScript}`,
    `--input-ipc-server=${socket}`, '--vo=null', '--ao=null', '--pause', '--idle=yes',
    '--keep-open=yes', '--cache=yes', '--cache-pause-wait=1', '--demuxer-readahead-secs=30',
    '--demuxer-max-bytes=64MiB', '--demuxer-max-back-bytes=64MiB', '--sid=1', source], {
    env: {...process.env, XDG_CONFIG_HOME: config, XDG_STATE_HOME: path.join(root, 'state')},
    stdio: ['ignore', log, log]
});
let ipc, input = '', sequence = 0;
const pending = new Map();
function command(command) {
    return new Promise((resolve, reject) => {
        const request_id = ++sequence;
        const timer = setTimeout(() => {
            pending.delete(request_id);
            reject(Error('mpv IPC timed out'));
        }, 5000);
        pending.set(request_id, response => {
            clearTimeout(timer);
            response.error === 'success' ? resolve(response.data) : reject(Error(response.error));
        });
        ipc.write(JSON.stringify({command, request_id}) + '\n');
    });
}
const get = name => command(['get_property', name]);
async function until(check, description, timeout = 20000) {
    const end = performance.now() + timeout;
    while (performance.now() < end) {
        try { if (await check()) return; }
        catch (e) { if (e.message !== 'property unavailable') throw e; }
        await sleep(50);
    }
    throw Error(`Timed out: ${description}`);
}
const report = {main_media: true};
try {
    await until(() => fs.existsSync(socket), 'IPC socket');
    ipc = net.connect(socket);
    await new Promise(resolve => ipc.once('connect', resolve));
    ipc.on('data', bytes => {
        input += bytes;
        while (input.includes('\n')) {
            const end = input.indexOf('\n'), response = JSON.parse(input.slice(0, end));
            input = input.slice(end + 1);
            if (pending.has(response.request_id)) {
                pending.get(response.request_id)(response);
                pending.delete(response.request_id);
            }
        }
    });
    await until(async () => Boolean(await get('video-params')), 'original media');
    const original = await get('stream-open-filename');
    const seekCache = await get('demuxer-seekable-cache');
    await command(['loadfile', source, 'append']);
    const entries = async () => (await get('playlist')).map(({id, filename}) => ({id, filename}));
    const playlist = await entries();
    await command(['sub-add', sidecar, 'auto']);
    await command(['set_property', 'secondary-sid', 2]);
    await command(['set_property', 'audio-delay', 0.04]);
    await command(['set_property', 'sub-delay', 0.1]);
    report.original_selection = {sid: await get('sid'), secondary_sid: await get('secondary-sid'),
        tracks: (await get('track-list')).map(t => ({id:t.id,type:t.type,ff_index:t['ff-index'],selection:t['main-selection'],selected:t.selected}))};
    const processed = async () => (await get('track-list')).some(t =>
        t.type === 'video' && t.selected && t.codec === 'hevc' && !t.external);
    await command(['script-message', 'toggle']);
    await until(processed, 'processed main media');
    assert.equal(await get('path'), source);
    assert.deepEqual(await entries(), playlist);
    assert((await get('stream-open-filename')).endsWith('/timeline.edl'));
    assert.equal(await get('demuxer-seekable-cache'), seekCache);
    report.tracks = (await get('track-list')).map(({type, codec, external, selected}) =>
        ({type, codec, external, selected}));
    assert(report.tracks.filter(t => t.type !== 'sub').every(t => !t.external && t.selected));
    assert.equal(await get('secondary-sid'), 2);
    assert.equal(await get('audio-delay'), 0.04);
    assert.equal(await get('sub-delay'), 0.1);
    assert(!fs.existsSync(sidecar));
    await until(async () => (await get('demuxer-cache-state'))['cache-end'] >= 3.8,
        'initial forward buffer');
    await command(['set_property', 'pause', false]);
    await until(async () => (await get('time-pos')) > 3 && await get('paused-for-cache'),
        'native cache backpressure');
    const stopped = await get('time-pos');
    assert.equal(await get('pause'), false);
    await sleep(700);
    assert(Math.abs((await get('time-pos')) - stopped) < 0.1);
    assert.equal(await get('pause'), false);
    report.native_backpressure = {user_pause: false, stopped_at: stopped, audio_clock_stopped: true};
    release();
    await until(async () => !await get('paused-for-cache') && (await get('time-pos')) > 4.5,
        'automatic recovery after data arrives');
    report.avsync = await get('avsync');
    assert(Math.abs(report.avsync) < 0.1);
    await command(['set_property', 'pause', true]);
    await until(async () => (await get('demuxer-cache-state'))['cache-end'] >= 11.8,
        'forward buffering while the user pauses');
    report.buffers_while_user_paused = true;
    const reads = requests.length;
    for (const target of [8, 2, 9, 3, 0, 7]) {
        await command(['seek', target, 'absolute+exact']);
        await until(async () => !await get('seeking') && Math.abs((await get('time-pos')) - target) < 0.15,
            `seek ${target}`);
        await sleep(250);
        assert(Math.abs((await get('time-pos')) - target) < 0.15);
        if (target === 2) assert((await get('sub-text')).includes('Across segment'));
        if (target === 2) assert((await get('secondary-sub-text')).includes('External snapshot'));
    }
    assert.equal(requests.length, reads);
    report.cached_seeks = [8, 2, 9, 3, 0, 7];
    report.cache_ranges = (await get('demuxer-cache-state'))['seekable-ranges'];
    await command(['script-message', 'toggle']);
    await until(async () => (await get('stream-open-filename')) === original, 'original restored');
    await until(async () => !await get('seeking') && Math.abs((await get('time-pos')) - 7) < 0.15,
        'original position restored');
    report.original_restored = true;
    assert.deepEqual(await entries(), playlist);
    assert.equal(await get('secondary-sid'), 2);
    assert.equal(await get('audio-delay'), 0.04);
    await command(['script-message', 'toggle']);
    await until(processed, 'processed media restored');
    await until(async () => !await get('seeking') && Math.abs((await get('time-pos')) - 7) < 0.15,
        'processed position restored');
    assert.equal(await get('pause'), true);
    await sleep(300);
    assert.equal(requests.length, reads);
    report.toggle_reuses_processed_chunks = true;
    report.deleted_external_subtitle_preserved = true;
    report.playlist_preserved = true;
    report.passed = true;
} catch (error) {
    report.error = error.message;
    if (ipc) report.failure_state = await get('demuxer-cache-state').catch(() => null);
    console.error(fs.readFileSync(path.join(root, 'mpv.stderr'), 'utf8'));
    throw error;
} finally {
    release();
    if (ipc) {
        try { await command(['quit']); } catch {}
        ipc.destroy();
    }
    if (player.exitCode === null) player.kill('SIGTERM');
    await new Promise(resolve => player.exitCode === null ? player.once('exit', resolve) : resolve());
    fs.closeSync(log);
    server.closeAllConnections();
    await new Promise(resolve => server.close(resolve));
    fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
    fs.rmSync(root, {recursive: true, force: true});
    console.log(JSON.stringify(report, null, 2));
}
