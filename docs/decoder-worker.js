self.importScripts('decoder.js');

var base = new URL('.', self.location.href).href;
var Module = null;
var decodeBusy = false;

createDecoder({
  locateFile: function (path) { return base + path; },
  print: function (text) { console.log('[decoder]', text); },
  printErr: function (text) { console.error('[decoder]', text); }
}).then(function (module) {
  Module = module;
  self.postMessage({ type: 'ready' });
});

function responseFields(request) {
  return {
    imageId: request.imageId || request.id || 0,
    requestId: request.requestId || request.id || 0
  };
}

function postProgress(request, pct, label) {
  var fields = responseFields(request);
  fields.type = 'progress';
  fields.pct = pct;
  fields.label = label;
  self.postMessage(fields);
}

function heapCopy(ptr, length) {
  var bytes = new Uint8Array(length);
  bytes.set(Module.HEAPU8.subarray(ptr, ptr + length));
  return bytes.buffer;
}

function fail(request, code) {
  var message = Module ? Module.UTF8ToString(Module._dec_meta_status()) : '';
  var fields = responseFields(request);
  fields.type = 'error';
  fields.msg = message + ' [' + code + ']';
  self.postMessage(fields);
}

function handleDecode(request) {
  if (decodeBusy) {
    var busy = responseFields(request);
    busy.type = 'error';
    busy.msg = 'Already decoding a file';
    self.postMessage(busy);
    return;
  }
  decodeBusy = true;
  var ptr = 0;
  try {
    var file = new Uint8Array(request.file);
    ptr = Module._malloc(file.length);
    Module.writeArrayToMemory(file, ptr);

    postProgress(request, 10, 'Opening ' + (request.name || 'file') + ' ...');
    var result = Module._dec_decode_phase1(ptr, file.length);
    if (result) { Module._free(ptr); ptr = 0; decodeBusy = false; fail(request, result); return; }

    postProgress(request, 45, 'Unpacking pixel data (' + (request.name || 'file') + ') ...');
    result = Module._dec_decode_phase2();
    if (result) { Module._free(ptr); ptr = 0; decodeBusy = false; fail(request, result); return; }

    postProgress(request, 80, 'Demosaicing ...');
    result = Module._dec_decode_phase3();
    if (result) { Module._free(ptr); ptr = 0; decodeBusy = false; fail(request, result); return; }

    postProgress(request, 98, 'Building preview ...');
    result = Module._dec_decode_phase4();
    Module._free(ptr);
    ptr = 0;
    if (result) { decodeBusy = false; fail(request, result); return; }

    var pw = Module._dec_preview_pw();
    var ph = Module._dec_preview_ph();
    var count = pw * ph * 3;
    postProgress(request, 99, 'Transferring preview ...');
    var preview = heapCopy(Module._dec_preview_ptr(), count * 4);
    var fields = responseFields(request);
    fields.type = 'decoded';
    fields.preview = preview;
    fields.w = Module._dec_w();
    fields.h = Module._dec_h();
    fields.pw = pw;
    fields.ph = ph;
    fields.iso = Module._dec_iso();
    fields.shutter = Module._dec_shutter();
    fields.aperture = Module._dec_aperture();
    fields.focal = Module._dec_focal();
    fields.make = Module.UTF8ToString(Module._dec_meta_make());
    fields.model = Module.UTF8ToString(Module._dec_meta_model());
    fields.lens = Module.UTF8ToString(Module._dec_meta_lens());
    fields.status = Module.UTF8ToString(Module._dec_meta_status());
    self.postMessage(fields, [preview]);
    decodeBusy = false;
  } catch (error) {
    if (ptr) Module._free(ptr);
    decodeBusy = false;
    var fields = responseFields(request);
    fields.type = 'error';
    fields.msg = 'decoder threw: ' + error;
    self.postMessage(fields);
  }
}

function handleProcess(request) {
  var quality = request.quality === undefined ? 1 : request.quality;
  var result = Module._dec_render_preview(
    request.exp, request.black, request.white, request.contrast, request.sat,
    request.temperature, request.tint, request.wbR, request.wbG, request.wbB,
    request.vignette, request.clarity, request.clarityRadius, request.sharpening,
    request.sharpenRadius, request.denoise, request.vibrance, request.ca,
    request.distortion, request.shadows, request.highlights,
    request.cy0, request.cy1, request.cy2, request.cy3, request.cy4,
    quality);
  var size = Module._dec_rendered_preview_size();
  if (!result || !size) {
    var errorFields = responseFields(request);
    errorFields.type = 'preview-error';
    errorFields.msg = 'Preview source is not ready';
    self.postMessage(errorFields);
    return;
  }
  var bytes = heapCopy(Module._dec_rendered_preview_ptr(), size);
  var fields = responseFields(request);
  fields.type = 'preview';
  fields.bytes = bytes;
  fields.w = Module._dec_rendered_preview_width();
  fields.h = Module._dec_rendered_preview_height();
  fields.quality = quality;
  self.postMessage(fields, [bytes]);
}

function handleExport(request) {
  var fields = responseFields(request);
  var result = Module._dec_export_write(
    request.exp, request.black, request.white, request.contrast, request.sat,
    request.temperature, request.tint, request.wbR, request.wbG, request.wbB,
    request.vignette, request.clarity, request.clarityRadius, request.sharpening,
    request.sharpenRadius, request.denoise, request.vibrance, request.ca,
    request.distortion, request.shadows, request.highlights,
    request.cy0, request.cy1, request.cy2, request.cy3, request.cy4,
    request.fullRes ? 1 : 0, request.fmt, request.quality);
  var size = Module._dec_export_size();
  if (!result || !size) {
    fields.type = 'export-error';
    fields.msg = Module.UTF8ToString(Module._dec_export_status());
    self.postMessage(fields);
    return;
  }
  var bytes = heapCopy(Module._dec_export_data(), size);
  fields.type = 'exported';
  fields.bytes = bytes;
  fields.fmt = request.fmt;
  fields.status = Module.UTF8ToString(Module._dec_export_status());
  self.postMessage(fields, [bytes]);
}

self.onmessage = function (event) {
  var request = event.data || {};
  if (request.cmd === 'decode') {
    if (!Module) {
      var fields = responseFields(request);
      fields.type = 'error';
      fields.msg = 'decoder not ready';
      self.postMessage(fields);
      return;
    }
    handleDecode(request);
  } else if (request.cmd === 'process') {
    if (!Module) {
      var processFields = responseFields(request);
      processFields.type = 'preview-error';
      processFields.msg = 'decoder not ready';
      self.postMessage(processFields);
      return;
    }
    handleProcess(request);
  } else if (request.cmd === 'export') {
    if (!Module) {
      var exportFields = responseFields(request);
      exportFields.type = 'export-error';
      exportFields.msg = 'decoder not ready';
      self.postMessage(exportFields);
      return;
    }
    handleExport(request);
  }
};
