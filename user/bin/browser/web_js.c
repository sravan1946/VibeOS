/* web_js.c - the Web APIs that don't touch the node tree, in JavaScript over js.c's natives: URL/URLSearchParams, fetch
 * (Request/Response/Headers), XMLHttpRequest, FormData/Blob, TextEncoder, atob, localStorage (persisted per origin under
 * ~/.config/browser), document.cookie, Intersection/Resize observers, matchMedia, performance, crypto, Intl stubs, Range
 * and TreeWalker basics. Runs right after dom_js.c's prelude (same global, shares the internals object __v), then removes
 * __d and __v so page scripts can't see them. */
const char js_prelude2[] = R"JS(
(function (g) {
'use strict';
const V = g.__v, d = V.d, D = V.classes, report = V.report, document = V.document, ET = V.ET;
const def = (o, p) => { for (const k of Object.keys(p)) Object.defineProperty(o, k, { value: p[k], writable: true, configurable: true }); };

// ---------- DOMException and friends ----------
const CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5, NoModificationAllowedError: 7, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12, InvalidAccessError: 15, TimeoutError: 23, DataCloneError: 25, AbortError: 20, QuotaExceededError: 22, NetworkError: 19 };
class DOMException extends Error {
    constructor(msg = '', name = 'Error') { super(msg); Object.defineProperty(this, 'name', { value: name, configurable: true }); this.code = CODES[name] || 0; }
}
def(g, { DOMException });

// ---------- encoding ----------
const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
def(g, {
    btoa(s) {
        s = String(s);
        let o = '';
        for (let i = 0; i < s.length; i += 3) {
            const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
            if (a > 255 || b > 255 || c > 255) throw new DOMException('The string to be encoded contains characters outside of the Latin1 range.', 'InvalidCharacterError');
            const n = (a << 16) | ((b || 0) << 8) | (c || 0);
            o += B64[n >> 18] + B64[(n >> 12) & 63] + (i + 1 < s.length ? B64[(n >> 6) & 63] : '=') + (i + 2 < s.length ? B64[n & 63] : '=');
        }
        return o;
    },
    atob(s) {
        s = String(s).replace(/[\t\n\f\r ]/g, '').replace(/=+$/, '');
        if (/[^A-Za-z0-9+\/\-_]/.test(s) || s.length % 4 === 1) throw new DOMException('The string to be decoded is not correctly encoded.', 'InvalidCharacterError');
        let o = '', n = 0, bits = 0;
        for (const ch of s) {
            let v = B64.indexOf(ch);
            if (v < 0) v = ch === '-' ? 62 : 63;
            n = (n << 6) | v; bits += 6;
            if (bits >= 8) { bits -= 8; o += String.fromCharCode((n >> bits) & 255); }
        }
        return o;
    },
});
class TextEncoder {
    get encoding() { return 'utf-8'; }
    encode(s = '') {
        s = String(s);
        const out = [];
        for (let i = 0; i < s.length; i++) {
            let c = s.codePointAt(i);
            if (c > 0xffff) i++;
            if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
            if (c < 0x80) out.push(c);
            else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 63));
            else if (c < 0x10000) out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
            else out.push(0xf0 | (c >> 18), 0x80 | ((c >> 12) & 63), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        }
        return new Uint8Array(out);
    }
    encodeInto(s, u8) { const e = this.encode(s); const n = Math.min(e.length, u8.length); u8.set(e.subarray(0, n)); return { read: s.length, written: n }; }
}
class TextDecoder {
    constructor(enc = 'utf-8', o) { this.encoding = String(enc).toLowerCase().replace('utf8', 'utf-8'); this.fatal = !!(o && o.fatal); this.ignoreBOM = false; }
    decode(b) {
        if (b === undefined) return '';
        const u = b instanceof ArrayBuffer ? new Uint8Array(b) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
        let out = '';
        if (this.encoding === 'latin1' || this.encoding === 'iso-8859-1' || this.encoding === 'windows-1252' || this.encoding === 'ascii') { for (let i = 0; i < u.length; i++) out += String.fromCharCode(u[i]); return out; }
        for (let i = 0; i < u.length;) {
            const c = u[i++];
            let cp = c, n = 0;
            if (c >= 0xf0 && c < 0xf8) { cp = c & 7; n = 3; } else if (c >= 0xe0) { cp = c & 15; n = 2; } else if (c >= 0xc0) { cp = c & 31; n = 1; } else if (c >= 0x80) cp = 0xfffd;
            for (; n > 0 && i < u.length && (u[i] & 0xc0) === 0x80; n--) cp = (cp << 6) | (u[i++] & 63);
            if (n > 0) cp = 0xfffd;
            if (i === 3 && cp === 0xfeff && !this.ignoreBOM) continue;
            out += String.fromCodePoint(cp > 0x10ffff ? 0xfffd : cp);
        }
        return out;
    }
}
def(g, { TextEncoder, TextDecoder });

