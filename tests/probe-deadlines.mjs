// Real native API regression test. Start modeld-fault-source and an isolated
// modeld with max_sessions=1, source_timeout_seconds=3 before running this.
// node tests/probe-deadlines.mjs API TOKEN_FILE SOURCE_FIXTURE REPORT.json
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import {setTimeout as sleep} from 'node:timers/promises';

const [api, tokenFile, source, reportFile] = process.argv.slice(2);
if (!reportFile) throw new Error('Expected API, private token file, source fixture URL and report path');
const token = fs.readFileSync(tokenFile, 'utf8').trim();
const headers = {Authorization: `Bearer ${token}`, 'Content-Type': 'application/json'};
const body = path => JSON.stringify({source_url: source + path});
async function request(path, options = {}) {
  const response = await fetch(api + path, {headers, signal: AbortSignal.timeout(12000), ...options});
  return {status: response.status, body: await response.json()};
}
const report = {backend: 'native TensorRT-RTX', source_timeout_seconds: 3};
let session;
try {
  assert.equal((await request('/health')).body.status, 'ready');
  const start = performance.now();
  const timeout = await request('/v1/sessions', {method: 'POST', body: body('/drip')});
  report.dripping_source = {status: timeout.status, seconds: (performance.now() - start) / 1000,
                           error: timeout.body.error};
  assert.equal(timeout.status, 504);
  assert.match(timeout.body.error, /Source probe.*time limit/);
  assert(report.dripping_source.seconds >= 2.5 && report.dripping_source.seconds < 6);

  // Destroy the HTTP connection while native probing is still consuming bytes.
  // The probe must release its single slot promptly, before the 3s deadline.
  const pending = http.request(api + '/v1/sessions', {method: 'POST', headers});
  pending.on('error', () => {});
  pending.end(body('/drip'));
  await sleep(400);
  const canceledAt = performance.now();
  pending.destroy();
  let healthy;
  do {
    await sleep(100);
    healthy = await request('/v1/sessions', {method: 'POST', body: body('/backup')});
    if (healthy.status !== 429) break;
  } while (performance.now() - canceledAt < 1800);
  if (healthy.status === 201) session = healthy.body.id;
  report.canceled_probe = {next_status: healthy.status,
                          next_probe_seconds: (performance.now() - canceledAt) / 1000};
  assert.equal(healthy.status, 201, 'canceling a probe must release its slot before its timeout');
  assert(report.canceled_probe.next_probe_seconds < 2);
  report.portrait_probe = {width: healthy.body.width, height: healthy.body.height,
                           source_fps: healthy.body.source_fps};
  assert.equal(healthy.body.width, 1080);
  assert.equal(healthy.body.height, 1920);
  report.passed = true;
} finally {
  if (session) await request('/v1/sessions/' + session, {method: 'DELETE'});
  fs.writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report, null, 2));
}
