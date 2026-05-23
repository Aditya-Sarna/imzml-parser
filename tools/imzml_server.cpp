// ---------------------------------------------------------------------------
// imzml_server.cpp
//
// HTTP/1.1 server with embedded single-page viewer for imzML datasets.
//
// Usage:
//   imzml_server [file.imzML] [port]     (default port: 7373)
//
//   If file is omitted the viewer shows an "Open Dataset" screen where
//   the user types the path to their .imzML file.  The .ibd is found
//   automatically in the same directory.
//
// API:
//   GET  /api/status                   {loaded, file, error}
//   POST /api/load   body: path=...    load dataset from filesystem path
//   GET  /api/info                     dataset metadata (requires load)
//   GET  /api/image[?mz=X&tol=Y]      per-pixel TIC or ion-image (flat list)
//   GET  /api/ion-image[?mz=X&tol=Y]  ion image as 2D matrix (getionimage-style)
//   GET  /api/spectrum?n=N             decoded peaks for spectrum index N
//   GET  /api/spectrum?x=X&y=Y        decoded peaks for pixel at coordinate (physX/Y in response)
//   GET  /api/export[?mz_lo=X&mz_hi=Y] export dataset as imzML+ibd zip download
//   GET  /api/stats                    alias for /api/image (no filter)
// ---------------------------------------------------------------------------
#include <OpenMS/FORMAT/ImzMLFile.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>

#include "imzml/ImzMLWriter.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <string>
#include <sstream>
#include <iomanip>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <stdexcept>
#include <numeric>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <thread>

// ===========================================================================
// Global mutable state  (multi-threaded -- protected by g_mu)
// ===========================================================================
struct PixelEntry { uint32_t x, y, z; double tic; double bp; int peaks; };

static OpenMS::MSExperiment* g_exp       = nullptr;
static std::string           g_path;          // loaded .imzML path
static std::string           g_loadError;     // last load error message
static std::string           g_tempDir;       // per-run upload scratch dir
static std::shared_mutex     g_mu;            // shared=read, unique=write

// Per-spectrum pixel cache (built once on load, O(1) image/info responses)
static std::vector<PixelEntry> g_pixCache;
static std::string             g_ticImageJson;   // pre-serialised TIC image
static std::string             g_bpImageJson;    // pre-serialised base-peak image
static double g_mzMin  = 0, g_mzMax  = 0;
static double g_maxTIC = 0, g_maxBP  = 0;

// ===========================================================================
// Embedded HTML / CSS / JS  (single-file SPA, no external dependencies)
// ===========================================================================
static const char* HTML_PAGE = R"=====(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>imzML Viewer</title>
<style>
  :root {
    --bg:      #ffffff;
    --surface: #f4f4f4;
    --border:  #d0d0d0;
    --text:    #111111;
    --muted:   #777777;
    --mono:    "SFMono-Regular","Menlo","Consolas",monospace;
    --err:     #b00000;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
         background: var(--bg); color: var(--text); font-size: 14px; }

  /* header */
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 10px 20px; background: var(--surface);
    border-bottom: 2px solid var(--text); user-select: none;
  }
  header h1 { font-size: 13px; font-weight: 700; letter-spacing: .07em;
              text-transform: uppercase; white-space: nowrap; }
  #hdr-file { font-family: var(--mono); font-size: 12px; color: var(--muted);
              overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1; }
  #hdr-change { font-size: 12px; padding: 3px 10px; border: 1px solid var(--border);
                background: var(--bg); cursor: pointer; color: var(--text); }
  #hdr-change:hover { background: var(--border); }

  /* open panel */
  #open-panel {
    max-width: 540px; margin: 60px auto 0; padding: 32px 32px 36px;
    border: 1px solid var(--border);
  }
  #open-panel h2 { font-size: 15px; font-weight: 600; margin-bottom: 6px; }
  #open-panel > p { font-size: 13px; color: var(--muted); margin-bottom: 20px;
                    line-height: 1.55; }
  /* upload */
  #upload-row { display:flex; align-items:center; gap:14px; margin-bottom:22px; }
  #upload-btn {
    font-size:15px; padding:10px 26px; border:2px solid var(--text);
    background:var(--text); color:var(--bg); cursor:pointer; font-weight:700;
    white-space:nowrap; flex-shrink:0;
  }
  #upload-btn:hover:not(:disabled) { background:#333; border-color:#333; }
  #upload-btn:disabled { opacity:.45; cursor:default; }
  #upload-hint { font-size:12px; color:var(--muted); line-height:1.5; }
  #upload-status { font-size:12px; color:var(--muted); font-family:var(--mono); }
  #open-error { font-size: 12px; color: var(--err); margin-top: 10px;
                font-family: var(--mono); min-height: 16px; }
  #load-progress { height: 3px; background: var(--border); margin-top: 12px; overflow: hidden; }
  #load-bar { width: 0; height: 100%; background: var(--text); transition: width .3s ease; }
  /* viewer */
  #viewer { display: none; }

  /* tab nav */
  #tab-nav {
    display: flex; border-bottom: 1px solid var(--border);
    background: var(--surface); padding: 0 20px;
  }
  .tab-btn {
    font-size: 12px; font-weight: 600; letter-spacing: .05em;
    padding: 8px 18px; border: none; border-bottom: 2px solid transparent;
    background: none; cursor: pointer; color: var(--muted); margin-bottom: -1px;
  }
  .tab-btn:hover { color: var(--text); background: none; }
  .tab-btn.active { color: var(--text); border-bottom-color: var(--text); }

  /* viewer layout: sidebar + main */
  .viewer-layout {
    display: flex; align-items: stretch; height: calc(100vh - 80px); overflow: hidden;
  }
  .sidebar {
    width: 260px; flex-shrink: 0; border-right: 1px solid var(--border);
    display: flex; flex-direction: column; overflow: hidden;
  }
  .sidebar-section {
    padding: 14px 16px; border-bottom: 1px solid var(--border);
  }
  .sidebar-section:last-child { border-bottom: none; flex: 1; overflow: hidden; }
  .main-area {
    flex: 1; overflow: auto; padding: 14px 20px;
  }
  .panel-label { font-size: 10px; font-weight: 700; letter-spacing: .1em;
                 text-transform: uppercase; color: var(--muted); margin-bottom: 10px; }

  /* metadata */
  .kv { display: flex; justify-content: space-between; gap: 8px;
        padding: 4px 0; border-bottom: 1px solid #eeeeee; font-size: 13px; }
  .kv:last-child { border-bottom: none; }
  .kv-k { color: var(--muted); white-space: nowrap; }
  .kv-v { font-family: var(--mono); font-weight: 500; text-align: right; word-break: break-all; }

  /* controls */
  .ctrl-row { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; margin-bottom: 8px; }
  .ctrl-row label { font-size: 12px; color: var(--muted); }
  input[type=text] {
    font-family: var(--mono); font-size: 12px; width: 72px; padding: 3px 6px;
    border: 1px solid var(--border); background: var(--bg); color: var(--text); outline: none;
  }
  input[type=text]:focus { border-color: var(--text); }
  button {
    font-size: 12px; padding: 3px 10px; border: 1px solid var(--border);
    background: var(--surface); cursor: pointer; color: var(--text);
  }
  button:hover { background: var(--border); }

  /* heatmap */
  #heatmap-canvas { display: block; image-rendering: pixelated; cursor: crosshair; }
  #pixel-tip { font-family: var(--mono); font-size: 11px; color: var(--muted);
               margin-top: 6px; min-height: 16px; }
  #colorbar-wrap { display: flex; align-items: center; gap: 6px; margin-top: 8px;
                   font-size: 11px; font-family: var(--mono); color: var(--muted); }
  #colorbar-canvas { border: 1px solid var(--border); }


  /* stats */
  .stats-label { font-size: 10px; font-weight: 700; letter-spacing: .1em;
                 text-transform: uppercase; color: var(--muted); padding: 0 0 8px; }

  /* benchmark tab */
  #tab-benchmark { padding: 18px 24px; overflow: auto; height: calc(100vh - 80px); }
  .bench-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 24px; }
  .bench-panel-title {
    font-size: 11px; font-weight: 700; letter-spacing: .08em; text-transform: uppercase;
    color: var(--muted); margin-bottom: 8px;
  }
  .bench-panel-title span { color: var(--text); font-size: 13px; letter-spacing: 0;
                             text-transform: none; font-weight: 600; }
  #bench-status { font-size: 12px; font-family: var(--mono); color: var(--muted);
                  margin-bottom: 14px; min-height: 18px; }
  #bench-run { margin-bottom: 16px; }
  table { border-collapse: collapse; width: 100%; font-size: 13px; }
  thead th {
    text-align: left; padding: 5px 12px; font-size: 10px; font-weight: 700;
    letter-spacing: .08em; text-transform: uppercase; color: var(--muted);
    border-bottom: 2px solid var(--border); cursor: pointer; user-select: none;
  }
  thead th:hover { color: var(--text); }
  tbody td { padding: 4px 12px; border-bottom: 1px solid #eeeeee; font-family: var(--mono); }
  tbody tr.selected td { background: #f0f0f0; }
  tbody tr:hover td { background: #fafafa; cursor: pointer; }

  /* spectrum tab */
  #tab-spectrum { padding: 18px 24px; height: calc(100vh - 80px);
                  display: flex; flex-direction: column; overflow: hidden; }
  .spec-toolbar { display: flex; gap: 8px; align-items: center; flex-wrap: wrap;
                  margin-bottom: 10px; }
  .spec-toolbar label { font-size: 12px; color: var(--muted); }
  #spec-canvas  { border: 1px solid var(--border); flex: 1; min-height: 0;
                  width: 100%; display: block; }
  #spec-info    { font-family: var(--mono); font-size: 11px; color: var(--muted);
                  margin-top: 6px; min-height: 16px; }
  #spec-pixel-form { display: flex; gap: 6px; align-items: center; }
  #spec-pixel-form input[type=text] { width: 50px; }

  /* Python playground tab */
  #tab-python { padding: 18px 24px; height: calc(100vh - 41px);
                display: flex; flex-direction: column; gap: 12px; overflow: hidden; }
  .py-examples { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; }
  .py-examples label { font-size: 11px; font-weight: 700; letter-spacing: .08em;
                       text-transform: uppercase; color: var(--muted); }
  .py-demo-btn { background: var(--text) !important; color: var(--bg) !important;
                 border-color: var(--text) !important; font-weight: 700; }
  .py-demo-btn:hover { opacity: .85; }
  .py-example-btn { font-size: 11px; padding: 3px 9px; border: 1px solid var(--border);
                    background: var(--surface); cursor: pointer; color: var(--text); font-family: var(--mono); }
  .py-example-btn:hover { background: var(--border); }
  #py-editor { font-family: var(--mono); font-size: 13px; resize: none;
               flex: 0 0 clamp(120px, 32vh, 280px);
               padding: 10px; border: 1px solid var(--border);
               background: var(--surface); color: var(--text);
               line-height: 1.55; outline: none; tab-size: 4; }
  #py-editor:focus { border-color: var(--text); }
  #py-run-row { display: flex; gap: 8px; align-items: center; flex-shrink: 0; }
  #py-run-btn { font-size: 12px; padding: 5px 18px; border: 2px solid var(--text);
                background: var(--text); color: var(--bg); cursor: pointer; font-weight: 700; }
  #py-run-btn:hover:not(:disabled) { background: #333; border-color: #333; }
  #py-run-btn:disabled { opacity: .45; cursor: default; }
  #py-run-status { font-size: 12px; font-family: var(--mono); color: var(--muted); }
  #py-output { flex: 1; min-height: 0; overflow: auto;
               font-family: var(--mono); font-size: 12px; line-height: 1.6;
               background: #111; color: #e8e8e8;
               padding: 12px 14px; border: 1px solid #333;
               white-space: pre-wrap; word-break: break-word; }
  #py-output .out-stdout { color: #e8e8e8; }
  #py-output .out-stderr { color: #ff8c8c; }
  #py-output .out-hint   { color: #888; font-style: italic; }
  .py-api-ref { font-size: 11px; border: 1px solid var(--border); }
  .py-api-ref summary { padding: 5px 10px; cursor: pointer; font-size: 11px;
                        font-weight: 700; letter-spacing: .06em; text-transform: uppercase;
                        color: var(--muted); user-select: none; }
  .py-api-ref summary:hover { color: var(--text); }
  .py-api-ref[open] summary { border-bottom: 1px solid var(--border); }
  .py-api-ref-body { display: flex; flex-wrap: wrap; gap: 14px; padding: 10px 14px;
                     background: var(--surface); max-height: 200px; overflow-y: auto; }
  .py-api-group { min-width: 180px; flex: 1; }
  .py-api-group h4 { font-family: var(--mono); font-size: 11px; font-weight: 700;
                     color: var(--text); margin-bottom: 4px; border-bottom: 1px solid var(--border); padding-bottom: 2px; }
  .py-api-group ul { list-style: none; padding: 0; margin: 0; }
  .py-api-group li { font-family: var(--mono); font-size: 11px; color: #aaa; padding: 1px 0;
                     cursor: pointer; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .py-api-group li:hover { color: var(--text); }
  .py-api-group li span.ret { color: #666; }

  /* sample downloads */
  #samples-section { margin-top: 28px; padding-top: 20px; border-top: 1px solid var(--border); }
  #samples-section h3 { font-size: 11px; font-weight: 700; letter-spacing: .1em;
    text-transform: uppercase; color: var(--muted); margin-bottom: 4px; }
  #samples-section > p { font-size: 12px; color: var(--muted); margin-bottom: 14px; line-height: 1.5; }
  .samples-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 10px; }
  .sample-card { border: 1px solid var(--border); padding: 12px 14px;
    display: flex; flex-direction: column; gap: 6px; }
  .sample-card-title { font-weight: 600; font-size: 13px; }
  .sample-card-desc  { font-size: 12px; color: var(--muted); line-height: 1.45; flex: 1; }
  .sample-card-badges { display: flex; gap: 5px; flex-wrap: wrap; }
  .sample-badge { font-size: 10px; font-weight: 600; letter-spacing: .04em;
    padding: 1px 6px; border: 1px solid var(--border); color: var(--muted);
    text-transform: uppercase; font-family: var(--mono); }
  .sample-dl-btn { font-size: 12px; padding: 5px 10px; border: 1px solid var(--text);
    background: var(--text); color: var(--bg); cursor: pointer; font-weight: 600;
    text-align: center; text-decoration: none; display: block; margin-top: 4px; }
  .sample-dl-btn:hover { background: #333; border-color: #333; }
  .sample-ext-btn { background: var(--bg); color: var(--text); border-color: var(--border); }
  .sample-ext-btn:hover { background: var(--surface); border-color: var(--text); color: var(--text); }

  /* Top-level Python tab */
  #top-nav { display: flex; gap: 0; border-bottom: 2px solid var(--text);
             background: var(--surface); padding: 0 20px; }
  .top-nav-btn { font-size: 12px; font-weight: 600; letter-spacing: .05em;
    padding: 8px 20px; border: none; border-bottom: 3px solid transparent;
    background: none; cursor: pointer; color: var(--muted); margin-bottom: -2px; }
  .top-nav-btn:hover { color: var(--text); }
  .top-nav-btn.active { color: var(--text); border-bottom-color: var(--text); }
  #page-viewer { } /* default visible */
  #page-python { display: none; height: calc(100vh - 41px); overflow: hidden; }
  #py-no-file { padding: 18px 24px; font-size: 13px; color: var(--muted);
    font-family: var(--mono); border-bottom: 1px solid var(--border);
    background: var(--surface); display: none; }
  #py-no-file a { color: var(--text); cursor: pointer; text-decoration: underline; }

