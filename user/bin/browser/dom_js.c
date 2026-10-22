/* dom_js.c - the JavaScript half of the DOM: classes over js.c's natives (__d), which work on integer node ids.
 * One wrapper object per node (cached, so === and expando properties hold); events, timers, fetch(), console,
 * location, classList and style live here. Evaluated once per page before the page's own scripts. */
const char js_prelude[] = R"JS(
(function (g) {
'use strict';
const d = g.__d;
delete g.__d;
const W = [];                                   // node id -> wrapper
function wrap(id) {
    if (!id) return null;
    let o = W[id];
    if (o) return o;
    const t = d.type(id);
    o = Object.create(t === 3 ? Text.prototype : t === 9 ? Document.prototype : t === 11 ? DocumentFragment.prototype : Element.prototype);
    Object.defineProperty(o, '__id', { value: id });
    return W[id] = o;
}
const idOf = n => { if (!n || !n.__id) throw new TypeError('not a DOM node: ' + n); return n.__id; };

function report(e) { d.log(2, 'Uncaught ' + (e && e.stack ? e + '\n' + e.stack : String(e))); }

// ---------- events ----------
class Event {
    constructor(type, init) {
        this.type = String(type);
        this.bubbles = !!(init && init.bubbles);
        this.cancelable = !(init && init.cancelable === false);
        this.defaultPrevented = false;
        this.target = this.currentTarget = null;
        this.timeStamp = d.now();
        this.__stop = this.__imm = false;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this.__stop = true; }
    stopImmediatePropagation() { this.__stop = this.__imm = true; }
}
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; } }
class KeyboardEvent extends Event {}
class MouseEvent extends Event {}

const handlers = new WeakMap();                 // element -> { onclick: [source, function] }
function inline(t, type) {                      // onclick="..." attributes
    if (!(t instanceof Element)) return null;
    const src = t.getAttribute('on' + type);
    if (src === null) return null;
    let h = handlers.get(t);
    if (!h) handlers.set(t, h = {});
    const c = h[type];
    if (c && c[0] === src) return c[1];
    let f;
    try { f = new Function('event', src); } catch (e) { report(e); f = () => {}; }
    h[type] = [src, f];
    return f;
}
function fire(t, ev) {
    ev.currentTarget = t;
    const l = t.__l && t.__l[ev.type];
    if (l) for (const x of l.slice()) {
        if (x.once) t.removeEventListener(ev.type, x.fn);
        try { typeof x.fn === 'function' ? x.fn.call(t, ev) : x.fn.handleEvent(ev); } catch (e) { report(e); }
        if (ev.__imm) return;
    }
    const p = Object.getOwnPropertyDescriptor(t, 'on' + ev.type);
    const h = p && typeof p.value === 'function' ? p.value : inline(t, ev.type);
    if (h) try { if (h.call(t, ev) === false) ev.preventDefault(); } catch (e) { report(e); }
}
function dispatch(t, ev) {
    ev.target = t;
    const path = [];
    for (let n = t; n && n !== g; n = n.parentNode) path.push(n);
    if (t instanceof Node && path[path.length - 1] === document) path.push(g);
    if (t === g) path.push(g);
    for (const n of ev.bubbles ? path : [t]) { fire(n, ev); if (ev.__stop) break; }
    ev.currentTarget = null;
    return ev.defaultPrevented;
}
const ET = {
    addEventListener(type, fn, opt) {
        if (!fn) return;
        const l = this.__l || Object.defineProperty(this, '__l', { value: {} }).__l;
        const a = l[type] || (l[type] = []);
        if (!a.some(x => x.fn === fn)) a.push({ fn, once: !!(opt && opt.once) });
    },
    removeEventListener(type, fn) {
        const a = this.__l && this.__l[type];
        const i = a ? a.findIndex(x => x.fn === fn) : -1;
        if (i >= 0) a.splice(i, 1);
    },
    dispatchEvent(ev) { return !dispatch(this, ev); },
};
g.__dispatch = (id, type, x) => {
    const ev = new (type.startsWith('key') ? KeyboardEvent : type === 'click' ? MouseEvent : Event)(type, { bubbles: type !== 'load' });
    if (x) Object.assign(ev, x);
    return dispatch(id ? wrap(id) : g, ev);
};

