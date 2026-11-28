// Web APIs that are written in JavaScript on top of the native DOM and network primitives. This text is run
// once for every page that has scripts, before any of the page's own code.
#pragma once

namespace tb {

static const char kScriptPrelude[] = R"JS((function () {
'use strict';
var G = globalThis;
var net = G.__net, netSync = G.__netSync, netAbort = G.__netAbort, parseUrl = G.__url;
delete G.__net; delete G.__netSync; delete G.__netAbort; delete G.__url;
delete G.SharedArrayBuffer;  // only for cross-origin isolated pages, which do not exist here yet

class DOMException extends Error {
  constructor(message, name) { super(message); this.name = name || 'Error'; this.code = 0; }
}
G.__domException(DOMException);  // what the engine's own DOM code throws
delete G.__domException;

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
Document.prototype.adoptNode = function (n) { return n; };
Document.prototype.importNode = function (n, deep) { return n.cloneNode(!!deep); };
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
HTMLElement.prototype.animate = function () { return { cancel: function () {}, finish: function () {}, play: function () {}, pause: function () {}, finished: Promise.resolve(), addEventListener: function () {}, removeEventListener: function () {} }; };
HTMLElement.prototype.getAnimations = function () { return []; };
HTMLElement.prototype.setPointerCapture = HTMLElement.prototype.releasePointerCapture = HTMLElement.prototype.scrollTo = HTMLElement.prototype.scroll = HTMLElement.prototype.scrollBy = HTMLElement.prototype.requestFullscreen = function () {};
Object.defineProperty(HTMLElement.prototype, 'isContentEditable', { get: function () { return false; }, configurable: true });
)JS"
R"JS(
function prop(ctor, name, get, set) { Object.defineProperty(ctor.prototype, name, { get: get, set: set, configurable: true, enumerable: true }); }
function textProp(ctor) { prop(ctor, 'text', function () { return this.textContent; }, function (v) { this.textContent = v; }); }
[HTMLScriptElement, HTMLAnchorElement, HTMLOptionElement, HTMLTitleElement].forEach(textProp);
// getContext (HTML 4.12.5.1): a WebGL 1 context, made the first time and the same one after; no other kind yet
var webglContext = G.__webglContext;
delete G.__webglContext;
HTMLCanvasElement.prototype.getContext = function (type, attributes) {
  type = String(type);
  if (type === 'webgl' || type === 'experimental-webgl') return webglContext(this, type, attributes);
  return null;
};
// width and height reflect the attributes as unsigned longs with the bitmap's defaults (HTML 4.12.5)
[['width', 300], ['height', 150]].forEach(function (d) {
  Object.defineProperty(HTMLCanvasElement.prototype, d[0], {
    get: function () {
      var m = /^[ \t\n\f\r]*\+?(\d+)/.exec(this.getAttribute(d[0]) || '');
      var n = m ? Number(m[1]) : NaN;
      return n >= 0 && n <= 2147483647 ? n : d[1];
    },
    set: function (v) { v = v >>> 0; this.setAttribute(d[0], String(v > 2147483647 ? d[1] : v)); },
    enumerable: true, configurable: true
  });
});
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
prop(HTMLImageElement, 'currentSrc', function () { return this.src; });
HTMLImageElement.prototype.decode = function () { return Promise.resolve(); };
function sheetOf() { return { cssRules: [], rules: [], disabled: false, ownerNode: this, insertRule: function () { return 0; }, deleteRule: function () {}, addRule: function () {}, removeRule: function () {} }; }
prop(HTMLStyleElement, 'sheet', sheetOf); prop(HTMLLinkElement, 'sheet', sheetOf);
prop(HTMLLinkElement, 'relList', function () { var el = this; return { contains: function (t) { return (' ' + el.rel + ' ').indexOf(' ' + t + ' ') >= 0; }, supports: function () { return false; }, add: function (t) { el.rel = (el.rel + ' ' + t).trim(); } }; });
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
named('Navigator', navigator); named('Location', location); named('Screen', screen); named('Performance', performance);
var windowProto = Object.create(EventTarget.prototype);
Object.setPrototypeOf(G, windowProto);
named('Window').prototype = windowProto;
['Range', 'Selection', 'NamedNodeMap', 'Attr', 'DocumentType', 'ProcessingInstruction', 'CDATASection', 'XMLDocument', 'MediaQueryList', 'StyleSheet',
 'CSSRule', 'CSSRuleList', 'StyleSheetList', 'MutationRecord', 'DOMRectList', 'DataTransfer', 'FileList', 'ValidityState', 'Touch', 'TextMetrics',
 'CanvasRenderingContext2D', 'MediaError', 'TimeRanges', 'DOMImplementation', 'DOMStringMap', 'NodeIterator', 'Plugin', 'MimeType', 'PluginArray', 'MimeTypeArray'].forEach(function (n) { named(n); });