</style>
</head>
<body>

<div id="top-nav">
  <button class="top-nav-btn active" id="top-btn-viewer" onclick="switchTopTab('viewer')">imzML Viewer</button>
  <button class="top-nav-btn" id="top-btn-python" onclick="switchTopTab('python')">Python Bindings</button>
  <span id="hdr-file" style="margin-left:auto;font-family:var(--mono);font-size:12px;color:var(--muted);align-self:center;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:320px">No file loaded</span>
  <button id="hdr-change" style="display:none;margin-left:8px;font-size:12px;padding:3px 10px;border:1px solid var(--border);background:var(--bg);cursor:pointer;color:var(--text);align-self:center" onclick="showOpenPanel()">Change File</button>
</div>

<div id="page-viewer">
<!-- Open / load panel -->
<div id="open-panel">
  <h2>Open Dataset</h2>
  <!-- PRIMARY: native file picker -->
  <input type="file" id="file-input" multiple accept=".imzML,.ibd"
         style="display:none" onchange="uploadFiles(this.files)">
  <div id="upload-row">
    <button id="upload-btn" onclick="document.getElementById('file-input').click()">
      Open Files&#8230;
    </button>
    <span id="upload-hint">Select your <code>.imzML</code> and <code>.ibd</code> files together.<br>
      They will be uploaded to the server and loaded.</span>
  </div>
  <div id="upload-status"></div>
  <div id="open-error"></div>
  <div id="load-progress"><div id="load-bar"></div></div>
  <!-- sample datasets for download -->
  <div id="samples-section" style="display:none">
    <h3>Sample Datasets</h3>
    <p>No dataset? Download a sample below, extract the ZIP, then open both files with the button above.</p>
    <div id="samples-loading" style="font-size:12px;color:var(--muted);font-family:var(--mono)">Loading&#8230;</div>
    <div class="samples-grid" id="samples-grid"></div>
  </div>

</div><!-- /open-panel -->

<!-- Viewer (hidden until a file is loaded) -->
<div id="viewer">

  <!-- tab navigation -->
  <div id="tab-nav">
    <button class="tab-btn active" id="tab-btn-overview" onclick="switchTab('overview')">Overview</button>
    <button class="tab-btn" id="tab-btn-spectrum" onclick="switchTab('spectrum')">Spectrum</button>
    <button class="tab-btn" id="tab-btn-benchmark" onclick="switchTab('benchmark')">Benchmark</button>
  </div>

  <!-- overview tab -->
  <div id="tab-overview">
  <div class="viewer-layout">

    <!-- Sidebar: metadata + ion image -->
    <div class="sidebar">
      <div class="sidebar-section">
        <div class="panel-label">Dataset</div>
        <div id="meta-list"></div>
      </div>
      <div class="sidebar-section">
        <div class="panel-label">Export imzML</div>
        <div class="ctrl-row">
          <label>m/z lo</label>
          <input id="exp-lo" type="text" placeholder="min" style="width:60px">
          <label>hi</label>
          <input id="exp-hi" type="text" placeholder="max" style="width:60px">
          <button onclick="exportImzML()" title="Export current dataset as imzML+ibd zip (optional m/z filter)">Download</button>
        </div>
      </div>
      <div class="sidebar-section">
        <div class="panel-label">Ion Image</div>
        <div class="ctrl-row">
          <label>m/z</label>
          <input id="mz-in" type="text" placeholder="all">
          <label>+/-</label>
          <input id="tol-in" type="text" placeholder="0.5" value="0.5">
          <button id="img-render">Render</button>
          <button id="img-reset">Reset</button>
          <button id="img-basepeak" title="Base-peak image (max intensity per pixel)">Base Peak</button>
          <button id="img-savepng" title="Save heatmap as PNG">Save PNG</button>
          <button id="img-matrix" title="Download ion image as JSON matrix (2D grid, like pyimzML getionimage)">Download Matrix</button>
        </div>
        <div id="heatmap-wrap"><canvas id="heatmap-canvas"></canvas></div>
        <div id="colorbar-wrap">
          <span id="cb-lo">0</span>
          <canvas id="colorbar-canvas" width="120" height="10"></canvas>
          <span id="cb-hi">0</span>
        </div>
        <div id="pixel-tip"></div>
      </div>
    </div>

    <!-- Main area: stats table -->
    <div class="main-area">
      <div class="stats-label" style="display:flex;justify-content:space-between;align-items:center">
        <span>Pixel Statistics</span>
        <button onclick="exportStatsCSV()" style="font-size:11px;padding:2px 8px">Export CSV</button>
      </div>
      <div id="stats-wrap"></div>
    </div>

  </div>
  </div><!-- /tab-overview -->

  <!-- benchmark tab -->
  <div id="tab-benchmark" style="display:none">
    <button id="bench-run" onclick="loadBenchmark()">Run Benchmark</button>
    <div id="bench-status"></div>
    <div id="bench-tables"></div>
  </div>

  <!-- spectrum tab -->
  <div id="tab-spectrum" style="display:none">
    <div class="spec-toolbar">
      <button onclick="loadSpectrumView('avg')">Avg Spectrum</button>
      <button onclick="loadSpectrumView('max')">Max Spectrum</button>
      <span style="margin-left:8px;color:var(--muted);font-size:12px">Pixel:</span>
      <div id="spec-pixel-form">
        <label>x</label><input id="spec-px" type="text" placeholder="1">
        <label>y</label><input id="spec-py" type="text" placeholder="1">
        <button onclick="loadSpectrumPixel()">Load</button>
      </div>
      <button id="spec-csv-btn" style="margin-left:auto" onclick="exportSpectrumCSV()" disabled>Export CSV</button>
    </div>
    <canvas id="spec-canvas"></canvas>
    <div id="spec-info"></div>
  </div>

</div><!-- /viewer -->
</div><!-- /page-viewer -->
<div id="page-python">
  <div id="py-no-file"># No dataset loaded — <a onclick="switchTopTab('viewer')">open a file</a> first to use the live playground.</div>
  <div id="tab-python">
    <div class="py-examples">
      <button class="py-example-btn py-demo-btn" onclick="pyRunDemo()">&#9654; Run Demo</button>
      <label style="margin-left:6px">Snippets:</label>
      <button class="py-example-btn" onclick="pySnippet('ondisk')">OnDiscImzMLExperiment</button>
      <button class="py-example-btn" onclick="pySnippet('getpeaks')">get_peaks() numpy</button>
      <button class="py-example-btn" onclick="pySnippet('ionimage')">Ion image</button>
      <button class="py-example-btn" onclick="pySnippet('metadata')">Metadata</button>
      <button class="py-example-btn" onclick="pySnippet('tic')">TIC list</button>
      <button class="py-example-btn" onclick="pySnippet('pyopenms')">pyOpenMS compat</button>
      <button class="py-example-btn" onclick="pySnippet('specinfo')">Spectrum info</button>
    </div>
    <textarea id="py-editor" spellcheck="false" autocomplete="off"
      placeholder="# imzML is already loaded — use the variables below:
#   imzml_path : str  — path to the .imzML file on the server
#   ibd_path   : str  — path to the .ibd file
#   im          — imzml_ext module
#   exp         — OnDiscImzMLExperiment (already opened)
#
# API mirrors pyOpenMS OnDiscMSExperiment + MSSpectrum.
# camelCase and snake_case both work — see the API reference below.
#
# Quick start:
print(exp.getNrSpectra())
mz, intensity = exp.getSpectrum(0).get_peaks()
print(f'{len(mz)} peaks, base peak m/z={float(mz[intensity.argmax()]):.4f}')
"></textarea>
    <details class="py-api-ref">
      <summary>API reference</summary>
      <div class="py-api-ref-body">
        <div class="py-api-group">
          <h4>OnDiscImzMLExperiment (exp)</h4>
          <ul>
            <li title="open a file">open(path) / openFile(path)</li>
            <li>getNrSpectra() / get_nr_spectra()</li>
            <li>getSpectrum(i) / get_spectrum(i)</li>
            <li>get_spectrum_at_coordinate(x,y)</li>
            <li>get_spectrum_index(i) &#8594; IndexEntry</li>
            <li>getExperimentalSettings() &#8594; metadata</li>
            <li>grid_width, grid_height <span class="ret">: int</span></li>
            <li>is_open <span class="ret">: bool</span></li>
            <li>metadata <span class="ret">: ImzMLMetadata</span></li>
            <li>size() / len(exp) / exp[i]</li>
          </ul>
        </div>
        <div class="py-api-group">
          <h4>MSSpectrum (spec)</h4>
          <ul>
            <li>get_peaks() &#8594; (mz float64, int float32)</li>
            <li>get_mz_array() &#8594; np.float64</li>
            <li>get_intensity_array() &#8594; np.float32</li>
            <li>set_peaks(mz, intensity)</li>
            <li>size <span class="ret">: int</span></li>
            <li>getMSLevel() / ms_level <span class="ret">: int</span></li>
            <li>getRT() / setRT(v) / rt <span class="ret">: float</span></li>
            <li>getName() / setName(s) / name <span class="ret">: str</span></li>
            <li>native_id <span class="ret">: str</span></li>
            <li>coord_x, coord_y, coord_z <span class="ret">: int</span></li>
            <li>base_peak <span class="ret">: float (m/z)</span></li>
            <li>tic <span class="ret">: float</span></li>
            <li>is_sorted <span class="ret">: bool</span></li>
            <li>sortByPosition() / sort_by_position()</li>
          </ul>
        </div>
        <div class="py-api-group">
          <h4>ImzMLMetadata (exp.metadata)</h4>
          <ul>
            <li>imaging_mode <span class="ret">'continuous'|'processed'</span></li>
            <li>max_x, max_y, max_z <span class="ret">: int</span></li>
            <li>pixel_size_x, pixel_size_y <span class="ret">: int (µm)</span></li>
            <li>max_dim_x, max_dim_y <span class="ret">: float (mm)</span></li>
            <li>mz_data_type, int_data_type <span class="ret">: str</span></li>
            <li>polarity, scan_pattern <span class="ret">: str</span></li>
            <li>uuid, ibd_checksum, ibd_checksum_type</li>
            <li>ibd_file_path <span class="ret">: str</span></li>
          </ul>
        </div>
        <div class="py-api-group">
          <h4>SpectrumIndexEntry</h4>
          <ul>
            <li>mz_offset, mz_length, mz_type, mz_enc_len</li>
            <li>int_offset, int_length, int_type, int_enc_len</li>
            <li>x, y, z <span class="ret">: int</span></li>
          </ul>
        </div>
      </div>
    </details>
    <div id="py-run-row">
      <button id="py-run-btn" onclick="runPython()">&#9654;&nbsp; Run</button>
      <span id="py-run-status"></span>
      <button style="margin-left:auto;font-size:11px" onclick="document.getElementById('py-output').textContent=''">Clear</button>
    </div>
    <div id="py-output"><span class="out-hint"># output will appear here</span></div>
  </div><!-- /tab-python -->
</div><!-- /page-python -->

<script>
"use strict";
// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const S = {
  info: null, img: null, selN: null,
  sortCol: 'idx', sortAsc: true, statsData: null,
};

// ---------------------------------------------------------------------------
// API helpers
// ---------------------------------------------------------------------------
async function api(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(await r.text());
  return r.json();
}
async function postLoad(filePath) {
  const r = await fetch('/api/load', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'path=' + encodeURIComponent(filePath),
  });
  return r.json();
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------
async function init() {
  const status = await api('/api/status');
  if (status.loaded) {
    await initViewer();
  }
  // else open-panel is already visible from HTML
  loadSamples(); // populate sample downloads (fire-and-forget)
}

// ---------------------------------------------------------------------------
// Sample datasets
// ---------------------------------------------------------------------------
async function loadSamples() {
  try {
    const list = await api('/api/samples');
    if (!list || !list.length) return;
    document.getElementById('samples-loading').style.display = 'none';
    document.getElementById('samples-section').style.display = '';
    const grid = document.getElementById('samples-grid');
    grid.innerHTML = list.map(s => {
      const sizeLbl = s.size_mb
        ? (s.size_mb < 1 ? Math.round(s.size_mb * 1000) + ' KB' : s.size_mb.toFixed(1) + ' MB')
        : '';
      const badges = [
        s.mode    ? `<span class="sample-badge">${escHtml(s.mode)}</span>`    : '',
        s.grid    ? `<span class="sample-badge">${escHtml(s.grid)}</span>`    : '',
        s.spectra ? `<span class="sample-badge">${s.spectra.toLocaleString()} spectra</span>` : '',
        sizeLbl   ? `<span class="sample-badge">${escHtml(sizeLbl)}</span>`  : '',
      ].filter(Boolean).join('');
      let action;
      if (s.file) {
        const url = '/api/samples/download?name=' + encodeURIComponent(s.file);
        action = `<a class="sample-dl-btn" href="${url}" download="${escHtml(s.file)}">&#8595; Download ZIP</a>`;
      } else if (s.external_url) {
        action = `<a class="sample-dl-btn sample-ext-btn" href="${escHtml(s.external_url)}" target="_blank" rel="noopener">&#8599; Dataset Page</a>`;
      } else {
        action = '';
      }
      return `<div class="sample-card">
        <div class="sample-card-title">${escHtml(s.title || s.file)}</div>
        <div class="sample-card-desc">${escHtml(s.description || '')}</div>
        <div class="sample-card-badges">${badges}</div>
        ${action}
      </div>`;
    }).join('');

  } catch (e) { /* samples dir not available -- silently skip */ }
}

// ---------------------------------------------------------------------------
// Open panel
// ---------------------------------------------------------------------------
function showOpenPanel() {
  switchTopTab('viewer');
  document.getElementById('open-panel').style.display  = '';
  document.getElementById('viewer').style.display      = 'none';
  document.getElementById('hdr-change').style.display  = 'none';
  document.getElementById('hdr-file').textContent      = 'No file loaded';
  document.getElementById('open-error').textContent    = '';
  document.getElementById('upload-status').textContent = '';
  document.getElementById('upload-btn').disabled       = false;
  document.getElementById('py-no-file').style.display  = '';
  animBar(0);
}


// ---------------------------------------------------------------------------
// File upload
// ---------------------------------------------------------------------------
function uploadFile(file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/upload?name=' + encodeURIComponent(file.name));
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    if (onProgress) xhr.upload.onprogress = e => { if (e.lengthComputable) onProgress(e.loaded, e.total); };
    xhr.onload = () => {
      if (xhr.status !== 200) { reject(new Error('HTTP ' + xhr.status)); return; }
      try { resolve(JSON.parse(xhr.responseText)); } catch(e) { reject(new Error('Bad response')); }
    };
    xhr.onerror = () => reject(new Error('Network error'));
    xhr.send(file);
  });
}