// ---------- nodes ----------
class Node {
    get nodeType() { return d.type(this.__id); }
    get nodeName() { const t = d.tag(this.__id); return this.nodeType === 1 ? t.toUpperCase() : t; }
    get parentNode() { return wrap(d.parent(this.__id)); }
    get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
    get firstChild() { return wrap(d.first(this.__id)); }
    get lastChild() { return wrap(d.last(this.__id)); }
    get nextSibling() { return wrap(d.next(this.__id)); }
    get previousSibling() { return wrap(d.prev(this.__id)); }
    get childNodes() { const r = []; for (let c = d.first(this.__id); c; c = d.next(c)) r.push(wrap(c)); return r; }
    get ownerDocument() { return document; }
    get isConnected() { for (let n = this; n; n = n.parentNode) if (n === document) return true; return false; }
    get textContent() { return d.text(this.__id); }
    set textContent(v) { d.setText(this.__id, v == null ? '' : String(v)); }
    get nodeValue() { return this.nodeType === 3 ? d.text(this.__id) : null; }
    set nodeValue(v) { if (this.nodeType === 3) d.setText(this.__id, String(v)); }
    hasChildNodes() { return !!d.first(this.__id); }
    appendChild(c) { d.insert(this.__id, idOf(c), 0); return c; }
    insertBefore(c, ref) { d.insert(this.__id, idOf(c), ref ? idOf(ref) : 0); return c; }
    removeChild(c) { d.remove(this.__id, idOf(c)); return c; }
    replaceChild(n, o) { this.insertBefore(n, o); if (n !== o) this.removeChild(o); return o; }
    cloneNode(deep) { return wrap(d.clone(this.__id, !!deep)); }
    contains(n) { for (; n; n = n.parentNode) if (n === this) return true; return false; }
    append(...a) { for (const n of a) this.appendChild(typeof n === 'object' ? n : document.createTextNode(n)); }
    prepend(...a) { const f = this.firstChild; for (const n of a) this.insertBefore(typeof n === 'object' ? n : document.createTextNode(n), f); }
    remove() { const p = d.parent(this.__id); if (p) d.remove(p, this.__id); }
    replaceWith(...a) { const p = this.parentNode; if (!p) return; const nx = this.nextSibling; this.remove(); for (const n of a) p.insertBefore(typeof n === 'object' ? n : document.createTextNode(n), nx); }
    before(...a) { const p = this.parentNode; if (p) for (const n of a) p.insertBefore(typeof n === 'object' ? n : document.createTextNode(n), this); }
    after(...a) { const p = this.parentNode; if (!p) return; const nx = this.nextSibling; for (const n of a) p.insertBefore(typeof n === 'object' ? n : document.createTextNode(n), nx); }
}
Object.assign(Node.prototype, ET);
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;

class Text extends Node {
    get data() { return d.text(this.__id); }
    set data(v) { d.setText(this.__id, String(v)); }
    get length() { return this.data.length; }
}

const query = {
    querySelector(s) { return wrap(d.qsa(this.__id, String(s), true)[0] || 0); },
    querySelectorAll(s) { return d.qsa(this.__id, String(s), false).map(wrap); },
    getElementsByTagName(t) { return t === '*' ? this.querySelectorAll('*') : this.querySelectorAll(String(t).toLowerCase()); },
    getElementsByClassName(c) { return this.querySelectorAll(String(c).trim().split(/\s+/).map(x => '.' + x).join('')); },
    get children() { return this.childNodes.filter(n => n.nodeType === 1); },
    get firstElementChild() { return this.children[0] || null; },
    get lastElementChild() { const c = this.children; return c[c.length - 1] || null; },
    get childElementCount() { return this.children.length; },
};
function mix(dst, src) { Object.defineProperties(dst, Object.getOwnPropertyDescriptors(src)); }

class DocumentFragment extends Node {}
mix(DocumentFragment.prototype, query);

