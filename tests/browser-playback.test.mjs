import assert from 'node:assert/strict';
import fs from 'node:fs';
import {setImmediate} from 'node:timers/promises';
import {test} from 'node:test';
import vm from 'node:vm';

function playback({toolbarHeight = 35, bufferSeconds = 12, backSeconds = 60, mediaDuration = 6} = {}) {
  const requests = [], intervals = new Map();
  const attributes = new Set();
  const state = {fullscreen: false, ranges: [], ends: 0, reopens: 0, bottom: 600, opens: 0, restores: 0};
  function style() {
    const values = new Map(), priorities = new Map();
    return {
      getPropertyValue: name => values.get(name) || '',
      getPropertyPriority: name => priorities.get(name) || '',
      setProperty(name, value, priority = '') {
        const serialized = value.endsWith('px') ? String(Number(parseFloat(value).toPrecision(6))) + 'px' : value;
        values.set(name, serialized); priorities.set(name, priority);
      },
      removeProperty(name) {values.delete(name); priorities.delete(name);}
    };
  }
  function node() {
    return Object.assign(new EventTarget(), {
      dataset: {},
      style: style(),
      getBoundingClientRect: () => ({height: toolbarHeight}),
      remove() {this.parentElement = null;},
      classList: {toggle() {}},
      toggleAttribute(name, enabled) { enabled ? attributes.add(name) : attributes.delete(name); },
      closest() { return null; }
    });
  }
  const root = node(), toggle = node(), label = node(), settings = node();
  root.attachShadow = () => ({
    set innerHTML(value) {},
    querySelector: selector => ({'#toggle': toggle, '#state': label, '#settings': settings})[selector]
  });
  const controls = {prepend(element) {element.parentElement = controls;}};
  const nativeSibling = {};
  const wrapper = Object.assign(node(), {
    nextElementSibling: nativeSibling,
    getBoundingClientRect: () => ({left: 10, bottom: state.bottom})
  });
  const container = {
    closest(selector) {
      if (selector === '#bilibili-player') return container;
      if (selector === '#playerWrap,.player-wrap') return wrapper;
      return state.fullscreen ? container : null;
    },
    querySelector: () => controls
  };
  const video = Object.assign(node(), {
    currentTime: 0, paused: true, isConnected: true,
    buffered: {
      get length() {return state.ranges.length;},
      start: index => state.ranges[index][0],
      end: index => state.ranges[index][1]
    },
    async play() {video.paused = false;}
  });
  class MediaSource extends EventTarget {
    static isTypeSupported() {return true;}
    constructor() {super(); this.readyState = 'closed'; state.source = this;}
    addSourceBuffer() {
      const buffer = Object.assign(new EventTarget(), {
        updating: false,
        appendBuffer(data) {
          if (data[0] === state.quotaIndex + 1) throw new DOMException('Media buffer is full', 'QuotaExceededError');
          state.source.readyState = 'open';
          buffer.updating = true;
          queueMicrotask(() => {
            if (data[0]) state.ranges = [[0, Math.max(data[0] * 2, state.ranges.at(-1)?.[1] || 0)]];
            buffer.updating = false;
            buffer.dispatchEvent(new Event('updateend'));
          });
        }
      });
      state.buffer = buffer;
      Object.defineProperty(buffer, 'timestampOffset', {
        get: () => 0,
        set() {state.source.readyState = 'open'; state.reopens++;}
      });
      return buffer;
    }
    endOfStream() {
      assert.equal(state.buffer.updating, false);
      assert.equal(this.readyState, 'open');
      this.readyState = 'ended';
      state.ends++;
    }
  }
  const body = {append(element) {element.parentElement = body;}};
  const document = Object.assign(new EventTarget(), {
    body,
    createElement: () => root,
    querySelector: selector => selector.includes('video') ? video : state.missing ? null : container
  });
  const preferences = {bufferSeconds, backSeconds};
  const reply = value => ({ok: true, value});
  const chrome = {runtime: {
    onMessage: {addListener() {}},
    async sendMessage(message) {
      switch (message.type) {
        case 'config.get': return reply({preferences, service: {configured: true}});
        case 'session.open':
          state.opens++;
          return reply({preferences, created: {
            mime_type: 'video/mp4', manifest: {media: {duration_seconds: mediaDuration, segment_seconds: 2, segment_count: Math.ceil(mediaDuration / 2)}}
          }});
        case 'session.stop': await state.holdClose; return reply(null);
        case 'session.pause': return state.failLease ? {ok: false, error: 'Temporary lease failure'} : reply(null);
        case 'page.restore': state.restores++; return reply(null);
        case 'page.takeover':
          state.ranges = [];
          queueMicrotask(() => {state.source.readyState = 'open'; state.source.dispatchEvent(new Event('sourceopen'));});
          return reply({position: 0, paused: true});
        case 'media.init': return reply({base64: 'AA=='});
        case 'media.segment':
          requests.push(message.index);
          return reply({base64: Buffer.from([message.index + 1]).toString('base64')});
        default: return reply(null);
      }
    }
  }};
  const window = new EventTarget();
  const context = {
    chrome, document, MediaSource, Uint8Array, atob, Event, queueMicrotask,
    location: {pathname: '/video/test', search: ''}, URLSearchParams,
    URL: {createObjectURL: () => 'blob:test', revokeObjectURL() {}},
    window,
    MutationObserver: class {observe() {} disconnect() {}},
    ResizeObserver: class {observe() {} unobserve() {}},
    getComputedStyle: element => ({marginBottom: element.style.getPropertyValue('margin-bottom') || '0px'}),
    scrollX: 0, scrollY: 0,
    requestAnimationFrame: callback => {queueMicrotask(callback); return 1;},
    setInterval: (callback, ms) => {intervals.set(ms, callback); return ms;},
    clearInterval: ms => intervals.delete(ms)
  };
  vm.runInNewContext(fs.readFileSync(new URL('../browser/content.js', import.meta.url), 'utf8'), context);
  const flush = async () => {for (let n = 0; n < 12; n++) await setImmediate();};
  return {state, root, toggle, video, requests, intervals, attributes, wrapper, nativeSibling, body, controls, window, flush};
}

