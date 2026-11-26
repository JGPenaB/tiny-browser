// Web APIs that are written in JavaScript on top of the native DOM and network primitives. This text is run
// once for every page that has scripts, before any of the page's own code.
#pragma once

namespace tb {

static const char kScriptPrelude[] = R"JS((function () {
'use strict';
var G = globalThis;
var net = G.__net, netSync = G.__netSync, netAbort = G.__netAbort, parseUrl = G.__url;
delete G.__net; delete G.__netSync; delete G.__netAbort; delete G.__url;

class DOMException extends Error {
  constructor(message, name) { super(message); this.name = name || 'Error'; this.code = 0; }
}

// ---- event targets that are not DOM nodes (XMLHttpRequest, AbortSignal, MessagePort...)
class EventTarget {
  addEventListener(type, fn) { if (!fn) return; var m = this._ls || (this._ls = {}); (m[type] || (m[type] = [])).push(fn); }
  removeEventListener(type, fn) { var a = this._ls && this._ls[type]; if (a) { var i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); } }
  dispatchEvent(e) {
    if (!e.target) e.target = this;
    e.currentTarget = this;
    var h = this['on' + e.type];
    if (typeof h === 'function') { try { h.call(this, e); } catch (err) { console.error(err); } }
    var a = this._ls && this._ls[e.type];
    if (a) for (var f of a.slice()) { try { typeof f === 'function' ? f.call(this, e) : f.handleEvent(e); } catch (err) { console.error(err); } }
    return !e.defaultPrevented;
  }
}
Object.setPrototypeOf(Node.prototype, EventTarget.prototype);

class AbortSignal extends EventTarget {
  constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
  throwIfAborted() { if (this.aborted) throw this.reason; }
  static abort(reason) { var s = new AbortSignal(); s.aborted = true; s.reason = reason; return s; }
  static timeout(ms) { var c = new AbortController(); setTimeout(function () { c.abort(new DOMException('The operation timed out.', 'TimeoutError')); }, ms); return c.signal; }
}
class AbortController {
  constructor() { this.signal = new AbortSignal(); }
  abort(reason) {
    var s = this.signal; if (s.aborted) return;
    s.aborted = true; s.reason = reason === undefined ? new DOMException('The operation was aborted.', 'AbortError') : reason;
    s.dispatchEvent(new Event('abort'));
  }
}

// ---- URL and URLSearchParams
function enc(s) { return encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, function (c) { return '%' + c.charCodeAt(0).toString(16).toUpperCase(); }); }
function dec(s) { try { return decodeURIComponent(s.replace(/\+/g, ' ')); } catch (e) { return s; } }
class URLSearchParams {
  constructor(init) {
    this._l = []; this._url = null;
    if (init == null) return;
    if (typeof init === 'string') {
      if (init[0] === '?') init = init.slice(1);
      for (var part of init.split('&')) { if (!part) continue; var i = part.indexOf('='); this._l.push(i < 0 ? [dec(part), ''] : [dec(part.slice(0, i)), dec(part.slice(i + 1))]); }
    } else if (init instanceof URLSearchParams) this._l = init._l.map(function (p) { return [p[0], p[1]]; });
    else if (typeof init[Symbol.iterator] === 'function') for (var p of init) this._l.push([String(p[0]), String(p[1])]);
    else for (var k of Object.keys(init)) this._l.push([k, String(init[k])]);
  }
  _sync() { if (this._url) this._url._setSearch(this.toString()); }
  append(k, v) { this._l.push([String(k), String(v)]); this._sync(); }
  delete(k) { k = String(k); this._l = this._l.filter(function (p) { return p[0] !== k; }); this._sync(); }
  get(k) { k = String(k); var p = this._l.find(function (p) { return p[0] === k; }); return p ? p[1] : null; }
  getAll(k) { k = String(k); return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); }
  has(k) { k = String(k); return this._l.some(function (p) { return p[0] === k; }); }
  set(k, v) {
    k = String(k); var i = this._l.findIndex(function (p) { return p[0] === k; });
    if (i < 0) this._l.push([k, String(v)]);
    else { this._l[i][1] = String(v); this._l = this._l.filter(function (p, j) { return j <= i || p[0] !== k; }); }
    this._sync();
  }
  sort() { this._l.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); this._sync(); }
  forEach(f, t) { for (var p of this._l.slice()) f.call(t, p[1], p[0], this); }
  keys() { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); }
  values() { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); }
  entries() { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
  get size() { return this._l.length; }
  toString() { return this._l.map(function (p) { return enc(p[0]) + '=' + enc(p[1]); }).join('&'); }
}
// parts: 0 href, 1 protocol, 2 host, 3 hostname, 4 port, 5 pathname, 6 search, 7 hash, 8 origin
class URL {
  constructor(url, base) {
    var p = parseUrl(String(url), base === undefined ? undefined : String(base));
    if (!p) throw new TypeError("Failed to construct 'URL': Invalid URL");
    this._p = p; this._sp = null;
  }
  _re(href) { var p = parseUrl(href); if (p) { this._p = p; } }
  _base() { return this._p[1] + '//' + this._p[2]; }
  _setSearch(q) { this._re(this._base() + this._p[5] + (q ? '?' + q : '') + this._p[7]); }
  get href() { return this._p[0]; }
  set href(v) { var p = parseUrl(String(v)); if (!p) throw new TypeError('Invalid URL'); this._p = p; this._sp = null; }
  get protocol() { return this._p[1]; }
  set protocol(v) { v = String(v).replace(/:$/, ''); this._re(v + '://' + this._p[2] + this._p[5] + this._p[6] + this._p[7]); }
  get host() { return this._p[2]; }
  set host(v) { this._re(this._p[1] + '//' + v + this._p[5] + this._p[6] + this._p[7]); }
  get hostname() { return this._p[3]; }
  set hostname(v) { this._re(this._p[1] + '//' + v + (this._p[4] ? ':' + this._p[4] : '') + this._p[5] + this._p[6] + this._p[7]); }
  get port() { return this._p[4]; }
  set port(v) { this._re(this._p[1] + '//' + this._p[3] + (v === '' ? '' : ':' + v) + this._p[5] + this._p[6] + this._p[7]); }
  get pathname() { return this._p[5]; }
  set pathname(v) { v = String(v); this._re(this._base() + (v[0] === '/' ? v : '/' + v) + this._p[6] + this._p[7]); }
  get search() { return this._p[6]; }
  set search(v) { v = String(v); this._setSearch(v[0] === '?' ? v.slice(1) : v); this._sp = null; }
  get hash() { return this._p[7]; }
  set hash(v) { v = String(v); this._re(this._base() + this._p[5] + this._p[6] + (v && v[0] !== '#' ? '#' + v : v)); }
  get origin() { return this._p[8]; }
  get username() { return ''; }
  get password() { return ''; }
  get searchParams() { if (!this._sp) { this._sp = new URLSearchParams(this._p[6]); this._sp._url = this; } return this._sp; }
  toString() { return this._p[0]; }
  toJSON() { return this._p[0]; }
  static canParse(u, b) { return !!parseUrl(String(u), b === undefined ? undefined : String(b)); }
  static createObjectURL() { return 'blob:null/' + crypto.randomUUID(); }
  static revokeObjectURL() {}
}