class ClassList {
    constructor(e) { this.e = e; }
    get _a() { return (this.e.getAttribute('class') || '').split(/\s+/).filter(Boolean); }
    _set(a) { this.e.setAttribute('class', a.join(' ')); }
    add(...c) { const a = this._a; for (const x of c) if (!a.includes(x)) a.push(x); this._set(a); }
    remove(...c) { this._set(this._a.filter(x => !c.includes(x))); }
    contains(c) { return this._a.includes(c); }
    toggle(c, force) { const on = force === undefined ? !this.contains(c) : !!force; on ? this.add(c) : this.remove(c); return on; }
    replace(a, b) { const l = this._a, i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
    item(i) { return this._a[i] || null; }
    get length() { return this._a.length; }
    get value() { return this.e.getAttribute('class') || ''; }
    toString() { return this.value; }
    [Symbol.iterator]() { return this._a[Symbol.iterator](); }
}

const kebab = k => k.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
function styleOf(e) {
    const parse = () => {
        const m = new Map();
        for (const p of (e.getAttribute('style') || '').split(';')) {
            const i = p.indexOf(':');
            if (i > 0) m.set(p.slice(0, i).trim().toLowerCase(), p.slice(i + 1).trim());
        }
        return m;
    };
    const set = (n, v) => {
        const m = parse();
        v = v == null ? '' : String(v);
        if (v === '') m.delete(n); else m.set(n, v);
        const s = [...m].map(([a, b]) => a + ': ' + b).join('; ');
        s ? e.setAttribute('style', s) : e.removeAttribute('style');
    };
    const api = {
        getPropertyValue: n => parse().get(String(n).toLowerCase()) || '',
        setProperty: (n, v) => set(String(n).toLowerCase(), v),
        removeProperty: n => { const v = api.getPropertyValue(n); set(String(n).toLowerCase(), ''); return v; },
    };
    return new Proxy({}, {
        get(t, k) {
            if (typeof k !== 'string') return undefined;
            if (k === 'cssText') return e.getAttribute('style') || '';
            if (k in api) return api[k];
            if (k === 'length') return parse().size;
            return parse().get(kebab(k)) || '';
        },
        set(t, k, v) { if (k === 'cssText') e.setAttribute('style', String(v)); else set(kebab(String(k)), v); return true; },
    });
}

function datasetOf(e) {
    const name = k => 'data-' + kebab(String(k));
    return new Proxy({}, {
        get: (t, k) => typeof k === 'string' ? (e.getAttribute(name(k)) ?? undefined) : undefined,
        set: (t, k, v) => { e.setAttribute(name(k), String(v)); return true; },
        deleteProperty: (t, k) => { e.removeAttribute(name(k)); return true; },
        has: (t, k) => e.hasAttribute(name(k)),
    });
}

class Element extends Node {
    get tagName() { return d.tag(this.__id).toUpperCase(); }
    get localName() { return d.tag(this.__id); }
    getAttribute(n) { return d.getAttr(this.__id, String(n).toLowerCase()); }
    setAttribute(n, v) { d.setAttr(this.__id, String(n).toLowerCase(), String(v)); }
    removeAttribute(n) { d.delAttr(this.__id, String(n).toLowerCase()); }
    hasAttribute(n) { return this.getAttribute(n) !== null; }
    toggleAttribute(n, f) { const on = f === undefined ? !this.hasAttribute(n) : !!f; on ? this.setAttribute(n, '') : this.removeAttribute(n); return on; }
    getAttributeNames() { return d.attrs(this.__id); }
    get attributes() { return d.attrs(this.__id).map(name => ({ name, value: this.getAttribute(name) })); }
    get id() { return this.getAttribute('id') || ''; }
    set id(v) { this.setAttribute('id', v); }
    get className() { return this.getAttribute('class') || ''; }
    set className(v) { this.setAttribute('class', v); }
    get classList() { return this.__cl || Object.defineProperty(this, '__cl', { value: new ClassList(this) }).__cl; }
    get style() { return this.__st || Object.defineProperty(this, '__st', { value: styleOf(this) }).__st; }
    get dataset() { return this.__ds || Object.defineProperty(this, '__ds', { value: datasetOf(this) }).__ds; }
    get innerHTML() { return d.html(this.__id, false); }
    set innerHTML(v) { d.setHTML(this.__id, v == null ? '' : String(v)); }
    get outerHTML() { return d.html(this.__id, true); }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    get nextElementSibling() { for (let n = this.nextSibling; n; n = n.nextSibling) if (n.nodeType === 1) return n; return null; }
    get previousElementSibling() { for (let n = this.previousSibling; n; n = n.previousSibling) if (n.nodeType === 1) return n; return null; }
    matches(s) { return d.matches(this.__id, String(s)); }
    closest(s) { for (let e = this; e && e.nodeType === 1; e = e.parentNode) if (e.matches(s)) return e; return null; }
    insertAdjacentHTML(where, html) {
        const t = document.createElement('div');
        t.innerHTML = html;
        const nodes = t.childNodes, w = String(where).toLowerCase();
        for (const n of nodes) {
            if (w === 'beforebegin') this.before(n);
            else if (w === 'afterbegin') this.insertBefore(n, this.firstChild);
            else if (w === 'beforeend') this.appendChild(n);
            else this.parentNode && this.parentNode.insertBefore(n, this.nextSibling);
        }
    }
    get value() { return d.value(this.__id); }
    set value(v) { d.setValue(this.__id, v == null ? '' : String(v)); }
    get checked() { return d.checked(this.__id); }
    set checked(v) { d.setChecked(this.__id, !!v); }
    get selectedIndex() { return d.selIndex(this.__id); }
    set selectedIndex(v) { d.setSelIndex(this.__id, v | 0); }
    get options() { return this.querySelectorAll('option'); }
    get disabled() { return this.hasAttribute('disabled'); }
    set disabled(v) { this.toggleAttribute('disabled', !!v); }
    get hidden() { return this.hasAttribute('hidden'); }
    set hidden(v) { this.toggleAttribute('hidden', !!v); }
    get form() { return this.closest('form'); }
    get elements() { return this.querySelectorAll('input, textarea, select, button'); }
    focus() { d.focus(this.__id); }
    blur() {}
    click() { dispatch(this, new MouseEvent('click', { bubbles: true })); }
    getBoundingClientRect() { return { x: 0, y: 0, left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0 }; }
    scrollIntoView() {}
}
mix(Element.prototype, query);
for (const [p, a] of [['href', 'href'], ['src', 'src'], ['type', 'type'], ['name', 'name'], ['title', 'title'], ['alt', 'alt'],
        ['placeholder', 'placeholder'], ['action', 'action'], ['method', 'method'], ['htmlFor', 'for'], ['rel', 'rel'],
        ['target', 'target'], ['lang', 'lang'], ['width', 'width'], ['height', 'height'], ['colSpan', 'colspan'], ['rows', 'rows'], ['cols', 'cols']])
    Object.defineProperty(Element.prototype, p, {
        get() { const v = this.getAttribute(a); return (p === 'href' || p === 'src' || p === 'action') && v !== null ? absurl(v) : v ?? ''; },
        set(v) { this.setAttribute(a, v); }, configurable: true });
for (const t of ['HTMLElement', 'HTMLDivElement', 'HTMLInputElement', 'HTMLButtonElement', 'HTMLFormElement', 'HTMLAnchorElement',
        'HTMLSpanElement', 'HTMLImageElement', 'HTMLSelectElement', 'HTMLTextAreaElement', 'HTMLLIElement', 'HTMLUListElement'])
    g[t] = Element;

class Document extends Node {
    get documentElement() { return wrap(2); }
    get head() { return this.querySelector('head'); }
    get body() { return this.querySelector('body'); }
    get title() { const t = this.querySelector('title'); return t ? t.textContent.trim() : ''; }
    set title(v) {
        let t = this.querySelector('title');
        if (!t) { t = this.createElement('title'); this.head.appendChild(t); }
        t.textContent = v;
    }
    get readyState() { return 'complete'; }
    get location() { return location; }
    get URL() { return location.href; }
    get cookie() { return ''; }
    set cookie(v) {}
    get defaultView() { return g; }
    get activeElement() { return this.body; }
    getElementById(id) { return wrap(d.byId(String(id))); }
    createElement(t) { return wrap(d.create(String(t).toLowerCase())); }
    createTextNode(s) { return wrap(d.createText(String(s))); }
    createDocumentFragment() { return wrap(d.createFrag()); }
    createEvent() { return new Event(''); }
}
mix(Document.prototype, query);

// ---------- window ----------
const document = wrap(1);
const URLRE = /^([a-z][a-z0-9+.-]*:)(?:\/\/([^\/?#:]*)(?::(\d+))?)?([^?#]*)(\?[^#]*)?(#.*)?$/i;
const absurl = u => d.resolve(String(u));
const location = {
    get href() { return d.url(); }, set href(v) { d.navigate(String(v)); },
    get protocol() { return (URLRE.exec(d.url()) || [])[1] || ''; },
    get hostname() { return (URLRE.exec(d.url()) || [])[2] || ''; },
    get port() { return (URLRE.exec(d.url()) || [])[3] || ''; },
    get host() { const m = URLRE.exec(d.url()) || []; return (m[2] || '') + (m[3] ? ':' + m[3] : ''); },
    get origin() { return this.protocol + '//' + this.host; },
    get pathname() { return (URLRE.exec(d.url()) || [])[4] || '/'; },
    get search() { return (URLRE.exec(d.url()) || [])[5] || ''; },
    get hash() { return (URLRE.exec(d.url()) || [])[6] || ''; },
    set hash(v) { d.navigate('#' + String(v).replace(/^#/, '')); },
    assign(u) { d.navigate(String(u)); }, replace(u) { d.navigate(String(u)); }, reload() { d.navigate(d.url()); },
    toString() { return d.url(); },
};

function show(v, depth = 0) {
    if (typeof v === 'string') return depth ? JSON.stringify(v) : v;
    if (v instanceof Error) return String(v);
    if (v instanceof Node) return v.nodeType === 1 ? '<' + v.localName + (v.id ? '#' + v.id : '') + '>' : v.nodeName;
    if (typeof v === 'function') return '[Function ' + (v.name || 'anonymous') + ']';
    if (v && typeof v === 'object' && depth < 2) {
        if (Array.isArray(v)) return '[' + v.slice(0, 20).map(x => show(x, depth + 1)).join(', ') + (v.length > 20 ? ', ...' : '') + ']';
        return '{' + Object.keys(v).slice(0, 20).map(k => k + ': ' + show(v[k], depth + 1)).join(', ') + '}';
    }
    return String(v);
}
const out = k => (...a) => d.log(k, a.map(x => show(x)).join(' '));
const console = { log: out(0), info: out(0), debug: out(0), warn: out(1), error: out(2), trace: out(0),
                  assert: (c, ...a) => { if (!c) out(2)('Assertion failed:', ...a); }, clear() {}, table: out(0),
                  group: out(0), groupEnd() {}, time() {}, timeEnd() {} };

const TM = new Map();
function timer(fn, ms, args, rep) {
    const f = typeof fn === 'function' ? fn : () => (0, eval)(String(fn));
    const id = d.timer(+ms || 0, rep);
    TM.set(id, [f, args]);
    return id;
}
g.__fire = (id, rep) => {
    const t = TM.get(id);
    if (!t) return;
    if (!rep) TM.delete(id);
    try { t[0](...t[1]); } catch (e) { report(e); }
};
const clear = id => { TM.delete(id); d.clear(id | 0); };

class Headers {
    constructor(ct) { this._ct = ct; }
    get(n) { return String(n).toLowerCase() === 'content-type' && this._ct ? this._ct : null; }
    has(n) { return this.get(n) !== null; }
}
class Response {
    constructor(body, status, text, ct, url) {
        Object.assign(this, { _b: body, status, statusText: text, ok: status >= 200 && status < 300, url, headers: new Headers(ct), redirected: false });
    }
    text() { return Promise.resolve(this._b); }
    json() { return Promise.resolve(this._b).then(JSON.parse); }
}
const FM = new Map();
g.__fetchDone = (id, status, text, ct, body, err, url) => {
    const p = FM.get(id);
    FM.delete(id);
    if (!p) return;
    if (err !== null) p[1](new TypeError('fetch failed: ' + err + ' (' + url + ')'));
    else p[0](new Response(body, status, text, ct, url));
};
function fetch(url, opt) {
    return new Promise((res, rej) => {
        const m = opt && opt.method ? String(opt.method).toUpperCase() : 'GET';
        if (m !== 'GET') throw new TypeError('fetch: only GET is supported (got ' + m + ')');
        FM.set(d.fetch(String(url && url.url ? url.url : url)), [res, rej]);
    });
}

const storage = () => { const m = new Map(); return { getItem: k => m.has(String(k)) ? m.get(String(k)) : null,
    setItem: (k, v) => m.set(String(k), String(v)), removeItem: k => m.delete(String(k)), clear: () => m.clear(),
    key: i => [...m.keys()][i] ?? null, get length() { return m.size; } }; };

Object.assign(g, {
    window: g, self: g, globalThis: g, document, location, console, Node, Text, Element, Document, DocumentFragment,
    Event, CustomEvent, KeyboardEvent, MouseEvent, fetch, Response, Headers,
    setTimeout: (f, ms, ...a) => timer(f, ms, a, false), setInterval: (f, ms, ...a) => timer(f, ms, a, true),
    clearTimeout: clear, clearInterval: clear,
    requestAnimationFrame: f => timer(() => f(d.now()), 16, [], false), cancelAnimationFrame: clear,
    queueMicrotask: f => { Promise.resolve().then(f); },
    alert: m => d.alert(m === undefined ? '' : String(m)),
    performance: { now: () => d.now() },
    navigator: { userAgent: 'VibeBrowser/1.0 (VibeOS) QuickJS/2026-06-04', platform: 'VibeOS', language: 'en', languages: ['en'], onLine: true, cookieEnabled: false },
    localStorage: storage(), sessionStorage: storage(),
    innerWidth: 900, innerHeight: 580, devicePixelRatio: 1,
    getComputedStyle: e => e.style,
});
Object.assign(g, ET);
})(globalThis);
)JS";