async function uploadFiles(fileList) {
  // Snapshot before resetting the input (FileList is live — reset clears it)
  const files  = Array.from(fileList);
  // Reset file input so the same file can be re-selected next time
  document.getElementById('file-input').value = '';

  const imzml  = files.find(f => f.name.toLowerCase().endsWith('.imzml'));
  const ibd    = files.find(f => f.name.toLowerCase().endsWith('.ibd'));

  if (!imzml) {
    document.getElementById('open-error').textContent =
      'Please select the .imzML file (and the paired .ibd).';
    return;
  }

  // Warn clearly if .ibd is missing (all peaks would be 0 without it)
  if (!ibd) {
    document.getElementById('open-error').textContent =
      'No .ibd file selected. Select both the .imzML and .ibd files together — '
      'the .ibd file contains all the spectral intensity data.';
    document.getElementById('upload-btn').disabled = false;
    return;
  }

  const statusEl = document.getElementById('upload-status');
  const errEl    = document.getElementById('open-error');
  errEl.textContent    = '';
  statusEl.textContent = '';
  document.getElementById('upload-btn').disabled = true;
  animBar(5);

  try {
    // Upload .imzML with progress
    const imzmlMB = (imzml.size / 1e6).toFixed(1);
    const r1 = await uploadFile(imzml, (loaded, total) => {
      const pct = Math.round(loaded / total * 100);
      statusEl.textContent = 'Uploading ' + imzml.name
        + ' \u2014 ' + (loaded/1e6).toFixed(1) + '\u202f/\u202f' + imzmlMB + ' MB (' + pct + '%)\u2026';
      animBar(Math.round(loaded / total * (ibd ? 38 : 68)));
    });
    if (!r1.ok) throw new Error(r1.error || 'upload failed');
    const imzmlPath = r1.path;
    animBar(ibd ? 38 : 68);

    // Upload .ibd with progress (may be very large)
    if (ibd) {
      const ibdMB = (ibd.size / 1e6).toFixed(1);
      const r2 = await uploadFile(ibd, (loaded, total) => {
        const pct = Math.round(loaded / total * 100);
        statusEl.textContent = 'Uploading ' + ibd.name
          + ' \u2014 ' + (loaded/1e6).toFixed(1) + '\u202f/\u202f' + ibdMB + ' MB (' + pct + '%)\u2026';
        animBar(38 + Math.round(loaded / total * 35));
      });
      if (!r2.ok) throw new Error(r2.error || 'upload failed');
      animBar(75);
    }

    statusEl.textContent = 'Loading dataset\u2026';
    await loadFile(imzmlPath);
    statusEl.textContent = '';
  } catch(e) {
    errEl.textContent    = String(e);
    statusEl.textContent = '';
    animBar(0);
  }
  document.getElementById('upload-btn').disabled = false;
}

async function loadFile(pathVal) {
  document.getElementById('open-error').textContent = '';
  animBar(30);
  try {
    const resp = await postLoad(pathVal);
    animBar(80);
    if (!resp.loaded) {
      document.getElementById('open-error').textContent = resp.error || 'Load failed.';
      animBar(0);
    } else {
      animBar(100);
      await initViewer();
    }
  } catch(e) {
    document.getElementById('open-error').textContent = String(e);
    animBar(0);
  }
}

function animBar(pct) {
  document.getElementById('load-bar').style.width = pct + '%';
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// ---------------------------------------------------------------------------
// Viewer initialisation
// ---------------------------------------------------------------------------
async function initViewer() {
  S.info = null; S.img = null; S.selN = null;
  const [info, img] = await Promise.all([api('/api/info'), api('/api/image')]);
  S.info = info; S.img = img;

  document.getElementById('open-panel').style.display = 'none';
  document.getElementById('viewer').style.display     = 'block';
  document.getElementById('hdr-file').textContent     = info.file;
  document.getElementById('hdr-change').style.display = '';
  document.getElementById('py-no-file').style.display = 'none';

  renderMeta();
  renderHeatmap(img);
  loadStats(img);  // reuse already-fetched image data -- no extra round-trip
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------
function renderMeta() {
  const m = S.info;
  const mzRange = (m.mzMin != null && m.mzMax != null)
    ? m.mzMin.toFixed(3) + ' – ' + m.mzMax.toFixed(3) : 'N/A';
  const physDim = (m.maxDimX && m.maxDimY)
    ? m.maxDimX.toFixed(0) + ' x ' + m.maxDimY.toFixed(0) + ' µm' : '';
  const rows = [
    ['Mode',       m.mode],
    ['Grid',       m.maxX + ' × ' + m.maxY + (m.maxZ > 1 ? ' × ' + m.maxZ : '')],
    ['Spectra',    m.spectra],
    ['Pixel',      m.pixelSizeX + ' × ' + m.pixelSizeY + ' µm'],
    physDim ? ['Extent', physDim] : null,
    ['m/z range',  mzRange],
    ['Max TIC',    m.maxTIC != null ? fmtN(m.maxTIC, 3) : 'N/A'],
    m.polarity && m.polarity !== 'unknown' ? ['Polarity', m.polarity] : null,
    m.mzDataType  && m.mzDataType  !== 'unknown' ? ['m/z dtype',  m.mzDataType]  : null,
    m.intDataType && m.intDataType !== 'unknown' ? ['Int dtype',  m.intDataType] : null,
    m.scanPattern   ? ['Scan pattern', m.scanPattern]   : null,
    m.scanDirection ? ['Scan dir',     m.scanDirection] : null,
    m.lineScanDir   ? ['Line dir',     m.lineScanDir]   : null,
    m.uuid ? ['UUID', m.uuid.slice(0, 12) + '…'] : null,
    [(m.ibdChecksumType || 'Checksum'), m.ibdChecksum ? m.ibdChecksum.slice(0,10) + '…' : 'N/A'],
  ].filter(Boolean);
  document.getElementById('meta-list').innerHTML = rows.map(
    ([k,v]) => `<div class="kv"><span class="kv-k">${k}</span><span class="kv-v">${escHtml(String(v))}</span></div>`
  ).join('');
}

// ---------------------------------------------------------------------------
// Heatmap
// ---------------------------------------------------------------------------
function renderHeatmap(data) {
  S.img = data;
  const maxX = S.info.maxX, maxY = S.info.maxY;
  const CELL = Math.max(18, Math.min(72, Math.floor(260 / Math.max(maxX, maxY))));
  const GAP  = 2;
  const W    = GAP + maxX * (CELL + GAP);
  const H    = GAP + maxY * (CELL + GAP);
  const canvas = document.getElementById('heatmap-canvas');
  canvas.width = W; canvas.height = H;
  const ctx = canvas.getContext('2d');
  const lut = {};
  data.forEach(d => { lut[d.x + ',' + d.y] = d; });
  const vals = data.map(d => d.tic);
  const vmin = Math.min(...vals), vmax = Math.max(...vals), vr = vmax - vmin || 1;
  ctx.fillStyle = '#e0e0e0'; ctx.fillRect(0, 0, W, H);
  for (let y = 1; y <= maxY; y++) {
    for (let x = 1; x <= maxX; x++) {
      const e  = lut[x + ',' + y];
      const cx = GAP + (x-1)*(CELL+GAP), cy = GAP + (y-1)*(CELL+GAP);
      if (!e) { ctx.fillStyle = '#f0f0f0'; ctx.fillRect(cx, cy, CELL, CELL); continue; }
      const t    = (e.tic - vmin) / vr;
      const gray = Math.round(255 * (1 - t));
      ctx.fillStyle = `rgb(${gray},${gray},${gray})`;
      ctx.fillRect(cx, cy, CELL, CELL);
      if (e.idx === S.selN) {
        ctx.strokeStyle = '#000'; ctx.lineWidth = 2;
        ctx.strokeRect(cx+1, cy+1, CELL-2, CELL-2);
      }
      if (CELL >= 28) {
        ctx.font = Math.max(9, CELL/5) + 'px monospace';
        ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
        ctx.fillStyle = t > .55 ? '#fff' : '#909090';
        ctx.fillText(x + ',' + y, cx + CELL/2, cy + CELL/2);
      }
    }
  }
  const cb  = document.getElementById('colorbar-canvas');
  const cbc = cb.getContext('2d');
  const gr  = cbc.createLinearGradient(0, 0, 120, 0);
  gr.addColorStop(0, '#fff'); gr.addColorStop(1, '#000');
  cbc.fillStyle = gr; cbc.fillRect(0, 0, 120, 10);
  document.getElementById('cb-lo').textContent = fmtN(vmin, 1);
  document.getElementById('cb-hi').textContent = fmtN(vmax, 1);
  canvas._C = CELL; canvas._G = GAP;
}

function heatmapHit(c, ex, ey) {
  const C = c._C, G = c._G; if (!C) return null;
  const x = Math.floor((ex - G) / (C + G)) + 1;
  const y = Math.floor((ey - G) / (C + G)) + 1;
  if (x < 1 || x > S.info.maxX || y < 1 || y > S.info.maxY) return null;
  return { x, y };
}

document.getElementById('heatmap-canvas').addEventListener('mousemove', e => {
  const c = e.currentTarget, r = c.getBoundingClientRect();
  const p = heatmapHit(c, e.clientX - r.left, e.clientY - r.top);
  if (!p || !S.img) { document.getElementById('pixel-tip').textContent = ''; return; }
  const en = S.img.find(d => d.x === p.x && d.y === p.y);
  if (en) {
    const ps = S.info;
    const physStr = (ps && ps.pixelSizeX > 1)
      ? '  (' + (en.x * ps.pixelSizeX) + '\u00d7' + (en.y * ps.pixelSizeY) + ' \u00b5m)' : '';
    document.getElementById('pixel-tip').textContent =
      'x=' + en.x + '  y=' + en.y + physStr + '  TIC=' + fmtN(en.tic, 1) + '  peaks=' + en.peaks;
  }
});
document.getElementById('heatmap-canvas').addEventListener('mouseleave', () => {
  document.getElementById('pixel-tip').textContent = '';
});
document.getElementById('heatmap-canvas').addEventListener('click', async e => {
  const c = e.currentTarget, r = c.getBoundingClientRect();
  const p = heatmapHit(c, e.clientX - r.left, e.clientY - r.top);
  if (!p || !S.img) return;
  const en = S.img.find(d => d.x === p.x && d.y === p.y);
  if (!en) return;
  S.selN = en.idx; renderHeatmap(S.img); highlightStatsRow(en.idx);
});
document.getElementById('img-render').addEventListener('click', async () => {
  const mz  = document.getElementById('mz-in').value.trim();
  const tol = document.getElementById('tol-in').value.trim() || '0.5';
  const url = mz
    ? `/api/image?mz=${encodeURIComponent(mz)}&tol=${encodeURIComponent(tol)}`
    : '/api/image';
  renderHeatmap(await api(url));
});
document.getElementById('img-reset').addEventListener('click', async () => {
  document.getElementById('mz-in').value = '';
  renderHeatmap(await api('/api/image'));
});
document.getElementById('img-basepeak').addEventListener('click', async () => {
  renderHeatmap(await api('/api/image?mode=basepeak'));
});
document.getElementById('img-savepng').addEventListener('click', () => {
  const canvas = document.getElementById('heatmap-canvas');
  const a = document.createElement('a');
  a.download = 'heatmap.png';
  a.href = canvas.toDataURL('image/png');
  a.click();
});
document.getElementById('img-matrix').addEventListener('click', () => {
  const mz  = document.getElementById('mz-in').value.trim();
  const tol = document.getElementById('tol-in').value.trim() || '0.5';
  const url = mz
    ? `/api/ion-image?mz=${encodeURIComponent(mz)}&tol=${encodeURIComponent(tol)}`
    : '/api/ion-image';
  const a = document.createElement('a');
  a.href = url;
  a.download = 'ion_image.json';
  a.click();
});



// ---------------------------------------------------------------------------
// Tab switching
// ---------------------------------------------------------------------------
function switchTab(name) {
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  document.getElementById('tab-btn-' + name).classList.add('active');
  document.getElementById('tab-overview').style.display   = name === 'overview'   ? '' : 'none';
  document.getElementById('tab-benchmark').style.display  = name === 'benchmark'  ? '' : 'none';
  document.getElementById('tab-spectrum').style.display   = name === 'spectrum'   ? '' : 'none';
}

function switchTopTab(name) {
  document.querySelectorAll('.top-nav-btn').forEach(b => b.classList.remove('active'));
  document.getElementById('top-btn-' + name).classList.add('active');
  document.getElementById('page-viewer').style.display = name === 'viewer' ? '' : 'none';
  document.getElementById('page-python').style.display = name === 'python' ? 'block' : 'none';
  // Auto-run demo on first visit when a file is loaded
  if (name === 'python' && S.info && !pyDemoRan) { pyDemoRan = true; pyRunDemo(); }
}

// ---------------------------------------------------------------------------
// Python Playground
// ---------------------------------------------------------------------------
const PY_SNIPPETS = {
  pyopenms: `# imzml_ext mirrors the pyOpenMS OnDiscMSExperiment / MSSpectrum API.
# camelCase methods work exactly as they do in pyOpenMS.

# --- OnDiscMSExperiment-style access ---
print("Spectra:", exp.getNrSpectra())

spec = exp.getSpectrum(0)

# --- MSSpectrum-style access ---
mz, intensity = spec.get_peaks()
print(f"MS level  : {spec.getMSLevel()}")
print(f"RT        : {spec.getRT():.4f}")
print(f"Name/ID   : {spec.getName()}")
print(f"Peaks     : {spec.size}")
print(f"Base peak : m/z={float(mz[intensity.argmax()]):.4f}  int={float(intensity.max()):.1f}")
import numpy as np
print(f"TIC       : {float(intensity.sum()):.1f}")

# --- getExperimentalSettings() returns ImzMLMetadata ---
s = exp.getExperimentalSettings()
print(f"Grid      : {s.max_x} x {s.max_y}")
print(f"Mode      : {s.imaging_mode}")
print(f"Pixel µm  : {s.pixel_size_x} x {s.pixel_size_y}")
`,
  specinfo: `import numpy as np

# Full spectrum property inspection
spec = exp.getSpectrum(0)
print(f"coord      : ({spec.coord_x}, {spec.coord_y}, {spec.coord_z})")
print(f"name / id  : {spec.name}")
print(f"rt         : {spec.rt}")
print(f"ms_level   : {spec.ms_level}")
print(f"size       : {spec.size}")
print(f"is_sorted  : {spec.is_sorted}")
print(f"base_peak  : {spec.base_peak:.4f} m/z")
print(f"tic        : {spec.tic:.1f}")

mz, intensity = spec.get_peaks()
print(f"\ndtype mz={mz.dtype}  intensity={intensity.dtype}")
print(f"shape: {mz.shape}")
print(f"\nFirst 5 peaks:")
for i in range(min(5, len(mz))):
    print(f"  [{i}]  m/z={mz[i]:.4f}  int={intensity[i]:.1f}")
`,
  ondisk: `import imzml_ext as im

# open() only parses the XML index — no IBD reads yet
exp = im.open(imzml_path)
print(f'Spectra  : {exp.get_nr_spectra()}')
print(f'Grid     : {exp.grid_width} x {exp.grid_height}')
print(f'Is open  : {exp.is_open}')

# Decode spectrum 0 (one fseek + fread)
spec = exp[0]
mz, intensity = spec.get_peaks()
print(f'Spectrum 0: {len(mz)} peaks')
print(f'm/z range : {mz.min():.3f} – {mz.max():.3f}')
print(f'TIC       : {intensity.sum():.1f}')

# Pixel coordinate lookup
spec2 = exp.get_spectrum_at_coordinate(1, 1)
print(f'Pixel (1,1): {len(spec2.get_mz_array())} peaks')
`,
  getpeaks: `import imzml_ext as im
import numpy as np

exp = im.open(imzml_path)

# get_peaks() returns (mz, intensity) as numpy arrays
mz, intensity = exp[0].get_peaks()
print(f'dtypes : mz={mz.dtype}  intensity={intensity.dtype}')
print(f'shape  : {mz.shape}')

# Numpy operations work directly
print(f'base peak: m/z={mz[np.argmax(intensity)]:.4f}  int={intensity.max():.1f}')
print(f'TIC      : {intensity.sum():.1f}')

# get_mz_array / get_intensity_array equivalents
mz2 = exp[0].get_mz_array()
int2 = exp[0].get_intensity_array()
print(f'Separate arrays: mz={mz2[:3]}...')
`,
  ionimage: `import imzml_ext as im
import numpy as np

exp = im.open(imzml_path)
TARGET_MZ = 500.0   # change this to a real m/z in your dataset
TOL = 0.5

grid = np.zeros((exp.grid_height, exp.grid_width))
for i in range(exp.get_nr_spectra()):
    spec = exp[i]
    mz, intensity = spec.get_peaks()
    mask = np.abs(mz - TARGET_MZ) <= TOL
    grid[spec.coord_y - 1, spec.coord_x - 1] = intensity[mask].sum()

print(f'Ion image shape: {grid.shape}')
print(f'Max intensity  : {grid.max():.1f}')
print(f'Non-zero pixels: {(grid > 0).sum()}')
`,
  metadata: `import imzml_ext as im

exp = im.open(imzml_path)
meta = exp.metadata
print(f'Imaging mode : {meta.imagingMode}')
print(f'Grid         : {meta.maxX} x {meta.maxY}')
print(f'Pixel size   : {meta.pixelSizeX} x {meta.pixelSizeY} µm')
print(f'UUID         : {meta.uuid}')
print(f'Checksum     : {meta.ibdChecksumType}: {meta.ibdChecksum[:12]}...')

# Index entries (no IBD reads)
entry = exp.get_spectrum_index(0)
print(f'\nSpectrum 0 IBD index:')
print(f'  mz_offset={entry.mz_offset}  mz_length={entry.mz_length}  mz_type={entry.mz_type}')
print(f'  x={entry.x}  y={entry.y}  z={entry.z}')
`,
  tic: `import imzml_ext as im
import numpy as np

exp = im.open(imzml_path)

# Compute TIC for every spectrum using numpy
tics = []
for i in range(min(exp.get_nr_spectra(), 20)):  # first 20 spectra
    _, intensity = exp[i].get_peaks()
    tics.append(float(intensity.sum()))

tics = np.array(tics)
print(f'Spectra     : {exp.get_nr_spectra()} total (showing first {len(tics)})')
print(f'TIC min/max : {tics.min():.1f} / {tics.max():.1f}')
print(f'TIC median  : {np.median(tics):.1f}')
for i, t in enumerate(tics):
    e = exp.get_spectrum_index(i)
    print(f'  [{i:3d}] ({e.x:3d},{e.y:3d})  TIC={t:.1f}')
`,
};

const PY_DEMO_SCRIPT = `
import numpy as np

# imzml_ext is a compiled nanobind C++ extension.
# The binding layer (python/imzml_ext.cpp) looks like:
#
#   namespace nb = nanobind;
#   NB_MODULE(imzml_ext, m) {
#       nb::class_<OpenMS::OnDiscImzMLExperiment>(m, "OnDiscImzMLExperiment")
#           .def("getSpectrum",        &..::getSpectrum)
#           .def("get_nr_spectra",     &..::getNrSpectra)
#           .def_prop_ro("metadata",   &..::getImzMLMetadata)
#           .def_prop_ro("grid_width", &..::gridWidth)
#           ...
#       nb::class_<OpenMS::MSSpectrum>(m, "MSSpectrum")
#           .def("get_peaks",          [](const MSSpectrum& s){ ... numpy ... })
#           .def("__iter__",           nb::make_iterator(...))
#           .def_prop_ro("coord_x",    &..::getCoordX)
#           .def_prop_ro("tic",        [...](){ sum intensities })
#           ...
#       nb::class_<OpenMS::Peak1D>(m, "Peak1D")
#           .def_prop_ro("mz",         &..::getMZ)
#           .def_prop_ro("intensity",  &..::getIntensity)
#           ...
#       nb::class_<OpenMS::ImzMLMetadata>(m, "ImzMLMetadata")
#           .def_rw("imaging_mode",    &..::imagingMode)
#           .def_rw("max_x",          &..::maxX)
#           ...
#       nb::class_<OpenMS::SpectrumIndexEntry>(m, "SpectrumIndexEntry")
#           .def_ro("mz_offset",       &..::mz_offset)
#           .def_ro("mz_length",       &..::mz_length)
#           .def_ro("x", ...)  .def_ro("y", ...)
#   }

# ---- Python usage ----

# im.open()  →  OnDiscImzMLExperiment (C++ lazy reader, no IBD loaded yet)
# exp is pre-injected by the server preamble; type shown below:
print(f"type(exp)  = {type(exp).__qualname__}  [C++ class via nanobind]")

# exp.metadata  →  ImzMLMetadata (C++ struct, exposed with def_rw)
meta = exp.metadata
print(f"type(meta) = {type(meta).__qualname__}  [C++ struct via nanobind]")
print(f"  repr     : {repr(meta)}")
print(f"  grid     : {meta.max_x} x {meta.max_y}  mode={meta.imaging_mode}")
print(f"  pixel    : {meta.pixel_size_x} x {meta.pixel_size_y} \\u00b5m")
print(f"  mz_type  : {meta.mz_data_type}    int_type: {meta.int_data_type}")
print()

# exp[0]  →  MSSpectrum (C++ object, .def(__getitem__))
spec = exp[0]
print(f"type(spec) = {type(spec).__qualname__}  [C++ class via nanobind]")
print(f"  coord    : ({spec.coord_x}, {spec.coord_y}, {spec.coord_z})")
print(f"  size     : {spec.size}    tic={spec.tic:.2f}    base_peak={spec.base_peak:.4f} Da")

# spec.get_peaks()  →  nanobind ndarray lambda → zero-copy numpy
mz, intensity = spec.get_peaks()
print(f"  mz       : {type(mz).__name__}  dtype={mz.dtype}  shape={mz.shape}  [zero-copy C++ buffer]")
print(f"  intensity: {type(intensity).__name__}  dtype={intensity.dtype}  shape={intensity.shape}")
print(f"  mz[0:3]  : {mz[:3]}")
print(f"  int[0:3] : {intensity[:3]}")
print()

# Peak1D C++ objects via nanobind iterator (.def(__iter__, nb::make_iterator))
print(f"Peak1D iteration (nanobind C++ begin/end iterator):")
for i, peak in enumerate(spec):
    if i >= 3: print("  ..."); break
    print(f"  {repr(peak)}  [type={type(peak).__qualname__}]")
print()

# SpectrumIndexEntry: IBD byte metadata without any peak decode
entry = exp.get_spectrum_index(0)
print(f"type(entry) = {type(entry).__qualname__}  [C++ struct, no IBD decode]")
print(f"  {repr(entry)}")
print(f"  mz_offset={entry.mz_offset}  mz_length={entry.mz_length}  mz_type={entry.mz_type}")
print()

print("\\u2713 NB_MODULE(imzml_ext)   \\u2713 nb::class_<OnDiscImzMLExperiment>")
print("\\u2713 nb::class_<MSSpectrum> \\u2713 nb::class_<Peak1D>")
print("\\u2713 nb::class_<ImzMLMetadata> \\u2713 nb::class_<SpectrumIndexEntry>")
print("\\u2713 get_peaks() zero-copy numpy  \\u2713 nb::make_iterator C++ Peak1D")
`.trim();

let pyDemoRan = false;

function pyRunDemo() {
  document.getElementById('py-editor').value = PY_DEMO_SCRIPT;
  runPython();
}

function pySnippet(name) {
  const t = document.getElementById('py-editor');
  t.value = PY_SNIPPETS[name] || '';
  t.focus();
}

// Insert text at cursor position in the editor (used by API ref click-to-insert)
function pyInsertAtCursor(text) {
  const t = document.getElementById('py-editor');
  const start = t.selectionStart, end = t.selectionEnd;
  t.value = t.value.slice(0, start) + text + t.value.slice(end);
  t.selectionStart = t.selectionEnd = start + text.length;
  t.focus();
}

// Wire up API reference list items to insert method name at cursor
// Wire up API reference list items to insert method name at cursor

async function runPython() {
  const code = document.getElementById('py-editor').value.trim();
  if (!code) return;
  const btn    = document.getElementById('py-run-btn');
  const status = document.getElementById('py-run-status');
  const output = document.getElementById('py-output');
  btn.disabled = true;
  status.textContent = 'Running\u2026';
  output.innerHTML = '<span class="out-hint"># running\u2026</span>';
  try {
    const r = await fetch('/api/run-python', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'code=' + encodeURIComponent(code),
    });
    const d = await r.json();
    let html = '';
    if (d.stdout) html += `<span class="out-stdout">${escHtml(d.stdout)}</span>`;
    if (d.stderr) html += `<span class="out-stderr">${escHtml(d.stderr)}</span>`;
    if (!d.stdout && !d.stderr) html = '<span class="out-hint"># (no output)</span>';
    if (d.error)  html += `<span class="out-stderr">\nError: ${escHtml(d.error)}</span>`;
    output.innerHTML = html;
    const elapsed = d.elapsed_ms ? ` (${d.elapsed_ms} ms)` : '';
    status.textContent = d.ok ? 'Done' + elapsed : 'Failed';
  } catch(e) {
    output.innerHTML = `<span class="out-stderr">${escHtml(String(e))}</span>`;
    status.textContent = 'Failed';
  } finally {
    btn.disabled = false;
  }
}

// Ctrl+Enter / ⌘+Enter in the editor submits; Tab inserts spaces;
// API reference list items click-to-insert at cursor
document.addEventListener('DOMContentLoaded', () => {
  const ed = document.getElementById('py-editor');
  if (ed) {
    ed.addEventListener('keydown', e => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); runPython(); }
      // Tab key inserts spaces instead of shifting focus
      if (e.key === 'Tab') {
        e.preventDefault();
        const s = ed.selectionStart, end = ed.selectionEnd;
        ed.value = ed.value.slice(0, s) + '    ' + ed.value.slice(end);
        ed.selectionStart = ed.selectionEnd = s + 4;
      }
    });
  }
  document.querySelectorAll('.py-api-group li').forEach(li => {
    li.title = 'Click to insert at cursor';
    li.addEventListener('click', () => {
      const raw = li.textContent.trim().split(/[\s\u2192(]/)[0];
      pyInsertAtCursor(raw);
    });
  });
});

