import assert from 'node:assert/strict';
import fs from 'node:fs';
import {setImmediate} from 'node:timers/promises';
import {test} from 'node:test';
import vm from 'node:vm';

function playback() {
  const requests = [], intervals = new Map();
  const attributes = new Set();
  const state = {fullscreen: false, ranges: [], ends: 0, reopens: 0};
  function node() {
    return Object.assign(new EventTarget(), {
      dataset: {},
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
  const wrapper = {
    insertAdjacentElement(position, element) {
      assert.equal(position, 'afterend');
      wrapper.nextElementSibling = element;
      element.parentElement = wrapper;
    }
  };
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
  const document = Object.assign(new EventTarget(), {
    createElement: () => root,
    querySelector: selector => selector.includes('video') ? video : container
  });
  const preferences = {bufferSeconds: 12, backSeconds: 60};
  const reply = value => ({ok: true, value});
  const chrome = {runtime: {
    onMessage: {addListener() {}},
    async sendMessage(message) {
      switch (message.type) {
        case 'config.get': return reply({preferences, service: {configured: true}});
        case 'session.open': return reply({preferences, created: {
          mime_type: 'video/mp4', manifest: {media: {duration_seconds: 6, segment_seconds: 2, segment_count: 3}}
        }});
        case 'page.takeover':
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
  const context = {
    chrome, document, MediaSource, Uint8Array, atob, Event, queueMicrotask,
    location: {pathname: '/video/test', search: ''}, URLSearchParams,
    URL: {createObjectURL: () => 'blob:test', revokeObjectURL() {}},
    window: new EventTarget(),
    MutationObserver: class {observe() {} disconnect() {}},
    setInterval: (callback, ms) => {intervals.set(ms, callback); return ms;},
    clearInterval: ms => intervals.delete(ms)
  };
  vm.runInNewContext(fs.readFileSync(new URL('../browser/content.js', import.meta.url), 'utf8'), context);
  const flush = async () => {for (let n = 0; n < 12; n++) await setImmediate();};
  return {state, root, toggle, video, requests, intervals, attributes, wrapper, controls, flush};
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
  assert.equal(p.wrapper.nextElementSibling, p.root);
  assert.equal(p.attributes.has('data-controls'), false);
  p.state.fullscreen = true;
  p.intervals.get(1000)();
  assert.equal(p.root.parentElement, p.controls);
  assert.equal(p.attributes.has('data-controls'), true);
  p.state.fullscreen = false;
  p.wrapper.nextElementSibling = null;
  p.intervals.get(1000)();
  assert.equal(p.wrapper.nextElementSibling, p.root);
  assert.equal(p.attributes.has('data-controls'), false);
});
