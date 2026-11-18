import assert from 'node:assert/strict';
import fs from 'node:fs';
import {setTimeout as sleep} from 'node:timers/promises';
import {Cdp} from './cdp.mjs';

const [profile, output, bvid = 'BV137sU6pEcr'] = process.argv.slice(2);
if (!profile || !output) throw Error('Usage: verify-end.mjs CHROME_PROFILE REPORT_DIRECTORY [BVID]');
const cdp = await Cdp.profile(profile);
const target = (await cdp.send('Target.getTargets')).targetInfos.find(tab => tab.url.includes('/video/' + bvid));
assert(target, 'The requested Bilibili video tab must be open');
const session = await cdp.attach(target.targetId);
const evaluate = fn => cdp.evaluate(session, '(' + fn.toString() + ')()');
const report = {video: bvid, samples: []};
async function until(check, label, seconds = 90) {
  const deadline = performance.now() + seconds * 1000;
  while (performance.now() < deadline) {
    const value = await check();
    if (value) return value;
    await sleep(250);
  }
  throw Error('Timed out waiting for ' + label);
}
function snapshot() {
  const video = document.querySelector('video'), root = document.querySelector('#modeld-browser');
  return {
    state: root?.dataset.state, label: root?.shadowRoot.querySelector('#state').textContent,
    time: video?.currentTime, duration: video?.duration, ended: video?.ended, paused: video?.paused,
    ready: video?.readyState, frames: video?.getVideoPlaybackQuality().totalVideoFrames,
    buffer: video ? Array.from({length: video.buffered.length}, (_, i) => [video.buffered.start(i), video.buffered.end(i)]) : []
  };
}
function layout() {
  const root = document.querySelector('#modeld-browser'), video = document.querySelector('video');
  const rect = element => {
    const r = element.getBoundingClientRect();
    return {x: r.x, y: r.y, right: r.right, bottom: r.bottom, width: r.width, height: r.height};
  };
  const a = rect(root), b = rect(video);
  return {
    widget: a, video: b, parent: root.parentElement.className,
    controls: root.hasAttribute('data-controls'), fullscreen: !!document.fullscreenElement,
    overlap: Math.max(0, Math.min(a.right, b.right) - Math.max(a.x, b.x)) * Math.max(0, Math.min(a.bottom, b.bottom) - Math.max(a.y, b.y))
  };
}
try {
  await cdp.send('Page.bringToFront', {}, session);
  await until(() => evaluate(() => !!document.querySelector('#modeld-browser') && document.querySelector('video')?.readyState >= 2), 'page ready');
  report.original = await evaluate(snapshot);
  assert.equal(report.original.state, 'off');
  await evaluate(() => {window.player.pause(); window.player.seek(document.querySelector('video').duration - 7);});
  await sleep(300);
  await evaluate(() => document.querySelector('#modeld-browser').shadowRoot.querySelector('#toggle').click());
  await until(async () => {
    const current = await evaluate(snapshot);
    if (current.state === 'off') throw Error(current.label);
    return current.state === 'on';
  }, 'interpolated playback');
  await evaluate(() => window.player.play());
  for (let n = 0; n < 30; n++) {
    await sleep(1000);
    const current = await evaluate(snapshot);
    report.samples.push(current);
    if (current.ended) break;
  }
  report.ended = await evaluate(snapshot);
  assert.equal(report.ended.ended, true);
  assert.equal(report.ended.state, 'on');
  await sleep(3000);
  report.stableEnd = await evaluate(snapshot);
  assert.equal(report.stableEnd.ended, true);
  assert.equal(report.stableEnd.time, report.ended.time);

  await evaluate(() => {window.player.seek(20); window.player.play();});
  await until(async () => {
    const current = await evaluate(snapshot);
    return current.time > 21 && current.time < 24 && current.ready >= 3;
  }, 'backward seek after end');
  report.replayed = await evaluate(snapshot);
  await sleep(4000);
  report.continued = await evaluate(snapshot);
  assert(report.continued.time - report.replayed.time > 3);
  assert(report.continued.frames - report.replayed.frames > 180);
  await evaluate(() => window.player.pause());
  report.normal = await evaluate(layout);
  assert.equal(report.normal.overlap, 0);
  assert.equal(report.normal.controls, false);
  const image = await cdp.send('Page.captureScreenshot', {format: 'png', captureBeyondViewport: false}, session);
  fs.writeFileSync(output + '/normal.png', Buffer.from(image.data, 'base64'));

  // Use the browser's fullscreen API with user activation to exercise the native layout transition.
  await cdp.send('Runtime.evaluate', {
    expression: "document.querySelector('.bpx-player-container').requestFullscreen()",
    awaitPromise: true, userGesture: true
  }, session);
  await until(() => evaluate(() => !!document.fullscreenElement && document.querySelector('#modeld-browser').hasAttribute('data-controls')), 'fullscreen controls', 5);
  report.fullscreen = await evaluate(layout);
  assert.match(report.fullscreen.parent, /bpx-player-control-bottom-right/);
  await evaluate(() => document.exitFullscreen());
  await until(() => evaluate(() => !document.querySelector('#modeld-browser').hasAttribute('data-controls')), 'normal controls', 5);
  report.returned = await evaluate(layout);
  assert.equal(report.returned.overlap, 0);
  report.passed = true;
} catch (error) {
  report.error = error.message;
  report.current = await evaluate(snapshot).catch(() => null);
  process.exitCode = 1;
} finally {
  fs.writeFileSync(output + '/regression.json', JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report));
  cdp.close();
}