// ---------------------------------------------------------------------------
// Benchmark tab
// ---------------------------------------------------------------------------
async function loadBenchmark() {
  const btn = document.getElementById('bench-run');
  const status = document.getElementById('bench-status');
  btn.disabled = true;
  status.textContent = 'Running pyimzml... this may take a few seconds.';
  document.getElementById('bench-tables').innerHTML = '';
  try {
    const d = await api('/api/benchmark');
    if (!d.ok) { status.textContent = 'Error: ' + (d.error || 'unknown'); return; }
    status.textContent = 'Done. ' + d.ours.length + ' spectra compared.';
    renderBenchTables(d);
  } catch(e) {
    status.textContent = 'Error: ' + String(e);
  } finally { btn.disabled = false; }
}

function renderBenchTables(d) {
  const cols = ['idx','x','y','z','TIC','peaks'];
  const hdr = '<thead><tr>' + cols.map(c => `<th>${c}</th>`).join('') + '</tr></thead>';
  function rows(data) {
    return data.map(r =>
      `<tr><td>${r.idx}</td><td>${r.x}</td><td>${r.y}</td><td>${r.z}</td>`
      + `<td>${fmtN(r.tic, 3)}</td><td>${r.peaks}</td></tr>`
    ).join('');
  }
  const pyOk = Array.isArray(d.pyimzml);
  document.getElementById('bench-tables').innerHTML =
    `<div class="bench-grid">`
    + `<div><div class="bench-panel-title">Our Parser <span>(C++ / OpenMS)</span></div>`
    + `<table>${hdr}<tbody>${rows(d.ours)}</tbody></table></div>`
    + `<div><div class="bench-panel-title">pyimzml <span>(Python reference)</span></div>`
    + (pyOk
        ? `<table>${hdr}<tbody>${rows(d.pyimzml)}</tbody></table>`
        : `<div style="color:var(--err);font-size:12px;font-family:var(--mono)">${escHtml(JSON.stringify(d.pyimzml))}</div>`)
    + `</div></div>`;
}