// ---- Blob and FormData (enough for requests and feature tests)
class Blob {
  constructor(parts, opts) {
    this.type = (opts && opts.type) || '';
    var chunks = [], size = 0;
    for (var p of parts || []) {
      var u8;
      if (p instanceof Blob) u8 = p._d;
      else if (p instanceof ArrayBuffer) u8 = new Uint8Array(p);
      else if (ArrayBuffer.isView(p)) u8 = new Uint8Array(p.buffer, p.byteOffset, p.byteLength);
      else u8 = new TextEncoder().encode(String(p));
      chunks.push(u8); size += u8.length;
    }
    var d = new Uint8Array(size), o = 0;
    for (var c of chunks) { d.set(c, o); o += c.length; }
    this._d = d; this.size = size;
  }
  text() { return Promise.resolve(new TextDecoder().decode(this._d)); }
  arrayBuffer() { return Promise.resolve(this._d.slice().buffer); }
  slice(a, b, type) { return new Blob([this._d.slice(a, b)], { type: type }); }
}
class File extends Blob {
  constructor(parts, name, opts) { super(parts, opts); this.name = String(name); this.lastModified = Date.now(); }
}
class FormData {
  constructor(form) {
    this._l = [];
    if (form && form.querySelectorAll) for (var el of form.querySelectorAll('input, select, textarea')) {
      var n = el.name, t = (el.type || '').toLowerCase();
      if (!n || el.disabled || t === 'submit' || t === 'button' || t === 'file' || t === 'image' || t === 'reset') continue;
      if ((t === 'checkbox' || t === 'radio') && !el.checked) continue;
      this._l.push([n, t === 'checkbox' || t === 'radio' ? (el.getAttribute('value') === null ? 'on' : el.value) : String(el.value)]);
    }
  }
  append(k, v) { this._l.push([String(k), v instanceof Blob ? v : String(v)]); }
  delete(k) { k = String(k); this._l = this._l.filter(function (p) { return p[0] !== k; }); }
  get(k) { k = String(k); var p = this._l.find(function (p) { return p[0] === k; }); return p ? p[1] : null; }
  getAll(k) { k = String(k); return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); }
  has(k) { k = String(k); return this._l.some(function (p) { return p[0] === k; }); }
  set(k, v) { this.delete(k); this.append(k, v); }
  forEach(f, t) { for (var p of this._l.slice()) f.call(t, p[1], p[0], this); }
  keys() { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); }
  values() { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); }
  entries() { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
}
)JS"
R"JS(
// ---- fetch, Headers, Request, Response, XMLHttpRequest
class Headers {
  constructor(init) {
    this._m = new Map();
    if (!init) return;
    if (init instanceof Headers) { var self = this; init.forEach(function (v, k) { self.append(k, v); }); }
    else if (Array.isArray(init)) for (var p of init) this.append(p[0], p[1]);
    else for (var k of Object.keys(init)) this.append(k, init[k]);
  }
  append(k, v) { k = String(k).toLowerCase(); v = String(v); this._m.set(k, this._m.has(k) ? this._m.get(k) + ', ' + v : v); }
  set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
  get(k) { var v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
  has(k) { return this._m.has(String(k).toLowerCase()); }
  delete(k) { this._m.delete(String(k).toLowerCase()); }
  forEach(f, t) { var self = this; this._m.forEach(function (v, k) { f.call(t, v, k, self); }); }
  keys() { return this._m.keys(); }
  values() { return this._m.values(); }
  entries() { return this._m.entries(); }
  [Symbol.iterator]() { return this._m.entries(); }
}
function parseHeaders(raw) {
  var h = new Headers();
  for (var line of String(raw || '').split('\r\n')) { var i = line.indexOf(':'); if (i > 0) h.append(line.slice(0, i).trim(), line.slice(i + 1).trim()); }
  return h;
}
// Request body -> [what to send, default Content-Type]
function bodyOf(b) {
  if (b == null) return [null, null];
  if (typeof b === 'string') return [b, 'text/plain;charset=UTF-8'];
  if (b instanceof URLSearchParams) return [b.toString(), 'application/x-www-form-urlencoded;charset=UTF-8'];
  if (b instanceof FormData) return [new URLSearchParams(b._l.filter(function (p) { return typeof p[1] === 'string'; })).toString(), 'application/x-www-form-urlencoded;charset=UTF-8'];
  if (b instanceof Blob) return [b._d, b.type || null];
  if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) return [b, null];
  return [String(b), 'text/plain;charset=UTF-8'];
}
class Response {
  constructor(body, init) {
    init = init || {};
    this.status = init.status === undefined ? 200 : init.status;
    this.statusText = init.statusText || '';
    this.headers = new Headers(init.headers);
    this.ok = this.status >= 200 && this.status < 300;
    this.url = init.url || ''; this.redirected = false; this.type = 'basic'; this.bodyUsed = false; this.body = null;
    this._b = body == null ? '' : (body instanceof Blob ? body._d : body);
  }
  _take() { if (this.bodyUsed) return Promise.reject(new TypeError('body stream already read')); this.bodyUsed = true; return Promise.resolve(this._b); }
  text() { return this._take().then(function (b) { return typeof b === 'string' ? b : new TextDecoder().decode(b); }); }
  json() { return this.text().then(JSON.parse); }
  arrayBuffer() { return this._take().then(function (b) { return typeof b === 'string' ? new TextEncoder().encode(b).buffer : (b instanceof ArrayBuffer ? b : b.slice().buffer); }); }
  blob() { var type = this.headers.get('content-type') || ''; return this._take().then(function (b) { return new Blob([b], { type: type }); }); }
  formData() { return this.text().then(function (t) { var f = new FormData(); new URLSearchParams(t).forEach(function (v, k) { f.append(k, v); }); return f; }); }
  clone() { var r = new Response(this._b, { status: this.status, statusText: this.statusText, headers: this.headers, url: this.url }); r.redirected = this.redirected; return r; }
  static error() { var r = new Response(null, { status: 0 }); r.type = 'error'; return r; }
  static json(data, init) { init = init || {}; var h = new Headers(init.headers); if (!h.has('content-type')) h.set('content-type', 'application/json'); return new Response(JSON.stringify(data), { status: init.status, statusText: init.statusText, headers: h }); }
  static redirect(url, status) { return new Response(null, { status: status || 302, headers: { location: String(url) } }); }
}
class Request {
  constructor(input, init) {
    init = init || {};
    if (input instanceof Request) { this.url = input.url; this.method = input.method; this.headers = new Headers(input.headers); this._b = input._b; this.credentials = input.credentials; this.signal = input.signal; }
    else { this.url = new URL(String(input), document.baseURI).href; this.method = 'GET'; this.headers = new Headers(); this._b = null; this.credentials = 'same-origin'; this.signal = null; }
    if (init.method) this.method = String(init.method).toUpperCase();
    if (init.headers) this.headers = new Headers(init.headers);
    if (init.body !== undefined) this._b = init.body;
    if (init.credentials) this.credentials = init.credentials;
    if (init.signal) this.signal = init.signal;
    this.mode = init.mode || 'cors'; this.cache = init.cache || 'default'; this.redirect = init.redirect || 'follow'; this.referrer = 'about:client';
  }
  clone() { return new Request(this); }
  text() { return Promise.resolve(this._b == null ? '' : String(this._b)); }
  json() { return this.text().then(JSON.parse); }
}
function fetch(input, init) {
  return new Promise(function (resolve, reject) {
    var req;
    try { req = new Request(input, init); } catch (e) { reject(e); return; }
    var sig = req.signal;
    if (sig && sig.aborted) { reject(sig.reason); return; }
    var pair = bodyOf(req._b), hs = [], hasType = false;
    req.headers.forEach(function (v, k) { hs.push([k, v]); if (k === 'content-type') hasType = true; });
    if (!hasType && pair[1]) hs.push(['content-type', pair[1]]);
    var id = net(req.method, req.url, hs, pair[0], (req.credentials === 'include' ? 1 : 0) | 2, function (err, status, statusText, url, raw, data) {
      if (sig && sig.aborted) return;
      if (err) { reject(new TypeError('Failed to fetch: ' + err)); return; }
      var r = new Response(data, { status: status, statusText: statusText, headers: parseHeaders(raw), url: url });
      r.redirected = url !== req.url;
      resolve(r);
    });
    if (sig) sig.addEventListener('abort', function () { netAbort(id); reject(sig.reason); });
  });
}
class XMLHttpRequest extends EventTarget {
  constructor() {
    super();
    this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = '';
    this.responseURL = ''; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget();
    this._h = []; this._rh = null; this._id = 0; this._async = true;
  }
  open(method, url, async) { this._m = String(method).toUpperCase(); this._u = new URL(String(url), document.baseURI).href; this._async = async !== false; this._h = []; this._rh = null; this.status = 0; this._set(1); }
  setRequestHeader(k, v) { this._h.push([String(k), String(v)]); }
  getResponseHeader(k) { return this._rh ? this._rh.get(k) : null; }
  getAllResponseHeaders() { if (!this._rh) return ''; var s = ''; this._rh.forEach(function (v, k) { s += k + ': ' + v + '\r\n'; }); return s; }
  overrideMimeType() {}
  _set(state) { this.readyState = state; this.dispatchEvent(new Event('readystatechange')); }
  _fire(type) { var e = new Event(type); e.loaded = this.responseText.length; e.total = e.loaded; e.lengthComputable = true; this.dispatchEvent(e); }
  _done(err, status, statusText, url, raw, data) {
    this._id = 0;
    if (err) { this.status = 0; this._set(4); this._fire('error'); this._fire('loadend'); return; }
    this.status = status; this.statusText = statusText; this.responseURL = url; this._rh = parseHeaders(raw);
    var t = this.responseType;
    if (t === 'arraybuffer') this.response = data;
    else if (t === 'blob') this.response = new Blob([data], { type: this._rh.get('content-type') || '' });
    else {
      var text = typeof data === 'string' ? data : new TextDecoder().decode(data);
      this.responseText = text;
      if (t === 'json') { try { this.response = JSON.parse(text); } catch (e) { this.response = null; } } else this.response = text;
    }
    this._set(2); this._set(3); this._set(4); this._fire('load'); this._fire('loadend');
  }
  send(body) {
    var pair = bodyOf(body), hs = this._h.slice(), self = this;
    if (pair[1] && !hs.some(function (h) { return h[0].toLowerCase() === 'content-type'; })) hs.push(['content-type', pair[1]]);
    var flags = (this.withCredentials ? 1 : 0) | (this.responseType === 'arraybuffer' || this.responseType === 'blob' ? 2 : 0);
    if (!this._async) { this._done.apply(this, netSync(this._m, this._u, hs, pair[0], flags)); return; }
    this._id = net(this._m, this._u, hs, pair[0], flags, function () { self._done.apply(self, arguments); });
    if (this.timeout > 0) setTimeout(function () { if (!self._id) return; netAbort(self._id); self._id = 0; self.status = 0; self._set(4); self._fire('timeout'); self._fire('loadend'); }, this.timeout);
  }
  abort() { if (this._id) { netAbort(this._id); this._id = 0; this._set(4); this._fire('abort'); this._fire('loadend'); } this.readyState = 0; }
}
['UNSENT', 'OPENED', 'HEADERS_RECEIVED', 'LOADING', 'DONE'].forEach(function (n, i) { XMLHttpRequest[n] = XMLHttpRequest.prototype[n] = i; });