// ImageData (HTML 4.12.5.1.15): width by height RGBA pixels in a Uint8ClampedArray
class ImageData {
  #width; #height; #data;
  constructor(a, b, c) {
    if (arguments.length < 2) throw new TypeError("Failed to construct 'ImageData': 2 arguments required");
    var data, w, h;
    if (a instanceof Uint8ClampedArray) {
      data = a; w = b >>> 0;
      if (!data.length || data.length % 4) throw new DOMException('The input data length is not a multiple of 4.', 'InvalidStateError');
      if (!w) throw new DOMException('The source width is zero or not a number.', 'IndexSizeError');
      if ((data.length / 4) % w) throw new DOMException('The input data length is not a multiple of (4 * width).', 'IndexSizeError');
      h = data.length / 4 / w;
      if (c !== undefined && (c >>> 0) !== h) throw new DOMException('The input data length is not equal to (4 * width * height).', 'IndexSizeError');
    } else {
      w = a >>> 0; h = b >>> 0;
      if (!w || !h) throw new DOMException('The source ' + (w ? 'height' : 'width') + ' is zero or not a number.', 'IndexSizeError');
      data = new Uint8ClampedArray(w * h * 4);
    }
    this.#width = w; this.#height = h; this.#data = data;
  }
  get width() { return this.#width; }
  get height() { return this.#height; }
  get data() { return this.#data; }
  get colorSpace() { return 'srgb'; }
}
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
var more = { ImageData: ImageData, DOMRect: DOMRect, DOMRectReadOnly: DOMRect, DOMPoint: DOMPoint, FileReader: FileReader, WebSocket: WebSocket, Notification: Notification };
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
// ---- module scripts: how specifiers become URLs (import maps), how module text is fetched, and the order in
// which a page's deferred scripts and modules run
var modules = G.__modules, reportError = G.__reportError;
delete G.__modules; delete G.__reportError;
var importMap = { imports: [], scopes: [] }, inlineModules = 0;
var JS_TYPE = /^(application|text)\/(x-)?(javascript|ecmascript)$|^text\/(javascript1\.[0-5]|jscript|livescript)$/;
function urlLike(specifier, base) {
  if (/^(\/|\.\/|\.\.\/)/.test(specifier)) { try { return new URL(specifier, base).href; } catch (e) { return null; } }
  try { return new URL(specifier).href; } catch (e) { return null; }
}
function specifierMap(obj, base) {  // sorted so that the longest, most specific keys are tried first
  var out = [];
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) throw new TypeError('An import map must hold objects of specifier to address');
  Object.keys(obj).forEach(function (key) {
    if (key === '') return;
    var address = typeof obj[key] === 'string' ? urlLike(obj[key], base) : null;
    if (address !== null && key.endsWith('/') && !address.endsWith('/')) address = null;
    out.push([urlLike(key, base) || key, address]);
  });
  return out;
}
function byKeyDescending(a, b) { return a[0] < b[0] ? 1 : a[0] > b[0] ? -1 : 0; }
function addEntries(list, more) {  // earlier import maps win
  more.forEach(function (e) { if (!list.some(function (x) { return x[0] === e[0]; })) list.push(e); });
  list.sort(byKeyDescending);
}
function registerImportMap(text) {
  try {
    var parsed = JSON.parse(text), base = document.baseURI;
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) throw new TypeError('An import map must be a JSON object');
    if (parsed.imports !== undefined) addEntries(importMap.imports, specifierMap(parsed.imports, base));
    if (parsed.scopes !== undefined) {
      if (!parsed.scopes || typeof parsed.scopes !== 'object' || Array.isArray(parsed.scopes)) throw new TypeError("An import map's scopes must be an object");
      Object.keys(parsed.scopes).forEach(function (prefix) {
        var url; try { url = new URL(prefix, base).href; } catch (e) { return; }
        var entry = importMap.scopes.find(function (s) { return s[0] === url; });
        if (!entry) importMap.scopes.push(entry = [url, []]);
        addEntries(entry[1], specifierMap(parsed.scopes[prefix], base));
      });
      importMap.scopes.sort(byKeyDescending);
    }
  } catch (e) { reportError(e); }
}
function importsMatch(specifier, asURL, map) {
  for (var i = 0; i < map.length; i++) {
    var key = map[i][0], address = map[i][1];
    if (key === specifier) {
      if (address === null) throw new TypeError('The import map blocks the module specifier "' + specifier + '"');
      return address;
    }
    if (key.endsWith('/') && specifier.startsWith(key) && (asURL === null || /^(https?|file|ftp|wss?):/.test(asURL))) {
      if (address === null) throw new TypeError('The import map blocks the module specifier "' + specifier + '"');
      var url; try { url = new URL(specifier.slice(key.length), address).href; } catch (e) { url = ''; }
      if (!url.startsWith(address)) throw new TypeError('The import map entry for "' + key + '" cannot resolve "' + specifier + '"');
      return url;
    }
  }
  return null;
}
modules.host.resolve = function (specifier, referrer) {
  var base; try { base = new URL(referrer, document.baseURI).href; } catch (e) { base = document.baseURI; }
  var asURL = urlLike(specifier, base), normalized = asURL !== null ? asURL : specifier;
  for (var i = 0; i < importMap.scopes.length; i++) {
    var prefix = importMap.scopes[i][0];
    if (prefix === base || (prefix.endsWith('/') && base.startsWith(prefix))) {
      var scoped = importsMatch(normalized, asURL, importMap.scopes[i][1]);
      if (scoped !== null) return scoped;
    }
  }
  var top = importsMatch(normalized, asURL, importMap.imports);
  if (top !== null) return top;
  if (asURL !== null) return asURL;
  throw new TypeError('Failed to resolve module specifier "' + specifier + '". Relative references must start with either "/", "./", or "../".');
};
modules.host.fetch = function (url) {
  return fetch(url, { credentials: 'same-origin' }).then(function (r) {
    if (!r.ok) throw new TypeError('Failed to load module ' + url + ' (status ' + r.status + ')');
    var type = (r.headers.get('content-type') || '').split(';')[0].trim().toLowerCase();
    if (!JS_TYPE.test(type) && !(type === '' && /^file:/.test(url)))
      throw new TypeError('Failed to load module ' + url + ': expected JavaScript but the server sent "' + type + '"');
    return r.text().then(function (source) { return { source: source, url: r.url || url }; });
  }, function () { throw new TypeError('Failed to load module ' + url); });
};
function loadModuleScript(src, code) {
  var ready = src ? modules.load(src) : modules.load(document.URL.split('#')[0] + '#module-script-' + (++inlineModules), code, document.baseURI);
  ready.catch(function () {});
  return ready;
}
function signal(el, type) { if (el) { try { el.dispatchEvent(new Event(type)); } catch (e) { reportError(e); } } }
function runModuleScript(el, ready, external) {
  return ready.then(function (record) {
    try { modules.run(record).catch(reportError); } catch (e) { reportError(e); }
    if (external) signal(el, 'load');
  }, function (e) { reportError(e); signal(el, 'error'); });
}
Object.defineProperty(G, '__pageScripts', { configurable: true, value: {
  importMap: registerImportMap,
  // items: [is module, element, src, inline code, run (classic scripts), async], in document order
  deferred: function (items, done) {
    var chain = Promise.resolve();
    items.forEach(function (it) {
      if (!it[0]) { chain = chain.then(it[4]); return; }
      var ready = loadModuleScript(it[2], it[3]);
      var exec = function () { return runModuleScript(it[1], ready, !!it[2]); };
      if (it[5]) exec(); else chain = chain.then(exec);
    });
    chain.then(done, done);
  },
  dynamic: function (el, src, code) { runModuleScript(el, loadModuleScript(src, code), !!src); }
} });
G.reportError = function (e) { reportError(e); };

)JS"
R"JS(
// ---- custom elements (HTML 4.13). The engine says which elements are custom and notes when they enter or leave
// the document or have an attribute changed; the registry, the upgrades and the callbacks are here.
var shadowOf = G.__shadowOf;  // shadowOf.call(element): its shadow root, open or closed
delete G.__shadowOf;
var ceHook = G.__ceHook, ceState = G.__ceState, isConstructor = G.__isConstructor;  // ceState(element[, state]): 0 not custom (yet), 1 custom, 2 failed
delete G.__ceHook; delete G.__ceState; delete G.__isConstructor;
var reflectApply = Reflect.apply, reflectConstruct = Reflect.construct, setPrototypeOf = Object.setPrototypeOf;
var nativeCreateElement = Document.prototype.createElement;
var definitions = new Map(), byConstructor = new Map(), definitionOf = new WeakMap(), waitingForName = new Map();
var CONSTRUCTED = {};  // on a construction stack: the element there has been handed out already
var defining = false;
function validCustomName(name) {  // PotentialCustomElementName: a lowercase letter first, at least one hyphen
  return /^[a-z][-.0-9_a-z·À-￿]*-[-.0-9_a-z·À-￿]*$/.test(name) &&
    ['annotation-xml', 'color-profile', 'font-face', 'font-face-src', 'font-face-uri', 'font-face-format', 'font-face-name', 'missing-glyph'].indexOf(name) < 0;
}
function callbackOf(proto, name) {
  var f = proto[name];
  if (f !== undefined && typeof f !== 'function') throw new TypeError(name + ' must be a function');
  return f;
}
// "Upgrade an element" (4.13.5): its constructor runs with the element as the one it will be given by HTMLElement
function upgradeElement(el, def) {
  if (ceState(el) !== 0) return;
  definitionOf.set(el, def);
  ceState(el, 2);  // failed, until the constructor has come back with it
  var names = el.getAttributeNames(), values = [], connected = el.isConnected;
  for (var i = 0; i < names.length; i++) values.push(el.getAttribute(names[i]));
  def.stack.push(el);
  try {
    if (reflectConstruct(def.ctor, []) !== el) throw new TypeError("The constructor of <" + def.name + "> did not return the element being upgraded");
  } catch (e) {
    definitionOf.delete(el);
    throw e;
  } finally { def.stack.pop(); }
  ceState(el, 1);
  for (var i = 0; i < names.length; i++) react(2, el, names[i], null, values[i]);
  if (connected) react(0, el);
}
// The [HTMLConstructor] steps (3.2.3), which every custom element's constructor reaches through super()
function htmlConstructor(newTarget) {
  var def = byConstructor.get(newTarget);
  if (!def) throw new TypeError('Illegal constructor');
  var proto = newTarget.prototype, el;
  if (Object(proto) !== proto) proto = HTMLElement.prototype;
  if (def.stack.length === 0) {  // new X(): the element is made here
    el = reflectApply(nativeCreateElement, document, [def.name]);
    setPrototypeOf(el, proto);
    definitionOf.set(el, def);
    ceState(el, 1);
    return el;
  }
  el = def.stack[def.stack.length - 1];
  if (el === CONSTRUCTED) throw new TypeError("The constructor of <" + def.name + "> made its element twice");
  setPrototypeOf(el, proto);
  def.stack[def.stack.length - 1] = CONSTRUCTED;
  return el;
}
// One reaction (4.13.6). kind: 0 connected, 1 disconnected, 2 attribute changed, 3 an element that may be waiting
// for its definition, 4 the HTMLElement constructor (el is then new.target)
function react(kind, el, name, oldValue, newValue) {
  if (kind === 4) return htmlConstructor(el);
  try {
    if (kind === 3) {
      var waitedFor = definitions.get(el.localName);
      if (waitedFor) upgradeElement(el, waitedFor);
      return;
    }
    var def = definitionOf.get(el);
    if (!def || ceState(el) !== 1) return;
    if (kind === 0) { if (def.connected) reflectApply(def.connected, el, []); }
    else if (kind === 1) { if (def.disconnected) reflectApply(def.disconnected, el, []); }
    else if (def.attributeChanged && def.observed.has(name)) reflectApply(def.attributeChanged, el, [name, oldValue, newValue, null]);
  } catch (e) { reportError(e); }
}
// Upgrades what was waiting for a definition, in tree order, shadow trees included (name: only elements of that name)
function upgradeAll(root, name) {
  var all = root.querySelectorAll('*');
  for (var i = 0; i < all.length; i++) {
    var el = all[i], shadow = reflectApply(shadowOf, el, []);
    if ((name === undefined || el.localName === name) && ceState(el) === 0) react(3, el);
    if (shadow) upgradeAll(shadow, name);
  }
}
// ---- style sheets made by script (CSSOM 6.1.2 and 6.2): new CSSStyleSheet(), replaceSync, adoptedStyleSheets.
// The engine gets the text of what each document or shadow root has adopted, again whenever that changes.
var adopt = G.__adopt;
delete G.__adopt;
var adoptedBy = new WeakMap();  // document or shadow root -> its list of sheets
function syncAdopted(root) {
  var texts = [], list = adoptedBy.get(root) || [];
  for (var i = 0; i < list.length; i++) if (!list[i].disabled) texts.push(list[i]._text);
  adopt(root, texts);
}
class CSSStyleSheet {
  constructor(options) {
    Object.defineProperty(this, '_text', { value: '', writable: true });
    Object.defineProperty(this, '_roots', { value: new Set() });
    this.disabled = !!(options && options.disabled);
    this.cssRules = []; this.rules = this.cssRules; this.ownerNode = null; this.media = { mediaText: (options && options.media) || '' };
  }
  replaceSync(text) { this._text = String(text); this._roots.forEach(syncAdopted); }
  replace(text) { this.replaceSync(text); return Promise.resolve(this); }
  insertRule(rule, index) { this._text += '\n' + rule; this._roots.forEach(syncAdopted); return index === undefined ? 0 : index; }
  deleteRule() {}
  addRule(selector, style) { return this.insertRule(selector + '{' + style + '}'); }
  removeRule() {}
}
[Document.prototype, ShadowRoot.prototype].forEach(function (proto) {
  Object.defineProperty(proto, 'adoptedStyleSheets', {
    configurable: true, enumerable: true,
    get: function () {
      var root = this, list = adoptedBy.get(root);
      if (!list) adoptedBy.set(root, list = []);
      // an observable array: changing it in place counts as well
      return new Proxy(list, {
        set: function (t, k, v) { t[k] = v; if (v instanceof CSSStyleSheet) v._roots.add(root); syncAdopted(root); return true; },
        deleteProperty: function (t, k) { delete t[k]; syncAdopted(root); return true; }
      });
    },
    set: function (sheets) {
      var list = Array.from(sheets), root = this;
      list.forEach(function (sheet) {
        if (!(sheet instanceof CSSStyleSheet)) throw new TypeError('adoptedStyleSheets takes CSSStyleSheet objects');
        sheet._roots.add(root);
      });
      adoptedBy.set(root, list);
      syncAdopted(root);
    }
  });
});
// attachShadow: the engine's refusals become DOMExceptions, and a custom element may have opted out of shadow trees
var nativeAttachShadow = Element.prototype.attachShadow;
Element.prototype.attachShadow = function attachShadow(init) {
  var def = definitionOf.get(this);
  if (def && def.noShadow) throw new DOMException('This custom element does not take a shadow root', 'NotSupportedError');
  try { return reflectApply(nativeAttachShadow, this, [init]); } catch (e) {
    var named = e && !(e instanceof TypeError) && /^(\w+Error): (.*)$/.exec(e.message);
    throw named ? new DOMException(named[2], named[1]) : e;
  }
};
function CustomElementRegistry() { throw new TypeError('Illegal constructor'); }
CustomElementRegistry.prototype = {
  constructor: CustomElementRegistry,
  define: function define(name, ctor, options) {
    name = String(name);
    if (!isConstructor(ctor)) throw new TypeError('customElements.define: the second argument must be a constructor');
    if (!validCustomName(name)) throw new DOMException("'" + name + "' is not a valid custom element name", 'SyntaxError');
    if (definitions.has(name)) throw new DOMException("The name '" + name + "' has already been defined", 'NotSupportedError');
    if (byConstructor.has(ctor)) throw new DOMException('This constructor has already been used for another custom element', 'NotSupportedError');
    if (options && options.extends !== undefined && options.extends !== null) throw new DOMException('Customized built-in elements are not supported', 'NotSupportedError');
    if (defining) throw new DOMException('Another custom element is being defined', 'NotSupportedError');
    var def = { name: name, ctor: ctor, stack: [], observed: new Set() };
    defining = true;
    try {
      var proto = ctor.prototype;
      if (Object(proto) !== proto) throw new TypeError("The constructor's prototype is not an object");
      def.connected = callbackOf(proto, 'connectedCallback');
      def.disconnected = callbackOf(proto, 'disconnectedCallback');
      def.adopted = callbackOf(proto, 'adoptedCallback');
      def.attributeChanged = callbackOf(proto, 'attributeChangedCallback');
      if (def.attributeChanged) {
        var observed = ctor.observedAttributes;
        if (observed !== undefined) for (var a of observed) def.observed.add(String(a));
      }
      var disabled = ctor.disabledFeatures;
      if (disabled !== undefined) for (var feature of disabled) if (String(feature) === 'shadow') def.noShadow = true;
      def.formAssociated = !!ctor.formAssociated;
    } finally { defining = false; }
    definitions.set(name, def);
    byConstructor.set(ctor, def);
    ceHook(react);  // from here on the engine notes what happens to custom elements
    upgradeAll(document, name);
    var promised = waitingForName.get(name);
    if (promised) { waitingForName.delete(name); promised.resolve(ctor); }
  },
  get: function get(name) { var def = definitions.get(String(name)); return def ? def.ctor : undefined; },
  getName: function getName(ctor) { var def = byConstructor.get(ctor); return def ? def.name : null; },
  whenDefined: function whenDefined(name) {
    name = String(name);
    if (!validCustomName(name)) return Promise.reject(new DOMException("'" + name + "' is not a valid custom element name", 'SyntaxError'));
    var def = definitions.get(name), promised = waitingForName.get(name);
    if (def) return Promise.resolve(def.ctor);
    if (!promised) {
      promised = {};
      promised.promise = new Promise(function (resolve) { promised.resolve = resolve; });
      waitingForName.set(name, promised);
    }
    return promised.promise;
  },
  upgrade: function upgrade(root) {
    if (root.nodeType === 1) react(3, root);
    upgradeAll(root);
  }
};
Object.defineProperty(G, 'CustomElementRegistry', { value: CustomElementRegistry, writable: true, configurable: true });
Object.defineProperty(G, 'customElements', { value: Object.create(CustomElementRegistry.prototype), configurable: true, enumerable: true });
// document.createElement of a defined name runs the constructor at once (DOM 4.9, "create an element", synchronously)
Document.prototype.createElement = function createElement(name) {
  var def = definitions.size ? definitions.get(String(name).toLowerCase()) : undefined;
  if (!def) return reflectApply(nativeCreateElement, this, [name]);
  try {
    var el = reflectConstruct(def.ctor, []);
    if (!(el instanceof HTMLElement) || el.localName !== def.name) throw new TypeError("The constructor of <" + def.name + "> returned something else");
    if (el.getAttributeNames().length || el.firstChild || el.parentNode) throw new DOMException("The constructor of <" + def.name + "> must not give its element attributes, children or a parent", 'NotSupportedError');
    return el;
  } catch (e) {
    reportError(e);
    var failed = reflectApply(nativeCreateElement, this, [name]);
    ceState(failed, 2);
    return failed;
  }
};

)JS"
R"JS(
// ---- IntersectionObserver and ResizeObserver. The browser has no rendering steps to hang them on yet, so both
// look at their targets on a timer for as long as there is something to watch.
var setIntervalOwn = G.setInterval, clearIntervalOwn = G.clearInterval;
var watchers = new Set(), watchTimer = 0;
function checkWatchers() {
  watchers.forEach(function (w) { try { w._check(); } catch (e) { reportError(e); } });
}
function watch(w) {
  watchers.add(w);
  if (!watchTimer) watchTimer = setIntervalOwn(checkWatchers, 100);
}
function unwatch(w) {
  if (w._targets.size) return;
  watchers.delete(w);
  if (!watchers.size) { clearIntervalOwn(watchTimer); watchTimer = 0; }
}
function rectOf(left, top, width, height) {
  return { x: left, y: top, left: left, top: top, width: width, height: height, right: left + width, bottom: top + height,
    toJSON: function () { return { x: left, y: top, width: width, height: height, top: top, left: left, right: left + width, bottom: top + height }; } };
}
function hidden(object, name, value) { Object.defineProperty(object, name, { value: value, writable: true }); }
function IntersectionObserverEntry() { throw new TypeError('Illegal constructor'); }
function ResizeObserverEntry() { throw new TypeError('Illegal constructor'); }
// rootMargin: one to four lengths in px or %, as in the CSS margin shorthand
function marginsOf(text) {
  var parts = String(text === undefined ? '0px' : text).trim().split(/\s+/);
  if (parts.length > 4 || parts[0] === '') throw new SyntaxError('rootMargin takes one to four lengths');
  var m = parts.map(function (p) {
    var found = /^(-?(?:\d+\.?\d*|\.\d+))(px|%)$/.exec(p);
    if (!found) throw new SyntaxError("rootMargin lengths are in pixels or percent, not '" + p + "'");
    return { n: +found[1], percent: found[2] === '%' };
  });
  return [m[0], m[1] || m[0], m[2] || m[0], m[3] || m[1] || m[0]];
}
class IntersectionObserver {
  constructor(callback, options) {
    if (typeof callback !== 'function') throw new TypeError('IntersectionObserver needs a callback function');
    options = options || {};
    var margins = marginsOf(options.rootMargin);
    var thresholds = (options.threshold === undefined ? [0] : [].concat(options.threshold)).map(Number);
    thresholds.forEach(function (t) { if (!(t >= 0 && t <= 1)) throw new RangeError('A threshold must be between 0 and 1'); });
    thresholds.sort(function (a, b) { return a - b; });
    this.root = options.root || null;
    this.rootMargin = margins.map(function (m) { return m.n + (m.percent ? '%' : 'px'); }).join(' ');
    this.thresholds = thresholds;
    hidden(this, '_callback', callback); hidden(this, '_margins', margins); hidden(this, '_targets', new Map()); hidden(this, '_queue', []);
  }
  observe(target) {
    if (!(target instanceof Element)) throw new TypeError('IntersectionObserver.observe needs an element');
    if (this._targets.has(target)) return;
    this._targets.set(target, { index: -1, intersecting: false });
    watch(this);
  }
  unobserve(target) { this._targets.delete(target); unwatch(this); }
  disconnect() { this._targets.clear(); this._queue = []; unwatch(this); }
  takeRecords() { var q = this._queue; this._queue = []; return q; }
  // "Run the update intersection observations steps" (Intersection Observer 3.2.7) for this observer
  _check() {
    var self = this, m = this._margins, time = performance.now(), left = 0, top = 0, right = G.innerWidth, bottom = G.innerHeight;
    if (this.root && this.root.nodeType === 1) { var rr = this.root.getBoundingClientRect(); left = rr.left; top = rr.top; right = rr.right; bottom = rr.bottom; }
    var w = right - left, h = bottom - top;
    top -= m[0].percent ? h * m[0].n / 100 : m[0].n; right += m[1].percent ? w * m[1].n / 100 : m[1].n;
    bottom += m[2].percent ? h * m[2].n / 100 : m[2].n; left -= m[3].percent ? w * m[3].n / 100 : m[3].n;
    this._targets.forEach(function (state, target) {
      var r = target.getBoundingClientRect(), rendered = target.isConnected && target.getClientRects().length > 0;
      var x0 = Math.max(r.left, left), y0 = Math.max(r.top, top), x1 = Math.min(r.right, right), y1 = Math.min(r.bottom, bottom);
      var intersecting = rendered && x1 >= x0 && y1 >= y0;  // edge-adjacent counts
      var area = r.width * r.height, ratio = !intersecting ? 0 : area > 0 ? Math.min(1, (x1 - x0) * (y1 - y0) / area) : 1;
      var index = 0;
      while (index < self.thresholds.length && self.thresholds[index] <= ratio) index++;
      if (index === state.index && intersecting === state.intersecting) return;
      state.index = index; state.intersecting = intersecting;
      var entry = Object.create(IntersectionObserverEntry.prototype);
      entry.time = time; entry.target = target; entry.isIntersecting = intersecting; entry.intersectionRatio = ratio; entry.isVisible = false;
      entry.boundingClientRect = rectOf(r.left, r.top, r.width, r.height);
      entry.rootBounds = rectOf(left, top, right - left, bottom - top);
      entry.intersectionRect = intersecting ? rectOf(x0, y0, x1 - x0, y1 - y0) : rectOf(0, 0, 0, 0);
      self._queue.push(entry);
    });
    if (this._queue.length) reflectApply(this._callback, this, [this.takeRecords(), this]);
  }
}
class ResizeObserver {
  constructor(callback) {
    if (typeof callback !== 'function') throw new TypeError('ResizeObserver needs a callback function');
    hidden(this, '_callback', callback); hidden(this, '_targets', new Map());
  }
  observe(target) {
    if (!(target instanceof Element)) throw new TypeError('ResizeObserver.observe needs an element');
    this._targets.set(target, { width: 0, height: 0 });  // the size last reported (Resize Observer 3.3)
    watch(this);
  }
  unobserve(target) { this._targets.delete(target); unwatch(this); }
  disconnect() { this._targets.clear(); unwatch(this); }
  _check() {
    var entries = [];
    this._targets.forEach(function (state, target) {
      var r = target.getBoundingClientRect();
      if (r.width === state.width && r.height === state.height) return;
      state.width = r.width; state.height = r.height;
      var entry = Object.create(ResizeObserverEntry.prototype), size = [{ inlineSize: r.width, blockSize: r.height }];
      entry.target = target; entry.contentRect = rectOf(0, 0, r.width, r.height);
      entry.borderBoxSize = size; entry.contentBoxSize = size; entry.devicePixelContentBoxSize = size;
      entries.push(entry);
    });
    if (entries.length) reflectApply(this._callback, this, [entries, this]);
  }
}