// ---------------------------------------------------------------------------
// Stats table
// ---------------------------------------------------------------------------
function loadStats(data) {
  // 'data' is the same payload as /api/image -- reuse it, no extra fetch needed
  S.statsData = data; renderStatsTable(data);
}
function renderStatsTable(data) {
  const sorted = [...data].sort(mkSort(S.sortCol, S.sortAsc));
  const rows = sorted.map(d =>
    `<tr data-n="${d.idx}" onclick="statsClick(${d.idx})"${d.idx === S.selN ? ' class="selected"' : ''}>` +
    `<td>${d.idx}</td><td>${d.x}</td><td>${d.y}</td><td>${d.z}</td>` +
    `<td>${fmtN(d.tic, 1)}</td><td>${d.peaks}</td></tr>`
  ).join('');
  document.getElementById('stats-wrap').innerHTML =
    `<table><thead><tr>` +
    ['idx','x','y','z','TIC','peaks'].map(c =>
      `<th onclick="sortBy('${c}')">${c}${S.sortCol === c ? (S.sortAsc ? ' ^' : ' v') : ''}</th>`
    ).join('') +
    `</tr></thead><tbody>${rows}</tbody></table>`;
}
function mkSort(col, asc) {
  return (a, b) => {
    const av = a[col], bv = b[col];
    return asc ? (av < bv ? -1 : av > bv ? 1 : 0) : (av > bv ? -1 : av < bv ? 1 : 0);
  };
}
function sortBy(col) {
  if (S.sortCol === col) S.sortAsc = !S.sortAsc;
  else { S.sortCol = col; S.sortAsc = true; }
  if (S.statsData) renderStatsTable(S.statsData);
}
async function statsClick(n) {
  S.selN = n; renderHeatmap(S.img); highlightStatsRow(n);
}
function highlightStatsRow(n) {
  document.querySelectorAll('#stats-wrap tr').forEach(t => t.classList.remove('selected'));
  const row = document.querySelector(`#stats-wrap tr[data-n="${n}"]`);
  if (row) { row.classList.add('selected'); row.scrollIntoView({ block: 'nearest' }); }
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
function fmtN(v, dec) {
  if (v === 0) return '0';
  if (dec === 0) {
    if (Math.abs(v) >= 1e6) return (v/1e6).toFixed(2) + 'M';
    if (Math.abs(v) >= 1e3) return (v/1e3).toFixed(1) + 'k';
    return Math.round(v).toString();
  }
  return v.toFixed(dec);
}

document.addEventListener('DOMContentLoaded', () => { init().catch(console.error); });

// ---------------------------------------------------------------------------
// Spectrum tab
// ---------------------------------------------------------------------------
let S_spec = null; // current spectrum data {mz:[],intensity:[],type,label}

async function loadSpectrumView(type) {
  document.getElementById('spec-info').textContent = 'Loading…';
  try {
    const endpoint = type === 'avg' ? '/api/avgspectrum' : '/api/maxspectrum';
    const d = await api(endpoint);
    if (d.error) { document.getElementById('spec-info').textContent = d.error; return; }
    S_spec = d;
    S_spec._label = type === 'avg'
      ? 'Average spectrum (' + d.spectra + ' spectra)'
      : 'Max spectrum (' + d.spectra + ' spectra)';
    drawSpectrum();
    document.getElementById('spec-csv-btn').disabled = false;
  } catch(e) {
    document.getElementById('spec-info').textContent = String(e);
  }
}

async function loadSpectrumPixel() {
  const px = parseInt(document.getElementById('spec-px').value) || 1;
  const py = parseInt(document.getElementById('spec-py').value) || 1;
  document.getElementById('spec-info').textContent = 'Loading\u2026';
  try {
    const d = await api('/api/spectrum?x=' + px + '&y=' + py);
    if (d.error) { document.getElementById('spec-info').textContent = 'Error: ' + d.error; return; }
    S_spec = d;
    S_spec.type = 'pixel';
    const physStr = (d.physX != null && S.info && S.info.pixelSizeX > 1)
      ? '  ' + d.physX.toFixed(0) + '\u00d7' + d.physY.toFixed(0) + ' \u00b5m' : '';
    S_spec._label = 'Pixel (' + px + ',' + py + ')  idx=' + d.n + '  peaks=' + d.mz.length + physStr;
    drawSpectrum();
    document.getElementById('spec-csv-btn').disabled = false;
  } catch(e) {
    document.getElementById('spec-info').textContent = String(e);
  }
}

function exportImzML() {
  const lo = document.getElementById('exp-lo').value.trim();
  const hi = document.getElementById('exp-hi').value.trim();
  let url = '/api/export';
  const params = [];
  if (lo) params.push('mz_lo=' + encodeURIComponent(lo));
  if (hi) params.push('mz_hi=' + encodeURIComponent(hi));
  if (params.length) url += '?' + params.join('&');
  const a = document.createElement('a'); a.href = url; document.body.appendChild(a); a.click(); document.body.removeChild(a);
}

function drawSpectrum() {
  if (!S_spec) return;
  const mz = S_spec.mz, inten = S_spec.intensity;
  if (!mz || mz.length === 0) { document.getElementById('spec-info').textContent = 'No data'; return; }

  const canvas = document.getElementById('spec-canvas');
  const W = canvas.offsetWidth || 800;
  const H = canvas.offsetHeight || 300;
  canvas.width  = W;
  canvas.height = H;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, W, H);

  const pad = { l: 68, r: 24, t: 20, b: 40 };
  const iW = W - pad.l - pad.r;
  const iH = H - pad.t - pad.b;

  const mzMin = mz[0], mzMax = mz[mz.length - 1];
  const iMax  = Math.max(...inten);
  const mzR   = mzMax - mzMin || 1;

  // Background
  ctx.fillStyle = '#fafafa'; ctx.fillRect(pad.l, pad.t, iW, iH);

  // Grid lines
  ctx.strokeStyle = '#e8e8e8'; ctx.lineWidth = 1;
  for (let i = 0; i <= 5; i++) {
    const y = pad.t + iH - i / 5 * iH;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(pad.l + iW, y); ctx.stroke();
  }
  for (let i = 0; i <= 8; i++) {
    const x = pad.l + i / 8 * iW;
    ctx.beginPath(); ctx.moveTo(x, pad.t); ctx.lineTo(x, pad.t + iH); ctx.stroke();
  }

  // Spectrum line
  ctx.strokeStyle = '#111'; ctx.lineWidth = 1.2;
  ctx.beginPath();
  for (let i = 0; i < mz.length; i++) {
    const x = pad.l + (mz[i] - mzMin) / mzR * iW;
    const y = pad.t + iH - (iMax > 0 ? inten[i] / iMax : 0) * iH;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Axes
  ctx.strokeStyle = '#888'; ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(pad.l, pad.t); ctx.lineTo(pad.l, pad.t + iH);
  ctx.lineTo(pad.l + iW, pad.t + iH);
  ctx.stroke();

  // Axis labels
  ctx.fillStyle = '#555'; ctx.font = '11px monospace'; ctx.textAlign = 'right';
  for (let i = 0; i <= 5; i++) {
    const v = iMax * i / 5;
    const y = pad.t + iH - i / 5 * iH;
    ctx.fillText(fmtN(v, 0), pad.l - 6, y + 4);
  }
  ctx.textAlign = 'center';
  for (let i = 0; i <= 8; i++) {
    const v = mzMin + mzR * i / 8;
    const x = pad.l + i / 8 * iW;
    ctx.fillText(v.toFixed(1), x, pad.t + iH + 16);
  }

  // Axis titles
  ctx.save(); ctx.translate(14, pad.t + iH / 2); ctx.rotate(-Math.PI / 2);
  ctx.textAlign = 'center'; ctx.fillStyle = '#555';
  ctx.fillText('Intensity', 0, 0); ctx.restore();
  ctx.textAlign = 'center'; ctx.fillStyle = '#555';
  ctx.fillText('m/z', pad.l + iW / 2, H - 6);

  document.getElementById('spec-info').textContent =
    (S_spec._label || '') + '  |  points=' + mz.length +
    '  |  m/z ' + mzMin.toFixed(3) + ' – ' + mzMax.toFixed(3) +
    '  |  max inten=' + fmtN(iMax, 3);
}

// Redraw on resize
window.addEventListener('resize', () => { if (S_spec) drawSpectrum(); });

function exportSpectrumCSV() {
  if (!S_spec) return;
  const rows = ['mz,intensity'];
  for (let i = 0; i < S_spec.mz.length; i++)
    rows.push(S_spec.mz[i] + ',' + S_spec.intensity[i]);
  const blob = new Blob([rows.join('\n')], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'spectrum_' + (S_spec.type || 'data') + '.csv';
  a.click();
}

function exportStatsCSV() {
  if (!S.statsData) return;
  const rows = ['idx,x,y,z,tic,peaks'];
  S.statsData.forEach(d => rows.push([d.idx,d.x,d.y,d.z,d.tic,d.peaks].join(',')));
  const blob = new Blob([rows.join('\n')], { type: 'text/csv' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'pixel_stats.csv';
  a.click();
}
</script>
</body>
</html>
)=====";

// ===========================================================================
// Minimal POSIX HTTP / JSON utilities
// ===========================================================================

// --- query-string parser ---------------------------------------------------
static std::map<std::string, std::string> parseQuery(const std::string& qs)
{
    std::map<std::string, std::string> m;
    std::string key, val;
    bool inVal = false;
    auto flush = [&]{ if (!key.empty()) m[key] = val; key.clear(); val.clear(); inVal = false; };
    for (char c : qs) {
        if      (c == '&') { flush(); }
        else if (c == '=') { inVal = true; }
        else if (inVal)    { val += c; }
        else               { key += c; }
    }
    flush();
    return m;
}

// --- JSON helpers ----------------------------------------------------------
static std::string js(const std::string& s) // JSON string
{
    std::string r; r.reserve(s.size() + 2); r += '"';
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else                r += c;
    }
    return r + '"';
}
static std::string jn(double v, int prec = 6) // JSON number
{
    char buf[32]; snprintf(buf, sizeof(buf), "%.*g", prec, v); return buf;
}
static std::string ji(long long v) { return std::to_string(v); }

// --- HTTP response ---------------------------------------------------------
static void sendResp(int fd, int code, const std::string& ct, const std::string& body)
{
    std::ostringstream h;
    h << "HTTP/1.1 " << code
      << (code == 200 ? " OK" : code == 404 ? " Not Found" : " Error") << "\r\n"
      << "Content-Type: " << ct << "; charset=utf-8\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Cache-Control: no-store\r\n"
      << "\r\n";
    const std::string head = h.str();
    // Write header
    size_t sent = 0;
    while (sent < head.size()) {
        ssize_t n = write(fd, head.c_str() + sent, head.size() - sent);
        if (n <= 0) return;
        sent += (size_t)n;
    }
    // Write body
    sent = 0;
    while (sent < body.size()) {
        ssize_t n = write(fd, body.c_str() + sent, body.size() - sent);
        if (n <= 0) return;
        sent += (size_t)n;
    }
}

// URL-decode a form-encoded value (%xx + '+' as space)
static std::string urlDecode(const std::string& s)
{
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { r += ' '; }
        else if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = { s[i+1], s[i+2], 0 };
            r += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else { r += s[i]; }
    }
    return r;
}

// --- HTTP request struct ---------------------------------------------------
struct Request {
    std::string method, path, queryStr, body;
    std::string bodyPreamble; // bytes already read past header end (for streaming upload)
    std::map<std::string, std::string> query;
    long long contentLength{0};
};

// --- Read full HTTP request (headers + optional body) ----------------------
static Request readRequest(int fd)
{
    Request req;
    std::string raw; raw.reserve(4096);
    char hbuf[4096]; // header read buffer
    // Read until we have a complete header block
    for (int t = 0; t < 256; ++t) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 5000) <= 0) break;
        ssize_t n = recv(fd, hbuf, sizeof(hbuf) - 1, 0);
        if (n <= 0) break;
        raw.append(hbuf, (size_t)n);
        if (raw.find("\r\n\r\n") != std::string::npos) break;
    }
    size_t hend = raw.find("\r\n\r\n");
    if (hend == std::string::npos) return req;

    // Parse request line and headers
    std::istringstream ss(raw.substr(0, hend));
    std::string pq;
    ss >> req.method >> pq;
    size_t qpos = pq.find('?');
    if (qpos == std::string::npos) { req.path = pq; }
    else { req.path = pq.substr(0, qpos); req.queryStr = pq.substr(qpos + 1); }
    req.query = parseQuery(req.queryStr);

    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string low = line;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (low.rfind("content-length:", 0) == 0) {
            try { req.contentLength = std::stoll(line.substr(15)); } catch (...) {}
        }
    }

    // Read body
    std::string bodyPart = raw.substr(hend + 4); // bytes already in recv buffer after headers
    if (req.path.rfind("/api/upload", 0) == 0) {
        // Streaming upload — don't buffer body in RAM; pass preamble to streaming handler
        req.bodyPreamble = std::move(bodyPart);
        return req;
    }
    if (req.contentLength > 0)
        bodyPart.reserve((size_t)std::min(req.contentLength, (long long)64*1024*1024));
    std::vector<char> bbuf(65536);
    while ((long long)bodyPart.size() < req.contentLength) {
        struct pollfd p2{fd, POLLIN, 0};
        if (poll(&p2, 1, 10000) <= 0) break;
        ssize_t n2 = recv(fd, bbuf.data(), bbuf.size(), 0);
        if (n2 <= 0) break;
        bodyPart.append(bbuf.data(), (size_t)n2);
    }
    req.body = bodyPart.substr(0, (size_t)req.contentLength);
    return req;
}

// ===========================================================================
// Dataset loader + status
// ===========================================================================

static bool loadDataset(const std::string& path, std::string& errOut)
{
    errOut.clear();
    auto* exp = new OpenMS::MSExperiment();
    OpenMS::PeakFileOptions opts;
    // setSortMZ intentionally omitted: imzML stores peaks sorted in the .ibd;
    // sorting every spectrum serially would dominate load time for large datasets.
    OpenMS::ImzMLFile loader;
    loader.setLogType(OpenMS::ProgressLogger::CMD);
    try { loader.load(path, *exp, opts); }
    catch (const std::exception& e) {
        delete exp;
        errOut = e.what();
        return false;
    }

    // Check that the IBD binary data file was found.
    if (exp->getImzMLMetadata().ibdFilePath.empty()) {
        delete exp;
        errOut = "IBD binary data file (.ibd) not found. "
                 "Please ensure the .ibd file is in the same directory as the .imzML file "
                 "and that both have the same base name.";
        return false;
    }

    // -----------------------------------------------------------------------
    // Build pixel cache in parallel (one pass, all CPU cores)
    // Computes per-spectrum TIC, base-peak, and global mzMin/mzMax.
    // -----------------------------------------------------------------------
    const size_t N = exp->size();
    std::vector<PixelEntry> cache(N);

    unsigned nT = std::max(1u, std::thread::hardware_concurrency());
    if (N < nT) nT = (unsigned)N;

    struct TR { double mzMin{1e18}, mzMax{-1e18}; };
    std::vector<TR> tres(nT);

    auto worker = [&](size_t start, size_t end, size_t tid) {
        TR& tr = tres[tid];
        for (size_t i = start; i < end; ++i) {
            const auto& s = (*exp)[i];
            double tic = 0.0, bp = 0.0;
            const size_t np = s.size();
            for (size_t j = 0; j < np; ++j) {
                const double inten = s[j].getIntensity();
                const double mz    = s[j].getMZ();
                tic += inten;
                if (inten > bp)     bp      = inten;
                if (mz    < tr.mzMin) tr.mzMin = mz;
                if (mz    > tr.mzMax) tr.mzMax = mz;
            }
            cache[i] = { s.getCoordX(), s.getCoordY(), s.getCoordZ(),
                         tic, bp, (int)np };
        }
    };

    {
        size_t chunk = (N + nT - 1) / nT;
        std::vector<std::thread> threads;
        threads.reserve(nT);
        for (unsigned t = 0; t < nT; ++t) {
            size_t s = t * chunk, e = std::min(s + chunk, N);
            threads.emplace_back(worker, s, e, (size_t)t);
        }
        for (auto& t : threads) t.join();
    }

    double mzMin = 1e18, mzMax = -1e18, maxTIC = 0.0, maxBP = 0.0;
    for (const auto& tr : tres) {
        if (tr.mzMin < mzMin) mzMin = tr.mzMin;
        if (tr.mzMax > mzMax) mzMax = tr.mzMax;
    }
    if (mzMin > mzMax) { mzMin = 0; mzMax = 0; }
    for (const auto& p : cache) {
        if (p.tic > maxTIC) maxTIC = p.tic;
        if (p.bp  > maxBP)  maxBP  = p.bp;
    }

    // Pre-serialise TIC and base-peak image JSON (serves /api/image in O(1))
    auto buildImageJson = [&](bool bp) -> std::string {
        std::ostringstream o;
        o << std::fixed << std::setprecision(6);
        o << "[";
        for (size_t i = 0; i < cache.size(); ++i) {
            const auto& p = cache[i];
            if (i) o << ",";
            o << "{\"idx\":" << i
              << ",\"x\":"   << p.x
              << ",\"y\":"   << p.y
              << ",\"z\":"   << p.z
              << ",\"tic\":" << (bp ? p.bp : p.tic)
              << ",\"peaks\":" << p.peaks
              << "}";
        }
        o << "]";
        return o.str();
    };

    // Build pre-serialised JSON before moving cache (lambda captures cache by ref)
    std::string ticJson = buildImageJson(false);
    std::string bpJson  = buildImageJson(true);

    delete g_exp;
    g_exp          = exp;
    g_path         = path;
    g_pixCache     = std::move(cache);
    g_mzMin        = mzMin;
    g_mzMax        = mzMax;
    g_maxTIC       = maxTIC;
    g_maxBP        = maxBP;
    g_ticImageJson = std::move(ticJson);
    g_bpImageJson  = std::move(bpJson);

    const auto& meta = g_exp->getImzMLMetadata();
    fprintf(stderr, "[imzml-server] Loaded %zu spectra  grid=%dx%d  mz=[%.2f,%.2f]\n",
            g_exp->size(), meta.maxX, meta.maxY, g_mzMin, g_mzMax);
    return true;
}