// ---- TreeWalker / NodeIterator
var NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_FRAGMENT: 1024 };
class TreeWalker {
  constructor(root, show, filter) { this.root = root; this.whatToShow = show == null ? 0xFFFFFFFF : show >>> 0; this.filter = filter || null; this.currentNode = root; }
  _accept(n) {
    if (!((1 << (n.nodeType - 1)) & this.whatToShow)) return 3;
    var f = this.filter;
    return !f ? 1 : typeof f === 'function' ? f(n) : f.acceptNode(n);
  }
  nextNode() {
    var n = this.currentNode, r = 1;
    for (;;) {
      while (r !== 2 && n.firstChild) { n = n.firstChild; r = this._accept(n); if (r === 1) return this.currentNode = n; }
      var s = null;
      for (var t = n; t; t = t.parentNode) { if (t === this.root) return null; s = t.nextSibling; if (s) break; }
      if (!s) return null;
      n = s; r = this._accept(n);
      if (r === 1) return this.currentNode = n;
    }
  }
  previousNode() {
    var n = this.currentNode;
    while (n !== this.root) {
      var s = n.previousSibling;
      while (s) {
        n = s; var r = this._accept(n);
        while (r !== 2 && n.lastChild) { n = n.lastChild; r = this._accept(n); }
        if (r === 1) return this.currentNode = n;
        s = n.previousSibling;
      }
      if (n === this.root || !n.parentNode) return null;
      n = n.parentNode;
      if (this._accept(n) === 1) return this.currentNode = n;
    }
    return null;
  }
  parentNode() { var n = this.currentNode; while (n && n !== this.root) { n = n.parentNode; if (n && this._accept(n) === 1) return this.currentNode = n; } return null; }
  firstChild() { for (var c = this.currentNode.firstChild; c; c = c.nextSibling) if (this._accept(c) === 1) return this.currentNode = c; return null; }
  lastChild() { for (var c = this.currentNode.lastChild; c; c = c.previousSibling) if (this._accept(c) === 1) return this.currentNode = c; return null; }
  nextSibling() { for (var c = this.currentNode.nextSibling; c; c = c.nextSibling) if (this._accept(c) === 1) return this.currentNode = c; return null; }
  previousSibling() { for (var c = this.currentNode.previousSibling; c; c = c.previousSibling) if (this._accept(c) === 1) return this.currentNode = c; return null; }
}
Document.prototype.createTreeWalker = function (root, show, filter) { return new TreeWalker(root, show, filter); };
Document.prototype.createNodeIterator = function (root, show, filter) { var w = new TreeWalker(root, show, filter); w.referenceNode = root; w.detach = function () {}; return w; };
)JS"
R"JS(
// ---- smaller pieces pages expect to find
function zeroRect() { return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 }; }
Document.prototype.createRange = function () {
  return {
    setStart: function () {}, setEnd: function () {}, setStartBefore: function () {}, setEndAfter: function () {}, selectNode: function () {}, selectNodeContents: function () {},
    collapse: function () {}, detach: function () {}, cloneRange: function () { return this; }, getBoundingClientRect: zeroRect, getClientRects: function () { return []; },
    createContextualFragment: function (html) { var t = document.createElement('div'); t.innerHTML = html; var f = document.createDocumentFragment(); while (t.firstChild) f.appendChild(t.firstChild); return f; },
    commonAncestorContainer: document, collapsed: true, startOffset: 0, endOffset: 0
  };
};
G.getSelection = Document.prototype.getSelection = function () {
  return { rangeCount: 0, isCollapsed: true, anchorNode: null, focusNode: null, type: 'None', toString: function () { return ''; }, removeAllRanges: function () {}, addRange: function () {}, collapse: function () {}, getRangeAt: function () { return document.createRange(); } };
};
Document.prototype.elementFromPoint = function () { return document.body; };
Document.prototype.elementsFromPoint = function () { return [document.body]; };
Document.prototype.execCommand = function () { return false; };
Document.prototype.queryCommandSupported = function () { return false; };
Document.prototype.adoptNode = Document.prototype.importNode = function (n, deep) { return deep === undefined ? n : n.cloneNode(deep); };
function inertDocument(title) {
  var html = document.createElement('html'), head = document.createElement('head'), body = document.createElement('body');
  html.appendChild(head); html.appendChild(body);
  if (title !== undefined && title !== null) { var t = document.createElement('title'); t.textContent = String(title); head.appendChild(t); }
  var d = {
    nodeType: 9, nodeName: '#document', documentElement: html, head: head, body: body, defaultView: null, readyState: 'complete', compatMode: 'CSS1Compat',
    createElement: function (n) { return document.createElement(n); }, createElementNS: function (ns, n) { return document.createElementNS(ns, n); },
    createTextNode: function (t) { return document.createTextNode(t); }, createComment: function (t) { return document.createComment(t); },
    createDocumentFragment: function () { return document.createDocumentFragment(); }, createEvent: function (t) { return document.createEvent(t); },
    createRange: function () { return document.createRange(); }, createTreeWalker: function (r, w, f) { return document.createTreeWalker(r, w, f); },
    importNode: function (n, deep) { return n.cloneNode(!!deep); }, adoptNode: function (n) { return n; },
    querySelector: function (q) { return html.querySelector(q); }, querySelectorAll: function (q) { return html.querySelectorAll(q); },
    getElementById: function (id) { return html.querySelector('#' + CSS.escape(String(id))); },
    getElementsByTagName: function (n) { return html.getElementsByTagName(n); }, getElementsByClassName: function (n) { return html.getElementsByClassName(n); },
    addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return true; },
    open: function () { body.innerHTML = ''; return d; }, close: function () {}, write: function (h) { body.insertAdjacentHTML('beforeend', String(h)); },
    appendChild: function (n) { return html.appendChild(n); }, contains: function (n) { return html.contains(n); }
  };
  Object.defineProperty(d, 'title', { get: function () { var t = head.querySelector('title'); return t ? t.textContent : ''; }, set: function (v) { var t = head.querySelector('title') || head.appendChild(document.createElement('title')); t.textContent = v; } });
  d.implementation = { hasFeature: function () { return true; }, createHTMLDocument: inertDocument, createDocument: function () { return inertDocument(); } };
  return d;
}
Object.defineProperty(Document.prototype, 'implementation', { get: function () { return { hasFeature: function () { return true; }, createHTMLDocument: inertDocument, createDocument: function () { return inertDocument(); }, createDocumentType: function () { return null; } }; }, configurable: true });
Object.defineProperty(Document.prototype, 'styleSheets', { get: function () { return []; }, configurable: true });
Object.defineProperty(Document.prototype, 'fonts', { get: function () { return { ready: Promise.resolve(), status: 'loaded', check: function () { return true; }, load: function () { return Promise.resolve([]); }, addEventListener: function () {}, removeEventListener: function () {} }; }, configurable: true });
Object.defineProperty(Document.prototype, 'scrollingElement', { get: function () { return document.documentElement; }, configurable: true });
HTMLElement.prototype.attachShadow = undefined;
HTMLElement.prototype.animate = function () { return { cancel: function () {}, finish: function () {}, play: function () {}, pause: function () {}, finished: Promise.resolve(), addEventListener: function () {}, removeEventListener: function () {} }; };
HTMLElement.prototype.getAnimations = function () { return []; };
HTMLElement.prototype.setPointerCapture = HTMLElement.prototype.releasePointerCapture = HTMLElement.prototype.scrollTo = HTMLElement.prototype.scroll = HTMLElement.prototype.scrollBy = HTMLElement.prototype.requestFullscreen = function () {};
Object.defineProperty(HTMLElement.prototype, 'isContentEditable', { get: function () { return false; }, configurable: true });
Object.defineProperty(HTMLElement.prototype, 'shadowRoot', { get: function () { return null; }, configurable: true });
)JS"
R"JS(
function prop(ctor, name, get, set) { Object.defineProperty(ctor.prototype, name, { get: get, set: set, configurable: true, enumerable: true }); }
function textProp(ctor) { prop(ctor, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; }); }
[HTMLScriptElement, HTMLAnchorElement, HTMLOptionElement, HTMLTitleElement].forEach(textProp);
HTMLCanvasElement.prototype.getContext = function () { return null; };
HTMLCanvasElement.prototype.toDataURL = function () { return 'data:,'; };
HTMLCanvasElement.prototype.toBlob = function (cb) { setTimeout(function () { cb(null); }, 0); };
HTMLMediaElement.prototype.play = function () { return Promise.reject(new DOMException('Media playback is not supported yet', 'NotSupportedError')); };
HTMLMediaElement.prototype.pause = HTMLMediaElement.prototype.load = function () {};
HTMLMediaElement.prototype.canPlayType = function () { return ''; };
HTMLMediaElement.prototype.paused = true; HTMLMediaElement.prototype.ended = false; HTMLMediaElement.prototype.currentTime = 0; HTMLMediaElement.prototype.duration = NaN;
HTMLMediaElement.prototype.volume = 1; HTMLMediaElement.prototype.readyState = 0; HTMLMediaElement.prototype.networkState = 0; HTMLMediaElement.prototype.playbackRate = 1;
var F = HTMLFormElement.prototype;
prop(HTMLFormElement, 'elements', function () { return this.querySelectorAll('input, select, textarea, button'); });
prop(HTMLFormElement, 'length', function () { return this.elements.length; });
F.submit = function () { var b = this.querySelector('button:not([type=button]), input[type=submit]'); if (b) b.click(); };
F.requestSubmit = F.submit;
F.reset = function () {};
[HTMLFormElement, HTMLInputElement, HTMLTextAreaElement, HTMLSelectElement, HTMLButtonElement].forEach(function (c) {
  c.prototype.checkValidity = c.prototype.reportValidity = function () { return true; };
  c.prototype.setCustomValidity = function () {};
  prop(c, 'validity', function () { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, customError: false }; });
  prop(c, 'willValidate', function () { return true; });
});
[HTMLInputElement, HTMLTextAreaElement].forEach(function (c) {
  c.prototype.select = c.prototype.setSelectionRange = c.prototype.setRangeText = function () {};
  prop(c, 'selectionStart', function () { return this.value.length; }, function () {});
  prop(c, 'selectionEnd', function () { return this.value.length; }, function () {});
});
prop(HTMLSelectElement, 'options', function () { return this.querySelectorAll('option'); });
prop(HTMLSelectElement, 'length', function () { return this.options.length; });
prop(HTMLSelectElement, 'selectedIndex', function () { var v = this.value, o = this.options; for (var i = 0; i < o.length; i++) if (o[i].value === v) return i; return o.length ? 0 : -1; }, function (i) { var o = this.options[i]; if (o) this.value = o.value; });
prop(HTMLSelectElement, 'selectedOptions', function () { var i = this.selectedIndex; return i < 0 ? [] : [this.options[i]]; });
prop(HTMLOptionElement, 'index', function () { var s = this.closest('select'); return s ? Array.prototype.indexOf.call(s.options, this) : 0; });
prop(HTMLImageElement, 'naturalWidth', function () { return this.offsetWidth; });
prop(HTMLImageElement, 'naturalHeight', function () { return this.offsetHeight; });
prop(HTMLImageElement, 'complete', function () { return true; });
prop(HTMLImageElement, 'currentSrc', function () { return this.src; });
HTMLImageElement.prototype.decode = function () { return Promise.resolve(); };
function sheetOf() { return { cssRules: [], rules: [], disabled: false, ownerNode: this, insertRule: function () { return 0; }, deleteRule: function () {}, addRule: function () {}, removeRule: function () {} }; }
prop(HTMLStyleElement, 'sheet', sheetOf); prop(HTMLLinkElement, 'sheet', sheetOf);
prop(HTMLLinkElement, 'relList', function () { var el = this; return { contains: function (t) { return (' ' + el.rel + ' ').indexOf(' ' + t + ' ') >= 0; }, supports: function () { return false; }, add: function (t) { el.rel = (el.rel + ' ' + t).trim(); } }; });
var innerDesc = Object.getOwnPropertyDescriptor(Element.prototype, 'innerHTML');
function templateContent(t) {
  var f = t.__content;
  if (!f) { f = document.createDocumentFragment(); Object.defineProperty(t, '__content', { value: f }); }
  while (t.firstChild) f.appendChild(t.firstChild);
  return f;
}
prop(HTMLTemplateElement, 'content', function () { return templateContent(this); });
Object.defineProperty(HTMLTemplateElement.prototype, 'innerHTML', {
  get: function () { var d = document.createElement('div'); d.appendChild(templateContent(this).cloneNode(true)); return innerDesc.get.call(d); },
  set: function (v) { var f = templateContent(this); while (f.firstChild) f.removeChild(f.firstChild); innerDesc.set.call(this, v); templateContent(this); },
  configurable: true, enumerable: true
});
var E = Element.prototype;
var attrsDesc = Object.getOwnPropertyDescriptor(E, 'attributes');
Object.defineProperty(E, 'attributes', { get: function () {
  var el = this, a = attrsDesc.get.call(this);
  a.forEach(function (x) { x.nodeName = x.localName = x.name; x.nodeValue = x.textContent = x.value; x.specified = true; x.ownerElement = el; x.nodeType = 2; });
  a.getNamedItem = function (n) { n = String(n).toLowerCase(); for (var i = 0; i < a.length; i++) if (a[i].name === n) return a[i]; return null; };
  a.item = function (i) { return a[i] || null; };
  a.setNamedItem = function (attr) { el.setAttribute(attr.name, attr.value); return null; };
  a.removeNamedItem = function (n) { el.removeAttribute(n); };
  return a;
}, configurable: true, enumerable: true });
E.hasAttributes = function () { return this.getAttributeNames().length > 0; };
E.getAttributeNS = function (ns, n) { return this.getAttribute(n); };
E.setAttributeNS = function (ns, n, v) { this.setAttribute(String(n).replace(/^.*:/, ''), v); };
E.removeAttributeNS = function (ns, n) { this.removeAttribute(n); };
E.hasAttributeNS = function (ns, n) { return this.hasAttribute(n); };
E.getAttributeNode = function (n) { var v = this.getAttribute(n); return v === null ? null : { name: n, value: v, specified: true, ownerElement: this }; };
E.getElementsByTagNameNS = function (ns, n) { return this.getElementsByTagName(n); };
E.msMatchesSelector = E.mozMatchesSelector = E.matches;
E.scrollIntoViewIfNeeded = E.scrollIntoView;
Object.defineProperty(E, 'namespaceURI', { get: function () { return 'http://www.w3.org/1999/xhtml'; }, configurable: true });
Object.defineProperty(E, 'prefix', { get: function () { return null; }, configurable: true });
Node.prototype.isSameNode = function (o) { return this === o; };
Node.prototype.isEqualNode = function (o) { return !!o && (this === o || (this.nodeType === o.nodeType && (this.nodeType === 1 ? this.outerHTML === o.outerHTML : this.nodeValue === o.nodeValue))); };
Node.prototype.normalize = function () {};
Node.prototype.lookupNamespaceURI = function () { return null; };
Node.prototype.isDefaultNamespace = function () { return true; };
Document.prototype.getElementsByTagNameNS = function (ns, n) { return this.getElementsByTagName(n); };
prop(HTMLLabelElement, 'control', function () { var id = this.htmlFor; return id ? document.getElementById(id) : this.querySelector('input, select, textarea, button'); });
prop(HTMLIFrameElement, 'contentWindow', function () { return null; }); prop(HTMLIFrameElement, 'contentDocument', function () { return null; });
['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin'].forEach(function (part) {
  prop(HTMLAnchorElement, part, function () { try { return new URL(this.href)[part]; } catch (e) { return ''; } }, function (v) { try { var u = new URL(this.href); u[part] = v; this.href = u.href; } catch (e) {} });
});
prop(CharacterData, 'length', function () { return this.data.length; });
Text.prototype.splitText = function (at) { var rest = document.createTextNode(this.data.slice(at)); this.data = this.data.slice(0, at); if (this.parentNode) this.parentNode.insertBefore(rest, this.nextSibling); return rest; };
Text.prototype.appendData = function (s) { this.data += s; };

)JS"
R"JS(
// interfaces that scripts look for by name (feature tests, instanceof, prototype patching)
function named(name, instance) {
  var ctor = function () { throw new TypeError('Illegal constructor'); };
  Object.defineProperty(ctor, 'name', { value: name });
  if (instance) { var p = Object.create(Object.getPrototypeOf(instance)); Object.setPrototypeOf(instance, p); ctor.prototype = p; }
  Object.defineProperty(ctor.prototype, 'constructor', { value: ctor, writable: true, configurable: true });
  Object.defineProperty(G, name, { value: ctor, writable: true, configurable: true });
  return ctor;
}
named('Navigator', navigator); named('Location', location); named('History', history); named('Screen', screen); named('Performance', performance);
var windowProto = Object.create(EventTarget.prototype);
Object.setPrototypeOf(G, windowProto);
named('Window').prototype = windowProto;
['Range', 'Selection', 'NamedNodeMap', 'Attr', 'ShadowRoot', 'DocumentType', 'ProcessingInstruction', 'CDATASection', 'XMLDocument', 'MediaQueryList', 'StyleSheet', 'CSSStyleSheet',
 'CSSRule', 'CSSRuleList', 'StyleSheetList', 'MutationRecord', 'DOMRectList', 'DataTransfer', 'FileList', 'ValidityState', 'Touch', 'TextMetrics', 'ImageData',
 'CanvasRenderingContext2D', 'MediaError', 'TimeRanges', 'HTMLAllCollection', 'DOMImplementation', 'DOMStringMap', 'NodeIterator', 'Plugin', 'MimeType', 'PluginArray', 'MimeTypeArray'].forEach(function (n) { named(n); });
