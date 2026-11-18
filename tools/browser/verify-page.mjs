import assert from 'node:assert/strict';
import fs from 'node:fs';
import {setTimeout as sleep} from 'node:timers/promises';
import {Cdp} from './cdp.mjs';

const [profile, output, bvid = 'BV137sU6pEcr'] = process.argv.slice(2);
if (!profile || !output) throw Error('Usage: verify-page.mjs CHROME_PROFILE REPORT_DIRECTORY [BVID]');
const cdp = await Cdp.profile(profile);
const target = (await cdp.send('Target.createTarget', {url: 'about:blank'})).targetId;
const session = await cdp.attach(target);
const evaluate = fn => cdp.evaluate(session, '(' + fn.toString() + ')()');
const report = {video: bvid, errors: [], consoleErrors: [], commentRequests: []};
await cdp.send('Runtime.enable', {}, session);
await cdp.send('Network.enable', {}, session);
cdp.listeners.add(message => {
  if (message.sessionId !== session) return;
  if (message.method === 'Runtime.exceptionThrown') {
    const detail = message.params.exceptionDetails;
    report.errors.push((detail.exception?.description || detail.text).slice(0, 1200));
  }
  if (message.method === 'Runtime.consoleAPICalled' && message.params.type === 'error')
    report.consoleErrors.push(message.params.args.map(arg => arg.value || arg.description || '').join(' ').slice(0, 1200));
  if (message.method === 'Network.responseReceived') {
    const response = message.params.response, url = new URL(response.url);
    if (url.pathname.includes('/reply/')) report.commentRequests.push({path: url.pathname, status: response.status});
  }
});
async function until(check, label, seconds = 90) {
  const deadline = performance.now() + seconds * 1000;
  while (performance.now() < deadline) {
    const result = await check();
    if (result) return result;
    await sleep(250);
  }
  throw Error('Timed out waiting for ' + label);
}
function snapshot() {
  const video = document.querySelector('video'), widget = document.querySelector('#modeld-browser');
  const comment = document.querySelector('bili-comments');
  function countComments(node) {
    if (!node) return 0;
    let count = 0;
    for (const child of node.children || []) {
      if (child.tagName === 'BILI-COMMENT-THREAD-RENDERER') count++;
      count += countComments(child.shadowRoot || child);
    }
    return count;
  }
  const v = video?.getBoundingClientRect(), w = widget?.getBoundingClientRect();
  return {
    state: widget?.dataset.state, label: widget?.shadowRoot.querySelector('#state').textContent,
    widgetCount: document.querySelectorAll('#modeld-browser').length,
    widgetParent: widget?.parentElement.tagName,
    overlap: v && w ? Math.max(0, Math.min(v.right, w.right) - Math.max(v.left, w.left)) * Math.max(0, Math.min(v.bottom, w.bottom) - Math.max(v.top, w.top)) : null,
    video: video ? {time: video.currentTime, paused: video.paused, ready: video.readyState,
      volume: video.volume, muted: video.muted, rate: video.playbackRate,
      frames: video.getVideoPlaybackQuality().totalVideoFrames} : null,
    commentComponent: !!comment?.shadowRoot, commentThreads: countComments(comment?.shadowRoot),
    pageHydrated: !!document.querySelector('#commentapp')?.firstElementChild,
    nativeSourceRestored: !!window.__modeldPageReview && window.player.__core().player === window.__modeldPageReview.originalDash
  };
}
async function toggle(enabled) {
  await evaluate(() => document.querySelector('#modeld-browser').shadowRoot.querySelector('#toggle').click());
  await until(async () => {
    const state = await evaluate(snapshot);
    if (enabled && state.state === 'off') throw Error(state.label);
    return enabled ? state.state === 'on' : state.state === 'off' && state.video.ready >= 3;
  }, enabled ? 'interpolation enabled' : 'original source restored');
}
function rememberPage() {
  const selectors = ['#viewbox_report', '#arc_toolbar_report', '.up-info-container', '.right-container', '#commentapp'];
  const regions = selectors.map(selector => ({selector, node: document.querySelector(selector)}));
  if (regions.some(region => !region.node)) throw Error('A protected page region is missing');
  const left = document.querySelector('.left-container');
  const mutations = [];
  const observer = new MutationObserver(records => {
    for (const record of records) {
      if (record.target.closest?.('#playerWrap,#bilibili-player,#modeld-browser')) continue;
      mutations.push({tag: record.target.nodeName, id: record.target.id || '', type: record.type,
        attribute: record.attributeName, added: record.addedNodes.length, removed: record.removedNodes.length});
    }
  });
  observer.observe(document.querySelector('#app'), {subtree: true, childList: true, attributes: true, characterData: true});
  window.__modeldPageReview.regions = regions;
  window.__modeldPageReview.left = left;
  window.__modeldPageReview.children = [...left.childNodes];
  window.__modeldPageReview.mutations = mutations;
  window.__modeldPageReview.observer = observer;
  window.__modeldPageReview.originalComment = document.querySelector('bili-comments');
}
function comparePage() {
  const saved = window.__modeldPageReview;
  return {
    regions: saved.regions.map(({selector, node}) => ({selector, sameNode: node.isConnected && document.querySelector(selector) === node})),
    siblingOrderPreserved: saved.children.length === saved.left.childNodes.length && saved.children.every((node, i) => saved.left.childNodes[i] === node),
    sameCommentComponent: document.querySelector('bili-comments') === saved.originalComment,
    outsidePlayerMutations: saved.mutations.slice(0, 30), outsidePlayerMutationCount: saved.mutations.length
  };
}
try {
  await cdp.send('Page.navigate', {url: 'https://www.bilibili.com/video/' + bvid + '/'}, session);
  await until(() => evaluate(() => document.querySelector('#modeld-browser') && document.querySelector('#commentapp')?.firstElementChild && document.querySelector('video')?.readyState >= 3), 'complete page initialization');
  await evaluate(() => {
    const video = document.querySelector('video');
    window.player.pause(); video.muted = true; video.volume = 0.37; video.playbackRate = 1.25;
    window.__modeldPageReview = {originalDash: window.player.__core().player};
  });
  report.before = await evaluate(snapshot);
  assert.equal(report.before.widgetParent, 'BODY');
  assert.equal(report.before.widgetCount, 1);
  assert.equal(report.before.overlap, 0);
  await toggle(true);
  await evaluate(() => window.player.play());
  await sleep(2500);
  report.firstOn = await evaluate(snapshot);
  assert.equal(report.firstOn.video.muted, true);
  assert.equal(report.firstOn.video.volume, 0.37);
  assert.equal(report.firstOn.video.rate, 1.25);
  await evaluate(() => {window.player.pause(); document.querySelector('#commentapp').scrollIntoView();});
  await until(async () => (await evaluate(snapshot)).commentThreads > 0, 'comments loading after interpolation is enabled');
  await sleep(1200);
  report.lazyCommentsOn = await evaluate(snapshot);
  assert.equal(report.lazyCommentsOn.overlap, 0);
  await toggle(false);
  await sleep(2500);
  report.off = await evaluate(snapshot);
  assert.equal(report.off.nativeSourceRestored, true);
  await evaluate(rememberPage);

  await toggle(true);
  await evaluate(() => window.player.play());
  await sleep(2500);
  report.on = await evaluate(snapshot);
  report.enabledComparison = await evaluate(comparePage);
  await evaluate(() => window.player.pause());
  await toggle(false);
  await sleep(1000);
  report.restored = await evaluate(snapshot);
  report.restoredComparison = await evaluate(comparePage);
  await evaluate(() => window.__modeldPageReview.observer.disconnect());
  for (const comparison of [report.enabledComparison, report.restoredComparison]) {
    assert(comparison.regions.every(region => region.sameNode));
    assert.equal(comparison.siblingOrderPreserved, true);
    assert.equal(comparison.sameCommentComponent, true);
    assert.equal(comparison.outsidePlayerMutationCount, 0);
  }
  assert.equal(report.restored.nativeSourceRestored, true);
  assert.equal(report.restored.commentThreads, report.off.commentThreads);
  assert.equal(report.restored.video.volume, 0.37);
  assert.equal(report.restored.video.rate, 1.25);
  assert.equal(report.errors.length, 0);
  assert.equal(report.consoleErrors.length, 0);

  await evaluate(() => {window.scrollTo(0, 0); document.querySelector('.bpx-player-ctrl-web').click();});
  await until(() => evaluate(() => !!document.querySelector('#bilibili-player.mode-webscreen') && document.querySelector('#modeld-browser').hasAttribute('data-controls')), 'native web fullscreen controls', 10);
  report.webFullscreen = await evaluate(() => ({mode: window.player.getManifest().screenKind, parent: document.querySelector('#modeld-browser').parentElement.className}));
  await evaluate(() => document.querySelector('.bpx-player-ctrl-web').click());
  await until(() => evaluate(() => document.querySelector('#modeld-browser').parentElement === document.body), 'normal layout after web fullscreen', 10);
  report.afterFullscreen = await evaluate(snapshot);
  assert.equal(report.afterFullscreen.overlap, 0);
  report.passed = true;
} catch (error) {
  report.error = error.message;
  report.current = await evaluate(snapshot).catch(() => null);
  process.exitCode = 1;
} finally {
  fs.writeFileSync(output + '/page-review.json', JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report));
  cdp.close();
}