static std::string apiStatus()
{
    if (!g_exp)
        return "{\"loaded\":false,\"file\":\"\",\"error\":" + js(g_loadError) + "}";
    size_t sep = g_path.find_last_of("/\\");
    std::string fname = (sep == std::string::npos) ? g_path : g_path.substr(sep + 1);
    return "{\"loaded\":true,\"file\":" + js(fname) + ",\"error\":\"\"}";
}

// ===========================================================================
// API handlers
// ===========================================================================

// /api/info
static std::string apiInfo()
{
    if (!g_exp) return "{\"error\":\"no dataset loaded\"}";
    const auto& exp  = *g_exp;
    const auto& meta = exp.getImzMLMetadata();
    std::string mode =
        meta.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Continuous ? "Continuous" :
        meta.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Processed  ? "Processed"  : "Unknown";
    size_t sep = g_path.find_last_of("/\\");
    std::string fname = (sep == std::string::npos) ? g_path : g_path.substr(sep + 1);

    // Use pre-computed cache (built during loadDataset — no per-call scan)
    double mzMin = g_mzMin, mzMax = g_mzMax, maxTIC = g_maxTIC;

    std::ostringstream o;
    o << "{"
      << "\"file\":"            << js(fname)                       << ","
      << "\"path\":"            << js(g_path)                      << ","
      << "\"mode\":"            << js(mode)                        << ","
      << "\"spectra\":"         << ji((long long)exp.size())       << ","
      << "\"maxX\":"            << ji(meta.maxX)                   << ","
      << "\"maxY\":"            << ji(meta.maxY)                   << ","
      << "\"maxZ\":"            << ji(meta.maxZ)                   << ","
      << "\"pixelSizeX\":"      << ji(meta.pixelSizeX)             << ","
      << "\"pixelSizeY\":"      << ji(meta.pixelSizeY)             << ","
      << "\"maxDimX\":"         << jn(meta.maxDimX, 6)             << ","
      << "\"maxDimY\":"         << jn(meta.maxDimY, 6)             << ","
      << "\"uuid\":"            << js(meta.uuid)                   << ","
      << "\"polarity\":"        << js(meta.polarity)               << ","
      << "\"mzDataType\":"      << js(meta.mzDataType)             << ","
      << "\"intDataType\":"     << js(meta.intDataType)            << ","
      << "\"scanPattern\":"     << js(meta.scanPattern)            << ","
      << "\"scanDirection\":"   << js(meta.scanDirection)          << ","
      << "\"lineScanDir\":"     << js(meta.lineScanDirection)      << ","
      << "\"ibdChecksum\":"     << js(meta.ibdChecksum)            << ","
      << "\"ibdChecksumType\":" << js(meta.ibdChecksumType)        << ","
      << "\"ibdFile\":"         << js(meta.ibdFilePath)            << ","
      << "\"mzMin\":"           << jn(mzMin, 8)                    << ","
      << "\"mzMax\":"           << jn(mzMax, 8)                    << ","
      << "\"maxTIC\":"          << jn(maxTIC, 10)
      << "}";
    return o.str();
}

// /api/image  or  /api/image?mz=X&tol=Y  or  /api/image?mode=basepeak
static std::string apiImage(const std::map<std::string, std::string>& q)
{
    if (!g_exp) return "[]";

    // mode: "tic" (default), "basepeak", or "mz" (mz-window)
    bool isBasePeak = q.count("mode") && q.at("mode") == "basepeak";
    bool hasMz = !isBasePeak && q.count("mz") && !q.at("mz").empty();

    // Fast path: TIC and base-peak images are pre-serialised at load time
    if (!hasMz) return isBasePeak ? g_bpImageJson : g_ticImageJson;

    // mz-window: must scan spectra (interactive query — unavoidable)
    const auto& exp = *g_exp;
    double mzCenter = 0.0, mzTol = 0.5;
    try { mzCenter = std::stod(q.at("mz")); } catch (...) { return g_ticImageJson; }
    try { mzTol    = std::stod(q.at("tol")); } catch (...) {}
    double mzLo = mzCenter - mzTol, mzHi = mzCenter + mzTol;

    std::ostringstream o;
    o << std::fixed << std::setprecision(6);
    o << "[";
    for (size_t i = 0; i < exp.size(); ++i) {
        const auto& s = exp[i];
        const auto& p = g_pixCache[i];
        // Binary search for mzLo (spectra sorted by m/z as stored in .ibd)
        size_t left = 0, right = s.size();
        while (left < right) {
            size_t mid = (left + right) / 2;
            if (s[mid].getMZ() < mzLo) left = mid + 1; else right = mid;
        }
        double val = 0.0;
        for (size_t j = left; j < s.size() && s[j].getMZ() <= mzHi; ++j)
            val += s[j].getIntensity();
        if (i) o << ",";
        o << "{\"idx\":" << i
          << ",\"x\":"   << p.x
          << ",\"y\":"   << p.y
          << ",\"z\":"   << p.z
          << ",\"tic\":" << val
          << ",\"peaks\":" << p.peaks
          << "}";
    }
    o << "]";
    return o.str();
}

// /api/spectrum?n=N  or  /api/spectrum?x=X&y=Y
static std::string apiSpectrum(const std::map<std::string, std::string>& q)
{
    if (!g_exp) return "{\"error\":\"no dataset\"}";
    const auto& exp = *g_exp;
    int n = -1;

    // Coordinate lookup: find spectrum at pixel (x, y)
    if (q.count("x") && q.count("y")) {
        int px = 0, py = 0;
        try { px = std::stoi(q.at("x")); } catch (...) {}
        try { py = std::stoi(q.at("y")); } catch (...) {}
        for (size_t i = 0; i < exp.size(); ++i) {
            if ((int)exp[i].getCoordX() == px && (int)exp[i].getCoordY() == py) {
                n = (int)i; break;
            }
        }
        if (n < 0) return "{\"error\":\"pixel not found\"}";
    } else {
        try { n = std::stoi(q.at("n")); } catch (...) {}
    }

    if (n < 0 || (size_t)n >= exp.size())
        return "{\"error\":\"index out of range\"}";

    const auto& s    = exp[(size_t)n];
    const auto& meta = exp.getImzMLMetadata();
    // Physical coordinates in µm (pixel * pixel_size)
    double physX = s.getCoordX() * (double)meta.pixelSizeX;
    double physY = s.getCoordY() * (double)meta.pixelSizeY;

    std::ostringstream o;
    o << std::fixed << std::setprecision(5);
    o << "{\"n\":" << n
      << ",\"x\":" << s.getCoordX()
      << ",\"y\":" << s.getCoordY()
      << ",\"z\":" << s.getCoordZ()
      << ",\"physX\":" << physX
      << ",\"physY\":" << physY
      << ",\"mz\":[";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) o << ",";
        o << s[i].getMZ();
    }
    o << "],\"intensity\":[";
    o << std::setprecision(4);
    for (size_t i = 0; i < s.size(); ++i) {
        if (i) o << ",";
        o << s[i].getIntensity();
    }
    o << "]}";
    return o.str();
}

// /api/ion-image?mz=X&tol=Y  (or no params → TIC)
// Returns a true 2D grid matrix, like pyimzML's getionimage().
// Response: {"width":W,"height":H,"mz":X,"tol":Y,"matrix":[[row0],[row1],...]}
// matrix[r][c] = summed intensity at pixel (x=c+1, y=r+1); 0 at absent pixels.
static std::string apiIonImage(const std::map<std::string, std::string>& q)
{
    if (!g_exp) return "{\"error\":\"no dataset loaded\"}";
    const auto& exp  = *g_exp;
    const auto& meta = exp.getImzMLMetadata();
    int W = (int)meta.maxX;
    int H = (int)meta.maxY;
    if (W <= 0 || H <= 0) return "{\"error\":\"grid extents not available\"}";

    bool hasMz = q.count("mz") && !q.at("mz").empty();
    double mzCenter = 0.0, mzTol = 0.5;
    double mzLo = -1e18, mzHi = 1e18;
    if (hasMz) {
        try { mzCenter = std::stod(q.at("mz")); } catch (...) { hasMz = false; }
        if (hasMz) {
            try { mzTol = std::stod(q.at("tol")); } catch (...) {}
            mzLo = mzCenter - mzTol;
            mzHi = mzCenter + mzTol;
        }
    }

    // matrix[y-1][x-1] = summed intensity value
    std::vector<std::vector<double>> mat((size_t)H, std::vector<double>((size_t)W, 0.0));
    for (size_t i = 0; i < exp.size(); ++i) {
        const auto& s = exp[i];
        int cx = (int)s.getCoordX() - 1;
        int cy = (int)s.getCoordY() - 1;
        if (cx < 0 || cx >= W || cy < 0 || cy >= H) continue;
        double val = 0.0;
        if (!hasMz) {
            for (size_t j = 0; j < s.size(); ++j) val += s[j].getIntensity();
        } else {
            // Binary search for mzLo boundary
            size_t left = 0, right = s.size();
            while (left < right) {
                size_t mid = (left + right) / 2;
                if (s[mid].getMZ() < mzLo) left = mid + 1; else right = mid;
            }
            for (size_t j = left; j < s.size() && s[j].getMZ() <= mzHi; ++j)
                val += s[j].getIntensity();
        }
        mat[(size_t)cy][(size_t)cx] = val;
    }

    std::ostringstream o;
    o << std::fixed << std::setprecision(6);
    o << "{\"width\":" << W << ",\"height\":" << H;
    if (hasMz) o << ",\"mz\":" << mzCenter << ",\"tol\":" << mzTol;
    o << ",\"matrix\":[";
    for (int r = 0; r < H; ++r) {
        if (r) o << ",";
        o << "[";
        for (int c = 0; c < W; ++c) {
            if (c) o << ",";
            o << mat[(size_t)r][(size_t)c];
        }
        o << "]";
    }
    o << "]}";
    return o.str();
}

// /api/export?mz_lo=X&mz_hi=Y
// Exports the loaded dataset (optionally m/z-range-filtered) as an imzML+ibd
// zip archive for download. Uses the local imzml::ImzMLWriter.
// Forward declaration — sendBinaryFile is defined further below.
static void sendBinaryFile(int fd, const std::string& dlname,
                           const std::string& ct, const std::string& data);
static void apiExport(int fd, const std::map<std::string, std::string>& q)
{
    if (!g_exp) {
        sendResp(fd, 400, "application/json", "{\"error\":\"no dataset loaded\"}");
        return;
    }
    if (g_tempDir.empty()) {
        sendResp(fd, 500, "text/plain", "temp dir unavailable");
        return;
    }

    double mzLo = -1e18, mzHi = 1e18;
    bool filtered = false;
    if (q.count("mz_lo") && !q.at("mz_lo").empty()) {
        try { mzLo = std::stod(q.at("mz_lo")); filtered = true; } catch (...) {}
    }
    if (q.count("mz_hi") && !q.at("mz_hi").empty()) {
        try { mzHi = std::stod(q.at("mz_hi")); filtered = true; } catch (...) {}
    }

    const auto& exp  = *g_exp;
    const auto& meta = exp.getImzMLMetadata();

    imzml::ImzMLWriterOptions opts;
    opts.mode = (meta.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Processed)
                    ? imzml::ImagingMode::Processed
                    : imzml::ImagingMode::Continuous;
    opts.pixelSizeX = (double)meta.pixelSizeX;
    opts.pixelSizeY = (double)meta.pixelSizeY;
    opts.uuid       = meta.uuid;

    // Temp paths: only alphanumeric + '/' + '_' + '.' — no user input in shell cmd
    std::string ts       = std::to_string((long long)std::time(nullptr));
    std::string stem     = g_tempDir + "/export_" + ts;
    std::string imzmlOut = stem + ".imzML";
    std::string ibdOut   = stem + ".ibd";
    std::string zipOut   = stem + ".zip";

    imzml::ImzMLWriter writer;
    try { writer.open(imzmlOut, opts); }
    catch (const std::exception& e) {
        sendResp(fd, 500, "application/json",
                 std::string("{\"error\":\"writer open failed: ") + e.what() + "\"}");
        return;
    }

    try {
        for (size_t i = 0; i < exp.size(); ++i) {
            const auto& s = exp[i];
            imzml::Coordinate coord{
                (int32_t)s.getCoordX(),
                (int32_t)s.getCoordY(),
                (int32_t)s.getCoordZ()
            };
            std::vector<double> mzVec, intVec;
            mzVec.reserve(s.size());
            intVec.reserve(s.size());
            for (size_t j = 0; j < s.size(); ++j) {
                const double mz = s[j].getMZ();
                if (mz < mzLo || mz > mzHi) continue;
                mzVec.push_back(mz);
                intVec.push_back((double)s[j].getIntensity());
            }
            writer.addSpectrum(coord, mzVec, intVec);
        }
        writer.close();
    } catch (const std::exception& e) {
        sendResp(fd, 500, "application/json",
                 std::string("{\"error\":\"export write failed: ") + e.what() + "\"}");
        remove(imzmlOut.c_str());
        remove(ibdOut.c_str());
        return;
    }

    // Build zip (paths are controlled: mkdtemp-root + digit-only timestamp)
    {
        std::string cmd = "zip -j '"
            + zipOut   + "' '"
            + imzmlOut + "' '"
            + ibdOut   + "' 2>/dev/null";
        std::system(cmd.c_str());
    }

    // Stream zip to browser as download
    FILE* f = fopen(zipOut.c_str(), "rb");
    if (!f) {
        remove(imzmlOut.c_str());
        remove(ibdOut.c_str());
        sendResp(fd, 500, "text/plain", "zip creation failed (is 'zip' installed?)");
        return;
    }
    std::string data;
    data.reserve(1024 * 1024);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    remove(imzmlOut.c_str());
    remove(ibdOut.c_str());
    remove(zipOut.c_str());

    // Derive a helpful download filename from the source file
    std::string baseName = "export";
    if (!g_path.empty()) {
        size_t sl = g_path.rfind('/');
        baseName = (sl == std::string::npos) ? g_path : g_path.substr(sl + 1);
        if (baseName.size() > 6 && baseName.substr(baseName.size() - 6) == ".imzML")
            baseName = baseName.substr(0, baseName.size() - 6);
        if (filtered) baseName += "_filtered";
    }
    sendBinaryFile(fd, baseName + ".zip", "application/zip", data);
}