test('the final appended segment ends the stream and backward seeks can reopen it', async () => {
  const p = playback();
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  assert.deepEqual(p.requests, [0, 1, 2]);
  assert.equal(p.state.ends, 1);
  assert.equal(p.state.source.readyState, 'ended');
  assert.equal(p.root.dataset.state, 'on');

  // Simulate seeking into an evicted range after the video has reached its end.
  p.state.ranges = [[4, 6]];
  p.video.dispatchEvent(new Event('modeld.before-seek'));
  assert.equal(p.state.source.readyState, 'open');
  assert.equal(p.state.reopens, 1);
  p.video.currentTime = 0;
  p.video.dispatchEvent(new Event('seeking'));
  await p.flush();
  assert.deepEqual(p.requests, [0, 1, 2, 0, 1]);
  assert.equal(p.state.source.readyState, 'ended');
  await p.intervals.get(300)();
  await p.flush();
  assert.equal(p.state.source.readyState, 'ended');
  assert.equal(p.state.ends, 2);
});

test('controls move below the normal player and into the fullscreen control bar', () => {
  const p = playback();
  assert.equal(p.root.parentElement, p.body);
  assert.equal(p.wrapper.nextElementSibling, p.nativeSibling);
  assert.equal(p.wrapper.style.getPropertyValue('margin-bottom'), '35px');
  assert.equal(p.root.style.top, '600px');
  assert.equal(p.attributes.has('data-controls'), false);
  p.state.fullscreen = true;
  p.intervals.get(1000)();
  assert.equal(p.root.parentElement, p.controls);
  assert.equal(p.attributes.has('data-controls'), true);
  assert.equal(p.wrapper.style.getPropertyValue('margin-bottom'), '');
  p.state.fullscreen = false;
  p.intervals.get(1000)();
  assert.equal(p.root.parentElement, p.body);
  assert.equal(p.wrapper.nextElementSibling, p.nativeSibling);
  assert.equal(p.attributes.has('data-controls'), false);
});

