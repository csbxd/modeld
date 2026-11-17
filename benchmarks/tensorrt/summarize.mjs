import fs from 'node:fs';
import path from 'node:path';

const root = process.argv[2];
if (!root) throw new Error('Usage: node summarize.mjs RESULT_DIRECTORY [OUTPUT_JSON]');
const read = file => JSON.parse(fs.readFileSync(path.join(root, file), 'utf8'));
const environment = fs.existsSync(path.join(root, 'environment.json')) ? read('environment.json') : {};
const median = values => {
  const sorted = values.toSorted((a, b) => a - b);
  return (sorted[Math.floor((sorted.length - 1) / 2)] + sorted[Math.ceil((sorted.length - 1) / 2)]) / 2;
};
function aggregate(files) {
  const runs = files.map(read), trials = runs.flatMap(run => run.trials);
  const latencies = trials.flatMap(trial => trial.single_stream.samples_ms).toSorted((a, b) => a - b);
  return {
    backend: runs[0].backend, sdk_version: runs[0].sdk_version,
    width: runs[0].width, height: runs[0].height,
    process_restarts: runs.length, trials: trials.length, latency_samples: latencies.length,
    single_stream_median_ms: median(latencies),
    single_stream_p95_ms: latencies[Math.ceil(latencies.length * .95) - 1],
    dual_stream_fps: median(trials.map(trial => trial.dual_stream_fps)),
    gpu_pipeline_fps: median(trials.map(trial => trial.uploaded_rgb_to_nv12_fps)),
    gpu_pipeline_fps_range: [Math.min(...trials.map(t => t.uploaded_rgb_to_nv12_fps)), Math.max(...trials.map(t => t.uploaded_rgb_to_nv12_fps))],
    gpu_allocation_delta_mib: median(runs.map(run => run.gpu_allocation_delta_bytes)) / 2 ** 20,
    cached_context_warmup_graph_prepare_seconds: median(runs.map(run => run.context_warmup_and_graph_capture_seconds)),
    cached_engine_load_seconds: median(runs.map(run => run.load_including_build_seconds)),
    source_files: files,
  };
}
const performance = [];
for (const height of [1080, 720]) for (const backend of ['rtx', 'trt']) {
  const summary = aggregate(['r1', 'r2'].map(run => `performance/${backend}-${height}-${run}.json`));
  const cold = read(`performance/${backend}-${height}-cold.json`);
  summary.new_engine_build_seconds = cold.build_seconds;
  summary.new_engine_context_warmup_graph_prepare_seconds = cold.context_warmup_and_graph_capture_seconds;
  summary.engine_bytes = cold.engine_bytes;
  performance.push(summary);
}
const first = read('performance/rtx-1080-r1.json');
const quality = read('quality-summary.json');
const references = read('quality.json').samples.filter(row => row.held_out_middle_frame).map(row => row.held_out_middle_frame);
quality.max_held_out_reference_psnr_difference_db = Math.max(...references.map(row => Math.abs(row.rtx_psnr_rgb_db - row.trt_psnr_rgb_db)));
quality.max_held_out_reference_ssim_difference = Math.max(...references.map(row => Math.abs(row.rtx_ssim_y - row.trt_ssim_y)));
const processRows = fs.readFileSync(path.join(root, 'process-gpu.log'), 'utf8').split('\n').filter(line => /^\s*\d/.test(line)).map(line => line.trim().split(/\s+/));
const background = processRows.filter(columns => columns.at(-1) === 'modeld');
let tuning;
if (fs.existsSync(path.join(root, 'optimization-level-5/trt-1080-opt5-r2.json'))) {
  tuning = aggregate(['r1', 'r2'].map(run => `optimization-level-5/trt-1080-opt5-${run}.json`));
  tuning.new_engine_build_seconds = read('optimization-level-5/trt-1080-opt5.json').build_seconds;
  tuning.builder_optimization_level = 5;
}
const report = {
  date: environment.date ?? new Date().toISOString().slice(0, 10), model: 'RIFE 4.26 Heavy FP32', model_sha256: first.model_sha256,
  gpu: first.device, cuda_driver_api_version: first.cuda_driver, cuda_runtime_api_version: first.cuda_runtime,
  configuration: {precision: 'FP32', tf32: false, nvidia_tf32_override: 0, workspace_gib: 4, auxiliary_streams: 0, execution_contexts: 2, cuda_graphs: true, warmup_seconds_at_least: 3},
  timing_scope: 'Inference graphs; separately RGB upload + GPU preprocessing + inference + NV12. Excludes decoding, NVENC and network.',
  speed_input: 'The first natural frame pair at each native resolution, fixed at t=0.5; data already resident for graph-only timings.',
  performance, optimization_level_5: tuning, quality,
  background_service: {process: 'modeld', samples: background.length, detected_gpu_active_samples: background.filter(p => p[3] !== '-' && Number(p[3]) > 0).length},
  environment,
};
const text = JSON.stringify(report, null, 2) + '\n';
if (process.argv[3]) fs.writeFileSync(process.argv[3], text); else process.stdout.write(text);