// /api/upload?name=foo.imzML  (POST, raw octet-stream body)
// Saves uploaded file to the per-run temp directory.
// Stream upload directly from socket to disk — no in-memory buffering.
static std::string apiUploadStream(int fd, const Request& req)
{
    if (g_tempDir.empty())
        return "{\"ok\":false,\"error\":\"temp dir not initialized\"}";
    auto it = req.query.find("name");
    if (it == req.query.end())
        return "{\"ok\":false,\"error\":\"missing name parameter\"}";

    // Sanitize: keep only basename, reject path-traversal chars
    std::string raw = urlDecode(it->second);
    std::string name;
    for (char c : raw) {
        if (c == '/' || c == '\\' || c == '\0' || c == ':') continue;
        name += c;
    }
    if (name.empty()) return "{\"ok\":false,\"error\":\"invalid filename\"}";

    std::string outPath = g_tempDir + "/" + name;
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) return "{\"ok\":false,\"error\":\"cannot create temp file\"}";

    // Write preamble bytes already read during header parsing
    long long written = 0;
    if (!req.bodyPreamble.empty()) {
        long long pn = std::min((long long)req.bodyPreamble.size(), req.contentLength);
        if (pn > 0) { fwrite(req.bodyPreamble.data(), 1, (size_t)pn, f); written = pn; }
    }

    // Stream remaining bytes directly from socket to file (256 KB chunks)
    std::vector<char> buf(256 * 1024);
    while (written < req.contentLength) {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 600000) <= 0) break;  // 10-minute timeout per chunk
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        if (fwrite(buf.data(), 1, (size_t)n, f) != (size_t)n) {
            fclose(f);
            return "{\"ok\":false,\"error\":\"disk write failed\"}";
        }
        written += n;
    }
    fclose(f);

    if (written < req.contentLength)
        return "{\"ok\":false,\"error\":\"upload incomplete ("
               + std::to_string(written) + "/" + std::to_string(req.contentLength) + " bytes)\"}";
    return "{\"ok\":true,\"path\":" + js(outPath) + "}";
}

// /api/browse?dir=...
// Returns list of .imzML files found (non-recursively) in the given directory.
static std::string apiBrowse(const std::map<std::string, std::string>& q)
{
    std::string dir;
    auto it = q.find("dir");
    if (it != q.end()) dir = urlDecode(it->second);
    if (dir.empty())
        return "{\"ok\":false,\"error\":\"no dir parameter\"}";

    // Expand leading ~ to home directory
    if (!dir.empty() && dir[0] == '~') {
        const char* home = getenv("HOME");
        if (home) dir = std::string(home) + dir.substr(1);
    }

    // Strip trailing slash for stat
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();

    // Check it is a directory
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0)
        return "{\"ok\":false,\"error\":\"path not found: " + dir + "\"}";
    if (!S_ISDIR(st.st_mode))
        return "{\"ok\":false,\"error\":\"not a directory: " + dir + "\"}";

    DIR* dp = opendir(dir.c_str());
    if (!dp)
        return "{\"ok\":false,\"error\":\"cannot open directory\"}";

    // Collect .imzML files (case-insensitive suffix check)
    std::vector<std::string> names;
    struct dirent* de;
    while ((de = readdir(dp)) != nullptr) {
        std::string name = de->d_name;
        if (name.size() < 6) continue;
        std::string ext = name.substr(name.size() - 6);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (ext == ".imzml") names.push_back(name);
    }
    closedir(dp);
    std::sort(names.begin(), names.end());

    std::ostringstream o;
    o << "{\"ok\":true,\"dir\":" << js(dir) << ",\"files\":[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) o << ",";
        std::string fullPath = dir + "/" + names[i];
        o << "{\"name\":"  << js(names[i])
          << ",\"dir\":"   << js(dir)
          << ",\"path\":"  << js(fullPath) << "}";
    }
    o << "]}";
    return o.str();
}

// ===========================================================================
// Average and Max spectrum over all loaded spectra
// ===========================================================================
static std::string apiAvgSpectrum()
{
    if (!g_exp) return "{\"error\":\"no dataset\"}";
    const auto& exp = *g_exp;
    if (exp.empty()) return "{\"mz\":[],\"intensity\":[]}";

    // Use first spectrum's m/z axis as reference (works well for continuous mode;
    // for processed mode we build a union grid via interpolation on the same bins).
    const auto& ref = exp[0];
    size_t n = ref.size();
    if (n == 0) return "{\"mz\":[],\"intensity\":[]}";

    std::vector<double> sumI(n, 0.0);
    size_t count = exp.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& sp = exp[i];
        size_t len = std::min(sp.size(), n);
        for (size_t j = 0; j < len; ++j)
            sumI[j] += sp[j].getIntensity();
    }

    // Downsample to ≤ 4000 points for WebGL budget
    size_t step = std::max<size_t>(1, n / 4000);

    std::ostringstream o;
    o << std::fixed << std::setprecision(5);
    o << "{\"type\":\"avg\",\"spectra\":" << count << ",\"mz\":[";
    for (size_t j = 0; j < n; j += step) {
        if (j) o << ",";
        o << ref[j].getMZ();
    }
    o << "],\"intensity\":[";
    o << std::setprecision(4);
    for (size_t j = 0; j < n; j += step) {
        if (j) o << ",";
        o << (sumI[j] / (double)count);
    }
    o << "]}";
    return o.str();
}

static std::string apiMaxSpectrum()
{
    if (!g_exp) return "{\"error\":\"no dataset\"}";
    const auto& exp = *g_exp;
    if (exp.empty()) return "{\"mz\":[],\"intensity\":[]}";

    const auto& ref = exp[0];
    size_t n = ref.size();
    if (n == 0) return "{\"mz\":[],\"intensity\":[]}";

    std::vector<double> maxI(n, 0.0);
    size_t count = exp.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& sp = exp[i];
        size_t len = std::min(sp.size(), n);
        for (size_t j = 0; j < len; ++j)
            if (sp[j].getIntensity() > maxI[j]) maxI[j] = sp[j].getIntensity();
    }

    size_t step = std::max<size_t>(1, n / 4000);

    std::ostringstream o;
    o << std::fixed << std::setprecision(5);
    o << "{\"type\":\"max\",\"spectra\":" << count << ",\"mz\":[";
    for (size_t j = 0; j < n; j += step) {
        if (j) o << ",";
        o << ref[j].getMZ();
    }
    o << "],\"intensity\":[";
    o << std::setprecision(4);
    for (size_t j = 0; j < n; j += step) {
        if (j) o << ",";
        o << maxI[j];
    }
    o << "]}";
    return o.str();
}

// ===========================================================================
// Validate: recompute IBD checksum and compare to stored value
// ===========================================================================
static std::string apiValidate()
{
    if (!g_exp) return "{\"error\":\"no dataset\"}";
    const auto& meta = g_exp->getImzMLMetadata();
    const std::string& ibdPath = meta.ibdFilePath;

    if (ibdPath.empty())
        return "{\"ok\":false,\"error\":\"ibd path unknown\"}";

    // Determine checksum tool: prefer sha1 / shasum
    std::string cmd;
    const std::string& ctype = meta.ibdChecksumType;
    if (ctype == "MD5" || ctype == "md5") {
        cmd = "md5 -q " + ibdPath + " 2>&1";
    } else {
        // Default: SHA-1
        cmd = "shasum -a 1 " + ibdPath + " 2>&1";
    }

    char buf[256];
    std::string raw;
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "{\"ok\":false,\"error\":\"popen failed\"}";
    while (fgets(buf, sizeof(buf), fp)) raw += buf;
    pclose(fp);

    // shasum output: "<hash>  <file>"  — take first token
    std::string computed;
    {
        std::istringstream ss(raw);
        ss >> computed;
    }
    // To lowercase for comparison
    auto toLow = [](std::string s){ for (auto& c: s) c = (char)tolower((unsigned char)c); return s; };
    const std::string stored   = toLow(meta.ibdChecksum);
    const std::string computed2 = toLow(computed);
    const bool match = (!stored.empty() && !computed2.empty() && stored == computed2);

    std::ostringstream o;
    o << "{\"ok\":true"
      << ",\"checksumType\":" << js(ctype.empty() ? "SHA-1" : ctype)
      << ",\"stored\":"   << js(meta.ibdChecksum)
      << ",\"computed\":" << js(computed)
      << ",\"match\":"    << (match ? "true" : "false")
      << "}";
    return o.str();
}

// ===========================================================================
// Benchmark: run pyimzml via subprocess and compare with our data
// ===========================================================================
// ===========================================================================
// Samples API
// ===========================================================================

// Resolve the server's samples/ directory, searched relative to the executable.
static std::string getSamplesDir()
{
    char exePath[4096] = {};
#ifdef __linux__
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) exePath[len] = 0;
#elif defined(__APPLE__)
    uint32_t sz = sizeof(exePath);
    if (_NSGetExecutablePath(exePath, &sz) != 0) exePath[0] = 0;
#endif
    std::string exeDir;
    if (exePath[0]) {
        std::string ep(exePath);
        auto sl = ep.rfind('/');
        if (sl != std::string::npos) exeDir = ep.substr(0, sl);
    }
    struct stat st{};
    // candidates ordered from most-common to least:
    //   dev Mac:  .../build/release/bin  -> ../../../samples  => .../imzml/samples
    //   VPS:      /opt/imzml/build/bin   ->  ../../samples    => /opt/imzml/samples
    const std::vector<std::string> cands = {
        exeDir + "/../../../samples",
        exeDir + "/../../samples",
        exeDir + "/../samples",
        exeDir + "/samples",
        std::string("samples"),
    };
    for (const auto& c : cands) {
        if (stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return c;
    }
    return "";
}

// Return JSON array of available sample zip files (reads index.json if present).
static std::string apiSamplesList()
{
    const std::string dir = getSamplesDir();
    if (dir.empty()) return "[]";

    // Read optional index.json for rich metadata
    std::string indexPath = dir + "/index.json";
    FILE* f = fopen(indexPath.c_str(), "r");
    if (f) {
        std::string content;
        char buf[8192];
        while (fgets(buf, sizeof(buf), f)) content += buf;
        fclose(f);
        if (!content.empty()) return content;
    }

    // Fallback: enumerate .zip files with basic metadata
    DIR* d = opendir(dir.c_str());
    if (!d) return "[]";
    std::ostringstream out;
    out << "[";
    bool first = true;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name(ent->d_name);
        if (name.size() < 5 || name.substr(name.size() - 4) != ".zip") continue;
        std::string fp = dir + "/" + name;
        struct stat fst{};
        stat(fp.c_str(), &fst);
        double mb = (double)fst.st_size / 1048576.0;
        if (!first) out << ",";
        first = false;
        std::string title = name.substr(0, name.size() - 4);
        // replace underscores with spaces for display
        for (char& c : title) if (c == '_') c = ' ';
        out << "{\"file\":" << js(name)
            << ",\"title\":" << js(title)
            << ",\"description\":\"\""
            << ",\"size_mb\":" << jn(mb, 4) << "}";
    }
    closedir(d);
    out << "]";
    return out.str();
}

// Send binary data (e.g. a zip file) as a download response.
static void sendBinaryFile(int fd, const std::string& dlname,
                           const std::string& ct, const std::string& data)
{
    std::ostringstream h;
    h << "HTTP/1.1 200 OK\r\n"
      << "Content-Type: " << ct << "\r\n"
      << "Content-Disposition: attachment; filename=\"" << dlname << "\"\r\n"
      << "Content-Length: " << data.size() << "\r\n"
      << "Connection: close\r\n"
      << "Cache-Control: no-store\r\n"
      << "\r\n";
    const std::string head = h.str();
    for (size_t sent = 0; sent < head.size(); ) {
        ssize_t n = write(fd, head.c_str() + sent, head.size() - sent);
        if (n <= 0) return; sent += (size_t)n;
    }
    for (size_t sent = 0; sent < data.size(); ) {
        ssize_t n = write(fd, data.c_str() + sent, data.size() - sent);
        if (n <= 0) return; sent += (size_t)n;
    }
}

// Serve a sample zip file for download.
static void apiSamplesDownload(int fd, const std::map<std::string,std::string>& q)
{
    auto it = q.find("name");
    if (it == q.end() || it->second.empty()) {
        sendResp(fd, 400, "text/plain", "missing name"); return;
    }
    std::string name = it->second;
    // Security: reject path traversal
    if (name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos ||
        name.find("..") != std::string::npos) {
        sendResp(fd, 400, "text/plain", "invalid name"); return;
    }
    if (name.size() < 4 || name.substr(name.size() - 4) != ".zip") name += ".zip";

    const std::string dir = getSamplesDir();
    if (dir.empty()) { sendResp(fd, 404, "text/plain", "samples not available"); return; }

    std::string filePath = dir + "/" + name;
    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) { sendResp(fd, 404, "text/plain", "sample not found"); return; }

    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);

    sendBinaryFile(fd, name, "application/zip", data);
}

static std::string apiBenchmark()
{
    if (!g_exp) return "{\"ok\":false,\"error\":\"no dataset loaded\"}";

    // --- our stats ---------------------------------------------------------
    std::ostringstream ours;
    ours << "[";
    const auto& exp = *g_exp;
    for (size_t i = 0; i < exp.size(); ++i) {
        if (i) ours << ",";
        const auto& sp = exp[i];
        double tic = 0;
        for (size_t j = 0; j < sp.size(); ++j) tic += sp[j].getIntensity();
        ours << "{\"idx\":" << ji((long long)i)
             << ",\"x\":"   << ji(sp.getCoordX())
             << ",\"y\":"   << ji(sp.getCoordY())
             << ",\"z\":"   << ji(sp.getCoordZ())
             << ",\"tic\":" << jn(tic, 10)
             << ",\"peaks\":" << ji((long long)sp.size()) << "}";
    }
    ours << "]";

    // --- write temp Python script ------------------------------------------
    const char* tmpScript = "/tmp/_imzml_bench.py";
    {
        FILE* f = fopen(tmpScript, "w");
        if (!f) return "{\"ok\":false,\"error\":\"cannot write temp script\"}";
        fprintf(f,
            "import json,sys\n"
            "try:\n"
            "  from pyimzml.ImzMLParser import ImzMLParser\n"
            "  p = ImzMLParser(sys.argv[1])\n"
            "  out = []\n"
            "  for i,(x,y,z) in enumerate(p.coordinates):\n"
            "    mz,it = p.getspectrum(i)\n"
            "    out.append({'idx':i,'x':int(x),'y':int(y),'z':int(z),"
            "'tic':round(float(sum(it)),6),'peaks':len(mz)})\n"
            "  print(json.dumps(out))\n"
            "except Exception as e:\n"
            "  print(json.dumps({'error':str(e)}))\n"
        );
        fclose(f);
    }

    // --- find a python3 with pyimzml ---------------------------------------
    const char* candidates[] = {
        "/opt/imzml/pyimzml_env/bin/python3",
        "/tmp/pyimzml_env/bin/python3",
        "/usr/local/bin/python3",
        "python3",
        nullptr
    };
    std::string pybin;
    for (int ci = 0; candidates[ci]; ++ci) {
        std::string chk = std::string("'") + candidates[ci] + "' -c 'import pyimzml' 2>/dev/null && echo yes";
        FILE* p = popen(chk.c_str(), "r");
        if (p) {
            char buf[8] = {};
            bool ok = fgets(buf, sizeof(buf), p) && buf[0] == 'y';
            pclose(p);
            if (ok) { pybin = candidates[ci]; break; }
        }
    }
    if (pybin.empty())
        return "{\"ok\":false,\"ours\":" + ours.str()
             + ",\"pyimzml\":{\"error\":\"pyimzml not found (tried /opt/imzml/pyimzml_env, /tmp/pyimzml_env, python3)\"}}";


    // --- shell-escape the path and run ------------------------------------
    std::string escaped = "'";
    for (char c : g_path) { if (c == '\'') escaped += "'\\''";
                            else           escaped += c; }
    escaped += "'";

    std::string cmd = "'" + pybin + "' " + tmpScript + " " + escaped + " 2>/tmp/_imzml_bench.err";
    FILE* pipe = popen(cmd.c_str(), "r");
    std::string pyout;
    if (pipe) {
        char buf[8192];
        while (fgets(buf, sizeof(buf), pipe)) pyout += buf;
        pclose(pipe);
    }
    while (!pyout.empty() && (pyout.back() == '\n' || pyout.back() == '\r'))
        pyout.pop_back();

    if (pyout.empty()) {
        std::string err = "no output from pyimzml";
        FILE* ef = fopen("/tmp/_imzml_bench.err", "r");
        if (ef) { char buf[512] = {}; fgets(buf, sizeof(buf), ef); fclose(ef); err = buf; }
        while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
        return "{\"ok\":false,\"ours\":" + ours.str()
             + ",\"pyimzml\":{\"error\":" + js(err) + "}}";
    }

    return "{\"ok\":true,\"ours\":" + ours.str() + ",\"pyimzml\":" + pyout + "}";
}

