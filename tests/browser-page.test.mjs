import assert from 'node:assert/strict';
import fs from 'node:fs';
import {test} from 'node:test';
import vm from 'node:vm';

function page(audio, {hasAudio = true} = {}) {
  const videoUrl = 'https://upos-test.bilivideo.com/video.m4s';
  const aac = 'https://upos-test.bilivideo.com/audio.m4s';
  let paused = 0, detached = 0;
  const element = {videoWidth: 1920, videoHeight: 1080, currentTime: 10, duration: 60, paused: false,
    playbackRate: 1, volume: 0.5, muted: false, buffered: {length: 0}};
  const dash = {
    constructor: function OriginalDash() {},
    toString: () => 'Original Dash',
    getSource: () => ({video: [{baseUrl: videoUrl}], audio: hasAudio ? [{baseUrl: aac}] : []}),
    attachSource() {detached++;}
  };
  const core = {player: dash, getCorePlayer: () => dash, getCurrentPlayURLFor: type => type === 'video' ? videoUrl : audio};
  const window = {player: {__core: () => core, mediaElement: () => element, getManifest: () => ({}), pause() {paused++;}}};
  vm.runInNewContext(fs.readFileSync(new URL('../browser/page.js', import.meta.url), 'utf8'), {
    window, document: {title: 'Test video'}, navigator: {userAgent: 'Test browser'}, URL, Symbol,
    location: {origin: 'https://www.bilibili.com'}
  });
  return {bridge: window[Symbol.for('org.csbxd.modeld.page')], element, core, dash,
    changes: () => ({paused, detached}), aac};
}

test('the exact selected AAC audio track is retained', () => {
  const p = page('https://upos-test.bilivideo.com/audio.m4s');
  assert.deepEqual([...p.bridge.probe().audio], [p.aac]);
  assert.deepEqual(p.changes(), {paused: 0, detached: 0});
});

test('an unsupported selected audio track is reported before the original player changes', () => {
  for (const selected of ['dolby.m4s', 'hires.flac']) {
    const p = page('https://upos-test.bilivideo.com/' + selected);
    assert.throws(() => p.bridge.probe(), /AAC/);
    assert.deepEqual(p.changes(), {paused: 0, detached: 0});
    assert.equal(p.core.player, p.dash);
    assert.equal(p.element.currentTime, 10);
    assert.equal(p.element.paused, false);
  }
});

test('silent videos do not gain an audio track', () => {
  const p = page(null, {hasAudio: false});
  assert.equal(p.bridge.probe().audio, null);
});

test('audio initialization must finish before an existing track can be selected', () => {
  const p = page(null);
  assert.throws(() => p.bridge.probe(), /音轨尚未准备好/);
  assert.deepEqual(p.changes(), {paused: 0, detached: 0});
});

test('the media adapter preserves native methods outside its own replacements', async () => {
  const p = page('https://upos-test.bilivideo.com/audio.m4s');
  await p.bridge.takeover({url: 'blob:https://www.bilibili.com/test', duration: 60});
  assert.equal(p.core.player.constructor, p.dash.constructor);
  assert.equal(p.core.player.toString(), 'Original Dash');
});

test('a selected PCDN track uses an official URL for the same file', () => {
  const p = page('https://audio.edge.mountaintoys.cn/audio.m4s');
  p.core.getCurrentPlayURLFor = type => type === 'video'
    ? 'https://video.edge.mountaintoys.cn/video.m4s'
    : 'https://audio.edge.mountaintoys.cn/audio.m4s';
  const source = p.bridge.probe();
  assert.deepEqual([...source.video], ['https://upos-test.bilivideo.com/video.m4s']);
  assert.deepEqual([...source.audio], [p.aac]);
});

test('a PCDN track with no matching official file leaves the original player intact', () => {
  const p = page('https://upos-test.bilivideo.com/audio.m4s');
  p.core.getCurrentPlayURLFor = () => 'https://untrusted.example/other.m4s';
  assert.throws(() => p.bridge.probe(), /官方备用地址/);
  assert.deepEqual(p.changes(), {paused: 0, detached: 0});
});
