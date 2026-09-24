// RawTherapee WASM decoder worker. Loads the MODULARIZE build of decoder.cpp
// (decoder.js) and drives decode/export on this thread so the UI stays live.
self.importScripts('decoder.js');

var base = new URL('.', self.location.href).href;

var Module = null;
var decodeBusy = false;
var exportSeq = 0;

createDecoder({
  locateFile: function (path) { return base + path; },
  print: function (t) { console.log('[decoder]', t); },
  printErr: function (t) { console.error('[decoder]', t); }
}).then(function (m) {
  Module = m;
  self.postMessage({ type: 'ready' });
});

function postProgress(pct, label) {
  self.postMessage({ type: 'progress', pct: pct, label: label });
}

function heapCopy(ptr, n) {
  var b = new Uint8Array(n);
  b.set(Module.HEAPU8.subarray(ptr, ptr + n));
  return b.buffer;
}

function fail(id, code) {
  var msg = Module ? Module.UTF8ToString(Module._dec_meta_status()) : '';
  self.postMessage({ type: 'error', id: id, msg: msg + ' [' + code + ']' });
}

function handleDecode(d) {
  if (decodeBusy) {
    self.postMessage({ type: 'error', id: d.id, msg: 'Already decoding a file' });
    return;
  }
  decodeBusy = true;
  try {
    var file = new Uint8Array(d.file);
    var ptr = Module._malloc(file.length);
    Module.writeArrayToMemory(file, ptr);

    postProgress(10, 'Opening ' + (d.name || 'file') + ' ...');
    var r = Module._dec_decode_phase1(ptr, file.length);
    if (r) { Module._free(ptr); decodeBusy = false; fail(d.id, r); return; }

    postProgress(45, 'Unpacking pixel data (' + (d.name || 'file') + ') ...');
    r = Module._dec_decode_phase2();
    if (r) { Module._free(ptr); decodeBusy = false; fail(d.id, r); return; }

    postProgress(80, 'Demosaicing ...');
    r = Module._dec_decode_phase3();
    if (r) { Module._free(ptr); decodeBusy = false; fail(d.id, r); return; }

    postProgress(98, 'Building preview ...');
    r = Module._dec_decode_phase4();
    Module._free(ptr);
    if (r) { decodeBusy = false; fail(d.id, r); return; }

    var pw = Module._dec_preview_pw();
    var ph = Module._dec_preview_ph();
    var nf = pw * ph * 3;
    postProgress(99, 'Transferring preview ...');
    var buf = heapCopy(Module._dec_preview_ptr(), nf * 4);

    self.postMessage({
      type: 'decoded',
      id: d.id,
      preview: buf,
      w: Module._dec_w(),
      h: Module._dec_h(),
      pw: pw,
      ph: ph,
      iso: Module._dec_iso(),
      shutter: Module._dec_shutter(),
      aperture: Module._dec_aperture(),
      focal: Module._dec_focal(),
      make: Module.UTF8ToString(Module._dec_meta_make()),
      model: Module.UTF8ToString(Module._dec_meta_model()),
      lens: Module.UTF8ToString(Module._dec_meta_lens()),
      status: Module.UTF8ToString(Module._dec_meta_status())
    }, [buf]);
    decodeBusy = false;
  } catch (err) {
    decodeBusy = false;
    self.postMessage({ type: 'error', id: d.id, msg: 'decoder threw: ' + err });
  }
}

function handleExport(d) {
  var id = d.id;
  var r = Module._dec_export_write(
    d.exp, d.black, d.white, d.contrast, d.sat,
    d.wbR, d.wbG, d.wbB, d.vignette,
    d.clarity, d.clarityRadius, d.shadows, d.highlights,
    d.cy0, d.cy1, d.cy2, d.cy3, d.cy4,
    d.fmt, d.quality);
  var sz = Module._dec_export_size();
  if (!r || !sz) {
    var status = Module.UTF8ToString(Module._dec_export_status());
    self.postMessage({ type: 'export-error', id: id, msg: status });
    return;
  }
  var buf = heapCopy(Module._dec_export_data(), sz);
  var status = Module.UTF8ToString(Module._dec_export_status());
  self.postMessage({
    type: 'exported',
    id: id,
    bytes: buf,
    fmt: d.fmt,
    status: status
  }, [buf]);
}

self.onmessage = function (e) {
  var d = e.data || {};
  if (d.cmd === 'decode') {
    if (!Module) { self.postMessage({ type: 'error', id: d.id, msg: 'decoder not ready' }); return; }
    handleDecode(d);
  } else if (d.cmd === 'export') {
    if (!Module) { self.postMessage({ type: 'export-error', id: d.id, msg: 'decoder not ready' }); return; }
    handleExport(d);
  }
};