test('layout preserves changes made by the page and releases its spacing when the player disappears', () => {
  const p = playback();
  p.wrapper.style.setProperty('margin-bottom', '12px', 'important');
  p.intervals.get(1000)();
  assert.equal(p.wrapper.style.getPropertyValue('margin-bottom'), '47px');
  p.state.missing = true;
  p.intervals.get(1000)();
  assert.equal(p.wrapper.style.getPropertyValue('margin-bottom'), '12px');
  assert.equal(p.wrapper.style.getPropertyPriority('margin-bottom'), 'important');
  assert.equal(p.root.parentElement, null);
  assert.equal(p.wrapper.nextElementSibling, p.nativeSibling);
});

test('browser CSS rounding does not accumulate extra spacing across layout updates', () => {
  const p = playback({toolbarHeight: 34.796875});
  for (let n = 0; n < 10; n++) p.intervals.get(1000)();
  assert.equal(p.wrapper.style.getPropertyValue('margin-bottom'), '34.7969px');
});

test('controls follow the sticky player while the page scrolls', async () => {
  const p = playback();
  p.state.bottom = 900;
  p.window.dispatchEvent(new Event('scroll'));
  await p.flush();
  assert.equal(p.root.style.top, '900px');
});

test('restoring the original player proceeds while remote cleanup waits and repeated toggles are serialized', async () => {
  const p = playback();
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  let finishClose;
  p.state.holdClose = new Promise(resolve => {finishClose = resolve;});
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  assert.equal(p.state.restores, 1);
  assert.equal(p.root.dataset.state, 'closing');
  assert.equal(p.toggle.disabled, true);
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  assert.equal(p.state.opens, 1);
  finishClose();
  await p.flush();
  assert.equal(p.root.dataset.state, 'off');
  assert.equal(p.toggle.disabled, false);
});

test('a temporary lease update failure keeps a playable media session active', async () => {
  const p = playback();
  p.state.failLease = true;
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  assert.equal(p.root.dataset.state, 'on');
  assert.equal(p.state.restores, 0);
});

test('a full browser media buffer waits for room and retries without restoring the original source', async () => {
  const p = playback({bufferSeconds: 86400, backSeconds: 86400});
  p.state.quotaIndex = 2;
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  assert.equal(p.root.dataset.state, 'on');
  assert.equal(p.state.restores, 0);
  assert.deepEqual(p.requests, [0, 1, 2]);
  p.state.quotaIndex = undefined;
  await p.intervals.get(300)();
  await p.flush();
  assert.deepEqual(p.requests, [0, 1, 2, 2]);
  assert.equal(p.state.source.readyState, 'ended');
});

test('automatically evicted future segments are fetched again without a seek', async () => {
  const p = playback();
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  p.state.ranges = [[0, 4]];
  p.video.currentTime = 3;
  await p.intervals.get(300)();
  await p.flush();
  assert.deepEqual(p.requests, [0, 1, 2, 2]);
  assert.equal(p.state.source.readyState, 'ended');
});

test('an evicted tail is appended again before signalling end of stream', async () => {
  const p = playback({bufferSeconds: 86400, mediaDuration: 10});
  p.toggle.dispatchEvent(new Event('click'));
  await p.flush();
  await p.intervals.get(300)();
  await p.flush();
  assert.equal(p.state.ends, 1);
  p.state.ranges = [[0, 4]];
  p.video.currentTime = 2;
  await p.intervals.get(300)();
  await p.flush();
  assert.equal(p.state.source.readyState, 'open');
  assert.equal(p.state.ends, 1);
  await p.intervals.get(300)();
  await p.flush();
  assert.equal(p.state.source.readyState, 'ended');
  assert.equal(p.state.ends, 2);
});