// ---- scrollTop (CSSOM View): the scroll event is not fired from inside the setter but in a task of its own, once
// for however many times the element scrolled until then
var scrollTopOf = G.__scrollTop;  // scrollTopOf(element[, offset]): reads, or scrolls and says whether it moved
delete G.__scrollTop;
var scrollEventsOwed = new Set();
Object.defineProperty(Element.prototype, 'scrollTop', {
  get: function () { return scrollTopOf(this); },
  set: function (y) {
    y = +y;
    if (!scrollTopOf(this, isFinite(y) ? y : 0)) return;
    var target = this;
    if (scrollEventsOwed.has(target)) return;
    scrollEventsOwed.add(target);
    setTimeout(function () { scrollEventsOwed.delete(target); target.dispatchEvent(new Event('scroll')); }, 0);
  },
  enumerable: true, configurable: true
});

)JS"
R"JS(
// ---- the Navigation API (HTML 7.2.6): window.navigation over this document's session history entries. The native
// side (G.__navigation) keeps the entries with their keys, ids and navigation API state, and tells this side when
// the browser itself navigates, so that the navigate event can be cancelled or intercepted.
var navNative = G.__navigation;
delete G.__navigation;
function makeEvent(proto, type, init, fields) {
  var e = new Event(type, { bubbles: false, cancelable: !!(init && init.cancelable) });
  Object.setPrototypeOf(e, proto);
  for (var k in fields) Object.defineProperty(e, k, { value: fields[k], enumerable: true, configurable: true });
  return e;
}
function NavigationHistoryEntry() { throw new TypeError('Illegal constructor'); }
NavigationHistoryEntry.prototype = Object.create(EventTarget.prototype, {
  constructor: { value: NavigationHistoryEntry, writable: true, configurable: true },
  url: { get: function () { return this._e[0]; }, configurable: true, enumerable: true },
  key: { get: function () { return this._e[1]; }, configurable: true, enumerable: true },
  id: { get: function () { return this._e[2]; }, configurable: true, enumerable: true },
  index: { get: function () { return this._i; }, configurable: true, enumerable: true },
  sameDocument: { get: function () { return true; }, configurable: true, enumerable: true },
  getState: { value: function getState() { var s = this._e[3]; return s === undefined ? undefined : structuredClone(s); }, writable: true, configurable: true }
});
var entryObjects = new Map();
function entryAt(i, data) {
  var o = entryObjects.get(data[2]);
  if (!o) { o = Object.create(NavigationHistoryEntry.prototype); o.ondispose = null; entryObjects.set(data[2], o); }
  o._i = i; o._e = data;
  return o;
}
function currentEntry() { var list = navNative.entries(), i = navNative.index(); return entryAt(i, list[i]); }
function NavigationDestination() { throw new TypeError('Illegal constructor'); }
NavigationDestination.prototype = {
  constructor: NavigationDestination,
  getState: function getState() { return this._state === undefined ? undefined : structuredClone(this._state); }
};
function destination(url, entry, state) {
  var d = Object.create(NavigationDestination.prototype);
  d.url = url; d.key = entry ? entry.key : ''; d.id = entry ? entry.id : ''; d.index = entry ? entry.index : -1;
  d.sameDocument = !!entry || sameDocumentUrl(url); d._state = state;
  return d;
}
function NavigateEvent(type, init) {
  if (!new.target) throw new TypeError("Constructor NavigateEvent requires 'new'");
  init = init || {};
  var e = makeEvent(NavigateEvent.prototype, type, init, {
    navigationType: init.navigationType || 'push', destination: init.destination, canIntercept: !!init.canIntercept,
    userInitiated: !!init.userInitiated, hashChange: !!init.hashChange, signal: init.signal || new AbortController().signal,
    formData: init.formData || null, downloadRequest: init.downloadRequest == null ? null : String(init.downloadRequest),
    info: init.info, hasUAVisualTransition: !!init.hasUAVisualTransition, sourceElement: init.sourceElement || null });
  e._handlers = []; e._intercepted = false;
  return e;
}
NavigateEvent.prototype = Object.create(Event.prototype, {
  constructor: { value: NavigateEvent, writable: true, configurable: true },
  intercept: { value: function intercept(options) {
    if (!this.canIntercept) throw new DOMException('This navigation cannot be intercepted', 'SecurityError');
    if (!this._dispatching) throw new DOMException('intercept() must be called while the navigate event is dispatched', 'InvalidStateError');
    this._intercepted = true;
    if (options && typeof options.handler === 'function') this._handlers.push(options.handler);
  }, writable: true, configurable: true },
  scroll: { value: function scroll() {}, writable: true, configurable: true }
});
function NavigationCurrentEntryChangeEvent(type, init) {
  if (!new.target) throw new TypeError("Constructor NavigationCurrentEntryChangeEvent requires 'new'");
  init = init || {};
  return makeEvent(NavigationCurrentEntryChangeEvent.prototype, type, init, { navigationType: init.navigationType == null ? null : init.navigationType, from: init.from });
}
NavigationCurrentEntryChangeEvent.prototype = Object.create(Event.prototype, { constructor: { value: NavigationCurrentEntryChangeEvent, writable: true, configurable: true } });
function NavigationTransition() { throw new TypeError('Illegal constructor'); }
function Navigation() { throw new TypeError('Illegal constructor'); }
Navigation.prototype = Object.create(EventTarget.prototype, {
  constructor: { value: Navigation, writable: true, configurable: true },
  entries: { value: function entries() { return navNative.entries().map(function (e, i) { return entryAt(i, e); }); }, writable: true, configurable: true },
  currentEntry: { get: function () { return currentEntry(); }, configurable: true, enumerable: true },
  canGoBack: { get: function () { return navNative.index() > 0; }, configurable: true, enumerable: true },
  canGoForward: { get: function () { return navNative.index() < navNative.entries().length - 1; }, configurable: true, enumerable: true },
  updateCurrentEntry: { value: function updateCurrentEntry(options) {
    if (!options || !('state' in options)) throw new TypeError("updateCurrentEntry: 'state' is required");
    navNative.setState(structuredClone(options.state));
    var from = lastCurrent;
    lastCurrent = currentEntry();
    navigation.dispatchEvent(new NavigationCurrentEntryChangeEvent('currententrychange', { navigationType: null, from: from }));
  }, writable: true, configurable: true },
  navigate: { value: function navigate(url, options) {
    options = options || {};
    try { url = new URL(String(url), document.baseURI).href; } catch (e) { return failed(new DOMException('Invalid URL', 'SyntaxError')); }
    var state = options.state === undefined ? undefined : structuredClone(options.state);
    // "auto" replaces the entry for the same URL, and while the document is still loading (HTML 7.4.2.2)
    var mode = options.history === 'replace' || (options.history !== 'push' && (url === location.href || document.readyState !== 'complete')) ? 'replace' : 'push';
    if (url.split('#')[0] !== location.href.split('#')[0] && /^javascript:/i.test(url)) mode = 'replace';
    return run(mode, url, state, options.info, function (r) {
      if (r.intercepted) { navNative.commit(url, state, mode === 'replace'); return 'done'; }
      navNative.go(url, mode === 'replace');  // a fragment navigation, or another document
      return sameDocumentUrl(url) ? 'later' : 'away';
    });
  }, writable: true, configurable: true },
  reload: { value: function reload(options) {
    options = options || {};
    var state = 'state' in options ? structuredClone(options.state) : currentEntry().getState();
    return run('reload', location.href, state, options.info, function (r) {
      if (r.intercepted) { navNative.setState(state); return 'done'; }
      navNative.reload();
      return 'away';
    });
  }, writable: true, configurable: true },
  traverseTo: { value: function traverseTo(key, options) {
    var list = navNative.entries(), at = navNative.index(), to = -1;
    for (var i = 0; i < list.length; ++i) if (list[i][1] === key) to = i;
    if (to < 0) return failed(new DOMException('No entry with that key', 'InvalidStateError'));
    if (to === at) { var e = currentEntry(); return { committed: Promise.resolve(e), finished: Promise.resolve(e) }; }
    var target = entryAt(to, list[to]);
    var r = fireNavigate('traverse', target.url, target, target.getState(), options && options.info, true, false);
    if (r.canceled) return failed(new DOMException('The navigation was cancelled', 'AbortError'));
    pending = r;
    navNative.traverse(to - at);
    var settled = settle(r);
    return { committed: settled.committed, finished: settled.finished };
  }, writable: true, configurable: true },
  back: { value: function back(options) {
    var at = navNative.index();
    if (at === 0) return failed(new DOMException('Cannot go back', 'InvalidStateError'));
    return this.traverseTo(navNative.entries()[at - 1][1], options);
  }, writable: true, configurable: true },
  forward: { value: function forward(options) {
    var list = navNative.entries(), at = navNative.index();
    if (at >= list.length - 1) return failed(new DOMException('Cannot go forward', 'InvalidStateError'));
    return this.traverseTo(list[at + 1][1], options);
  }, writable: true, configurable: true }
});
var navigation = Object.create(Navigation.prototype);
navigation.onnavigate = navigation.onnavigatesuccess = navigation.onnavigateerror = navigation.oncurrententrychange = null;
navigation.transition = null;
var lastCurrent = currentEntry();
navigation.activation = { entry: lastCurrent, from: null, navigationType: 'push' };
var pending = null;  // a navigation whose commit the browser carries out: its handlers run once it is committed
var ongoing = null;  // the navigation whose navigatesuccess or navigateerror has not come yet (HTML 7.2.6.10)
function navigateError(r, error) {
  if (navigation.transition && navigation.transition === r.transition) navigation.transition = null;
  var ev = new Event('navigateerror');
  Object.defineProperty(ev, 'error', { value: error, enumerable: true });
  Object.defineProperty(ev, 'message', { value: String(error && error.message || error), enumerable: true });
  navigation.dispatchEvent(ev);
}
// A new navigation aborts the one still under way: its signal, navigateerror, and its finished promise
function abortOngoing() {
  var r = ongoing;
  ongoing = null;
  if (!r || r.settled) return;
  r.settled = true;
  var error = new DOMException('The navigation was aborted', 'AbortError');
  r.controller.abort(error);
  navigateError(r, error);
  if (r.reject) r.reject(error);
}
function failed(error) {
  var p = Promise.reject(error), q = Promise.reject(error);
  p.catch(function () {}); q.catch(function () {});
  return { committed: p, finished: q };
}
function sameDocumentUrl(url) { return url.split('#')[0] === location.href.split('#')[0] && url.indexOf('#') >= 0; }
// The navigate event (HTML 7.4.2.3, "inner navigate event firing algorithm")
function fireNavigate(type, url, entry, state, info, cancelable, user) {
  abortOngoing();
  var controller = new AbortController();
  var ev = new NavigateEvent('navigate', {
    navigationType: type, destination: destination(url, entry, state), cancelable: cancelable, userInitiated: user,
    canIntercept: navNative.canRewrite(url), hashChange: type !== 'traverse' && type !== 'reload' && sameDocumentUrl(url) && url !== location.href,
    info: info, signal: controller.signal });
  ev._dispatching = true;
  var go = navigation.dispatchEvent(ev);
  ev._dispatching = false;
  var r = { canceled: !go && ev.cancelable, intercepted: ev._intercepted, handlers: ev._handlers, type: type, controller: controller };
  if (r.canceled) {
    var error = new DOMException('The navigation was cancelled', 'AbortError');
    controller.abort(error);
    r.settled = true;
    navigateError(r, error);
  } else ongoing = r;
  return r;
}
function entryChanged(type) {
  var from = lastCurrent;
  lastCurrent = currentEntry();
  if (from === lastCurrent && type !== 'reload') return;
  navigation.dispatchEvent(new NavigationCurrentEntryChangeEvent('currententrychange', { navigationType: type, from: from }));
}
// After the commit: the intercepting handlers, then navigatesuccess or navigateerror
function settle(r) {
  var commit, done, fail;
  var committed = new Promise(function (res) { commit = res; });
  var finished = new Promise(function (res, rej) { done = res; fail = rej; });
  finished.catch(function () {});
  r.reject = fail;
  r.onCommit = function () {
    var entry = currentEntry();
    commit(entry);
    var results = r.handlers.map(function (h) { try { return Promise.resolve(h()); } catch (e) { return Promise.reject(e); } });
    var all = Promise.all(results);
    var transition = Object.create(NavigationTransition.prototype);
    transition.navigationType = r.type; transition.from = r.from; transition.finished = all.then(function () {});
    transition.finished.catch(function () {});
    navigation.transition = transition;
    r.transition = transition;
    all.then(function () {
      if (r.settled) return;
      r.settled = true;
      if (ongoing === r) ongoing = null;
      if (navigation.transition === transition) navigation.transition = null;
      navigation.dispatchEvent(new Event('navigatesuccess'));
      done(entry);
    }, function (error) {
      if (r.settled) return;
      r.settled = true;
      if (ongoing === r) ongoing = null;
      navigateError(r, error);
      fail(error);
    });
  };
  return { committed: committed, finished: finished };
}
// A navigation the page asked for: 'done' when commitFn committed it here, 'later' when the browser will (the
// changed hook then finishes it), 'away' when another document is coming
function run(type, url, state, info, commitFn) {
  var target = type === 'reload' ? currentEntry() : null;
  var r = fireNavigate(type, url, target, state, info, true, false);
  if (r.canceled) return failed(new DOMException('The navigation was cancelled', 'AbortError'));
  r.from = lastCurrent;
  var settled = settle(r);
  var how = commitFn(r);
  if (how === 'done') { entryChanged(type); r.onCommit(); }
  else if (how === 'later') pending = r;
  return how === 'away' ? { committed: new Promise(function () {}), finished: new Promise(function () {}) } : settled;
}
navNative.hooks({
  // the browser is about to navigate (a link, a location change, a form): 0 go on, 1 cancelled, 2 intercepted
  before: function (url, type, user) {
    var traverse = type === 'traverse';
    var r = fireNavigate(type, url, null, undefined, undefined, !traverse, !!user);
    if (r.canceled) return 1;
    r.from = lastCurrent;
    settle(r);
    if (r.intercepted && !traverse && !sameDocumentUrl(url)) {
      navNative.commit(url, undefined, type === 'replace');
      entryChanged(type);
      r.onCommit();
      return 2;
    }
    pending = r;
    return 0;
  },
  // history.pushState() or replaceState(): true when cancelled
  history: function (url, type) {
    var r = fireNavigate(type, url, null, undefined, undefined, true, false);
    if (r.canceled) return true;
    r.from = lastCurrent;
    settle(r);
    pending = r;
    return false;
  },
  // the current entry changed without a load: currententrychange, then what intercepted the navigation
  changed: function (type) {
    var r = pending;
    pending = null;
    entryChanged(type);
    if (r && r.onCommit) r.onCommit();
  }
});
Object.defineProperty(G, 'navigation', { get: function () { return navigation; }, configurable: true, enumerable: true });
)JS"
R"JS(
var exported = { IntersectionObserver: IntersectionObserver, IntersectionObserverEntry: IntersectionObserverEntry, ResizeObserver: ResizeObserver,
  ResizeObserverEntry: ResizeObserverEntry, CSSStyleSheet: CSSStyleSheet, DOMException: DOMException, EventTarget: EventTarget, AbortController: AbortController, AbortSignal: AbortSignal, URL: URL, URLSearchParams: URLSearchParams,
  Blob: Blob, File: File, FormData: FormData, Headers: Headers, Request: Request, Response: Response, fetch: fetch, XMLHttpRequest: XMLHttpRequest,
  NodeFilter: NodeFilter, TreeWalker: TreeWalker, MessageChannel: MessageChannel, DOMParser: DOMParser, XMLSerializer: XMLSerializer,
  Navigation: Navigation, NavigateEvent: NavigateEvent, NavigationHistoryEntry: NavigationHistoryEntry, NavigationDestination: NavigationDestination,
  NavigationTransition: NavigationTransition, NavigationCurrentEntryChangeEvent: NavigationCurrentEntryChangeEvent };
for (var name of Object.keys(exported)) Object.defineProperty(G, name, { value: exported[name], writable: true, configurable: true, enumerable: false });
G.webkitURL = URL;
})();
)JS";

}  // namespace tb