// ---------- URL ----------
const URLRE = /^([a-z][a-z0-9+.-]*:)(?:\/\/(?:([^\/?#@]*)@)?(\[[^\]]*\]|[^\/?#:]*)(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i;
const DEFPORT = { 'http:': '80', 'https:': '443', 'ftp:': '21', 'ws:': '80', 'wss:': '443' };
function urlparts(h) {
    const m = URLRE.exec(h);
    if (!m) return null;
    const p = m[1].toLowerCase();
    let user = '', pass = '';
    if (m[2]) { const i = m[2].indexOf(':'); user = i < 0 ? m[2] : m[2].slice(0, i); pass = i < 0 ? '' : m[2].slice(i + 1); }
    return { protocol: p, username: user, password: pass, hostname: (m[3] || '').toLowerCase(), port: m[4] && m[4] !== DEFPORT[p] ? m[4] : '', pathname: m[5] || (m[3] !== undefined && DEFPORT[p] ? '/' : ''),
             search: m[6] && m[6] !== '?' ? m[6] : '', hash: m[7] && m[7] !== '#' ? m[7] : '', hasAuthority: m[3] !== undefined };
}
def(g, { __urlparts: urlparts });
const enc = s => encodeURIComponent(s).replace(/%20/g, '+').replace(/[!'()~]/g, c => '%' + c.charCodeAt(0).toString(16).toUpperCase());
const dec = s => { s = s.replace(/\+/g, ' '); try { return decodeURIComponent(s); } catch (e) { return s.replace(/%([0-9a-f]{2})/gi, (m, h) => String.fromCharCode(parseInt(h, 16))); } };
class URLSearchParams {
    constructor(init) {
        Object.defineProperty(this, '__l', { value: [], writable: true }); Object.defineProperty(this, '__u', { value: null, writable: true });
        if (init === undefined || init === null) return;
        if (init instanceof URLSearchParams) this.__l = init.__l.map(p => p.slice());
        else if (typeof init === 'object' && Symbol.iterator in init) for (const [k, v] of init) this.__l.push([String(k), String(v)]);
        else if (typeof init === 'object') for (const k of Object.keys(init)) this.__l.push([k, String(init[k])]);
        else this.__parse(String(init));
    }
    __parse(s) { this.__l = []; s = s.replace(/^\?/, ''); if (s) for (const p of s.split('&')) { if (!p) continue; const i = p.indexOf('='); this.__l.push(i < 0 ? [dec(p), ''] : [dec(p.slice(0, i)), dec(p.slice(i + 1))]); } }
    __upd() { if (this.__u) this.__u.__set('search', this.toString() ? '?' + this.toString() : ''); }
    append(k, v) { this.__l.push([String(k), String(v)]); this.__upd(); }
    delete(k, v) { k = String(k); this.__l = this.__l.filter(p => !(p[0] === k && (v === undefined || p[1] === String(v)))); this.__upd(); }
    get(k) { const p = this.__l.find(p => p[0] === String(k)); return p ? p[1] : null; }
    getAll(k) { return this.__l.filter(p => p[0] === String(k)).map(p => p[1]); }
    has(k, v) { return this.__l.some(p => p[0] === String(k) && (v === undefined || p[1] === String(v))); }
    set(k, v) { k = String(k); v = String(v); const i = this.__l.findIndex(p => p[0] === k); if (i < 0) this.__l.push([k, v]); else { this.__l[i][1] = v; this.__l = this.__l.filter((p, j) => j <= i || p[0] !== k); } this.__upd(); }
    sort() { this.__l.sort((a, b) => a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0); this.__upd(); }
    forEach(f, t) { for (const [k, v] of this.__l) f.call(t, v, k, this); }
    keys() { return this.__l.map(p => p[0])[Symbol.iterator](); }
    values() { return this.__l.map(p => p[1])[Symbol.iterator](); }
    entries() { return this.__l.map(p => [p[0], p[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    get size() { return this.__l.length; }
    toString() { return this.__l.map(([k, v]) => enc(k) + '=' + enc(v)).join('&'); }
}
const blobs = new Map();
class URL {
    constructor(u, base) {
        u = String(u);
        const abs = base !== undefined ? d.resolve(u, String(base)) : u;
        const p = urlparts(abs);
        if (!p || (base === undefined && !/^[a-z][a-z0-9+.-]*:/i.test(u))) throw new TypeError("Failed to construct 'URL': Invalid URL");
        Object.defineProperty(this, '__p', { value: p, writable: true });
        Object.defineProperty(this, '__sp', { value: new URLSearchParams(p.search), writable: true });
        this.__sp.__u = this;
    }
    __set(k, v) { this.__p[k] = v; if (k === 'search') this.__sp.__l = new URLSearchParams(v).__l; }
    get protocol() { return this.__p.protocol; } set protocol(v) { this.__p.protocol = String(v).replace(/:?$/, ':'); }
    get username() { return this.__p.username; } set username(v) { this.__p.username = String(v); }
    get password() { return this.__p.password; } set password(v) { this.__p.password = String(v); }
    get hostname() { return this.__p.hostname; } set hostname(v) { this.__p.hostname = String(v).toLowerCase(); }
    get port() { return this.__p.port; } set port(v) { v = String(v); this.__p.port = v === DEFPORT[this.__p.protocol] ? '' : v; }
    get host() { return this.__p.hostname + (this.__p.port ? ':' + this.__p.port : ''); }
    set host(v) { const m = /^([^:]*)(?::(\d*))?$/.exec(String(v)); if (m) { this.hostname = m[1]; this.port = m[2] || ''; } }
    get pathname() { return this.__p.pathname; } set pathname(v) { v = String(v); this.__p.pathname = v.startsWith('/') ? v : '/' + v; }
    get search() { return this.__p.search; } set search(v) { v = String(v); this.__set('search', v && v !== '?' ? (v[0] === '?' ? v : '?' + v) : ''); }
    get hash() { return this.__p.hash; } set hash(v) { v = String(v); this.__p.hash = v && v !== '#' ? (v[0] === '#' ? v : '#' + v) : ''; }
    get searchParams() { return this.__sp; }
    get origin() { const p = this.__p; return DEFPORT[p.protocol] ? p.protocol + '//' + this.host : 'null'; }
    get href() { const p = this.__p; return p.protocol + (p.hasAuthority ? '//' + (p.username ? p.username + (p.password ? ':' + p.password : '') + '@' : '') + this.host : '') + p.pathname + p.search + p.hash; }
    set href(v) { const p = urlparts(String(v)); if (!p) throw new TypeError('Invalid URL'); this.__p = p; this.__sp.__l = new URLSearchParams(p.search).__l; }
    toString() { return this.href; } toJSON() { return this.href; }
    static canParse(u, b) { try { new URL(u, b); return true; } catch (e) { return false; } }
    static parse(u, b) { try { return new URL(u, b); } catch (e) { return null; } }
    static createObjectURL(b) { const u = 'blob:' + location.origin + '/' + Math.random().toString(16).slice(2); blobs.set(u, b); return u; }
    static revokeObjectURL(u) { blobs.delete(u); }
}
def(g, { URL, URLSearchParams });

// ---------- Blob, File, FormData, FileReader ----------
class Blob {
    constructor(parts = [], o) {
        let s = '';
        for (const p of parts) s += p instanceof Blob ? p.__s : typeof p === 'string' ? p : (p instanceof ArrayBuffer || ArrayBuffer.isView(p)) ? new TextDecoder().decode(p) : String(p);
        Object.defineProperty(this, '__s', { value: s, writable: true });
        this.type = o && o.type ? String(o.type).toLowerCase() : '';
    }
    get size() { return new TextEncoder().encode(this.__s).length; }
    text() { return Promise.resolve(this.__s); }
    arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this.__s).buffer); }
    slice(a, b, t) { const e = new Blob([], { type: t }); e.__s = this.__s.slice(a, b); return e; }
    stream() { return new g.ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode(this.__s)); c.close(); } }); }
}
class File extends Blob { constructor(parts, name, o) { super(parts, o); this.name = String(name); this.lastModified = o && o.lastModified || Date.now(); } }
class FormData {
    constructor(form) {
        Object.defineProperty(this, '__l', { value: [] });
        if (form && form.elements) for (const e of form.elements) {
            const n = e.getAttribute('name'), t = e.type;
            if (!n || e.disabled || t === 'submit' || t === 'button' || t === 'reset' || t === 'image' || t === 'file') continue;
            if ((t === 'checkbox' || t === 'radio') && !e.checked) continue;
            if (e.localName === 'select' && e.multiple) { for (const o of e.options) if (o.selected) this.__l.push([n, o.value]); continue; }
            this.__l.push([n, e.value]);
        }
    }
    append(k, v, f) { this.__l.push([String(k), v instanceof Blob ? v : String(v)]); }
    delete(k) { k = String(k); for (let i = this.__l.length; i--;) if (this.__l[i][0] === k) this.__l.splice(i, 1); }
    get(k) { const p = this.__l.find(p => p[0] === String(k)); return p ? p[1] : null; }
    getAll(k) { return this.__l.filter(p => p[0] === String(k)).map(p => p[1]); }
    has(k) { return this.__l.some(p => p[0] === String(k)); }
    set(k, v) { k = String(k); const i = this.__l.findIndex(p => p[0] === k); const val = v instanceof Blob ? v : String(v); if (i < 0) this.__l.push([k, val]); else { this.__l[i][1] = val; for (let j = this.__l.length; j-- > i + 1;) if (this.__l[j][0] === k) this.__l.splice(j, 1); } }
    forEach(f, t) { for (const [k, v] of this.__l) f.call(t, v, k, this); }
    keys() { return this.__l.map(p => p[0])[Symbol.iterator](); } values() { return this.__l.map(p => p[1])[Symbol.iterator](); }
    entries() { return this.__l.map(p => [p[0], p[1]])[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); }
    toString() { return this.__l.map(([k, v]) => enc(k) + '=' + enc(v instanceof Blob ? v.name || '' : v)).join('&'); }
}
class FileReader extends D.EventTarget {
    constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
    __read(b, f) { setTimeout(() => { try { this.result = f(b); this.readyState = 2; } catch (e) { this.error = e; } for (const t of ['load', 'loadend']) this.dispatchEvent(new D.ProgressEvent(t)); }, 0); }
    readAsText(b) { this.__read(b, b => b.__s); }
    readAsDataURL(b) { this.__read(b, b => 'data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(String.fromCharCode(...new TextEncoder().encode(b.__s)))); }
    readAsArrayBuffer(b) { this.__read(b, b => new TextEncoder().encode(b.__s).buffer); }
    abort() {}
}
class AbortSignal extends D.EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(r) { const s = new AbortSignal(); s.aborted = true; s.reason = r === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : r; return s; }
    static timeout(ms) { const s = new AbortSignal(); setTimeout(() => s.__abort(new DOMException('signal timed out', 'TimeoutError')), ms); return s; }
    static any(a) { const s = new AbortSignal(); for (const x of a) { if (x.aborted) { s.__abort(x.reason); break; } x.addEventListener('abort', () => s.__abort(x.reason)); } return s; }
    __abort(r) { if (this.aborted) return; this.aborted = true; this.reason = r === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : r; this.dispatchEvent(new D.Event('abort')); }
}
class AbortController { constructor() { this.signal = new AbortSignal(); } abort(r) { this.signal.__abort(r); } }
def(g, { Blob, File, FormData, FileReader, AbortSignal, AbortController });

// ---------- fetch ----------
const FM = new Map();
function raw(url, body, cb) {                   // one request through the helper process: cb(status, text, ctype, body, err, url)
    const id = d.fetch(String(url), body);
    FM.set(id, cb);
}
g.__fetchDone = (id, status, text, ct, body, err, url) => { const cb = FM.get(id); FM.delete(id); if (cb) try { cb(status, text, ct, body, err, url); } catch (e) { report(e); } };
class Headers {
    constructor(init) {
        Object.defineProperty(this, '__m', { value: new Map() });
        if (init instanceof Headers) for (const [k, v] of init) this.append(k, v);
        else if (init && Symbol.iterator in Object(init)) for (const [k, v] of init) this.append(k, v);
        else if (init) for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { k = String(k).toLowerCase(); v = String(v).trim(); this.__m.set(k, this.__m.has(k) ? this.__m.get(k) + ', ' + v : v); }
    set(k, v) { this.__m.set(String(k).toLowerCase(), String(v).trim()); }
    get(k) { const v = this.__m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    has(k) { return this.__m.has(String(k).toLowerCase()); }
    delete(k) { this.__m.delete(String(k).toLowerCase()); }
    forEach(f, t) { for (const [k, v] of this) f.call(t, v, k, this); }
    getSetCookie() { return []; }
    keys() { return [...this.__m.keys()].sort()[Symbol.iterator](); } values() { return [...this.__m].sort().map(p => p[1])[Symbol.iterator](); }
    entries() { return [...this.__m].sort((a, b) => a[0] < b[0] ? -1 : 1)[Symbol.iterator](); } [Symbol.iterator]() { return this.entries(); }
}
function bodyText(b) {
    if (b === undefined || b === null) return null;
    if (typeof b === 'string') return b;
    if (b instanceof URLSearchParams || b instanceof FormData) return b.toString();
    if (b instanceof Blob) return b.__s;
    if (b instanceof ArrayBuffer || ArrayBuffer.isView(b)) return new TextDecoder().decode(b);
    return String(b);
}
class Body {
    __init(b, ct) { Object.defineProperty(this, '__b', { value: bodyText(b), writable: true }); this.bodyUsed = false; if (ct && !this.headers.has('content-type')) this.headers.set('content-type', ct); }
    __take() { if (this.bodyUsed) return Promise.reject(new TypeError('body stream already read')); this.bodyUsed = true; return Promise.resolve(this.__b === null ? '' : this.__b); }
    text() { return this.__take(); }
    json() { return this.__take().then(JSON.parse); }
    arrayBuffer() { return this.__take().then(s => new TextEncoder().encode(s).buffer); }
    bytes() { return this.__take().then(s => new TextEncoder().encode(s)); }
    blob() { return this.__take().then(s => new Blob([s], { type: this.headers.get('content-type') || '' })); }
    formData() { return this.__take().then(s => { const f = new FormData(); for (const [k, v] of new URLSearchParams(s)) f.append(k, v); return f; }); }
    get body() { if (this.__b === null) return null; const self = this; return { getReader() { let done = false; return { read() { if (done) return Promise.resolve({ done: true, value: undefined }); done = true; return self.arrayBuffer().then(b => ({ done: false, value: new Uint8Array(b) })); }, releaseLock() {}, cancel() { return Promise.resolve(); }, closed: Promise.resolve() }; }, locked: false, cancel() { return Promise.resolve(); } }; }
}
class Request extends Body {
    constructor(input, init = {}) {
        super();
        const r = input instanceof Request ? input : null;
        this.url = d.resolve(String(r ? r.url : input && input.href !== undefined ? input.href : input));
        this.method = String(init.method || (r && r.method) || 'GET').toUpperCase();
        this.headers = new Headers(init.headers || (r && r.headers));
        this.signal = init.signal || (r && r.signal) || new AbortSignal();
        this.credentials = init.credentials || 'same-origin'; this.mode = init.mode || 'cors'; this.cache = init.cache || 'default'; this.redirect = init.redirect || 'follow';
        this.referrer = 'about:client'; this.referrerPolicy = ''; this.integrity = ''; this.keepalive = false; this.destination = '';
        this.__init(init.body !== undefined ? init.body : r ? r.__b : null, init.body instanceof URLSearchParams ? 'application/x-www-form-urlencoded;charset=UTF-8' : typeof init.body === 'string' ? 'text/plain;charset=UTF-8' : '');
    }
    clone() { return new Request(this); }
}
class Response extends Body {
    constructor(body = null, init = {}) {
        super();
        this.status = init.status === undefined ? 200 : init.status; this.statusText = init.statusText || ''; this.ok = this.status >= 200 && this.status < 300;
        this.headers = new Headers(init.headers); this.url = ''; this.redirected = false; this.type = 'basic';
        this.__init(body, typeof body === 'string' ? 'text/plain;charset=UTF-8' : '');
    }
    clone() { const r = new Response(this.__b, { status: this.status, statusText: this.statusText, headers: this.headers }); r.url = this.url; return r; }
    static error() { const r = new Response(null, { status: 0 }); r.type = 'error'; return r; }
    static json(o, init) { const r = new Response(JSON.stringify(o), init); r.headers.set('content-type', 'application/json'); return r; }
    static redirect(u, s = 302) { return new Response(null, { status: s, headers: { location: String(u) } }); }
}
function dataURL(u) {
    const m = /^data:([^,]*?)(;base64)?,([\s\S]*)$/i.exec(u);
    if (!m) return null;
    let body = m[3];
    try { body = m[2] ? new TextDecoder().decode(Uint8Array.from(atob(decodeURIComponent(body)), c => c.charCodeAt(0))) : decodeURIComponent(body); } catch (e) {}
    const r = new Response(body, { status: 200, statusText: 'OK' });
    r.headers.set('content-type', m[1] || 'text/plain;charset=US-ASCII'); r.url = u;
    return r;
}
function fetch(input, init) {
    return new Promise((res, rej) => {
        const req = new Request(input, init);
        const aborted = () => rej(req.signal.reason || new DOMException('The operation was aborted.', 'AbortError'));
        if (req.signal.aborted) return aborted();
        const dr = /^data:/i.test(req.url) ? dataURL(req.url) : blobs.has(req.url) ? new Response(blobs.get(req.url).__s, { status: 200, headers: { 'content-type': blobs.get(req.url).type } }) : null;
        if (dr) return setTimeout(() => res(dr), 0);
        const m = req.method;
        if (m !== 'GET' && m !== 'HEAD' && m !== 'POST') return rej(new TypeError('fetch: ' + m + ' requests are not supported by this browser'));
        let done = false;
        req.signal.addEventListener('abort', () => { if (!done) { done = true; aborted(); } });
        raw(req.url.replace(/#.*$/, ''), m === 'POST' ? (req.__b || '') : null, (status, text, ct, body, err, url) => {
            if (done) return;
            done = true;
            if (err !== null) return rej(new TypeError('Failed to fetch (' + err + ': ' + req.url + ')'));
            const r = new Response(m === 'HEAD' ? '' : body, { status, statusText: text });
            r.headers.set('content-type', ct || ''); r.url = url; r.redirected = url !== req.url;
            res(r);
        });
    });
}
def(g, { Headers, Request, Response, fetch });
def(g, { ReadableStream: class ReadableStream { constructor(s) { const q = []; let closed = false; const c = { enqueue: v => q.push(v), close: () => { closed = true; }, error() {} }; if (s && s.start) s.start(c); this.getReader = () => ({ read: () => Promise.resolve(q.length ? { done: false, value: q.shift() } : { done: true, value: undefined }), releaseLock() {}, cancel: () => Promise.resolve() }); } } });

// ---------- XMLHttpRequest ----------
class XMLHttpRequestEventTarget extends D.EventTarget {}
class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = ''; this.responseType = ''; this.responseURL = ''; this.timeout = 0; this.withCredentials = false; this.upload = new XMLHttpRequestEventTarget(); this.__h = {}; this.__rh = new Headers(); }
    __ev(t) { const ev = new D.ProgressEvent(t, { loaded: this.responseText.length, total: this.responseText.length, lengthComputable: true }); this.dispatchEvent(ev); }
    __state(n) { this.readyState = n; this.__ev('readystatechange'); }
    open(m, u, async = true) { this.__m = String(m).toUpperCase(); this.__u = d.resolve(String(u)); this.__async = async !== false; this.__sent = false; this.__abort = false; this.__state(1); }
    setRequestHeader(k, v) { this.__h[String(k).toLowerCase()] = String(v); }
    getResponseHeader(k) { return this.__rh.get(k); }
    getAllResponseHeaders() { return [...this.__rh].map(([k, v]) => k + ': ' + v).join('\r\n') + (this.__rh.__m.size ? '\r\n' : ''); }
    overrideMimeType() {}
    abort() { this.__abort = true; if (this.readyState > 1 && this.readyState < 4) { this.readyState = 4; this.__ev('readystatechange'); this.__ev('abort'); this.__ev('loadend'); } this.readyState = 0; }
    __finish(status, text, ct, body, err, url) {
        if (this.__abort) return;
        if (err !== null) { this.status = 0; this.readyState = 4; this.__ev('readystatechange'); this.__ev('error'); this.__ev('loadend'); return; }
        this.status = status; this.statusText = text; this.responseURL = url; this.__rh.set('content-type', ct || '');
        this.responseText = body; this.__state(2); this.__state(3);
        const rt = this.responseType;
        if (rt === 'json') { try { this.response = JSON.parse(body); } catch (e) { this.response = null; } }
        else if (rt === 'arraybuffer') this.response = new TextEncoder().encode(body).buffer;
        else if (rt === 'blob') this.response = new Blob([body], { type: ct });
        else if (rt === 'document') this.response = new g.DOMParser().parseFromString(body, 'text/html');
        else this.response = body;
        this.__ev('progress'); this.__state(4); this.__ev('load'); this.__ev('loadend');
    }
    get responseXML() { return /xml|html/.test(this.__rh.get('content-type') || '') ? new g.DOMParser().parseFromString(this.responseText, 'text/xml') : null; }
    send(body) {
        if (this.readyState !== 1) throw new DOMException('The object is in an invalid state.', 'InvalidStateError');
        this.__sent = true;
        const m = this.__m, u = this.__u;
        const dr = /^data:/i.test(u) ? dataURL(u) : null;
        const text = bodyText(body);
        if (m !== 'GET' && m !== 'HEAD' && m !== 'POST') { setTimeout(() => this.__finish(0, '', '', '', 'unsupported method', u), 0); return; }
        if (dr) { dr.text().then(t => this.__finish(200, 'OK', dr.headers.get('content-type'), t, null, u)); return; }
        if (!this.__async) {                    // synchronous: blocks the page until the download is done
            const t = d.syncGet(u);
            this.__finish(t === null ? 0 : 200, t === null ? '' : 'OK', '', t || '', t === null ? 'network' : null, u);
            return;
        }
        raw(u.replace(/#.*$/, ''), m === 'POST' ? text || '' : null, (...a) => this.__finish(...a));
        if (this.timeout) setTimeout(() => { if (this.readyState < 4 && !this.__abort) { this.__abort = true; this.readyState = 4; this.__ev('readystatechange'); this.__ev('timeout'); this.__ev('loadend'); } }, this.timeout);
        this.__ev('loadstart');
    }
}
for (const [k, v] of [['UNSENT', 0], ['OPENED', 1], ['HEADERS_RECEIVED', 2], ['LOADING', 3], ['DONE', 4]]) XMLHttpRequest[k] = XMLHttpRequest.prototype[k] = v;
def(g, { XMLHttpRequest, XMLHttpRequestEventTarget });

// ---------- storage ----------
function origin() { return location.protocol === 'file:' || !location.host ? 'local' : location.protocol.replace(':', '') + '_' + location.host.replace(':', '_'); }
function storage(persist) {
    let m = new Map();
    const file = 'ls-' + origin();
    if (persist) { try { const j = d.load(file); if (j) m = new Map(Object.entries(JSON.parse(j))); } catch (e) {} }
    let pend = false;
    const flush = () => { if (!persist || pend) return; pend = true; Promise.resolve().then(() => { pend = false; d.save(file, JSON.stringify(Object.fromEntries(m))); }); };
    const api = {
        getItem: k => m.has(String(k)) ? m.get(String(k)) : null,
        setItem: (k, v) => { k = String(k); v = String(v); let n = v.length; for (const [a, b] of m) n += a.length + b.length; if (n > 5 << 20) throw new DOMException('The quota has been exceeded.', 'QuotaExceededError'); const old = m.get(k); m.set(k, v); flush();
            if (old !== v) setTimeout(() => {}, 0); },
        removeItem: k => { m.delete(String(k)); flush(); }, clear: () => { m.clear(); flush(); }, key: i => [...m.keys()][i] ?? null,
    };
    return new Proxy({}, {
        get(t, k) { if (k === 'length') return m.size; if (typeof k === 'symbol') return k === Symbol.toStringTag ? 'Storage' : undefined; if (k in api) return api[k]; return m.has(k) ? m.get(k) : undefined; },
        set(t, k, v) { api.setItem(k, v); return true; }, deleteProperty(t, k) { api.removeItem(k); return true; },
        has(t, k) { return k in api || m.has(k); }, ownKeys() { return [...m.keys()]; },
        getOwnPropertyDescriptor(t, k) { return m.has(k) ? { value: m.get(k), enumerable: true, configurable: true, writable: true } : undefined; },
    });
}
let ls, ss;
try { ls = storage(true); ss = storage(false); } catch (e) { ls = ss = storage(false); }
def(g, { Storage: function Storage() {} });
Object.defineProperties(g, { localStorage: { get() { return ls; }, configurable: true }, sessionStorage: { get() { return ss; }, configurable: true } });

// ---------- cookies (a per-page jar; ponytail: not shared with the network layer yet) ----------
const jar = new Map();
def(g, {
    __cookieGet() { const now = Date.now(); const r = []; for (const [k, c] of jar) { if (c.exp !== null && c.exp <= now) jar.delete(k); else r.push(k + '=' + c.v); } return r.join('; '); },
    __cookieSet(s) {
        const parts = s.split(';');
        const i = parts[0].indexOf('=');
        const k = (i < 0 ? '' : parts[0].slice(0, i)).trim(), v = (i < 0 ? parts[0] : parts[0].slice(i + 1)).trim();
        let exp = null;
        for (const a of parts.slice(1)) {
            const [n, val] = a.split('=').map(x => x.trim());
            if (/^max-age$/i.test(n)) exp = Date.now() + parseInt(val) * 1000;
            else if (/^expires$/i.test(n) && exp === null) { const t = Date.parse(val); if (!isNaN(t)) exp = t; }
        }
        if (exp !== null && exp <= Date.now()) jar.delete(k); else jar.set(k, { v, exp });
    },
});

// ---------- observers, media ----------
class IntersectionObserver {
    constructor(cb, o) { this.__cb = cb; this.root = o && o.root || null; this.rootMargin = o && o.rootMargin || '0px'; this.thresholds = [0]; this.__els = new Set(); }
    observe(e) {
        if (this.__els.has(e)) return; this.__els.add(e);
        setTimeout(() => { if (!this.__els.has(e)) return; const r = e.getBoundingClientRect(); try { this.__cb([{ target: e, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: performance.now() }], this); } catch (x) { report(x); } }, 0);
    }
    unobserve(e) { this.__els.delete(e); } disconnect() { this.__els.clear(); } takeRecords() { return []; }
}
class ResizeObserver {
    constructor(cb) { this.__cb = cb; this.__els = new Set(); }
    observe(e) { if (this.__els.has(e)) return; this.__els.add(e); setTimeout(() => { if (!this.__els.has(e)) return; const r = e.getBoundingClientRect(); const s = [{ inlineSize: r.width, blockSize: r.height }]; try { this.__cb([{ target: e, contentRect: r, borderBoxSize: s, contentBoxSize: s, devicePixelContentBoxSize: s }], this); } catch (x) { report(x); } }, 0); }
    unobserve(e) { this.__els.delete(e); } disconnect() { this.__els.clear(); }
}
class PerformanceObserver { constructor(cb) { this.__cb = cb; } observe() {} disconnect() {} takeRecords() { return []; } static get supportedEntryTypes() { return []; } }
function matchMedia(q) {
    q = String(q);
    const mql = new D.EventTarget();
    Object.defineProperties(mql, { matches: { get() { return d.media(q); } }, media: { value: q } });
    mql.addListener = mql.addEventListener; mql.removeListener = mql.removeEventListener;
    return mql;
}
def(g, { IntersectionObserver, ResizeObserver, PerformanceObserver, matchMedia });
def(g, { MutationObserver: D.MutationObserver });

// ---------- performance, crypto, navigator, screen ----------
const marks = [];
def(g, { performance: Object.assign(new D.EventTarget(), {
    now: () => d.now() - V.T0, timeOrigin: Date.now() - (d.now() - V.T0), toJSON() { return {}; },
    mark(n) { const e = { name: n, entryType: 'mark', startTime: d.now() - V.T0, duration: 0 }; marks.push(e); return e; },
    measure(n, a, b) { const f = x => { const m = marks.filter(e => e.name === x).pop(); return m ? m.startTime : 0; }; const s = a ? f(a) : 0, e = b ? f(b) : d.now() - V.T0; const r = { name: n, entryType: 'measure', startTime: s, duration: e - s }; marks.push(r); return r; },
    clearMarks() { marks.length = 0; }, clearMeasures() {}, getEntries: () => marks.slice(), getEntriesByName: n => marks.filter(e => e.name === n), getEntriesByType: t => marks.filter(e => e.entryType === t),
    timing: { navigationStart: Date.now(), fetchStart: Date.now(), domContentLoadedEventEnd: Date.now(), loadEventEnd: Date.now() }, navigation: { type: 0, redirectCount: 0 },
    memory: { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 64 << 20 } }) });
const hex = n => { let s = ''; for (let i = 0; i < n; i++) s += ((Math.random() * 16) | 0).toString(16); return s; };
def(g, { crypto: {
    getRandomValues(a) { for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; },
    randomUUID() { return hex(8) + '-' + hex(4) + '-4' + hex(3) + '-' + '89ab'[(Math.random() * 4) | 0] + hex(3) + '-' + hex(12); },
    subtle: undefined } });
def(g, { navigator: { userAgent: 'Mozilla/5.0 (X11; VibeOS i686) VibeBrowser/1.0 QuickJS', appName: 'Netscape', appVersion: '5.0 (VibeOS)', appCodeName: 'Mozilla', product: 'Gecko', vendor: '', platform: 'VibeOS', language: 'en-US', languages: ['en-US', 'en'],
    onLine: true, cookieEnabled: true, doNotTrack: null, hardwareConcurrency: 1, maxTouchPoints: 0, deviceMemory: 0.5, webdriver: false, pdfViewerEnabled: false, plugins: [], mimeTypes: [],
    clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve(''), write: () => Promise.resolve(), read: () => Promise.resolve([]) },
    permissions: { query: () => Promise.resolve({ state: 'denied', addEventListener() {} }) }, sendBeacon: () => true, vibrate: () => false, share: undefined,
    connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener() {} }, userAgentData: undefined, getBattery: undefined,
    registerProtocolHandler() {}, javaEnabled: () => false } });
def(g, { screen: { width: 1280, height: 800, availWidth: 1280, availHeight: 760, colorDepth: 24, pixelDepth: 24, availLeft: 0, availTop: 0, orientation: { type: 'landscape-primary', angle: 0, addEventListener() {} } },
    visualViewport: Object.assign(new D.EventTarget(), { get width() { return innerWidth; }, get height() { return innerHeight; }, scale: 1, offsetLeft: 0, offsetTop: 0, pageLeft: 0, get pageTop() { return scrollY; } }),
    Notification: Object.assign(function Notification() {}, { permission: 'denied', requestPermission: () => Promise.resolve('denied') }),
    structuredClone(v) { const seen = new Map(); const c = x => { if (typeof x === 'function' || typeof x === 'symbol') throw new DOMException('could not be cloned.', 'DataCloneError'); if (!x || typeof x !== 'object') return x; if (seen.has(x)) return seen.get(x);
        let r; if (x instanceof Date) r = new Date(x); else if (x instanceof RegExp) r = new RegExp(x); else if (x instanceof Map) { r = new Map(); seen.set(x, r); for (const [a, b] of x) r.set(c(a), c(b)); return r; } else if (x instanceof Set) { r = new Set(); seen.set(x, r); for (const a of x) r.add(c(a)); return r; }
        else if (ArrayBuffer.isView(x)) r = new x.constructor(x); else if (x instanceof ArrayBuffer) r = x.slice(0); else if (Array.isArray(x)) { r = []; seen.set(x, r); x.forEach((a, i) => { r[i] = c(a); }); return r; }
        else { r = {}; seen.set(x, r); for (const k of Object.keys(x)) r[k] = c(x[k]); return r; } seen.set(x, r); return r; }; return c(v); },
});

// ---------- WebSocket / EventSource / channels: not available; fail the way an unreachable server does ----------
class WebSocket extends D.EventTarget {
    constructor(u) { super(); this.url = String(u); this.readyState = 0; this.bufferedAmount = 0; this.protocol = ''; this.extensions = ''; this.binaryType = 'blob';
        setTimeout(() => { this.readyState = 3; for (const t of ['error', 'close']) this.dispatchEvent(new D.Event(t)); }, 10); }
    send() { throw new DOMException('WebSocket is not open', 'InvalidStateError'); } close() {}
}
for (const [k, v] of [['CONNECTING', 0], ['OPEN', 1], ['CLOSING', 2], ['CLOSED', 3]]) WebSocket[k] = WebSocket.prototype[k] = v;
class EventSource extends D.EventTarget { constructor(u) { super(); this.url = String(u); this.readyState = 2; setTimeout(() => this.dispatchEvent(new D.Event('error')), 10); } close() {} }
class BroadcastChannel extends D.EventTarget { constructor(n) { super(); this.name = n; } postMessage() {} close() {} }
class MessageChannel { constructor() { const mk = () => Object.assign(new D.EventTarget(), { start() {}, close() {} }); const a = mk(), b = mk(); a.postMessage = m => setTimeout(() => b.dispatchEvent(new D.MessageEvent('message', { data: m })), 0); b.postMessage = m => setTimeout(() => a.dispatchEvent(new D.MessageEvent('message', { data: m })), 0); this.port1 = a; this.port2 = b; } }
def(g, { WebSocket, EventSource, BroadcastChannel, MessageChannel });

// ---------- CSS, stylesheets ----------
const CSS = { escape: s => String(s).replace(/[^a-zA-Z0-9_ -￿-]/g, (c, i, str) => c === '\0' ? '�' : /[\x01-\x1f\x7f]/.test(c) || (i === 0 && /[0-9]/.test(c)) ? '\\' + c.charCodeAt(0).toString(16) + ' ' : '\\' + c).replace(/^-(?=[0-9])/, '\\-'),
    supports: (a, b) => b === undefined ? true : /^(display|color|background|margin|padding|width|height|font|border|position|float|text|line|overflow|opacity|cursor|visibility|z-index|flex|top|left|right|bottom)/.test(String(a)),
    registerProperty() {}, px: n => ({ value: n, unit: 'px' }) };
class CSSStyleSheet {
    constructor() { this.cssRules = []; this.rules = this.cssRules; this.disabled = false; this.href = null; this.ownerNode = null; this.media = { mediaText: '' }; this.type = 'text/css'; }
    insertRule(r, i = 0) { this.cssRules.splice(i, 0, { cssText: r, selectorText: String(r).split('{')[0].trim(), style: {} }); return i; }
    deleteRule(i) { this.cssRules.splice(i, 1); } addRule(s, r) { this.insertRule(s + '{' + r + '}', this.cssRules.length); } removeRule(i) { this.deleteRule(i); }
    replace() { return Promise.resolve(this); } replaceSync() {}
}
def(g, { CSS, CSSStyleSheet, CSSRule: function CSSRule() {}, StyleSheet: CSSStyleSheet });

// ---------- Range, Selection, TreeWalker ----------
class Range {
    constructor() { this.startContainer = this.endContainer = document; this.startOffset = this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = document; }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; this.__c(); } setEnd(n, o) { this.endContainer = n; this.endOffset = o; this.__c(); }
    __c() { this.collapsed = this.startContainer === this.endContainer && this.startOffset === this.endOffset; this.commonAncestorContainer = this.startContainer; }
    setStartBefore(n) { this.setStart(n.parentNode, 0); } setStartAfter(n) { this.setStart(n.parentNode, 0); } setEndBefore(n) { this.setEnd(n.parentNode, 0); } setEndAfter(n) { this.setEnd(n.parentNode, 0); }
    selectNode(n) { this.setStart(n.parentNode, 0); this.setEnd(n.parentNode, 0); this.commonAncestorContainer = n.parentNode; } selectNodeContents(n) { this.setStart(n, 0); this.setEnd(n, n.childNodes.length); }
    collapse(s) { if (s) this.setEnd(this.startContainer, this.startOffset); else this.setStart(this.endContainer, this.endOffset); }
    cloneRange() { const r = new Range(); r.setStart(this.startContainer, this.startOffset); r.setEnd(this.endContainer, this.endOffset); return r; }
    detach() {} toString() { return this.startContainer === this.endContainer && this.startContainer.nodeType === 3 ? this.startContainer.data.slice(this.startOffset, this.endOffset) : ''; }
    getBoundingClientRect() { const n = this.commonAncestorContainer; return n && n.getBoundingClientRect ? n.getBoundingClientRect() : new D.DOMRect(); } getClientRects() { return []; }
    createContextualFragment(h) { const t = document.createElement('template'); t.innerHTML = h; return t.content; }
    insertNode(n) { const c = this.startContainer; if (c.nodeType === 3) c.parentNode.insertBefore(n, c.splitText(this.startOffset)); else c.insertBefore(n, c.childNodes[this.startOffset] || null); }
    deleteContents() {} extractContents() { return document.createDocumentFragment(); } cloneContents() { return document.createDocumentFragment(); } surroundContents() {} compareBoundaryPoints() { return 0; } isPointInRange() { return false; } intersectsNode() { return false; }
}
const sel = { rangeCount: 0, anchorNode: null, focusNode: null, anchorOffset: 0, focusOffset: 0, isCollapsed: true, type: 'None', toString: () => '', removeAllRanges() {}, addRange() {}, removeRange() {}, getRangeAt() { return new Range(); },
    collapse() {}, extend() {}, selectAllChildren() {}, empty() {}, containsNode: () => false, setBaseAndExtent() {}, modify() {} };
const SHOW_ELEMENT = 1, SHOW_TEXT = 4, SHOW_ALL = 0xffffffff;
function walkList(root, what, filter, self) {
    const out = [];
    const rec = n => { for (let c = n.firstChild; c; c = c.nextSibling) { const bit = 1 << (c.nodeType - 1); if (what & bit) { const r = filter ? (typeof filter === 'function' ? filter(c) : filter.acceptNode(c)) : 1; if (r === 1) out.push(c); if (r === 3) continue; } rec(c); } };
    if (self && (what & (1 << (root.nodeType - 1)))) out.push(root);
    rec(root);
    return out;
}
class TreeWalker {
    constructor(root, what = SHOW_ALL, filter = null) { this.root = this.currentNode = root; this.whatToShow = what; this.filter = filter; }
    __l() { return walkList(this.root, this.whatToShow >>> 0, this.filter, false); }
    nextNode() { const l = this.__l(); const i = l.indexOf(this.currentNode); const n = l[i + 1]; if (n) this.currentNode = n; return n || null; }
    previousNode() { const l = this.__l(); const i = l.indexOf(this.currentNode); const n = i > 0 ? l[i - 1] : null; if (n) this.currentNode = n; return n; }
    firstChild() { const n = this.currentNode.firstChild; if (n) this.currentNode = n; return n; } lastChild() { const n = this.currentNode.lastChild; if (n) this.currentNode = n; return n; }
    nextSibling() { const n = this.currentNode.nextSibling; if (n) this.currentNode = n; return n; } previousSibling() { const n = this.currentNode.previousSibling; if (n) this.currentNode = n; return n; }
    parentNode() { const n = this.currentNode.parentNode; if (n && this.currentNode !== this.root) { this.currentNode = n; return n; } return null; }
}
class NodeIterator {
    constructor(root, what = SHOW_ALL, filter = null) { this.root = root; this.whatToShow = what; this.filter = filter; this.__i = -1; }
    nextNode() { const l = walkList(this.root, this.whatToShow >>> 0, this.filter, true); return l[++this.__i] || null; }
    previousNode() { const l = walkList(this.root, this.whatToShow >>> 0, this.filter, true); return this.__i > 0 ? l[--this.__i] : null; }
}
class DOMParser {
    parseFromString(s, type) {
        const root = document.createElement('div');
        root.innerHTML = String(s);
        const body = root.querySelector('body') || root, head = root.querySelector('head');
        const doc = { nodeType: 9, nodeName: '#document', documentElement: root.querySelector('html') || root, body, head, defaultView: null, contentType: type, location: null,
            get title() { const t = root.querySelector('title'); return t ? t.textContent : ''; }, createElement: t => document.createElement(t), createTextNode: t => document.createTextNode(t),
            querySelector: x => root.querySelector(x), querySelectorAll: x => root.querySelectorAll(x), getElementById: x => root.querySelector('#' + CSS.escape(x)), getElementsByTagName: x => root.getElementsByTagName(x),
            getElementsByClassName: x => root.getElementsByClassName(x), get textContent() { return root.textContent; }, get firstChild() { return root.firstChild; }, get childNodes() { return root.childNodes; },
            get children() { return root.children; }, get firstElementChild() { return root.firstElementChild; }, importNode: n => n.cloneNode(true), adoptNode: n => n };
        return doc;
    }
}
class XMLSerializer { serializeToString(n) { return n.nodeType === 1 ? n.outerHTML : n.nodeType === 9 ? n.documentElement.outerHTML : n.textContent; } }
def(g, { Range, TreeWalker, NodeIterator, DOMParser, XMLSerializer, getSelection: () => sel, NodeFilter: { SHOW_ALL, SHOW_ELEMENT, SHOW_TEXT, SHOW_COMMENT: 128, FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3 }, Selection: function Selection() {} });

// ---------- Intl (QuickJS has none): the English-only basics pages call without checking ----------
const group = (s, sep = ',') => s.replace(/\B(?=(\d{3})+(?!\d))/g, sep);
const CUR = { USD: '$', EUR: '€', GBP: '£', JPY: '¥', INR: '₹', CNY: '¥', KRW: '₩', CAD: 'CA$', AUD: 'A$' };
class NumberFormat {
    constructor(loc, o = {}) { this.o = o; }
    format(n) {
        const o = this.o; n = Number(n);
        if (!isFinite(n)) return isNaN(n) ? 'NaN' : (n < 0 ? '-' : '') + '∞';
        let sfx = '', v = Math.abs(n);
        if (o.style === 'percent') v *= 100;
        if (o.notation === 'compact') { for (const [m, s] of [[1e12, 'T'], [1e9, 'B'], [1e6, 'M'], [1e3, 'K']]) if (v >= m) { v /= m; sfx = s; break; } }
        const cur = o.style === 'currency', minF = o.minimumFractionDigits ?? (cur ? 2 : 0), maxF = Math.max(minF, o.maximumFractionDigits ?? (cur ? 2 : o.notation === 'compact' ? (v < 100 && sfx ? 1 : 0) : 3));
        let s = v.toFixed(maxF);
        if (s.includes('.')) { s = s.replace(/0+$/, ''); const f = s.split('.')[1] || ''; s = f.length < minF ? s.split('.')[0] + '.' + f.padEnd(minF, '0') : s.replace(/\.$/, ''); }
        let [i, f] = s.split('.');
        if (o.minimumIntegerDigits) i = i.padStart(o.minimumIntegerDigits, '0');
        if (o.useGrouping !== false) i = group(i);
        s = f ? i + '.' + f : i;
        s += sfx;
        if (o.style === 'percent') s += '%';
        if (cur) s = (CUR[o.currency] || o.currency + ' ') + s;
        if (o.style === 'unit') s += ' ' + (o.unit || '');
        return (n < 0 && +s.replace(/[^0-9.]/g, '') !== 0 ? '-' : '') + s;
    }
    formatToParts(n) { return [{ type: 'literal', value: this.format(n) }]; }
    resolvedOptions() { return Object.assign({ locale: 'en-US', numberingSystem: 'latn', style: 'decimal' }, this.o); }
    static supportedLocalesOf(l) { return [].concat(l || []); }
}
const MON = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'], DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
class DateTimeFormat {
    constructor(loc, o = {}) { this.o = o; }
    format(dt) {
        dt = dt === undefined ? new Date() : new Date(dt);
        const o = this.o, utc = o.timeZone === 'UTC', G = k => utc ? dt['getUTC' + k]() : dt['get' + k](), p2 = n => String(n).padStart(2, '0');
        const hasD = o.year || o.month || o.day || o.weekday || o.dateStyle, hasT = o.hour || o.minute || o.second || o.timeStyle;
        const parts = [];
        if (hasD || !hasT) {
            const m = o.month || (o.dateStyle ? (o.dateStyle === 'short' ? 'numeric' : o.dateStyle === 'medium' ? 'short' : 'long') : hasD ? undefined : 'numeric');
            const dd = o.day || (!hasD || o.dateStyle ? 'numeric' : undefined), yy = o.year || (!hasD || o.dateStyle ? 'numeric' : undefined);
            const wd = o.weekday ? (o.weekday === 'long' ? DAYS[G('Day')] : DAYS[G('Day')].slice(0, 3)) : o.dateStyle === 'full' ? DAYS[G('Day')] : '';
            let s;
            if (m === 'long' || m === 'short') { const mn = m === 'long' ? MON[G('Month')] : MON[G('Month')].slice(0, 3); s = mn + (dd ? ' ' + (dd === '2-digit' ? p2(G('Date')) : G('Date')) : '') + (yy ? (dd ? ', ' : ' ') + (yy === '2-digit' ? p2(G('FullYear') % 100) : G('FullYear')) : ''); }
            else { const a = []; if (m) a.push(m === '2-digit' ? p2(G('Month') + 1) : G('Month') + 1); if (dd) a.push(dd === '2-digit' ? p2(G('Date')) : G('Date')); if (yy) a.push(yy === '2-digit' ? p2(G('FullYear') % 100) : G('FullYear')); s = a.join('/'); }
            parts.push((wd ? wd + (s ? ', ' : '') : '') + s);
        }
        if (hasT) {
            const h12 = o.hour12 !== false && o.hourCycle !== 'h23', h = G('Hours');
            let s = (h12 ? (h % 12 || 12) : (o.hour === '2-digit' || !h12 ? p2(h) : h));
            if (o.minute || o.timeStyle || !o.hour) s += ':' + p2(G('Minutes'));
            if (o.second || o.timeStyle === 'medium' || o.timeStyle === 'long') s += ':' + p2(G('Seconds'));
            if (h12) s += ' ' + (h < 12 ? 'AM' : 'PM');
            parts.push(s);
        }
        return parts.join(', ');
    }
    formatToParts(dt) { return [{ type: 'literal', value: this.format(dt) }]; }
    formatRange(a, b) { return this.format(a) + ' – ' + this.format(b); }
    resolvedOptions() { return Object.assign({ locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' }, this.o); }
    static supportedLocalesOf(l) { return [].concat(l || []); }
}
class Collator { constructor(l, o = {}) { this.o = o; } compare = (a, b) => { a = String(a); b = String(b); if (this.o.numeric) { const x = a.split(/(\d+)/), y = b.split(/(\d+)/); for (let i = 0; i < Math.min(x.length, y.length); i++) { if (x[i] !== y[i]) { if (i % 2) return x[i] - y[i]; break; } } } return this.o.sensitivity === 'base' || this.o.sensitivity === 'accent' ? a.toLowerCase().localeCompare(b.toLowerCase()) : a.localeCompare(b); }; resolvedOptions() { return { locale: 'en-US' }; } }
class PluralRules { constructor(l, o = {}) { this.o = o; } select(n) { if (this.o.type === 'ordinal') { const a = n % 10, b = n % 100; return a === 1 && b !== 11 ? 'one' : a === 2 && b !== 12 ? 'two' : a === 3 && b !== 13 ? 'few' : 'other'; } return n === 1 ? 'one' : 'other'; } resolvedOptions() { return { locale: 'en-US' }; } }
class RelativeTimeFormat { constructor(l, o = {}) { this.o = o; } format(v, u) { u = String(u).replace(/s$/, ''); const a = Math.abs(v); if (this.o.numeric === 'auto' && a <= 1) { const w = { day: ['yesterday', 'today', 'tomorrow'], year: ['last year', 'this year', 'next year'], month: ['last month', 'this month', 'next month'], week: ['last week', 'this week', 'next week'] }[u]; if (w) return w[v + 1]; } const s = a + ' ' + u + (a === 1 ? '' : 's'); return v < 0 || Object.is(v, -0) ? s + ' ago' : 'in ' + s; } }
class ListFormat { constructor(l, o = {}) { this.o = o; } format(l) { l = [...l]; const w = this.o.type === 'disjunction' ? 'or' : 'and'; return l.length < 3 ? l.join(' ' + w + ' ') : l.slice(0, -1).join(', ') + ', ' + w + ' ' + l[l.length - 1]; } }
class Segmenter { constructor(l, o = {}) { this.o = o; } segment(s) { const g_ = this.o.granularity === 'word' ? s.match(/\w+|\W/g) || [] : Array.from(String(s)); let i = 0; return g_.map(x => { const r = { segment: x, index: i, input: s, isWordLike: /\w/.test(x) }; i += x.length; return r; }); } }
class Locale { constructor(t) { this.baseName = this.language = String(t).split('-')[0]; this.toString = () => String(t); } }
def(g, { Intl: { NumberFormat, DateTimeFormat, Collator, PluralRules, RelativeTimeFormat, ListFormat, Segmenter, Locale, getCanonicalLocales: l => [].concat(l || []), supportedValuesOf: () => [], DisplayNames: class DisplayNames { of(c) { return c; } } } });
if (!Date.prototype.toLocaleDateString || Date.prototype.toLocaleDateString.length === 0) { /* QuickJS has these; Intl-based options below override only when given */ }
{ const ols = Date.prototype.toLocaleDateString, olt = Date.prototype.toLocaleTimeString, olx = Date.prototype.toLocaleString;
  Date.prototype.toLocaleDateString = function (l, o) { return o ? new DateTimeFormat(l, Object.assign({}, o)).format(this) : ols.call(this); };
  Date.prototype.toLocaleTimeString = function (l, o) { return o ? new DateTimeFormat(l, Object.assign({ hour: 'numeric', minute: 'numeric' }, o)).format(this) : olt.call(this); };
  Date.prototype.toLocaleString = function (l, o) { return o ? new DateTimeFormat(l, o.dateStyle || o.timeStyle || o.hour || o.year || o.month ? o : Object.assign({ year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric', second: 'numeric' }, o)).format(this) : olx.call(this); };
  const onl = Number.prototype.toLocaleString;
  Number.prototype.toLocaleString = function (l, o) { return new NumberFormat(l, o).format(this); }; void onl; }

// ---------- unhandled rejections are logged by the host; window.onerror is wired in dom_js.c ----------
delete g.__d;
delete g.__v;
})(globalThis);
)JS";