class DOMRect {
  constructor(x, y, w, h) { this.x = x || 0; this.y = y || 0; this.width = w || 0; this.height = h || 0; }
  get left() { return this.x; } get top() { return this.y; } get right() { return this.x + this.width; } get bottom() { return this.y + this.height; }
  toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, left: this.left, right: this.right, bottom: this.bottom }; }
  static fromRect(r) { r = r || {}; return new DOMRect(r.x, r.y, r.width, r.height); }
}
class DOMPoint { constructor(x, y, z, w) { this.x = x || 0; this.y = y || 0; this.z = z || 0; this.w = w === undefined ? 1 : w; } }
class FileReader extends EventTarget {
  constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
  _read(blob, how) { var self = this; this.readyState = 1; Promise.resolve().then(function () { self.result = how(blob); self.readyState = 2; self.dispatchEvent(new Event('load')); self.dispatchEvent(new Event('loadend')); }); }
  readAsText(b) { this._read(b, function (b) { return new TextDecoder().decode(b._d); }); }
  readAsArrayBuffer(b) { this._read(b, function (b) { return b._d.slice().buffer; }); }
  readAsDataURL(b) { this._read(b, function (b) { var s = ''; for (var i = 0; i < b._d.length; i++) s += String.fromCharCode(b._d[i]); return 'data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(s); }); }
  abort() {}
}
// Connections that are not supported yet fail the way a refused connection would, instead of throwing.
class WebSocket extends EventTarget {
  constructor(url) { super(); this.url = String(url); this.readyState = 0; this.protocol = ''; this.extensions = ''; this.bufferedAmount = 0; this.binaryType = 'blob'; var self = this; setTimeout(function () { self.readyState = 3; self.dispatchEvent(new Event('error')); var e = new Event('close'); e.code = 1006; e.reason = ''; e.wasClean = false; self.dispatchEvent(e); }, 0); }
  send() { throw new DOMException('WebSocket is not open', 'InvalidStateError'); }
  close() { this.readyState = 3; }
}
WebSocket.CONNECTING = 0; WebSocket.OPEN = 1; WebSocket.CLOSING = 2; WebSocket.CLOSED = 3;
class Notification { constructor() { throw new TypeError('Notifications are not supported'); } static requestPermission() { return Promise.resolve('denied'); } }
Notification.permission = 'denied';
var more = { DOMRect: DOMRect, DOMRectReadOnly: DOMRect, DOMPoint: DOMPoint, FileReader: FileReader, WebSocket: WebSocket, Notification: Notification };
for (var key of Object.keys(more)) Object.defineProperty(G, key, { value: more[key], writable: true, configurable: true });

)JS"
R"JS(
G.Image = function Image(w, h) { var i = document.createElement('img'); if (w !== undefined) i.width = w; if (h !== undefined) i.height = h; return i; };
G.Audio = function Audio(src) { var a = document.createElement('audio'); if (src) a.src = src; return a; };
G.Option = function Option(text, value) { var o = document.createElement('option'); if (text !== undefined) o.textContent = text; if (value !== undefined) o.value = value; return o; };
G.Image.prototype = G.Audio.prototype = G.Option.prototype = HTMLElement.prototype;

function emptyList() { var a = []; a.item = function () { return null; }; a.namedItem = function () { return null; }; a.refresh = function () {}; return a; }
navigator.plugins = emptyList(); navigator.mimeTypes = emptyList();
navigator.permissions = { query: function () { return Promise.resolve({ state: 'prompt', addEventListener: function () {}, removeEventListener: function () {} }); } };
navigator.storage = { estimate: function () { return Promise.resolve({ quota: 0, usage: 0 }); }, persist: function () { return Promise.resolve(false); }, persisted: function () { return Promise.resolve(false); } };
navigator.connection = { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener: function () {}, removeEventListener: function () {} };
navigator.clipboard = { writeText: function () { return Promise.reject(new DOMException('Clipboard access is not available', 'NotAllowedError')); }, readText: function () { return Promise.reject(new DOMException('Clipboard access is not available', 'NotAllowedError')); } };
navigator.webdriver = false; navigator.pdfViewerEnabled = false; navigator.deviceMemory = 8;

performance.timeOrigin = Date.now() - performance.now();
var t0 = Math.floor(performance.timeOrigin);
performance.timing = { navigationStart: t0, fetchStart: t0, domainLookupStart: t0, domainLookupEnd: t0, connectStart: t0, connectEnd: t0, secureConnectionStart: t0, requestStart: t0, responseStart: t0, responseEnd: t0,
  domLoading: t0, domInteractive: 0, domContentLoadedEventStart: 0, domContentLoadedEventEnd: 0, domComplete: 0, loadEventStart: 0, loadEventEnd: 0, unloadEventStart: 0, unloadEventEnd: 0, redirectStart: 0, redirectEnd: 0 };
performance.navigation = { type: 0, redirectCount: 0 };
performance.memory = { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 0 };

G.requestIdleCallback = function (cb) { return setTimeout(function () { cb({ didTimeout: false, timeRemaining: function () { return 10; } }); }, 1); };
G.postMessage = function (data) { setTimeout(function () { var e = new Event('message'); e.data = data; e.origin = location.origin; e.source = G; G.dispatchEvent(e); }, 0); };
G.structuredClone = function (v) { return v === undefined ? undefined : JSON.parse(JSON.stringify(v)); };
G.reportError = function (e) { console.error(e); };
G.open = function () { return null; };
G.CSS = { supports: function () { return false; }, escape: function (s) { return String(s).replace(/[^a-zA-Z0-9_ -￿-]/g, function (c) { return '\\' + c; }); } };
class MessageChannel {
  constructor() {
    var a = new EventTarget(), b = new EventTarget();
    function link(from, to) { from.postMessage = function (data) { setTimeout(function () { var e = new Event('message'); e.data = data; to.dispatchEvent(e); }, 0); }; from.start = from.close = function () {}; }
    link(a, b); link(b, a); this.port1 = a; this.port2 = b;
  }
}
class DOMParser {
  parseFromString(text) { var d = inertDocument(); d.body.innerHTML = String(text); return d; }
}
class XMLSerializer { serializeToString(n) { return n.outerHTML !== undefined ? n.outerHTML : String(n.textContent || ''); } }

)JS"
R"JS(
// Intl: formatting falls back to the engine's own en-US output
function locales() { return ['en-US']; }
var Intl = {
  DateTimeFormat: class { constructor(l, o) { this._o = o || {}; } format(d) { var x = d === undefined ? new Date() : new Date(d); return this._o.timeStyle || this._o.hour ? (this._o.dateStyle || this._o.year ? x.toLocaleString() : x.toLocaleTimeString()) : x.toLocaleDateString(); } formatToParts(d) { return [{ type: 'literal', value: this.format(d) }]; } resolvedOptions() { return { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }; } static supportedLocalesOf() { return locales(); } },
  NumberFormat: class { constructor(l, o) { this._o = o || {}; } format(n) { var o = this._o, s = Number(n).toLocaleString(); if (o.style === 'percent') s = (Number(n) * 100).toLocaleString() + '%'; if (o.style === 'currency') s = (o.currency === 'EUR' ? '€' : o.currency === 'GBP' ? '£' : '$') + s; return s; } formatToParts(n) { return [{ type: 'integer', value: this.format(n) }]; } resolvedOptions() { return { locale: 'en-US', numberingSystem: 'latn' }; } static supportedLocalesOf() { return locales(); } },
  Collator: class { compare(a, b) { return String(a).localeCompare(String(b)); } resolvedOptions() { return { locale: 'en-US' }; } static supportedLocalesOf() { return locales(); } },
  PluralRules: class { select(n) { return n === 1 ? 'one' : 'other'; } resolvedOptions() { return { locale: 'en-US' }; } static supportedLocalesOf() { return locales(); } },
  RelativeTimeFormat: class { format(v, unit) { var n = Math.abs(v), u = unit + (n === 1 ? '' : 's'); return v < 0 ? n + ' ' + u + ' ago' : 'in ' + n + ' ' + u; } static supportedLocalesOf() { return locales(); } },
  ListFormat: class { format(list) { return Array.from(list).join(', '); } static supportedLocalesOf() { return locales(); } },
  Segmenter: class { segment(s) { return Array.from(String(s)).map(function (c, i) { return { segment: c, index: i }; }); } },
  getCanonicalLocales: function (l) { return l === undefined ? [] : [].concat(l); }, supportedValuesOf: function () { return []; }
};

var exported = { DOMException: DOMException, EventTarget: EventTarget, AbortController: AbortController, AbortSignal: AbortSignal, URL: URL, URLSearchParams: URLSearchParams,
  Blob: Blob, File: File, FormData: FormData, Headers: Headers, Request: Request, Response: Response, fetch: fetch, XMLHttpRequest: XMLHttpRequest,
  NodeFilter: NodeFilter, TreeWalker: TreeWalker, MessageChannel: MessageChannel, DOMParser: DOMParser, XMLSerializer: XMLSerializer, Intl: Intl };
for (var name of Object.keys(exported)) Object.defineProperty(G, name, { value: exported[name], writable: true, configurable: true, enumerable: false });
G.webkitURL = URL;
})();
)JS";

}  // namespace tb