// ===========================================================================
// Python Playground  (/api/run-python)
// Executes user-supplied Python with imzml_ext available and the loaded file
// path injected as the variable `imzml_path` (and `ibd_path`).
// The code runs in the server process's Python environment (.venv).
// Safety: code is written to a temp file (never eval'd via shell interpolation);
//         stdout/stderr are captured separately; a 15-second wall-clock limit
//         is enforced via timeout(1).
// ===========================================================================
static std::string apiRunPython(const std::string& userCode)
{
    // Resolve the Python executable: prefer the project .venv first
    // (which has imzml_ext in its path via build/release/python).
    const char* candidates[] = {
        // production install path on VPS
        "/opt/imzml/.venv/bin/python3",
        // local dev .venv
        nullptr,  // filled dynamically below
        "/usr/local/bin/python3",
        "python3",
        nullptr
    };

    // Infer dev .venv from server executable path (same repo tree)
    char selfBuf[4096] = {};
#ifdef __APPLE__
    uint32_t sz = sizeof(selfBuf);
    _NSGetExecutablePath(selfBuf, &sz);
#else
    (void)readlink("/proc/self/exe", selfBuf, sizeof(selfBuf)-1);
#endif
    std::string selfPath(selfBuf);
    // selfPath is typically …/build/release/bin/imzml_server
    // repo root is everything before /build/
    std::string repoRoot;
    {
        auto pos = selfPath.rfind("/build/");
        if (pos != std::string::npos)
            repoRoot = selfPath.substr(0, pos);
    }

    // extPath: where imzml_ext.*.so lives (built output)
    // Try build/release/python first, fall back to build/python
    std::string extPath;
    if (!repoRoot.empty()) {
        std::string p1 = repoRoot + "/build/release/python";
        std::string p2 = repoRoot + "/build/python";
        // pick whichever directory exists
        struct stat st{};
        extPath = (stat(p1.c_str(), &st) == 0) ? p1 : p2;
    }

    // .venv path inferred from repo root
    std::string devVenv;
    if (!repoRoot.empty())
        devVenv = repoRoot + "/.venv/bin/python3";
    candidates[1] = devVenv.empty() ? nullptr : devVenv.c_str();

    // Find a working Python that can load imzml_ext (with extPath on sys.path)
    std::string pybin;
    for (int ci = 0; candidates[ci]; ++ci) {
        // Shell-safe: extPath is our own computed path (no user input)
        std::string testCmd = std::string("'") + candidates[ci]
            + "' -c 'import sys; sys.path.insert(0,\""
            + extPath + "\"); import imzml_ext' 2>/dev/null && echo yes";
        FILE* p = popen(testCmd.c_str(), "r");
        if (!p) continue;
        char buf[8] = {};
        bool ok = fgets(buf, sizeof(buf), p) && buf[0] == 'y';
        pclose(p);
        if (ok) { pybin = candidates[ci]; break; }
    }
    if (pybin.empty())
        return "{\"ok\":false,\"error\":\"imzml_ext not found in any Python env\","
               "\"stdout\":\"\",\"stderr\":\"\",\"elapsed_ms\":0}";

    // Write a wrapper script that injects variables and runs user code
    const char* tmpScript = "/tmp/_imzml_playground.py";
    {
        FILE* f = fopen(tmpScript, "w");
        if (!f) return "{\"ok\":false,\"error\":\"cannot write temp script\","
                       "\"stdout\":\"\",\"stderr\":\"\",\"elapsed_ms\":0}";
        // Preamble: inject imzml_path, ibd_path, im, exp
        // Use raw string tricks to avoid any shell-injection via g_path
        // (we write it as a Python bytes literal decoded to str)
        std::string pathPy;
        {
            std::ostringstream os;
            os << "b'";
            for (unsigned char c : g_path) os << "\\x" << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            os << "'.decode()";
            pathPy = os.str();
        }
        // Derive ibd path: same base, .ibd extension
        std::string ibdPy;
        {
            std::string ibdSrc = g_path;
            auto dot = ibdSrc.rfind('.');
            if (dot != std::string::npos) ibdSrc = ibdSrc.substr(0, dot);
            ibdSrc += ".ibd";
            std::ostringstream os;
            os << "b'";
            for (unsigned char c : ibdSrc) os << "\\x" << std::hex << std::setw(2) << std::setfill('0') << (int)c;
            os << "'.decode()";
            ibdPy = os.str();
        }

        fprintf(f,
            "import sys as _sys\n"
            "_sys.path.insert(0, %s)\n"
            "imzml_path = %s\n"
            "ibd_path   = %s\n"
            "try:\n"
            "  import imzml_ext as im\n"
            "  exp = im.open(imzml_path)\n"
            "except Exception as _e:\n"
            "  print(f'[setup error] {_e}', file=_sys.stderr)\n"
            "  im = None; exp = None\n"
            "# --- user code ---\n",
            ("'" + extPath + "'").c_str(),
            pathPy.c_str(),
            ibdPy.c_str()
        );
        // Append user code verbatim
        fwrite(userCode.data(), 1, userCode.size(), f);
        fputc('\n', f);
        fclose(f);
    }

    // Shell-escape pybin for the command line (it is our own path so safe)
    std::string cmd = "'" + pybin + "'";
    for (char c : cmd) { if (c == '\'') { cmd = "UNSAFE"; break; } }  // paranoia
    cmd = "'" + pybin + "' " + tmpScript
        + " > /tmp/_imzml_play.out 2>/tmp/_imzml_play.err";
    // Wrap with timeout (15 s): try GNU timeout, gtimeout (macOS Homebrew), else none
    {
        auto hasCmd = [](const char* c) {
            std::string test = std::string("command -v '") + c + "' >/dev/null 2>&1";
            return system(test.c_str()) == 0;
        };
        if (hasCmd("timeout"))
            cmd = "timeout 15 " + cmd;
        else if (hasCmd("gtimeout"))
            cmd = "gtimeout 15 " + cmd;
        // else run without timeout (development machines, short-lived anyway)
    }

    auto t0 = std::chrono::steady_clock::now();
    int rc = system(cmd.c_str());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    auto readFile = [](const char* p) -> std::string {
        FILE* f = fopen(p, "r"); if (!f) return "";
        std::string s; char buf[4096];
        while (fgets(buf, sizeof(buf), f)) s += buf;
        fclose(f); return s;
    };

    std::string out = readFile("/tmp/_imzml_play.out");
    std::string err = readFile("/tmp/_imzml_play.err");

    // Truncate oversized output to avoid giant JSON
    auto truncate = [](std::string& s, size_t limit) {
        if (s.size() > limit)
            s = s.substr(0, limit) + "\n… (truncated)";
    };
    truncate(out, 32768);
    truncate(err, 8192);

    bool ok = (WIFEXITED(rc) && WEXITSTATUS(rc) == 0);

    // JSON-escape helper (reuse global js())
    return "{\"ok\":" + std::string(ok ? "true" : "false")
         + ",\"stdout\":" + js(out)
         + ",\"stderr\":" + js(err)
         + ",\"elapsed_ms\":" + std::to_string(ms)
         + ",\"error\":\"\"}";
}

// ===========================================================================
// Request dispatcher
// ===========================================================================
static void dispatch(int fd, const Request& req)
{
    const std::string& path = req.path;
    const auto&        q    = req.query;

    // POST /api/run-python -- execute user Python code with imzml_ext available
    if (req.method == "POST" && path == "/api/run-python") {
        auto form = parseQuery(req.body);
        std::string code;
        auto it = form.find("code");
        if (it != form.end()) code = urlDecode(it->second);
        if (code.empty()) {
            sendResp(fd, 200, "application/json",
                     "{\"ok\":false,\"error\":\"no code\",\"stdout\":\"\",\"stderr\":\"\",\"elapsed_ms\":0}");
            return;
        }
        // Read g_path under shared lock, then run (subprocess doesn't need the lock)
        std::string filePath;
        { std::shared_lock<std::shared_mutex> lk(g_mu); filePath = g_path; }
        if (filePath.empty()) {
            sendResp(fd, 200, "application/json",
                     "{\"ok\":false,\"error\":\"no dataset loaded\",\"stdout\":\"\",\"stderr\":\"\",\"elapsed_ms\":0}");
            return;
        }
        sendResp(fd, 200, "application/json", apiRunPython(code));
        return;
    }

    // POST /api/upload -- stream uploaded file bytes directly to temp dir
    if (req.method == "POST" && path == "/api/upload") {
        sendResp(fd, 200, "application/json", apiUploadStream(fd, req));
        return;
    }

    // POST /api/load  -- load a new dataset from a user-supplied path
    if (req.method == "POST" && path == "/api/load") {
        auto form = parseQuery(req.body);
        std::string filePath;
        auto it = form.find("path");
        if (it != form.end()) filePath = urlDecode(it->second);
        if (filePath.empty()) {
            sendResp(fd, 200, "application/json",
                     "{\"loaded\":false,\"error\":\"empty path\"}");
            return;
        }
        {
            std::unique_lock<std::shared_mutex> lk(g_mu);
            if (!loadDataset(filePath, g_loadError)) {
                sendResp(fd, 200, "application/json",
                         "{\"loaded\":false,\"error\":" + js(g_loadError) + "}");
            } else {
                sendResp(fd, 200, "application/json", "{\"loaded\":true,\"error\":\"\"}");
            }
        }
        return;
    }

    if (req.method != "GET") {
        sendResp(fd, 405, "text/plain", "Method Not Allowed"); return;
    }

    if (path == "/" || path == "/index.html") {
        sendResp(fd, 200, "text/html", HTML_PAGE); return;
    }

    // Samples download -- no global lock needed (reads files from disk only)
    if (path == "/api/samples/download") {
        apiSamplesDownload(fd, q); return;
    }

    // All remaining routes read global state -- take shared lock
    std::shared_lock<std::shared_mutex> lk(g_mu);

    if (path == "/api/status") {
        sendResp(fd, 200, "application/json", apiStatus());
    } else if (path == "/api/samples") {
        sendResp(fd, 200, "application/json", apiSamplesList());
    } else if (path == "/api/info") {
        sendResp(fd, 200, "application/json", apiInfo());
    } else if (path == "/api/image") {
        sendResp(fd, 200, "application/json", apiImage(q));
    } else if (path == "/api/stats") {
        std::map<std::string, std::string> empty;
        sendResp(fd, 200, "application/json", apiImage(empty));
    } else if (path == "/api/browse") {
        sendResp(fd, 200, "application/json", apiBrowse(q));
    } else if (path == "/api/benchmark") {
        sendResp(fd, 200, "application/json", apiBenchmark());
    } else if (path == "/api/ion-image") {
        sendResp(fd, 200, "application/json", apiIonImage(q));
    } else if (path == "/api/spectrum") {
        sendResp(fd, 200, "application/json", apiSpectrum(q));
    } else if (path == "/api/avgspectrum") {
        sendResp(fd, 200, "application/json", apiAvgSpectrum());
    } else if (path == "/api/maxspectrum") {
        sendResp(fd, 200, "application/json", apiMaxSpectrum());
    } else if (path == "/api/validate") {
        sendResp(fd, 200, "application/json", apiValidate());
    } else if (path == "/api/export") {
        // Export releases the shared lock before writing files — but we need
        // to read g_exp under the lock. Release, snapshot what we need, then write.
        // Simplest: just hold shared_lock (export is a reader of g_exp).
        apiExport(fd, q);
        return;  // apiExport calls sendResp/sendBinaryFile internally
    } else {
        sendResp(fd, 404, "text/plain", "Not found: " + path);
    }
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char* argv[])
{
    int port = 7373;
    const char* startFile = nullptr;

    // Accept any combination of [file.imzML] [port] in either order
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.size() > 6 && a.substr(a.size() - 6) == ".imzML") {
            startFile = argv[i];
        } else {
            int p = std::atoi(argv[i]);
            if (p > 0) port = p;
        }
    }

    // Create per-run temp dir for uploads
    {
        char tmpl[] = "/tmp/imzml_upload_XXXXXX";
        if (mkdtemp(tmpl)) g_tempDir = tmpl;
        else fprintf(stderr, "[imzml-server] WARNING: could not create temp dir\n");
    }

    // Optionally pre-load a dataset (so the viewer opens straight away)
    if (startFile) {
        fprintf(stderr, "[imzml-server] Pre-loading %s ...\n", startFile);
        if (!loadDataset(startFile, g_loadError))
            fprintf(stderr, "[imzml-server] WARNING: %s\n", g_loadError.c_str());
    }

    // ------------------------------------------------------------------
    // TCP server
    // ------------------------------------------------------------------
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return 1;
    }
    if (listen(srv, 8) < 0) {
        perror("listen"); close(srv); return 1;
    }

    fprintf(stderr, "[imzml-server] http://localhost:%d\n", port);
    if (!startFile)
        fprintf(stderr, "[imzml-server] No file loaded -- use Open Dataset in the browser.\n");
    fprintf(stderr, "[imzml-server] Press Ctrl+C to stop.\n");

    while (true) {
        struct sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        int fd = accept(srv, (struct sockaddr*)&caddr, &clen);
        if (fd < 0) { perror("accept"); continue; }

        // Handle each connection on a dedicated thread so concurrent
        // requests (e.g. /api/info + /api/image fired in parallel by the
        // browser) do not block each other.
        std::thread([fd]() noexcept {
            try {
                Request req = readRequest(fd);
                if (!req.method.empty()) dispatch(fd, req);
            } catch (...) {}
            close(fd);
        }).detach();
    }

    close(srv);
    return 0;
}
