/* dom_js.c - the JavaScript half of the DOM: classes over js.c's natives (__d), which work on integer node ids.
 * One wrapper object per node (cached, so === and expando properties hold); events (capture/target/bubble), the node and
 * element classes by tag, style/classList/dataset, MutationObserver hooks, location/history and timers live here.
 * Evaluated once per page before the page's own scripts; web_js.c (URL, fetch, XHR, storage, observers ...) follows it
 * and removes the internals (__d, __v). */
const char js_prelude[] = R"JS(
(function (g) {
'use strict';
const d = g.__d;
const V = {};                                   // internals shared with web_js.c
Object.defineProperty(g, '__v', { value: V, configurable: true });
const W = [];                                   // node id -> wrapper
const T0 = d.now();

let inReport = false;
function report(e) {                            // an uncaught exception: window 'error' listeners/onerror first, then the console
    if (!inReport) {
        inReport = true;
        try {
            const ev = new ErrorEvent('error', { cancelable: true, message: e && e.message !== undefined ? String(e.message) : String(e), error: e });
            dispatch(g, ev);
            if (ev.defaultPrevented) return;
        } catch (x) {} finally { inReport = false; }
    }
    d.log(2, 'Uncaught ' + (e && e.stack ? e + '\n' + e.stack : String(e)));
}
V.report = report;

// ---------- events ----------
class Event {
    constructor(type, init) {
        init = init || {};
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.target = this.currentTarget = this.srcElement = null;
        this.eventPhase = 0;
        this.isTrusted = false;
        this.timeStamp = d.now() - T0;
        Object.defineProperties(this, { __stop: { value: false, writable: true }, __imm: { value: false, writable: true }, __passive: { value: false, writable: true } });
    }
    preventDefault() { if (this.cancelable && !this.__passive) this.defaultPrevented = true; }
    stopPropagation() { this.__stop = true; }
    stopImmediatePropagation() { this.__stop = this.__imm = true; }
    composedPath() { return this.__path ? this.__path.slice() : []; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this.__stop; }
    set cancelBubble(v) { if (v) this.__stop = true; }
    initEvent(t, b, c) { this.type = String(t); this.bubbles = !!b; this.cancelable = !!c; }
}
Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
class UIEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail || 0; this.view = g; } }
class MouseEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.clientX = this.screenX = this.x = i.clientX || 0; this.clientY = this.screenY = this.y = i.clientY || 0; this.pageX = i.pageX === undefined ? this.clientX : i.pageX; this.pageY = i.pageY === undefined ? this.clientY : i.pageY;
        this.offsetX = this.offsetY = this.movementX = this.movementY = 0;
        this.button = i.button || 0; this.buttons = i.buttons || 0; this.relatedTarget = i.relatedTarget || null;
        this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
    }
    getModifierState() { return false; }
}
class PointerEvent extends MouseEvent { constructor(t, i) { super(t, i); this.pointerId = 1; this.pointerType = 'mouse'; this.isPrimary = true; } }
class WheelEvent extends MouseEvent { constructor(t, i) { super(t, i); this.deltaX = i && i.deltaX || 0; this.deltaY = i && i.deltaY || 0; this.deltaMode = 0; } }
class KeyboardEvent extends UIEvent {
    constructor(t, i) {
        super(t, i); i = i || {};
        this.key = i.key === undefined ? '' : i.key; this.code = i.code || ''; this.keyCode = this.which = i.keyCode || 0; this.charCode = 0;
        this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
        this.repeat = !!i.repeat; this.isComposing = false; this.location = 0;
    }
    getModifierState() { return false; }
}
class FocusEvent extends UIEvent { constructor(t, i) { super(t, i); this.relatedTarget = i && i.relatedTarget || null; } }
class InputEvent extends UIEvent { constructor(t, i) { super(t, i); this.data = i && i.data !== undefined ? i.data : null; this.inputType = i && i.inputType || ''; this.isComposing = false; } }
class CustomEvent extends Event { constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; } initCustomEvent(t, b, c, dt) { this.initEvent(t, b, c); this.detail = dt; } }
class ErrorEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.filename = i.filename || ''; this.lineno = i.lineno | 0; this.colno = i.colno | 0; this.error = i.error; } }
class MessageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.data = i.data; this.origin = i.origin || ''; this.source = i.source || null; this.ports = []; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); this.oldURL = i && i.oldURL || ''; this.newURL = i && i.newURL || ''; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state !== undefined ? i.state : null; } }
class ProgressEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.lengthComputable = !!i.lengthComputable; this.loaded = i.loaded || 0; this.total = i.total || 0; } }
class PromiseRejectionEvent extends Event { constructor(t, i) { super(t, i); this.promise = i && i.promise; this.reason = i && i.reason; } }
class StorageEvent extends Event { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || null; this.oldValue = i.oldValue || null; this.newValue = i.newValue || null; this.url = i.url || ''; } }
class TransitionEvent extends Event {}
class AnimationEvent extends Event {}
class ClipboardEvent extends Event {}
class DragEvent extends MouseEvent {}
class TouchEvent extends UIEvent {}

const handlers = new WeakMap();                 // element -> { click: [source, function] }: onclick="..." attributes
function inline(t, type) {
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
const WINATTR = new Set(['load', 'resize', 'scroll', 'hashchange', 'popstate', 'beforeunload', 'unload', 'error', 'message', 'storage', 'pageshow', 'pagehide', 'focus', 'blur']);
function invoke(t, ev, phase) {
    ev.currentTarget = t; ev.eventPhase = phase;
    const m = t.__l && t.__l.get(ev.type);
    if (m) for (const x of m.slice()) {
        if (x.dead) continue;
        if (phase === 1 && !x.capture) continue;
        if (phase === 3 && x.capture) continue;
        if (x.once) t.removeEventListener(ev.type, x.fn, x.capture);
        ev.__passive = x.passive;
        try { typeof x.fn === 'function' ? x.fn.call(t, ev) : x.fn.handleEvent(ev); } catch (e) { report(e); }
        ev.__passive = false;
        if (ev.__imm) return;
    }
    if (phase !== 1) {                          // on<type> property or attribute, in the bubble/target position
        const on = t.__on && t.__on[ev.type];
        let h = on !== undefined ? on : null;
        if (h === null) h = inline(t, ev.type);
        if (h === null && t === g && WINATTR.has(ev.type) && document.body) h = inline(document.body, ev.type);
        if (typeof h === 'function') try {
            if (t === g && ev.type === 'error' && ev instanceof ErrorEvent) { if (h.call(t, ev.message, ev.filename, ev.lineno, ev.colno, ev.error) === true) ev.preventDefault(); }
            else if (h.call(t, ev) === false) ev.preventDefault();
        } catch (e) { if (!inReport) report(e); }
    }
}
function pathOf(t, type) {
    const p = [];
    if (t === g) return [g];
    for (let n = t; n; n = n.parentNode || (n.nodeType === 11 && n.host)) p.push(n);       // a shadow root's parent for events is its host
    if (p[p.length - 1] === document && type !== 'load') p.push(g);      // load events from elements stop at the document (DOM spec)
    return p;
}
function dispatch(t, ev) {
    ev.target = ev.srcElement = t;
    const path = pathOf(t, ev.type);
    Object.defineProperty(ev, '__path', { value: path, configurable: true });
    for (let i = path.length - 1; i > 0 && !ev.__stop; i--) invoke(path[i], ev, 1);
    if (!ev.__stop) invoke(t, ev, 2);
    if (ev.bubbles) for (let i = 1; i < path.length && !ev.__stop; i++) invoke(path[i], ev, 3);
    ev.currentTarget = null; ev.eventPhase = 0;
    return !ev.defaultPrevented;
}
V.dispatch = dispatch;
class EventTarget {
    addEventListener(type, fn, opt) {
        if (!fn) return;
        const l = this.__l || Object.defineProperty(this, '__l', { value: new Map() }).__l;
        type = String(type);
        let a = l.get(type);
        if (!a) l.set(type, a = []);
        const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
        if (a.some(x => x.fn === fn && x.capture === capture)) return;
        const o = { fn, capture, once: !!(opt && opt.once), passive: !!(opt && opt.passive), dead: false };
        a.push(o);
        if (opt && opt.signal) opt.signal.addEventListener('abort', () => this.removeEventListener(type, fn, capture));
    }
    removeEventListener(type, fn, opt) {
        const a = this.__l && this.__l.get(String(type));
        const capture = typeof opt === 'boolean' ? opt : !!(opt && opt.capture);
        const i = a ? a.findIndex(x => x.fn === fn && x.capture === capture) : -1;
        if (i >= 0) { a[i].dead = true; a.splice(i, 1); }
    }
    dispatchEvent(ev) {
        if (!(ev instanceof Event)) throw new TypeError('dispatchEvent: not an Event');
        ev.__stop = ev.__imm = false;
        return dispatch(this, ev);
    }
}
for (const t of ['click', 'dblclick', 'mousedown', 'mouseup', 'mousemove', 'mouseover', 'mouseout', 'mouseenter', 'mouseleave', 'contextmenu', 'wheel',
        'keydown', 'keyup', 'keypress', 'input', 'change', 'submit', 'reset', 'focus', 'blur', 'focusin', 'focusout', 'scroll', 'resize', 'load', 'error',
        'abort', 'select', 'toggle', 'pointerdown', 'pointerup', 'pointermove', 'pointerover', 'pointerout', 'pointerenter', 'pointerleave', 'pointercancel',
        'touchstart', 'touchend', 'touchmove', 'touchcancel', 'drag', 'dragstart', 'dragend', 'dragover', 'dragenter', 'dragleave', 'drop', 'copy', 'cut', 'paste',
        'hashchange', 'popstate', 'beforeunload', 'unload', 'message', 'storage', 'online', 'offline', 'readystatechange', 'DOMContentLoaded', 'animationend',
        'animationstart', 'transitionend', 'canplay', 'ended', 'play', 'pause', 'loadeddata', 'loadedmetadata', 'timeupdate', 'volumechange', 'invalid', 'close',
        'cancel', 'pageshow', 'pagehide', 'visibilitychange', 'selectionchange', 'auxclick', 'beforeinput', 'compositionstart', 'compositionend', 'fullscreenchange',
        'loadstart', 'loadend', 'progress', 'timeout', 'open', 'messageerror', 'unhandledrejection', 'rejectionhandled', 'languagechange', 'beforeprint', 'afterprint'])
    Object.defineProperty(EventTarget.prototype, 'on' + t, {
        get() { return this.__on && this.__on[t] !== undefined ? this.__on[t] : null; },
        set(f) { const o = this.__on || Object.defineProperty(this, '__on', { value: {} }).__on; o[t] = typeof f === 'function' ? f : null; }, configurable: true });

// the page's events from main.c: (node id or 0 = window, type, props). Returns true if a handler called preventDefault
const MOUSE = new Set(['click', 'dblclick', 'mousedown', 'mouseup', 'mousemove', 'mouseover', 'mouseout', 'mouseenter', 'mouseleave', 'contextmenu', 'auxclick']);
const NOBUBBLE = new Set(['load', 'error', 'focus', 'blur', 'mouseenter', 'mouseleave', 'abort', 'toggle', 'readystatechange', 'unload', 'resize']);
const NOCANCEL = new Set(['load', 'error', 'focus', 'blur', 'scroll', 'resize', 'input', 'change', 'DOMContentLoaded', 'readystatechange', 'mouseenter', 'mouseleave']);
let trusted = 0;                                // inside a user-initiated event: window.open is allowed
g.__dispatch = (id, type, x) => {
    const o = { bubbles: !NOBUBBLE.has(type), cancelable: !NOCANCEL.has(type) };
    if (x) Object.assign(o, x);
    const ev = new (type.startsWith('key') ? KeyboardEvent : MOUSE.has(type) ? MouseEvent : type.startsWith('pointer') ? PointerEvent : type === 'wheel' ? WheelEvent :
        type === 'input' ? InputEvent : type.startsWith('focus') || type === 'blur' ? FocusEvent : Event)(type, o);
    ev.isTrusted = true;
    if (id && type === 'focus') document.__active = wrap(id); else if (id && type === 'blur' && document.__active === wrap(id)) document.__active = null;
    const keep = trusted;
    trusted = type === 'click' || type === 'submit' || type.startsWith('key') || type === 'mousedown' || type === 'mouseup';
    try { return !dispatch(id ? wrap(id) : (type === 'DOMContentLoaded' || type === 'readystatechange') ? document : g, ev); } finally { trusted = keep; }
};
V.trusted = () => trusted;

// ---------- collections ----------
const ListProto = Object.create(Array.prototype, {
    item: { value(i) { return this[i] || null; } },
    namedItem: { value(n) { return this.find(e => e.id === n || e.getAttribute('name') === n) || null; } },
    [Symbol.toStringTag]: { value: 'NodeList' },
});
const coll = a => Object.setPrototypeOf(a, ListProto);
// a live NodeList/HTMLCollection: re-evaluated when the tree changed (loops like `while (list.length) frag.appendChild(list[0])` rely on it)
const isIdx = k => typeof k === 'string' && k.length > 0 && k.length < 10 && k.charCodeAt(0) >= 48 && k.charCodeAt(0) <= 57 && String(k >>> 0) === k;
function live(compute) {
    let g = -1, arr = [];
    const cur = () => { const n = d.gen(); if (n !== g) { arr = compute(); g = n; } return arr; };
    return new Proxy(coll([]), {
        get(t, k, r) { if (k === 'length') return cur().length; if (isIdx(k)) return cur()[k]; return Reflect.get(t, k, r); },
        has(t, k) { return isIdx(k) ? k < cur().length : k in t; },
        ownKeys(t) { const a = cur(), r = []; for (let i = 0; i < a.length; i++) r.push(String(i)); r.push('length'); return r; },
        getOwnPropertyDescriptor(t, k) { if (isIdx(k)) { const a = cur(); return k < a.length ? { value: a[k], writable: false, enumerable: true, configurable: true } : undefined; } return Reflect.getOwnPropertyDescriptor(t, k); },
    });
}
for (const n of ['NodeList', 'HTMLCollection', 'HTMLOptionsCollection', 'HTMLFormControlsCollection']) g[n] = Object.assign(function () { throw new TypeError('Illegal constructor'); }, { prototype: ListProto });
g.CSSStyleDeclaration = function CSSStyleDeclaration() {}; g.NamedNodeMap = function NamedNodeMap() {}; g.Window = function Window() {}; Object.defineProperty(g.Window, Symbol.hasInstance, { value: v => v != null && v.window === v }); /* lenis: wrapper instanceof Window */
V.coll = coll;

// ---------- mutation observers ----------
const OBS = new Set();
function record(rec) {
    if (!OBS.size) return;
    for (const o of OBS) {
        const opt = o.__opt;
        if (rec.type === 'attributes' ? !(opt.attributes || opt.attributeFilter || opt.attributeOldValue) || (opt.attributeFilter && !opt.attributeFilter.includes(rec.attributeName))
            : rec.type === 'childList' ? !opt.childList : !(opt.characterData || opt.characterDataOldValue)) continue;    // cheap option checks before any tree walk
        for (const t of o.__targets) {
            const hit = rec.target === t || (opt.subtree && t.contains(rec.target));
            if (!hit) continue;
            o.__q.push(Object.assign({ addedNodes: coll([]), removedNodes: coll([]), previousSibling: null, nextSibling: null, attributeName: null, oldValue: null }, rec));
            if (!o.__sched) { o.__sched = true; Promise.resolve().then(() => { o.__sched = false; const q = o.__q.splice(0); if (q.length) try { o.__cb(q, o); } catch (e) { report(e); } }); }
            break;
        }
    }
}
class MutationObserver {
    constructor(cb) { this.__cb = cb; this.__q = []; this.__targets = []; this.__opt = {}; }
    observe(n, opt) {
        opt = Object.assign({}, opt);
        if (opt.attributeOldValue || opt.attributeFilter) opt.attributes = opt.attributes !== false;
        if (opt.characterDataOldValue) opt.characterData = opt.characterData !== false;
        this.__opt = opt; if (!this.__targets.includes(n)) this.__targets.push(n); OBS.add(this);
    }
    disconnect() { OBS.delete(this); this.__targets = []; this.__q = []; }
    takeRecords() { return this.__q.splice(0); }
}

// ---------- nodes ----------
const TAGS = {};                                // tag -> class, filled below
const CE = new Map();                           // custom element name -> constructor
V.CE = CE;
function wrap(id) {
    if (!id) return null;
    let o = W[id];
    if (o) return o;
    const t = d.type(id);
    let proto;
    if (t === 1) {
        const tag = d.tag(id);
        const ce = tag.indexOf('-') > 0 && CE.get(tag);
        if (ce && !V.upgrade) {                 // a parsed element of an already defined custom element: run its constructor (class fields, Lit's renderOptions ...)
            V.upgrade = id;
            try { const e = new ce(); if (e && e.__id === id) return e; } catch (x) { V.upgrade = 0; report(x); }
            V.upgrade = 0;
        }
        proto = ce ? ce.prototype : (TAGS[tag] || HTMLUnknownElement).prototype;
    } else proto = (t === 3 ? Text : t === 8 ? Comment : t === 9 ? Document : d.host(id) ? ShadowRoot : DocumentFragment).prototype;
    o = Object.create(proto);
    Object.defineProperty(o, '__id', { value: id });
    return W[id] = o;
}
V.wrap = wrap;
const idOf = n => { if (!n || !n.__id) throw new TypeError('not a DOM node: ' + n); return n.__id; };
const asNode = n => (n !== null && typeof n === 'object' && n.__id) ? n : document.createTextNode(String(n));

function connectedCallbacks(n, name) {
    if (!CE.size || n.nodeType !== 1) return;
    const rec = e => { const f = e[name]; if (typeof f === 'function' && e.localName.indexOf('-') > 0) try { f.call(e); } catch (x) { report(x); } for (let c = e.firstChild; c; c = c.nextSibling) if (c.nodeType === 1) rec(c); };
    rec(n);
}

class Animation extends EventTarget {
    constructor(el, kf, o) {
        super();
        this.effect = { target: el, getTiming: () => o, getComputedTiming: () => o }; this.id = o.id || ''; this.playState = 'running'; this.pending = false;
        this.onfinish = null; this.oncancel = null; this.currentTime = 0; this.playbackRate = 1; this.timeline = null;
        this.__el = el; this.__kf = kf; this.__fill = o.fill === 'forwards' || o.fill === 'both';
        this.finished = new Promise((res, rej) => { this.__res = res; this.__rej = rej; }); this.finished.catch(() => {}); this.ready = Promise.resolve(this);
        const dur = Math.min(Math.max(+o.duration || 0, 0), 600) * Math.max(+o.iterations || 1, 1) + Math.max(+o.delay || 0, 0);
        this.__t = timer(() => this.finish(), isFinite(dur) ? dur : 600, [], false);
    }
    __last() {
        const k = this.__kf, r = {};
        if (Array.isArray(k)) { for (const f of k) if (f) Object.assign(r, f); }
        else if (k) for (const p in k) { const v = k[p]; r[p] = Array.isArray(v) ? v[v.length - 1] : v; }
        return r;
    }
    finish() {
        if (this.playState === 'finished' || this.playState === 'idle') return;
        this.playState = 'finished'; clear(this.__t);
        if (this.__fill) { const f = this.__last(), st = this.__el.style; for (const p in f) if (p !== 'offset' && p !== 'easing' && p !== 'composite') try { st[p === 'cssFloat' ? 'float' : p] = f[p]; } catch (e) {} }
        this.__res(this);
        const ev = new Event('finish'); ev.currentTime = this.currentTime; ev.timelineTime = null;
        if (typeof this.onfinish === 'function') try { this.onfinish(ev); } catch (e) { report(e); }
        this.dispatchEvent(ev);
    }
    cancel() { if (this.playState === 'idle') return; this.playState = 'idle'; clear(this.__t); const ev = new Event('cancel'); if (typeof this.oncancel === 'function') try { this.oncancel(ev); } catch (e) { report(e); } this.dispatchEvent(ev); }
    play() {} pause() {} reverse() {} updatePlaybackRate() {} persist() {} commitStyles() {}
}
class Node extends EventTarget {
    get nodeType() { return d.type(this.__id); }
    get nodeName() { const t = d.tag(this.__id); return this.nodeType === 1 ? t.toUpperCase() : t; }
    get parentNode() { return wrap(d.parent(this.__id)); }
    get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
    get firstChild() { return wrap(d.first(this.__id)); }
    get lastChild() { return wrap(d.last(this.__id)); }
    get nextSibling() { return wrap(d.next(this.__id)); }
    get previousSibling() { return wrap(d.prev(this.__id)); }
    get childNodes() { const id = this.__id; return live(() => { const r = []; for (let c = d.first(id); c; c = d.next(c)) r.push(wrap(c)); return r; }); }
    get ownerDocument() { return this.nodeType === 9 ? null : document; }
    get isConnected() { return d.connected(this.__id); }
    get baseURI() { return d.url(); }
    get textContent() { return this.nodeType === 9 ? null : d.text(this.__id); }
    set textContent(v) {
        v = v == null ? '' : String(v);
        if (OBS.size) { const rem = this.nodeType === 1 || this.nodeType === 11 ? [...this.childNodes] : null; d.setText(this.__id, v); if (rem) record({ type: 'childList', target: this, removedNodes: coll(rem), addedNodes: coll([...this.childNodes]) }); else record({ type: 'characterData', target: this, oldValue: null }); }
        else d.setText(this.__id, v);
    }
    get nodeValue() { const t = this.nodeType; return t === 3 || t === 8 ? d.text(this.__id) : null; }
    set nodeValue(v) { const t = this.nodeType; if (t === 3 || t === 8) this.textContent = v; }
    hasChildNodes() { return !!d.first(this.__id); }
    getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; }
    appendChild(c) { return this.insertBefore(c, null); }
    insertBefore(c, ref) {
        if (c && c.nodeType === 11) { const added = [...c.childNodes]; d.insert(this.__id, idOf(c), ref ? idOf(ref) : 0); if (OBS.size) record({ type: 'childList', target: this, addedNodes: coll(added), nextSibling: ref || null }); if (this.isConnected) for (const n of added) connectedCallbacks(n, 'connectedCallback'); return c; }
        const old = c && c.parentNode;
        d.insert(this.__id, idOf(c), ref ? idOf(ref) : 0);
        if (OBS.size) { if (old) record({ type: 'childList', target: old, removedNodes: coll([c]) }); record({ type: 'childList', target: this, addedNodes: coll([c]), nextSibling: ref || null, previousSibling: c.previousSibling }); }
        if (CE.size && this.isConnected) connectedCallbacks(c, 'connectedCallback');
        return c;
    }
    removeChild(c) {
        const prev = c && c.previousSibling, next = c && c.nextSibling;
        d.remove(this.__id, idOf(c));
        if (OBS.size) record({ type: 'childList', target: this, removedNodes: coll([c]), previousSibling: prev, nextSibling: next });
        connectedCallbacks(c, 'disconnectedCallback');
        return c;
    }
    replaceChild(n, o) { if (n === o) return o; this.insertBefore(n, o); this.removeChild(o); return o; }
    replaceChildren(...a) { while (this.firstChild) this.removeChild(this.firstChild); this.append(...a); }
    cloneNode(deep) { return wrap(d.clone(this.__id, !!deep)); }
    contains(n) { if (!n || n.__id === undefined) return false; const me = this.__id; for (let i = n.__id; i; i = d.parent(i)) if (i === me) return true; return false; }
    isSameNode(n) { return this === n; }
    isEqualNode(n) { return !!n && this.nodeType === n.nodeType && (this.nodeType === 1 ? this.outerHTML === n.outerHTML : this.textContent === n.textContent); }
    compareDocumentPosition(o) {
        if (this === o) return 0;
        if (this.contains(o)) return 20;        // contained by + following
        if (o.contains(this)) return 10;        // contains + preceding
        const a = [], b = [];
        for (let n = this; n; n = n.parentNode) a.unshift(n);
        for (let n = o; n; n = n.parentNode) b.unshift(n);
        if (a[0] !== b[0]) return 1;
        let i = 0;
        while (a[i] === b[i]) i++;
        for (let n = a[i].nextSibling; n; n = n.nextSibling) if (n === b[i]) return 4;
        return 2;
    }
    normalize() { for (let c = this.firstChild; c;) { const n = c.nextSibling; if (c.nodeType === 3) { if (c.data === '') this.removeChild(c); else if (n && n.nodeType === 3) { c.data += n.data; this.removeChild(n); continue; } } else c.normalize && c.normalize(); c = n; } }
    append(...a) { for (const n of a) this.appendChild(asNode(n)); }
    prepend(...a) { const f = this.firstChild; for (const n of a) this.insertBefore(asNode(n), f); }
    remove() { const p = d.parent(this.__id); if (p) wrap(p).removeChild(this); }
    replaceWith(...a) { const p = this.parentNode; if (!p) return; const nx = this.nextSibling; p.removeChild(this); for (const n of a) p.insertBefore(asNode(n), nx); }
    before(...a) { const p = this.parentNode; if (p) for (const n of a) p.insertBefore(asNode(n), this); }
    after(...a) { const p = this.parentNode; if (!p) return; const nx = this.nextSibling; for (const n of a) p.insertBefore(asNode(n), nx); }
    lookupNamespaceURI() { return null; }
}
Node.ELEMENT_NODE = 1; Node.ATTRIBUTE_NODE = 2; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;
Node.DOCUMENT_POSITION_DISCONNECTED = 1; Node.DOCUMENT_POSITION_PRECEDING = 2; Node.DOCUMENT_POSITION_FOLLOWING = 4; Node.DOCUMENT_POSITION_CONTAINS = 8; Node.DOCUMENT_POSITION_CONTAINED_BY = 16;

class CharacterData extends Node {
    get data() { return d.text(this.__id); }
    set data(v) { const old = OBS.size ? this.data : null; d.setText(this.__id, String(v)); record({ type: 'characterData', target: this, oldValue: old }); }
    get length() { return this.data.length; }
    get nextElementSibling() { for (let n = this.nextSibling; n; n = n.nextSibling) if (n.nodeType === 1) return n; return null; }
    get previousElementSibling() { for (let n = this.previousSibling; n; n = n.previousSibling) if (n.nodeType === 1) return n; return null; }
    substringData(o, n) { return this.data.substr(o, n); }
    appendData(s) { this.data += s; }
    insertData(o, s) { const t = this.data; this.data = t.slice(0, o) + s + t.slice(o); }
    deleteData(o, n) { const t = this.data; this.data = t.slice(0, o) + t.slice(o + n); }
    replaceData(o, n, s) { const t = this.data; this.data = t.slice(0, o) + s + t.slice(o + n); }
}
class Text extends CharacterData {
    get wholeText() { return this.data; }
    splitText(o) { const t = this.data, n = document.createTextNode(t.slice(o)); this.data = t.slice(0, o); if (this.parentNode) this.parentNode.insertBefore(n, this.nextSibling); return n; }
}
class Comment extends CharacterData {}
class CDATASection extends Text {}

// ---- selectors and queries shared by Element, Document and DocumentFragment
// :scope is the element the query runs on: the engine doesn't know it, so mark that element for the duration of the call
const scoped = (id, s, f) => {
    s = String(s);
    if (!s.includes(':scope')) return f(s);
    if (d.type(id) !== 1) return f(s.replace(/:scope/g, ':root'));
    d.setAttr(id, 'data-vq', '1');
    try { return f(s.replace(/:scope/g, '[data-vq]')); } finally { d.delAttr(id, 'data-vq'); }
};
const query = {
    querySelector(s) { return wrap(scoped(this.__id, s, q => d.qsa(this.__id, q, true))[0] || 0); },
    querySelectorAll(s) { return coll(scoped(this.__id, s, q => d.qsa(this.__id, q, false)).map(wrap)); },
    getElementsByTagName(t) { t = String(t); const q = t === '*' ? '*' : t.toLowerCase(); return live(() => this.querySelectorAll(q)); },
    getElementsByTagNameNS(ns, t) { return this.getElementsByTagName(t); },
    getElementsByClassName(c) { c = String(c).trim(); const q = c.split(/\s+/).map(x => '.' + CSS.escape(x)).join(''); return c ? live(() => this.querySelectorAll(q)) : coll([]); },
    get children() { const id = this.__id; return live(() => { const r = []; for (let c = d.first(id); c; c = d.next(c)) if (d.type(c) === 1) r.push(wrap(c)); return r; }); },
    get firstElementChild() { for (let c = d.first(this.__id); c; c = d.next(c)) if (d.type(c) === 1) return wrap(c); return null; },
    get lastElementChild() { for (let c = d.last(this.__id); c; c = d.prev(c)) if (d.type(c) === 1) return wrap(c); return null; },
    get childElementCount() { let n = 0; for (let c = d.first(this.__id); c; c = d.next(c)) if (d.type(c) === 1) n++; return n; },
    getElementById(id) { id = String(id); for (const e of d.qsa(this.__id, '[id]', false)) if (d.getAttr(e, 'id') === id) return wrap(e); return null; },
};
// constructed stylesheets: each adopted sheet gets a hidden <style> in the document head / the shadow root that its rules are written to
function adopt(root, list) {
    list = [...list || []];
    const old = root.__ass || [], where = root.head || root;
    for (const sh of old) if (!list.includes(sh) && sh.__ad && sh.__o) { sh.__o.remove(); sh.__o = null; sh.__ad = false; }
    for (const sh of list) if (sh && !sh.__o) { const st = document.createElement('style'); st.setAttribute('data-adopted', ''); where.appendChild(st); sh.__o = st; sh.__ad = true; sh.__sync(); }
    root.__ass = list;
}
function mix(dst, src) { Object.defineProperties(dst, Object.getOwnPropertyDescriptors(src)); }

class DocumentFragment extends Node {
    get adoptedStyleSheets() { return this.__ass || (this.__ass = []); } set adoptedStyleSheets(v) { adopt(this, v); }
    get innerHTML() { return d.html(this.__id, false); }
    set innerHTML(v) { d.setHTML(this.__id, v == null ? '' : String(v)); }
}
mix(DocumentFragment.prototype, query);
class ShadowRoot extends DocumentFragment {
    get mode() { return d.smode(this.__id) === 2 ? 'closed' : 'open'; } get host() { return wrap(d.host(this.__id)); } get delegatesFocus() { return false; }
    get adoptedStyleSheets() { return this.__ass || (this.__ass = []); } set adoptedStyleSheets(v) { adopt(this, v); }
    get activeElement() { return null; } getSelection() { return g.getSelection(); }
}

// ---- class list, style, dataset
class DOMTokenList {
    constructor(e, a) { Object.defineProperty(this, 'e', { value: e }); Object.defineProperty(this, 'a', { value: a || 'class' }); }
    get _a() { return (this.e.getAttribute(this.a) || '').split(/\s+/).filter(Boolean); }
    _set(a) { this.e.setAttribute(this.a, a.join(' ')); }
    add(...c) { const a = this._a; for (const x of c) { if (x === '') throw new DOMException('The token provided must not be empty.', 'SyntaxError'); if (!a.includes(x)) a.push(x); } this._set(a); }
    remove(...c) { this._set(this._a.filter(x => !c.includes(x))); }
    contains(c) { return this._a.includes(c); }
    toggle(c, force) { const on = force === undefined ? !this.contains(c) : !!force; on ? this.add(c) : this.remove(c); return on; }
    replace(a, b) { const l = this._a, i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set([...new Set(l)]); return true; }
    supports() { return true; }
    item(i) { return this._a[i] || null; }
    get length() { return this._a.length; }
    get value() { return this.e.getAttribute(this.a) || ''; }
    set value(v) { this.e.setAttribute(this.a, v); }
    toString() { return this.value; }
    forEach(f, t) { this._a.forEach(f, t); }
    entries() { return this._a.entries(); } keys() { return this._a.keys(); } values() { return this._a[Symbol.iterator](); }
    [Symbol.iterator]() { return this._a[Symbol.iterator](); }
}

const kebab = k => k.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
const camel = k => k.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
function cssName(k) {
    if (k === 'cssFloat') return 'float';
    if (/^(webkit|moz|ms|o)[A-Z]/.test(k)) k = '-' + k;
    return kebab(k);
}
function parseStyle(s) {
    const m = new Map();
    s = s.replace(/\/\*[\s\S]*?\*\//g, '');
    for (const p of s.split(/;(?![^(]*\))/)) {
        const i = p.indexOf(':');
        if (i > 0) m.set(p.slice(0, i).trim().toLowerCase(), p.slice(i + 1).trim());
    }
    return m;
}
const KNOWN_CSS = new Set('align-content align-items align-self all animation animation-delay animation-direction animation-duration animation-fill-mode animation-iteration-count animation-name animation-play-state animation-timing-function appearance aspect-ratio backdrop-filter backface-visibility background background-attachment background-blend-mode background-clip background-color background-image background-origin background-position background-position-x background-position-y background-repeat background-size block-size border border-bottom border-bottom-color border-bottom-left-radius border-bottom-right-radius border-bottom-style border-bottom-width border-collapse border-color border-image border-inline border-inline-end border-inline-start border-left border-left-color border-left-style border-left-width border-radius border-right border-right-color border-right-style border-right-width border-spacing border-style border-top border-top-color border-top-left-radius border-top-right-radius border-top-style border-top-width border-width bottom box-shadow box-sizing break-after break-before break-inside caption-side caret-color clear clip clip-path color column-count column-gap column-rule column-width columns content counter-increment counter-reset cursor direction display empty-cells fill filter flex flex-basis flex-direction flex-flow flex-grow flex-shrink flex-wrap float font font-family font-feature-settings font-kerning font-size font-size-adjust font-stretch font-style font-variant font-variant-numeric font-weight gap grid grid-area grid-auto-columns grid-auto-flow grid-auto-rows grid-column grid-column-end grid-column-start grid-gap grid-row grid-row-end grid-row-start grid-template grid-template-areas grid-template-columns grid-template-rows height hyphens image-rendering inline-size inset inset-block inset-inline isolation justify-content justify-items justify-self left letter-spacing line-height list-style list-style-image list-style-position list-style-type margin margin-block margin-bottom margin-inline margin-left margin-right margin-top mask mask-image mask-position mask-repeat mask-size max-block-size max-height max-inline-size max-width min-block-size min-height min-inline-size min-width mix-blend-mode object-fit object-position offset opacity order orphans outline outline-color outline-offset outline-style outline-width overflow overflow-anchor overflow-wrap overflow-x overflow-y overscroll-behavior padding padding-block padding-bottom padding-inline padding-left padding-right padding-top page-break-after page-break-before page-break-inside perspective perspective-origin place-content place-items place-self pointer-events position quotes resize right rotate row-gap scale scroll-behavior scroll-margin scroll-padding scroll-snap-align scroll-snap-type shape-outside stroke stroke-dasharray stroke-dashoffset stroke-linecap stroke-linejoin stroke-width tab-size table-layout text-align text-align-last text-decoration text-decoration-color text-decoration-line text-decoration-style text-decoration-thickness text-indent text-overflow text-rendering text-shadow text-transform text-underline-offset top touch-action transform transform-box transform-origin transform-style transition transition-delay transition-duration transition-property transition-timing-function translate unicode-bidi user-select vertical-align visibility white-space widows width will-change word-break word-spacing word-wrap writing-mode z-index zoom'.split(' ').concat(['float']));
function styleOf(e) {
    const parse = () => parseStyle(e.getAttribute('style') || '');
    const write = m => { const s = [...m].map(([a, b]) => a + ': ' + b).join('; '); s ? e.setAttribute('style', s + ';') : e.removeAttribute('style'); };
    const set = (n, v, pri) => {
        const m = parse();
        v = v == null ? '' : String(v).trim();
        if (v === '') m.delete(n); else m.set(n, pri === 'important' ? v + ' !important' : v);
        write(m);
    };
    const val = n => (parse().get(n) || '').replace(/\s*!important$/, '');
    const api = {
        getPropertyValue: n => val(String(n).toLowerCase()),
        getPropertyPriority: n => /!important$/.test(parse().get(String(n).toLowerCase()) || '') ? 'important' : '',
        setProperty: (n, v, p) => set(String(n).toLowerCase(), v, p),
        removeProperty: n => { const v = val(String(n).toLowerCase()); set(String(n).toLowerCase(), ''); return v; },
        item: i => [...parse().keys()][i] || '',
        [Symbol.iterator]: () => parse().keys(),
        toString: () => e.getAttribute('style') || '',
    };
    return new Proxy({}, {
        get(t, k) {
            if (typeof k !== 'string') return api[k];
            if (k === 'cssText') return e.getAttribute('style') || '';
            if (k === 'length') return parse().size;
            if (k === 'parentRule') return null;
            if (k in api) return api[k];
            return val(cssName(k));
        },
        set(t, k, v) { if (k === 'cssText') e.setAttribute('style', String(v)); else if (typeof k === 'string') set(cssName(k), v); return true; },
        has(t, k) { return typeof k === 'string' && (k in api || KNOWN_CSS.has(cssName(k)) || parse().has(cssName(k))); },
        ownKeys() { return [...parse().keys()].map(camel); },
        getOwnPropertyDescriptor(t, k) { return typeof k === 'string' && parse().has(cssName(k)) ? { value: val(cssName(k)), enumerable: true, configurable: true, writable: true } : undefined; },
    });
}

function datasetOf(e) {
    const name = k => 'data-' + kebab(String(k));
    return new Proxy({}, {
        get: (t, k) => typeof k === 'string' ? (e.getAttribute(name(k)) ?? undefined) : undefined,
        set: (t, k, v) => { e.setAttribute(name(k), String(v)); return true; },
        deleteProperty: (t, k) => { e.removeAttribute(name(k)); return true; },
        has: (t, k) => typeof k === 'string' && e.hasAttribute(name(k)),
        ownKeys: () => e.getAttributeNames().filter(n => n.startsWith('data-')).map(n => camel(n.slice(5))),
        getOwnPropertyDescriptor: (t, k) => typeof k === 'string' && e.hasAttribute(name(k)) ? { value: e.getAttribute(name(k)), enumerable: true, configurable: true, writable: true } : undefined,
    });
}

class Attr {
    constructor(e, name) { this.ownerElement = e; this.name = this.localName = name; this.specified = true; this.namespaceURI = null; this.prefix = null; }
    get value() { return this.ownerElement.getAttribute(this.name); }
    set value(v) { this.ownerElement.setAttribute(this.name, v); }
    get nodeValue() { return this.value; }
    get textContent() { return this.value; }
}
function attrsOf(e) {
    const a = d.attrs(e.__id).map(n => new Attr(e, n));
    a.item = i => a[i] || null;
    a.getNamedItem = n => a.find(x => x.name === String(n).toLowerCase()) || null;
    a.setNamedItem = x => { e.setAttribute(x.name, x.value); };
    a.removeNamedItem = n => { const r = a.getNamedItem(n); e.removeAttribute(n); return r; };
    return a;
}

// innerText: rendered text approximated from the tree
const BLOCKS = new Set(['address', 'article', 'aside', 'blockquote', 'br', 'dd', 'details', 'div', 'dl', 'dt', 'fieldset', 'figcaption', 'figure', 'footer', 'form', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hr', 'li', 'main', 'nav', 'ol', 'p', 'pre', 'section', 'table', 'tr', 'ul']);
function itext(e, out, pre) {
    for (let c = e.firstChild; c; c = c.nextSibling) {
        const t = c.nodeType;
        if (t === 3) out.push(pre ? c.data : c.data.replace(/[ \t\r\n\f]+/g, ' '));
        else if (t === 1) {
            const n = c.localName;
            if (n === 'script' || n === 'style' || n === 'noscript' || n === 'template' || c.hasAttribute('hidden')) continue;
            if (n === 'br') { out.push('\n'); continue; }
            const b = BLOCKS.has(n);
            if (b) out.push('\u0001');
            else if (n === 'td' || n === 'th') out.push('\t');
            itext(c, out, pre || n === 'pre');
            if (b) out.push('\u0001');
        }
    }
}
function innerText(e) {
    const out = [];
    itext(e, out, false);
    return out.join('').replace(/\n?\u0001+/g, '\n').replace(/ *\n */g, '\n').replace(/^\n+|\n+$/g, '').replace(/^ +| +$/g, '');
}

const rectOf = e => { const r = d.rect(e.__id), s = d.view()[0]; return { x: r[0], y: r[1] - s, w: r[2], h: r[3], dy: r[1] }; };
class DOMRect {
    constructor(x = 0, y = 0, w = 0, h = 0) { Object.assign(this, { x, y, width: w, height: h }); }
    get left() { return Math.min(this.x, this.x + this.width); } get right() { return Math.max(this.x, this.x + this.width); }
    get top() { return Math.min(this.y, this.y + this.height); } get bottom() { return Math.max(this.y, this.y + this.height); }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, right: this.right, bottom: this.bottom, left: this.left }; }
    static fromRect(r) { return new DOMRect(r && r.x, r && r.y, r && r.width, r && r.height); }
}

class Element extends Node {
    get tagName() { return d.tag(this.__id).toUpperCase(); }
    get localName() { return d.tag(this.__id); }
    get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }
    get prefix() { return null; }
    getAttribute(n) { return d.getAttr(this.__id, String(n).toLowerCase()); }
    getAttributeNS(ns, n) { return this.getAttribute(n); }
    setAttribute(n, v) {
        n = String(n).toLowerCase(); v = String(v);
        if (OBS.size || CE.size) {
            const old = d.getAttr(this.__id, n);
            d.setAttr(this.__id, n, v);
            record({ type: 'attributes', target: this, attributeName: n, oldValue: old });
            if (CE.size && old !== v) this.attributeChangedCallback && (this.constructor.observedAttributes || []).includes(n) && this.attributeChangedCallback(n, old, v);
        } else d.setAttr(this.__id, n, v);
    }
    setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
    removeAttribute(n) {
        n = String(n).toLowerCase();
        const old = OBS.size || CE.size ? d.getAttr(this.__id, n) : null;
        d.delAttr(this.__id, n);
        if (old !== null) { record({ type: 'attributes', target: this, attributeName: n, oldValue: old }); if (CE.size && this.attributeChangedCallback && (this.constructor.observedAttributes || []).includes(n)) this.attributeChangedCallback(n, old, null); }
    }
    removeAttributeNS(ns, n) { this.removeAttribute(n); }
    hasAttribute(n) { return d.getAttr(this.__id, String(n).toLowerCase()) !== null; }
    hasAttributes() { return d.attrs(this.__id).length > 0; }
    toggleAttribute(n, f) { const on = f === undefined ? !this.hasAttribute(n) : !!f; on ? (this.hasAttribute(n) || this.setAttribute(n, '')) : this.removeAttribute(n); return on; }
    getAttributeNames() { return d.attrs(this.__id); }
    get attributes() { return attrsOf(this); }
    getAttributeNode(n) { return this.hasAttribute(n) ? new Attr(this, String(n).toLowerCase()) : null; }
    get id() { return this.getAttribute('id') || ''; }
    set id(v) { this.setAttribute('id', v); }
    get className() { return this.getAttribute('class') || ''; }
    set className(v) { this.setAttribute('class', v); }
    get classList() { return this.__cl || Object.defineProperty(this, '__cl', { value: new DOMTokenList(this) }).__cl; }
    get style() { return this.__st || Object.defineProperty(this, '__st', { value: styleOf(this) }).__st; }
    set style(v) { this.style.cssText = v; }
    get dataset() { return this.__ds || Object.defineProperty(this, '__ds', { value: datasetOf(this) }).__ds; }
    get innerHTML() { return d.html(this.__id, false); }
    set innerHTML(v) {
        v = v == null ? "" : String(v);
        if (/^(SCRIPT|STYLE|TEXTAREA|TITLE)$/.test(this.tagName)) { this.textContent = v; return; }
        if (OBS.size) { const rem = [...this.childNodes]; d.setHTML(this.__id, v); record({ type: 'childList', target: this, removedNodes: coll(rem), addedNodes: coll([...this.childNodes]) }); }
        else d.setHTML(this.__id, v);
        if (CE.size && this.isConnected) for (let c = this.firstChild; c; c = c.nextSibling) connectedCallbacks(c, 'connectedCallback');
    }
    get outerHTML() { return d.html(this.__id, true); }
    set outerHTML(v) { const p = this.parentNode; if (!p) return; const t = document.createElement('div'); t.innerHTML = String(v); const nx = this.nextSibling; p.removeChild(this); while (t.firstChild) p.insertBefore(t.firstChild, nx); }
    get innerText() { return innerText(this); }
    set innerText(v) { this.textContent = v; }
    get outerText() { return this.innerText; }
    get nextElementSibling() { for (let n = this.nextSibling; n; n = n.nextSibling) if (n.nodeType === 1) return n; return null; }
    get previousElementSibling() { for (let n = this.previousSibling; n; n = n.previousSibling) if (n.nodeType === 1) return n; return null; }
    matches(s) { return scoped(this.__id, s, q => d.matches(this.__id, q)); }
    webkitMatchesSelector(s) { return this.matches(s); }
    closest(s) { for (let e = this; e && e.nodeType === 1; e = e.parentNode) if (e.matches(s)) return e; return null; }
    insertAdjacentHTML(where, html) {
        const t = document.createElement('div');
        t.innerHTML = html;
        const w = String(where).toLowerCase(), f = document.createDocumentFragment();
        while (t.firstChild) f.appendChild(t.firstChild);
        this.insertAdjacentNode(w, f);
    }
    insertAdjacentNode(where, n) {
        const w = String(where).toLowerCase();
        if (w === 'beforebegin') { if (!this.parentNode) return null; this.parentNode.insertBefore(n, this); }
        else if (w === 'afterbegin') this.insertBefore(n, this.firstChild);
        else if (w === 'beforeend') this.appendChild(n);
        else if (w === 'afterend') { if (!this.parentNode) return null; this.parentNode.insertBefore(n, this.nextSibling); }
        else throw new DOMException('The value provided (' + where + ') is not one of beforebegin, afterbegin, beforeend, afterend', 'SyntaxError');
        return n;
    }
    insertAdjacentElement(where, e) { return this.insertAdjacentNode(where, e); }
    insertAdjacentText(where, s) { this.insertAdjacentNode(where, document.createTextNode(String(s))); }
    attachShadow(o) {
        const id = d.attachShadow(this.__id, o && o.mode === 'closed' ? 2 : 1);
        if (!id) throw new DOMException('This element does not support attachShadow', 'NotSupportedError');
        return wrap(id);
    }
    get shadowRoot() { const id = d.shadow(this.__id); return id && d.smode(id) === 1 ? wrap(id) : null; }
    animate(kf, o) {                            // runs for its duration (capped), then applies the end frame when fill holds it
        const a = new Animation(this, kf, typeof o === 'number' ? { duration: o } : o || {});
        return a;
    }
    getAnimations() { return []; }
    // geometry: from the display items laid out for the node (js.c n_rect)
    getBoundingClientRect() { const r = rectOf(this); return new DOMRect(r.x, r.y, r.w, r.h); }
    getClientRects() { const r = this.getBoundingClientRect(); return r.width || r.height ? [r] : []; }
    get offsetWidth() { return rectOf(this).w; }
    get offsetHeight() { return rectOf(this).h; }
    get offsetTop() { return rectOf(this).dy; }
    get offsetLeft() { return rectOf(this).x; }
    get offsetParent() { return this === document.body ? null : document.body; }
    get clientWidth() { return this === document.documentElement ? d.view()[1] : rectOf(this).w; }
    get clientHeight() { return this === document.documentElement ? d.view()[2] : rectOf(this).h; }
    get clientTop() { return 0; }
    get clientLeft() { return 0; }
    get scrollWidth() { return rectOf(this).w; }
    get scrollHeight() { return this === document.documentElement || this === document.body ? Math.max(rectOf(document.body).h, d.view()[2]) : rectOf(this).h; }
    get scrollTop() { return this === document.documentElement || this === document.body ? d.view()[0] : this.__sy || 0; }
    set scrollTop(v) { if (this === document.documentElement || this === document.body) d.scrollTo(0, +v || 0); else this.__sy = +v || 0; }
    get scrollLeft() { return this.__sx || 0; }
    set scrollLeft(v) { this.__sx = +v || 0; }
    scrollTo(x, y) { if (x && typeof x === 'object') { y = x.top; x = x.left; } if (this === document.documentElement || this === document.body) d.scrollTo(x || 0, y || 0); else { this.__sx = +x || 0; this.__sy = +y || 0; } }
    scrollBy(x, y) { if (x && typeof x === 'object') { y = x.top; x = x.left; } this.scrollTo((this.scrollLeft) + (x || 0), this.scrollTop + (y || 0)); }
    scrollIntoView() { d.scrollTo(0, Math.max(0, rectOf(this).dy - 16)); }
    scrollIntoViewIfNeeded() { const r = rectOf(this); if (r.y < 0 || r.y > d.view()[2]) this.scrollIntoView(); }
    get hidden() { return this.hasAttribute('hidden'); }
    set hidden(v) { this.toggleAttribute('hidden', !!v); }
    focus() {
        if (document.__active === this) return;
        const old = document.__active;
        if (old && old !== this) { old.dispatchEvent(new FocusEvent('blur', { relatedTarget: this })); old.dispatchEvent(new FocusEvent('focusout', { bubbles: true, relatedTarget: this })); }
        document.__active = this; d.focus(this.__id);
        this.dispatchEvent(new FocusEvent('focus', { relatedTarget: old })); this.dispatchEvent(new FocusEvent('focusin', { bubbles: true, relatedTarget: old }));
    }
    blur() {
        if (document.__active !== this) return;
        document.__active = null;
        this.dispatchEvent(new FocusEvent('blur')); this.dispatchEvent(new FocusEvent('focusout', { bubbles: true }));
    }
    click() { if (this.disabled) return; dispatch(this, new MouseEvent('click', { bubbles: true, cancelable: true })); }
    get tabIndex() { const v = this.getAttribute('tabindex'); return v === null ? -1 : parseInt(v) || 0; }
    set tabIndex(v) { this.setAttribute('tabindex', v); }
    get isContentEditable() { return false; }
    get contentEditable() { return this.getAttribute('contenteditable') || 'inherit'; }
    set contentEditable(v) { this.setAttribute('contenteditable', v); }
    get title() { return this.getAttribute('title') || ''; }
    set title(v) { this.setAttribute('title', v); }
    get lang() { return this.getAttribute('lang') || ''; }
    set lang(v) { this.setAttribute('lang', v); }
    get dir() { return this.getAttribute('dir') || ''; }
    set dir(v) { this.setAttribute('dir', v); }
    get slot() { return this.getAttribute('slot') || ''; }
    set slot(v) { this.setAttribute('slot', v); }
    get part() { return new DOMTokenList(this, 'part'); }
    checkVisibility() { return !this.closest('[hidden]'); }
    requestFullscreen() { return Promise.reject(new TypeError('fullscreen is not available')); }
    setPointerCapture() {} releasePointerCapture() {} hasPointerCapture() { return false; }
    // ParentNode / ChildNode extras
    get assignedSlot() { const p = this.parentNode; const sr = p && p.shadowRoot; if (!sr) return null; return [...sr.querySelectorAll('slot')].find(sl => sl.assignedNodes().includes(this)) || null; }
}
mix(Element.prototype, query);
// reflected attributes
function reflect(cls, strs, bools, ints, urls) {
    const P = cls.prototype;
    for (const [p, a] of Object.entries(strs || {})) Object.defineProperty(P, p, { get() { return this.getAttribute(a) ?? ''; }, set(v) { this.setAttribute(a, v); }, configurable: true });
    for (const [p, a] of Object.entries(bools || {})) Object.defineProperty(P, p, { get() { return this.hasAttribute(a); }, set(v) { this.toggleAttribute(a, !!v); }, configurable: true });
    for (const [p, a] of Object.entries(ints || {})) Object.defineProperty(P, p, { get() { const v = parseInt(this.getAttribute(a)); return isNaN(v) ? 0 : v; }, set(v) { this.setAttribute(a, v); }, configurable: true });
    for (const [p, a] of Object.entries(urls || {})) Object.defineProperty(P, p, { get() { const v = this.getAttribute(a); return v === null ? '' : d.resolve(v); }, set(v) { this.setAttribute(a, v); }, configurable: true });
}
class HTMLElement extends Element {
    constructor() {                             // new MyElement(): a custom element being created or upgraded
        super();
        const id = V.upgrade || d.create(V.nameOf(new.target));
        V.upgrade = 0;
        const o = Object.create(new.target.prototype);
        Object.defineProperty(o, '__id', { value: id });
        return W[id] = o;
    }
}
class HTMLUnknownElement extends HTMLElement {}
class SVGElement extends Element { get ownerSVGElement() { return null; } get viewportElement() { return null; } get className() { const e = this; return new SVGAnimatedString(() => e.getAttribute('class') || '', v => e.setAttribute('class', v)); } }
class SVGAnimatedString { constructor(g, s) { this.__g = g; this.__s = s; } get baseVal() { return this.__g(); } set baseVal(v) { this.__s(String(v)); } get animVal() { return this.__g(); } }
g.SVGAnimatedString = SVGAnimatedString;
reflect(Element, { accessKey: 'accesskey', ariaLabel: 'aria-label', role: 'role' });
reflect(HTMLElement, { accessKey: 'accesskey', translate: 'translate', draggable: 'draggable', inputMode: 'inputmode', enterKeyHint: 'enterkeyhint', nonce: 'nonce' });
V.nameOf = ctor => { for (const [n, c] of CE) if (c === ctor) return n; throw new TypeError('Illegal constructor'); };

function defcls(name, base, tags) {
    const c = { [name]: class extends base {} }[name];
    g[name] = c;
    for (const t of tags || []) TAGS[t] = c;
    return c;
}
for (const [n, tags] of [['HTMLDivElement', ['div']], ['HTMLSpanElement', ['span']], ['HTMLParagraphElement', ['p']], ['HTMLHeadingElement', ['h1', 'h2', 'h3', 'h4', 'h5', 'h6']],
        ['HTMLUListElement', ['ul']], ['HTMLOListElement', ['ol']], ['HTMLLIElement', ['li']], ['HTMLBRElement', ['br']], ['HTMLHRElement', ['hr']], ['HTMLPreElement', ['pre']],
        ['HTMLBodyElement', ['body']], ['HTMLHeadElement', ['head']], ['HTMLHtmlElement', ['html']], ['HTMLTitleElement', ['title']], ['HTMLTableElement', ['table']],
        ['HTMLTableRowElement', ['tr']], ['HTMLTableCellElement', ['td', 'th']], ['HTMLTableSectionElement', ['tbody', 'thead', 'tfoot']], ['HTMLTableCaptionElement', ['caption']],
        ['HTMLQuoteElement', ['blockquote', 'q']], ['HTMLDListElement', ['dl']], ['HTMLFieldSetElement', ['fieldset']], ['HTMLLegendElement', ['legend']],
        ['HTMLMenuElement', ['menu']], ['HTMLDataElement', ['data']], ['HTMLTimeElement', ['time']], ['HTMLPictureElement', ['picture']], ['HTMLSourceElement', ['source']],
        ['HTMLTrackElement', ['track']], ['HTMLParamElement', ['param']], ['HTMLMapElement', ['map']], ['HTMLAreaElement', ['area']], ['HTMLProgressElement', ['progress']],
        ['HTMLMeterElement', ['meter']], ['HTMLOutputElement', ['output']], ['HTMLObjectElement', ['object']], ['HTMLEmbedElement', ['embed']], ['HTMLSlotElement', ['slot']],
        ['HTMLOptGroupElement', ['optgroup']], ['HTMLDataListElement', ['datalist']], ['HTMLFontElement', ['font']]])
    defcls(n, HTMLElement, tags);
const HTMLAnchorElement = defcls('HTMLAnchorElement', HTMLElement, ['a']);
const HTMLImageElement = defcls('HTMLImageElement', HTMLElement, ['img']);
const HTMLInputElement = defcls('HTMLInputElement', HTMLElement, ['input']);
const HTMLButtonElement = defcls('HTMLButtonElement', HTMLElement, ['button']);
const HTMLFormElement = defcls('HTMLFormElement', HTMLElement, ['form']);
const HTMLSelectElement = defcls('HTMLSelectElement', HTMLElement, ['select']);
const HTMLOptionElement = defcls('HTMLOptionElement', HTMLElement, ['option']);
const HTMLTextAreaElement = defcls('HTMLTextAreaElement', HTMLElement, ['textarea']);
const HTMLLabelElement = defcls('HTMLLabelElement', HTMLElement, ['label']);
const HTMLScriptElement = defcls('HTMLScriptElement', HTMLElement, ['script']);
const HTMLLinkElement = defcls('HTMLLinkElement', HTMLElement, ['link']);
const HTMLStyleElement = defcls('HTMLStyleElement', HTMLElement, ['style']);
const HTMLMetaElement = defcls('HTMLMetaElement', HTMLElement, ['meta']);
const HTMLBaseElement = defcls('HTMLBaseElement', HTMLElement, ['base']);
const HTMLTemplateElement = defcls('HTMLTemplateElement', HTMLElement, ['template']);
const HTMLCanvasElement = defcls('HTMLCanvasElement', HTMLElement, ['canvas']);
const HTMLMediaElement = defcls('HTMLMediaElement', HTMLElement, []);
const HTMLVideoElement = defcls('HTMLVideoElement', HTMLMediaElement, ['video']);
const HTMLAudioElement = defcls('HTMLAudioElement', HTMLMediaElement, ['audio']);
const HTMLIFrameElement = defcls('HTMLIFrameElement', HTMLElement, ['iframe']);
const HTMLDialogElement = defcls('HTMLDialogElement', HTMLElement, ['dialog']);
const HTMLDetailsElement = defcls('HTMLDetailsElement', HTMLElement, ['details']);
for (const t of ['svg', 'path', 'g', 'circle', 'rect', 'line', 'polyline', 'polygon', 'ellipse', 'text', 'tspan', 'use', 'defs', 'symbol', 'clippath', 'lineargradient', 'radialgradient', 'stop', 'mask', 'image', 'foreignobject'])
    TAGS[t] = SVGElement;
g.SVGSVGElement = SVGElement; g.SVGGraphicsElement = SVGElement; g.SVGGeometryElement = SVGElement; g.SVGPathElement = SVGElement; g.SVGUseElement = SVGElement; g.SVGAElement = SVGElement; g.SVGTextContentElement = SVGElement;

reflect(HTMLAnchorElement, { target: 'target', rel: 'rel', download: 'download', hreflang: 'hreflang', type: 'type', referrerPolicy: 'referrerpolicy', ping: 'ping' }, 0, 0, { href: 'href' });
for (const [p, f] of [['protocol', 'protocol'], ['host', 'host'], ['hostname', 'hostname'], ['port', 'port'], ['pathname', 'pathname'], ['search', 'search'], ['hash', 'hash'], ['origin', 'origin'], ['username', 'username'], ['password', 'password']])
    Object.defineProperty(HTMLAnchorElement.prototype, p, { get() { const h = this.href; try { return new g.URL(h)[f]; } catch (e) { return ''; } },
        set(v) { try { const u = new g.URL(this.href); u[f] = v; this.setAttribute('href', u.href); } catch (e) {} }, configurable: true });
Object.defineProperty(HTMLAnchorElement.prototype, 'text', { get() { return this.textContent; }, set(v) { this.textContent = v; } });
HTMLAnchorElement.prototype.toString = function () { return this.href; };
get_rel(HTMLAnchorElement); get_rel(HTMLLinkElement);
function get_rel(c) { Object.defineProperty(c.prototype, 'relList', { get() { return new DOMTokenList(this, 'rel'); } }); }

reflect(HTMLImageElement, { alt: 'alt', srcset: 'srcset', sizes: 'sizes', useMap: 'usemap', loading: 'loading', decoding: 'decoding', crossOrigin: 'crossorigin', referrerPolicy: 'referrerpolicy' }, { isMap: 'ismap' }, 0, { src: 'src' });
for (const p of ['width', 'height']) Object.defineProperty(HTMLImageElement.prototype, p, { get() { const v = parseInt(this.getAttribute(p)); return isNaN(v) ? this['natural' + p[0].toUpperCase() + p.slice(1)] : v; }, set(v) { this.setAttribute(p, v); } });
Object.defineProperties(HTMLImageElement.prototype, { naturalWidth: { get() { return 0; } }, naturalHeight: { get() { return 0; } }, complete: { get() { return true; } },
    currentSrc: { get() { return this.src; } }, x: { get() { return rectOf(this).x; } }, y: { get() { return rectOf(this).y; } }, decode: { value() { return Promise.resolve(); } } });
g.Image = function Image(w, h) { const i = document.createElement('img'); if (w !== undefined) i.setAttribute('width', w); if (h !== undefined) i.setAttribute('height', h); return i; };
g.Image.prototype = HTMLImageElement.prototype;
g.Option = function Option(text, value, def, sel) { const o = document.createElement('option'); if (text !== undefined) o.text = text; if (value !== undefined) o.setAttribute('value', value); if (def) o.setAttribute('selected', ''); if (sel) o.selected = true; return o; };

// form controls: value/checked live in form.c (natives); the rest reflects attributes
const FORMCTL = {
    get form() { return this.closest('form'); },
    get labels() { const id = this.id; return coll(id ? [...document.querySelectorAll('label')].filter(l => l.getAttribute('for') === id) : []); },
    get validity() { return { valid: true, valueMissing: false, typeMismatch: false, patternMismatch: false, tooLong: false, tooShort: false, rangeUnderflow: false, rangeOverflow: false, stepMismatch: false, badInput: false, customError: false }; },
    get validationMessage() { return ''; }, get willValidate() { return !this.disabled; },
    checkValidity() { return true; }, reportValidity() { return true; }, setCustomValidity() {},
};
mix(HTMLInputElement.prototype, FORMCTL); mix(HTMLButtonElement.prototype, FORMCTL); mix(HTMLSelectElement.prototype, FORMCTL); mix(HTMLTextAreaElement.prototype, FORMCTL);
for (const c of [HTMLInputElement, HTMLButtonElement, HTMLSelectElement, HTMLTextAreaElement, HTMLOptionElement, g.HTMLFieldSetElement, g.HTMLOptGroupElement])
    Object.defineProperty(c.prototype, 'disabled', { get() { return this.hasAttribute('disabled'); }, set(v) { this.toggleAttribute('disabled', !!v); }, configurable: true });
const valueProps = {
    get value() { return d.value(this.__id); },
    set value(v) { d.setValue(this.__id, v == null ? '' : String(v)); },
};
mix(HTMLInputElement.prototype, valueProps); mix(HTMLTextAreaElement.prototype, valueProps);
Object.defineProperty(HTMLButtonElement.prototype, 'value', { get() { return this.getAttribute('value') ?? ''; }, set(v) { this.setAttribute('value', v); } });
Object.defineProperty(HTMLSelectElement.prototype, 'value', { get() { return d.value(this.__id); }, set(v) { d.setValue(this.__id, String(v)); } });
reflect(HTMLInputElement, { name: 'name', placeholder: 'placeholder', min: 'min', max: 'max', step: 'step', pattern: 'pattern', autocomplete: 'autocomplete', accept: 'accept', alt: 'alt', dirName: 'dirname', formAction: 'formaction', formMethod: 'formmethod', size: 'size' },
        { readOnly: 'readonly', required: 'required', multiple: 'multiple', autofocus: 'autofocus', defaultChecked: 'checked' }, { maxLength: 'maxlength', minLength: 'minlength' }, { src: 'src' });
Object.defineProperties(HTMLInputElement.prototype, {
    type: { get() { return (this.getAttribute('type') || 'text').toLowerCase(); }, set(v) { this.setAttribute('type', v); }, configurable: true },
    checked: { get() { return d.checked(this.__id); }, set(v) { d.setChecked(this.__id, !!v); }, configurable: true },
    defaultValue: { get() { return this.getAttribute('value') ?? ''; }, set(v) { this.setAttribute('value', v); } },
    indeterminate: { get() { return !!this.__ind; }, set(v) { this.__ind = !!v; } },
    files: { get() { return []; } }, list: { get() { return null; } },
    valueAsNumber: { get() { return parseFloat(this.value); }, set(v) { this.value = v; } },
    valueAsDate: { get() { return null; } }, selectionStart: { get() { return this.__ss ?? this.value.length; }, set(v) { this.__ss = v; } },
    selectionEnd: { get() { return this.__se ?? this.value.length; }, set(v) { this.__se = v; } },
    width: { get() { return 0; } }, height: { get() { return 0; } },
});
Object.assign(HTMLInputElement.prototype, { select() {}, setSelectionRange(a, b) { this.__ss = a; this.__se = b; }, setRangeText() {}, stepUp() { this.value = (parseFloat(this.value) || 0) + (parseFloat(this.step) || 1); }, stepDown() { this.value = (parseFloat(this.value) || 0) - (parseFloat(this.step) || 1); }, showPicker() {} });
reflect(HTMLTextAreaElement, { name: 'name', placeholder: 'placeholder', wrap: 'wrap', autocomplete: 'autocomplete' }, { readOnly: 'readonly', required: 'required', autofocus: 'autofocus' }, { rows: 'rows', cols: 'cols', maxLength: 'maxlength', minLength: 'minlength' });
Object.defineProperties(HTMLTextAreaElement.prototype, { type: { get() { return 'textarea'; } }, defaultValue: { get() { return this.textContent; }, set(v) { this.textContent = v; } },
    textLength: { get() { return this.value.length; } }, selectionStart: { get() { return this.__ss ?? this.value.length; }, set(v) { this.__ss = v; } }, selectionEnd: { get() { return this.__se ?? this.value.length; }, set(v) { this.__se = v; } } });
Object.assign(HTMLTextAreaElement.prototype, { select() {}, setSelectionRange(a, b) { this.__ss = a; this.__se = b; }, setRangeText() {} });
reflect(HTMLButtonElement, { name: 'name', formAction: 'formaction', formMethod: 'formmethod' }, { autofocus: 'autofocus' });
Object.defineProperty(HTMLButtonElement.prototype, 'type', { get() { return (this.getAttribute('type') || 'submit').toLowerCase(); }, set(v) { this.setAttribute('type', v); } });
HTMLButtonElement.prototype.click = function () { if (this.disabled) return; const ok = dispatch(this, new MouseEvent('click', { bubbles: true, cancelable: true })); const f = this.form; if (ok && f && this.type === 'submit') f.requestSubmit(this); else if (ok && f && this.type === 'reset') f.reset(); };
HTMLInputElement.prototype.click = function () {
    if (this.disabled) return;
    const t = this.type, was = this.checked;
    if (t === 'checkbox') this.checked = !was; else if (t === 'radio') this.checked = true;
    const ok = dispatch(this, new MouseEvent('click', { bubbles: true, cancelable: true }));
    if (!ok) { if (t === 'checkbox' || t === 'radio') this.checked = was; return; }
    if (t === 'checkbox' || t === 'radio') { this.dispatchEvent(new InputEvent('input', { bubbles: true })); this.dispatchEvent(new Event('change', { bubbles: true })); }
    else if (t === 'submit' && this.form) this.form.requestSubmit(this); else if (t === 'reset' && this.form) this.form.reset();
};
reflect(HTMLSelectElement, { name: 'name', autocomplete: 'autocomplete' }, { required: 'required', multiple: 'multiple', autofocus: 'autofocus' }, { size: 'size' });
Object.defineProperties(HTMLSelectElement.prototype, {
    selectedIndex: { get() { return d.selIndex(this.__id); }, set(v) { d.setSelIndex(this.__id, v | 0); } },
    options: { get() { return coll(this.querySelectorAll('option')); } },
    selectedOptions: { get() { const i = this.selectedIndex; return coll(i >= 0 ? [this.options[i]] : []); } },
    length: { get() { return this.options.length; } }, type: { get() { return this.multiple ? 'select-multiple' : 'select-one'; } },
});
Object.assign(HTMLSelectElement.prototype, { add(o, b) { this.insertBefore(o, b && b.nodeType ? b : null); }, item(i) { return this.options[i] || null; }, namedItem(n) { return this.options.namedItem(n); } });
reflect(HTMLOptionElement, { label: 'label' }, { defaultSelected: 'selected' });
Object.defineProperties(HTMLOptionElement.prototype, {
    value: { get() { const v = this.getAttribute('value'); return v !== null ? v : this.textContent.replace(/\s+/g, ' ').trim(); }, set(v) { this.setAttribute('value', v); } },
    text: { get() { return this.textContent.replace(/\s+/g, ' ').trim(); }, set(v) { this.textContent = v; } },
    index: { get() { const s = this.closest('select'); return s ? s.options.indexOf(this) : 0; } },
    selected: { get() { const s = this.closest('select'); return s ? s.selectedIndex === this.index : this.hasAttribute('selected'); }, set(v) { const s = this.closest('select'); if (s && v) s.selectedIndex = this.index; else this.toggleAttribute('selected', !!v); } },
    form: { get() { return this.closest('form'); } },
});
reflect(HTMLLabelElement, { htmlFor: 'for' });
Object.defineProperty(HTMLLabelElement.prototype, 'control', { get() { const f = this.getAttribute('for'); return f ? document.getElementById(f) : this.querySelector('input,select,textarea,button'); } });
Object.defineProperty(HTMLLabelElement.prototype, 'form', { get() { return this.closest('form'); } });
reflect(HTMLFormElement, { action: 'action', method: 'method', enctype: 'enctype', name: 'name', target: 'target', acceptCharset: 'accept-charset', encoding: 'enctype' }, { noValidate: 'novalidate' });
Object.defineProperties(HTMLFormElement.prototype, {
    elements: { get() { return coll(this.querySelectorAll('input, textarea, select, button, fieldset, output')); } },
    length: { get() { return this.elements.length; } },
});
Object.assign(g.HTMLSlotElement.prototype, { assignedNodes() { return d.assigned(this.__id).map(wrap); }, assignedElements() { return d.assigned(this.__id).map(wrap).filter(n => n.nodeType === 1); } });
reflect(g.HTMLSlotElement, { name: 'name' });
Object.assign(HTMLFormElement.prototype, {
    requestSubmit(btn) { const ev = new Event('submit', { bubbles: true, cancelable: true }); if (btn) ev.submitter = btn; if (dispatch(this, ev)) d.submit(this.__id); },
    submit() { d.submit(this.__id); },
    reset() { if (dispatch(this, new Event('reset', { bubbles: true, cancelable: true }))) for (const e of this.elements) { if (e.type === 'checkbox' || e.type === 'radio') e.checked = e.defaultChecked; else if (e.localName === 'input' || e.localName === 'textarea') e.value = e.defaultValue; } },
    checkValidity() { return true; }, reportValidity() { return true; },
});
reflect(HTMLScriptElement, { type: 'type', charset: 'charset', crossOrigin: 'crossorigin', integrity: 'integrity', referrerPolicy: 'referrerpolicy' }, { async: 'async', defer: 'defer', noModule: 'nomodule' }, 0, { src: 'src' });
Object.defineProperty(HTMLScriptElement.prototype, 'text', { get() { return this.textContent; }, set(v) { this.textContent = v; } });
HTMLScriptElement.supports = t => t === 'classic' || t === 'module';
reflect(HTMLLinkElement, { rel: 'rel', type: 'type', media: 'media', as: 'as', crossOrigin: 'crossorigin', integrity: 'integrity', hreflang: 'hreflang' }, { disabled: 'disabled' }, 0, { href: 'href' });
Object.defineProperty(HTMLLinkElement.prototype, 'sheet', { get() { return null; } });
reflect(HTMLStyleElement, { media: 'media', type: 'type' }, { disabled: 'disabled' });
Object.defineProperty(HTMLStyleElement.prototype, 'sheet', { get() { return this.isConnected ? (this.__sh || (this.__sh = new g.CSSStyleSheet(this))) : null; } });
reflect(HTMLMetaElement, { name: 'name', content: 'content', httpEquiv: 'http-equiv', charset: 'charset' });
reflect(HTMLBaseElement, {}, 0, 0, { href: 'href' });
Object.defineProperty(HTMLTemplateElement.prototype, 'content', { get() {
    if (!this.__c) { Object.defineProperty(this, '__c', { value: document.createDocumentFragment() }); while (this.firstChild) this.__c.appendChild(this.firstChild); }
    return this.__c; } });
Object.defineProperty(HTMLTemplateElement.prototype, 'innerHTML', { get() { return this.content.innerHTML; }, set(v) { this.content.innerHTML = v; } });
reflect(HTMLCanvasElement, {}, 0, { width: 'width', height: 'height' });
// ---------- <canvas> 2D: state, paths, strokes and transforms here; pixels, fills and text in canvas.c (the d.cv* natives) ----------
const CW = 300, CH = 150, TAU = Math.PI * 2;
const cvId = c => {                                    // the pixel slot of a canvas (HTMLCanvasElement / OffscreenCanvas), made on first use and kept in step with width/height
    const w = c.width, h = c.height;
    let k = c.__cv;
    if (!k) k = Object.defineProperty(c, '__cv', { value: { id: d.cvNew(w, h), w, h }, configurable: true }).__cv;
    else if (k.w !== w || k.h !== h) { k.id = d.cvSize(k.id, w, h) || d.cvNew(w, h); k.w = w; k.h = h; if (c.__cx) c.__cx.__reset(); }
    return k.id;
};
for (const [p, dv] of [['width', CW], ['height', CH]]) Object.defineProperty(HTMLCanvasElement.prototype, p, {
    get() { const v = parseInt(this.getAttribute(p)); return isNaN(v) || v < 0 ? dv : v; },
    set(v) { this.setAttribute(p, Math.max(0, Math.min(v >>> 0, 65535))); if (this.__cv) { this.__cv.w = -1; cvId(this); } }, configurable: true });
const cvDirty = c => {                                 // a drawn-on <canvas> in the page is shown: its pixels go to the image cache shortly (once per 40 ms at most)
    const k = c.__cv;
    if (k.q || !(c instanceof HTMLCanvasElement)) return;
    k.q = 1; k.tries = 0;
    const flush = () => { if (c.isConnected) { k.q = 0; d.cvShow(c.__id, k.id); } else if (k.tries++ < 25) timer(flush, 100, [], false); else k.q = 0; };   // ponytail: a canvas appended later than 2.5 s after its last draw stays blank until drawn again
    timer(flush, 40, [], false);
};
const mmul = (m, n) => [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
const minv = m => { const det = m[0] * m[3] - m[1] * m[2]; return det ? [m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det] : null; };
const fin = (...a) => a.every(Number.isFinite);
const idx = () => new DOMException('The index is not in the allowed range.', 'IndexSizeError');

class Path2D {
    constructor(p) { Object.defineProperty(this, '__c', { value: p instanceof Path2D ? p.__c.slice() : [], writable: true }); Object.defineProperty(this, '__p', { value: p instanceof Path2D ? p.__p.slice() : null, writable: true }); }   // __p: current and start point (user space)
    __cur() { return this.__p; }
    moveTo(x, y) { if (!fin(x, y)) return; this.__c.push(['M', x, y]); this.__p = [x, y, x, y]; }
    lineTo(x, y) { if (!fin(x, y)) return; if (!this.__p) return this.moveTo(x, y); this.__c.push(['L', x, y]); this.__p[0] = x; this.__p[1] = y; }
    closePath() { if (!this.__p) return; this.__c.push(['Z']); this.__p[0] = this.__p[2]; this.__p[1] = this.__p[3]; }
    quadraticCurveTo(a, b, x, y) { if (!fin(a, b, x, y)) return; if (!this.__p) this.moveTo(a, b); this.__c.push(['Q', a, b, x, y]); this.__p[0] = x; this.__p[1] = y; }
    bezierCurveTo(a, b, c, e, x, y) { if (!fin(a, b, c, e, x, y)) return; if (!this.__p) this.moveTo(a, b); this.__c.push(['C', a, b, c, e, x, y]); this.__p[0] = x; this.__p[1] = y; }
    ellipse(x, y, rx, ry, rot, a0, a1, ccw = false) {
        if (!fin(x, y, rx, ry, rot, a0, a1)) return;
        if (rx < 0 || ry < 0) throw idx();
        this.__c.push(['A', x, y, rx, ry, rot, a0, a1, !!ccw]);
        const sw = arcSweep(a0, a1, ccw), t = a0 + sw, ex = x + rx * Math.cos(t) * Math.cos(rot) - ry * Math.sin(t) * Math.sin(rot), ey = y + rx * Math.cos(t) * Math.sin(rot) + ry * Math.sin(t) * Math.cos(rot);
        const sx = x + rx * Math.cos(a0) * Math.cos(rot) - ry * Math.sin(a0) * Math.sin(rot), sy = y + rx * Math.cos(a0) * Math.sin(rot) + ry * Math.sin(a0) * Math.cos(rot);
        if (!this.__p) this.__p = [sx, sy, sx, sy];
        this.__p[0] = ex; this.__p[1] = ey;
    }
    arc(x, y, r, a0, a1, ccw) { if (fin(r) && r < 0) throw idx(); this.ellipse(x, y, r, r, 0, a0, a1, ccw); }
    arcTo(x1, y1, x2, y2, r) {
        if (!fin(x1, y1, x2, y2, r)) return;
        if (r < 0) throw idx();
        if (!this.__p) this.moveTo(x1, y1);
        const [x0, y0] = this.__p, d1x = x0 - x1, d1y = y0 - y1, d2x = x2 - x1, d2y = y2 - y1, l1 = Math.hypot(d1x, d1y), l2 = Math.hypot(d2x, d2y);
        const cr = d1x * d2y - d1y * d2x;
        if (!l1 || !l2 || !r || Math.abs(cr) < 1e-9 * l1 * l2) return this.lineTo(x1, y1);
        const ang = Math.acos(Math.max(-1, Math.min(1, (d1x * d2x + d1y * d2y) / (l1 * l2)))), t = r / Math.tan(ang / 2);
        const ax = x1 + d1x / l1 * t, ay = y1 + d1y / l1 * t, bx = x1 + d2x / l2 * t, by = y1 + d2y / l2 * t;
        const bx2 = d1x / l1 + d2x / l2, by2 = d1y / l1 + d2y / l2, bl = Math.hypot(bx2, by2), k = r / Math.sin(ang / 2), ccx = x1 + bx2 / bl * k, ccy = y1 + by2 / bl * k;
        this.lineTo(ax, ay);
        this.ellipse(ccx, ccy, r, r, 0, Math.atan2(ay - ccy, ax - ccx), Math.atan2(by - ccy, bx - ccx), cr > 0);
    }
    rect(x, y, w, h) { if (!fin(x, y, w, h)) return; this.moveTo(x, y); this.lineTo(x + w, y); this.lineTo(x + w, y + h); this.lineTo(x, y + h); this.closePath(); this.__p[0] = x; this.__p[1] = y; }
    roundRect(x, y, w, h, r = 0) {
        if (!fin(x, y, w, h)) return;
        let rr = (Array.isArray(r) ? r : [r]).map(v => typeof v === 'object' && v ? v.x : +v);
        rr = rr.length === 1 ? [rr[0], rr[0], rr[0], rr[0]] : rr.length === 2 ? [rr[0], rr[1], rr[0], rr[1]] : rr.length === 3 ? [rr[0], rr[1], rr[2], rr[1]] : rr;
        if (rr.some(v => !(v >= 0))) throw new RangeError('Radius value must be non-negative');
        const sx = w < 0 ? -1 : 1, sy = h < 0 ? -1 : 1, aw = Math.abs(w), ah = Math.abs(h), k = Math.min(1, aw / Math.max(rr[0] + rr[1], rr[2] + rr[3], 1e-9), ah / Math.max(rr[0] + rr[3], rr[1] + rr[2], 1e-9));
        const [a, b, c, e] = rr.map(v => v * k), X = sx < 0 ? x + w : x, Y = sy < 0 ? y + h : y;
        this.moveTo(X + a, Y); this.lineTo(X + aw - b, Y); this.arc(X + aw - b, Y + b, b, -Math.PI / 2, 0); this.lineTo(X + aw, Y + ah - c); this.arc(X + aw - c, Y + ah - c, c, 0, Math.PI / 2);
        this.lineTo(X + e, Y + ah); this.arc(X + e, Y + ah - e, e, Math.PI / 2, Math.PI); this.lineTo(X, Y + a); this.arc(X + a, Y + a, a, Math.PI, Math.PI * 1.5); this.closePath();
    }
    addPath(p, m) {
        const t = m ? [m.a, m.b, m.c, m.d, m.e, m.f] : [1, 0, 0, 1, 0, 0], T = (x, y) => [t[0] * x + t[2] * y + t[4], t[1] * x + t[3] * y + t[5]];
        for (const c of p.__c) {
            if (c[0] === 'Z') this.closePath();
            else if (c[0] === 'A') { const [cx, cy] = T(c[1], c[2]); this.ellipse(cx, cy, c[3] * Math.hypot(t[0], t[1]), c[4] * Math.hypot(t[2], t[3]), c[5] + Math.atan2(t[1], t[0]), c[6], c[7], c[8]); }
            else { const o = [c[0]]; for (let i = 1; i < c.length; i += 2) o.push(...T(c[i], c[i + 1])); this[{ M: 'moveTo', L: 'lineTo', Q: 'quadraticCurveTo', C: 'bezierCurveTo' }[c[0]]](...o.slice(1)); }
        }
    }
}
const arcSweep = (a0, a1, ccw) => {
    if (!ccw && a1 - a0 >= TAU) return TAU;
    if (ccw && a0 - a1 >= TAU) return -TAU;
    const m = x => ((x % TAU) + TAU) % TAU;
    return ccw ? -(m(a0 - a1)) : m(a1 - a0);
};
// path commands -> device-space polylines [{p: [x, y, ...], closed}]
const flatten = (cmds, m) => {
    const out = [], T = (x, y) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]], sc = Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1;
    let cur = null, sx = 0, sy = 0;
    const begin = (x, y) => { cur = { p: [x, y], closed: false }; out.push(cur); };
    const last = () => { if (!cur) begin(sx, sy); return [cur.p[cur.p.length - 2], cur.p[cur.p.length - 1]]; };
    for (const c of cmds) {
        if (c[0] === 'M') { const [x, y] = T(c[1], c[2]); begin(x, y); sx = x; sy = y; }
        else if (c[0] === 'L') { const [x, y] = T(c[1], c[2]); if (!cur) begin(sx, sy); cur.p.push(x, y); }
        else if (c[0] === 'Z') { if (cur) { cur.closed = true; cur = null; } }
        else if (c[0] === 'Q' || c[0] === 'C') {
            const [x0, y0] = last(), pts = [];
            for (let i = 1; i < c.length; i += 2) pts.push(T(c[i], c[i + 1]));
            if (c[0] === 'Q') { const [q, e] = pts; pts.length = 0; pts.push([x0 + 2 / 3 * (q[0] - x0), y0 + 2 / 3 * (q[1] - y0)], [e[0] + 2 / 3 * (q[0] - e[0]), e[1] + 2 / 3 * (q[1] - e[1])], e); }
            const [a, b, e] = pts, len = Math.hypot(a[0] - x0, a[1] - y0) + Math.hypot(b[0] - a[0], b[1] - a[1]) + Math.hypot(e[0] - b[0], e[1] - b[1]), n = Math.max(4, Math.min(64, Math.ceil(len / 3)));
            for (let i = 1; i <= n; i++) { const t = i / n, u = 1 - t; cur.p.push(u * u * u * x0 + 3 * u * u * t * a[0] + 3 * u * t * t * b[0] + t * t * t * e[0], u * u * u * y0 + 3 * u * u * t * a[1] + 3 * u * t * t * b[1] + t * t * t * e[1]); }
        } else if (c[0] === 'A') {
            const [, cx, cy, rx, ry, rot, a0, a1, ccw] = c, sw = arcSweep(a0, a1, ccw), R = Math.max(rx, ry) * sc, step = R > 0.3 ? 2 * Math.acos(Math.max(0, 1 - 0.15 / R)) : 1, n = Math.max(2, Math.min(256, Math.ceil(Math.abs(sw) / step))), cr = Math.cos(rot), sr = Math.sin(rot);
            for (let i = 0; i <= n; i++) {
                const t = a0 + sw * i / n, ex = rx * Math.cos(t), ey = ry * Math.sin(t), [x, y] = T(cx + ex * cr - ey * sr, cy + ex * sr + ey * cr);
                if (!cur) begin(x, y); else cur.p.push(x, y);
            }
        }
    }
    return out;
};
const packPolys = (polys, minPts = 3) => { const a = []; for (const s of polys) { const n = s.p.length / 2; if (n >= minPts) a.push(n, ...s.p); } return a.length ? new Float64Array(a).buffer : null; };
const orient = p => { let a = 0; for (let i = 0; i < p.length; i += 2) { const j = (i + 2) % p.length; a += p[i] * p[j + 1] - p[j] * p[i + 1]; } if (a < 0) { const r = []; for (let i = p.length - 2; i >= 0; i -= 2) r.push(p[i], p[i + 1]); return r; } return p; };
const circle = (x, y, r) => { const p = [], n = Math.max(8, Math.min(40, Math.ceil(r * 2))); for (let i = 0; i < n; i++) p.push(x + r * Math.cos(TAU * i / n), y + r * Math.sin(TAU * i / n)); return p; };
const dashed = (subs, pat, off) => {
    const total = pat.reduce((a, b) => a + b, 0), out = [];
    if (!(total > 0)) return subs;
    for (const s of subs) {
        const pts = s.p.slice(); if (s.closed) pts.push(pts[0], pts[1]);
        let i = 0, o = ((off % total) + total) % total;
        while (o >= pat[i % pat.length]) { o -= pat[i % pat.length]; i++; if (pat[i % pat.length] === 0 && o === 0) break; }
        let left = pat[i % pat.length] - o, on = i % 2 === 0, run = on ? [pts[0], pts[1]] : null;
        for (let k = 0; k + 3 < pts.length; k += 2) {
            let x0 = pts[k], y0 = pts[k + 1]; const x1 = pts[k + 2], y1 = pts[k + 3], L = Math.hypot(x1 - x0, y1 - y0);
            let pos = 0;
            while (L - pos > left) {
                pos += left; const t = pos / L, x = pts[k] + (x1 - pts[k]) * t, y = pts[k + 1] + (y1 - pts[k + 1]) * t;
                if (on) { run.push(x, y); out.push({ p: run, closed: false }); run = null; } else run = [x, y];
                on = !on; i++; left = pat[i % pat.length]; if (!left && i > 1000000) return out;
            }
            left -= L - pos;
            if (on) run.push(x1, y1);
        }
        if (on && run && run.length >= 4) out.push({ p: run, closed: false });
    }
    return out;
};
const strokeGeom = (subs, lw, cap, join, ml) => {
    const h = lw / 2, polys = [], push = p => { if (p.length >= 6) polys.push({ p: orient(p), closed: true }); };
    for (const s of subs) {
        const p = [];
        for (let i = 0; i < s.p.length; i += 2) if (!p.length || Math.hypot(s.p[i] - p[p.length - 2], s.p[i + 1] - p[p.length - 1]) > 1e-9) p.push(s.p[i], s.p[i + 1]);
        let closed = s.closed;
        if (closed && p.length > 2 && Math.hypot(p[0] - p[p.length - 2], p[1] - p[p.length - 1]) < 1e-9) { p.pop(); p.pop(); }
        const n = p.length / 2;
        if (n === 1) { if (cap === 'round') push(circle(p[0], p[1], h)); else if (cap === 'square' && closed) push([p[0] - h, p[1] - h, p[0] + h, p[1] - h, p[0] + h, p[1] + h, p[0] - h, p[1] + h]); continue; }
        if (n < 2) continue;
        if (n === 2) closed = false;
        const segs = closed ? n : n - 1, D = [];
        for (let i = 0; i < segs; i++) {
            const a = i * 2, b = ((i + 1) % n) * 2, dx = p[b] - p[a], dy = p[b + 1] - p[a + 1], l = Math.hypot(dx, dy), ux = dx / l, uy = dy / l, nx = -uy * h, ny = ux * h;
            D.push([ux, uy]);
            push([p[a] + nx, p[a + 1] + ny, p[b] + nx, p[b + 1] + ny, p[b] - nx, p[b + 1] - ny, p[a] - nx, p[a + 1] - ny]);
        }
        for (let i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
            const v = i * 2, d0 = D[(i + segs - 1) % segs], d1 = D[i % segs];
            if (join === 'round') { push(circle(p[v], p[v + 1], h)); continue; }
            const cr = d0[0] * d1[1] - d0[1] * d1[0], dot = d0[0] * d1[0] + d0[1] * d1[1];
            if (Math.abs(cr) < 1e-9) continue;
            const s = cr > 0 ? -1 : 1, n0 = [-d0[1] * h * s, d0[0] * h * s], n1 = [-d1[1] * h * s, d1[0] * h * s];
            const A = [p[v] + n0[0], p[v + 1] + n0[1]], B = [p[v] + n1[0], p[v + 1] + n1[1]];
            const ratio = 1 / Math.sqrt(Math.max((1 + dot) / 2, 1e-12));
            if (join === 'miter' && ratio <= ml) push([p[v], p[v + 1], ...A, p[v] + (n0[0] + n1[0]) / (1 + dot), p[v + 1] + (n0[1] + n1[1]) / (1 + dot), ...B]);
            else push([p[v], p[v + 1], ...A, ...B]);
        }
        if (!closed) for (const [e, dir] of [[0, -1], [n - 1, 1]]) {
            const v = e * 2, dd = D[e === 0 ? 0 : segs - 1], ux = dd[0] * dir, uy = dd[1] * dir;
            if (cap === 'round') push(circle(p[v], p[v + 1], h));
            else if (cap === 'square') { const nx = -uy * h, ny = ux * h; push([p[v] + nx, p[v + 1] + ny, p[v] + nx + ux * h, p[v + 1] + ny + uy * h, p[v] - nx + ux * h, p[v + 1] - ny + uy * h, p[v] - nx, p[v + 1] - ny]); }
        }
    }
    return polys;
};
const wind = (subs, x, y, eo) => {
    let w = 0;
    for (const s of subs) { const p = s.p, n = p.length / 2; for (let i = 0; i < n; i++) { const j = (i + 1) % n, x0 = p[2 * i], y0 = p[2 * i + 1], x1 = p[2 * j], y1 = p[2 * j + 1];
        if ((y0 <= y) !== (y1 <= y)) { const t = (y - y0) / (y1 - y0); if (x0 + t * (x1 - x0) > x) w += y1 > y0 ? 1 : -1; } } }
    return eo ? (w & 1) !== 0 : w !== 0;
};

const parseCol = s => { const v = d.color(String(s).trim()); return v < 0 ? null : [(v >>> 16) & 255, (v >>> 8) & 255, v & 255, (v >>> 24) / 255]; };
const colStr = c => { const [r, g, b, a] = c; return a >= 1 ? '#' + [r, g, b].map(x => (x + 256).toString(16).slice(1)).join('') : 'rgba(' + r + ', ' + g + ', ' + b + ', ' + +a.toFixed(3) + ')'; };
class CanvasGradient {
    constructor(t, c) { Object.defineProperty(this, '__g', { value: { t, c, s: [] } }); }
    addColorStop(o, c) {
        o = +o; if (!(o >= 0 && o <= 1)) throw idx();
        const k = parseCol(c); if (!k) throw new DOMException("The value provided ('" + c + "') could not be parsed as a color.", 'SyntaxError');
        const s = this.__g.s; let i = s.length; while (i > 0 && s[i - 1][0] > o) i--; s.splice(i, 0, [o, ...k]);
    }
}
class CanvasPattern { constructor(src, rep) { Object.defineProperty(this, '__p', { value: { src, rep } }); } setTransform(m) { this.__p.m = m ? [m.a, m.b, m.c, m.d, m.e, m.f] : null; } }
class TextMetrics { constructor(o) { Object.assign(this, o); } }
class ImageData {
    constructor(a, b, c, e) {
        let data, w, h;
        if (typeof a === 'number') { w = a; h = b; if (!(w > 0 && h > 0)) throw idx(); data = new Uint8ClampedArray(w * h * 4); }
        else { data = a; w = b; if (!(data instanceof Uint8ClampedArray)) throw new TypeError("Failed to construct 'ImageData': parameter 1 is not of type 'Uint8ClampedArray'."); if (data.length % 4 || !(w > 0)) throw idx(); h = c === undefined || typeof c === 'object' ? data.length / 4 / w : c; if (data.length !== w * h * 4) throw idx(); }
        Object.defineProperties(this, { data: { value: data, enumerable: true }, width: { value: w, enumerable: true }, height: { value: h, enumerable: true }, colorSpace: { value: 'srgb', enumerable: true } });
    }
}
const OPS = ['source-over', 'source-in', 'source-out', 'source-atop', 'destination-over', 'destination-in', 'destination-out', 'destination-atop', 'lighter', 'copy', 'xor'];
const BLENDS = ['multiply', 'screen', 'overlay', 'darken', 'lighten', 'color-dodge', 'color-burn', 'hard-light', 'soft-light', 'difference', 'exclusion', 'hue', 'saturation', 'color', 'luminosity'];
const newState = () => ({ m: [1, 0, 0, 1, 0, 0], fs: [0, 0, 0, 1], ss: [0, 0, 0, 1], lw: 1, cap: 'butt', join: 'miter', ml: 10, dash: [], doff: 0, ga: 1, gco: 'source-over', font: '10px sans-serif', ta: 'start', tb: 'alphabetic', dir: 'ltr', smooth: true, sq: 'low', mask: undefined, filter: 'none', sc: [0, 0, 0, 0], sb: 0, sx: 0, sy: 0 });
const fontInfo = f => {
    const m = /(\d+(?:\.\d+)?|\.\d+)(px|pt|em|rem|%|pc|cm|mm|in)/.exec(f), u = m ? { px: 1, pt: 4 / 3, em: 16, rem: 16, '%': 0.16, pc: 16, cm: 37.8, mm: 3.78, in: 96 }[m[2]] : 1;
    return { size: m ? +m[1] * u : 10, flags: (/\b(bold|bolder|[6-9]00)\b/.test(f) ? 1 : 0) | (/mono|courier|consolas/i.test(f) ? 2 : 0) };
};
class CanvasRenderingContext2D {
    constructor(c) { Object.defineProperty(this, '__c', { value: c }); Object.defineProperty(this, '__s', { value: newState(), writable: true }); Object.defineProperty(this, '__st', { value: [] }); Object.defineProperty(this, '__path', { value: new Path2D(), writable: true }); }
    get canvas() { return this.__c; }
    __reset() { this.__s = newState(); this.__st.length = 0; this.__path = new Path2D(); }
    get __k() { const id = cvId(this.__c); cvDirty(this.__c); return id; }
    save() { this.__st.push(Object.assign({}, this.__s, { dash: this.__s.dash.slice() })); }
    restore() { if (this.__st.length) this.__s = this.__st.pop(); }
    reset() { const id = this.__k; d.cvFill(id, new Float64Array([4, 0, 0, this.__c.width, 0, this.__c.width, this.__c.height, 0, this.__c.height]).buffer, 0, new Float64Array([0, 0, 0, 0, 1]).buffer, 1, 9, undefined); this.__reset(); }
    isContextLost() { return false; }
    getContextAttributes() { return { alpha: true, colorSpace: 'srgb', desynchronized: false, willReadFrequently: false }; }
    // transform
    scale(x, y) { if (fin(x, y)) this.__s.m = mmul(this.__s.m, [x, 0, 0, y, 0, 0]); }
    rotate(a) { if (fin(a)) { const c = Math.cos(a), s = Math.sin(a); this.__s.m = mmul(this.__s.m, [c, s, -s, c, 0, 0]); } }
    translate(x, y) { if (fin(x, y)) this.__s.m = mmul(this.__s.m, [1, 0, 0, 1, x, y]); }
    transform(a, b, c, e, f, g2) { if (fin(a, b, c, e, f, g2)) this.__s.m = mmul(this.__s.m, [a, b, c, e, f, g2]); }
    setTransform(a, b, c, e, f, g2) { if (a && typeof a === 'object') { this.__s.m = [a.a, a.b, a.c, a.d, a.e, a.f]; return; } if (fin(a, b, c, e, f, g2)) this.__s.m = [a, b, c, e, f, g2]; }
    resetTransform() { this.__s.m = [1, 0, 0, 1, 0, 0]; }
    getTransform() { return new DOMMatrix(this.__s.m); }
    // styles
    get fillStyle() { const f = this.__s.fsv; return f !== undefined ? f : colStr(this.__s.fs); }
    set fillStyle(v) { this.__style('fs', v); }
    get strokeStyle() { const f = this.__s.ssv; return f !== undefined ? f : colStr(this.__s.ss); }
    set strokeStyle(v) { this.__style('ss', v); }
    __style(k, v) {
        const s = this.__s;
        if (v instanceof CanvasGradient || v instanceof CanvasPattern) { s[k] = v; s[k + 'v'] = v; return; }
        const c = typeof v === 'string' ? (v.trim().toLowerCase() === 'currentcolor' ? [0, 0, 0, 1] : parseCol(v)) : null;
        if (c) { s[k] = c; s[k + 'v'] = undefined; }
    }
    get lineWidth() { return this.__s.lw; } set lineWidth(v) { v = +v; if (v > 0 && isFinite(v)) this.__s.lw = v; }
    get lineCap() { return this.__s.cap; } set lineCap(v) { if (['butt', 'round', 'square'].includes(v)) this.__s.cap = v; }
    get lineJoin() { return this.__s.join; } set lineJoin(v) { if (['round', 'bevel', 'miter'].includes(v)) this.__s.join = v; }
    get miterLimit() { return this.__s.ml; } set miterLimit(v) { v = +v; if (v > 0 && isFinite(v)) this.__s.ml = v; }
    get lineDashOffset() { return this.__s.doff; } set lineDashOffset(v) { if (isFinite(v)) this.__s.doff = +v; }
    setLineDash(a) { a = Array.from(a, Number); if (a.every(x => isFinite(x) && x >= 0)) this.__s.dash = a.length % 2 ? a.concat(a) : a; }
    getLineDash() { return this.__s.dash.slice(); }
    get globalAlpha() { return this.__s.ga; } set globalAlpha(v) { v = +v; if (v >= 0 && v <= 1) this.__s.ga = v; }
    get globalCompositeOperation() { return this.__s.gco; } set globalCompositeOperation(v) { if (OPS.includes(v) || BLENDS.includes(v)) this.__s.gco = v; }
    get font() { return this.__s.font; } set font(v) { v = String(v); if (/\d(px|pt|em|rem|%|pc|cm|mm|in)\b/.test(v)) this.__s.font = v.replace(/\s+/g, ' ').trim(); }
    get textAlign() { return this.__s.ta; } set textAlign(v) { if (['start', 'end', 'left', 'right', 'center'].includes(v)) this.__s.ta = v; }
    get textBaseline() { return this.__s.tb; } set textBaseline(v) { if (['top', 'hanging', 'middle', 'alphabetic', 'ideographic', 'bottom'].includes(v)) this.__s.tb = v; }
    get direction() { return this.__s.dir; } set direction(v) { if (['ltr', 'rtl', 'inherit'].includes(v)) this.__s.dir = v; }
    get imageSmoothingEnabled() { return this.__s.smooth; } set imageSmoothingEnabled(v) { this.__s.smooth = !!v; }
    get imageSmoothingQuality() { return this.__s.sq; } set imageSmoothingQuality(v) { if (['low', 'medium', 'high'].includes(v)) this.__s.sq = v; }
    get filter() { return this.__s.filter; } set filter(v) { this.__s.filter = String(v); }
    get shadowBlur() { return this.__s.sb; } set shadowBlur(v) { if (v >= 0 && isFinite(v)) this.__s.sb = +v; }
    get shadowOffsetX() { return this.__s.sx; } set shadowOffsetX(v) { if (isFinite(v)) this.__s.sx = +v; }
    get shadowOffsetY() { return this.__s.sy; } set shadowOffsetY(v) { if (isFinite(v)) this.__s.sy = +v; }
    get shadowColor() { return colStr(this.__s.sc); } set shadowColor(v) { const c = parseCol(v); if (c) this.__s.sc = c; }
    get letterSpacing() { return '0px'; } set letterSpacing(v) {} get wordSpacing() { return '0px'; } set wordSpacing(v) {} get fontKerning() { return 'auto'; } set fontKerning(v) {}
    get fontStretch() { return 'normal'; } set fontStretch(v) {} get fontVariantCaps() { return 'normal'; } set fontVariantCaps(v) {} get textRendering() { return 'auto'; } set textRendering(v) {}
    createLinearGradient(x0, y0, x1, y1) { if (!fin(x0, y0, x1, y1)) throw new TypeError('The provided double value is non-finite.'); return new CanvasGradient(1, [x0, y0, x1, y1]); }
    createRadialGradient(x0, y0, r0, x1, y1, r1) { if (!fin(x0, y0, r0, x1, y1, r1)) throw new TypeError('The provided double value is non-finite.'); if (r0 < 0 || r1 < 0) throw idx(); return new CanvasGradient(2, [x0, y0, r0, x1, y1, r1]); }
    createConicGradient(a, x, y) { return new CanvasGradient(1, [x, y, x + 1, y]); }
    createPattern(img, rep) {
        if (!(img instanceof HTMLCanvasElement || img instanceof OffscreenCanvas || img instanceof ImageBitmap)) return null;
        const r = rep == null ? 'repeat' : String(rep);
        if (!['repeat', 'repeat-x', 'repeat-y', 'no-repeat', ''].includes(r)) throw new DOMException("The provided type ('" + r + "') is not one of 'repeat', 'repeat-x', 'repeat-y', or 'no-repeat'.", 'SyntaxError');
        return new CanvasPattern(img, r || 'repeat');
    }
    __paint(st) {
        const m = this.__s.m, sc = Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1, T = (x, y) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]];
        if (st instanceof CanvasGradient) {
            const { t, c, s } = st.__g;
            if (!s.length) return new Float64Array([0, 0, 0, 0, 0]).buffer;
            const st2 = s.flat();
            if (t === 1) return new Float64Array([1, ...T(c[0], c[1]), ...T(c[2], c[3]), s.length, ...st2]).buffer;
            return new Float64Array([2, ...T(c[0], c[1]), c[2] * sc, ...T(c[3], c[4]), c[5] * sc, s.length, ...st2]).buffer;
        }
        if (st instanceof CanvasPattern) {
            const { src, rep, m: pm } = st.__p, sid = cvId(src.__oc || src), inv = minv(mmul(m, pm || [1, 0, 0, 1, 0, 0]));
            return new Float64Array(inv ? [3, sid, ...inv, rep === 'repeat' || rep === 'repeat-x' ? 1 : 0, rep === 'repeat' || rep === 'repeat-y' ? 1 : 0, this.__s.smooth ? 1 : 0] : [0, 0, 0, 0, 0]).buffer;
        }
        return new Float64Array([0, st[0], st[1], st[2], st[3]]).buffer;
    }
    __op() { const i = OPS.indexOf(this.__s.gco); return i < 0 ? 0 : i; }
    __fill(polys, eo, st) { const b = packPolys(polys); if (b) d.cvFill(this.__k, b, eo ? 1 : 0, this.__paint(st), this.__s.ga, this.__op(), this.__s.mask); }
    __stroke(subs, st) {
        const s = this.__s, m = s.m, sc = Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1;
        let p = subs;
        if (s.dash.length) p = dashed(p, s.dash.map(v => v * sc), s.doff * sc);
        this.__fill(strokeGeom(p, s.lw * sc, s.cap, s.join, s.ml), false, st);
    }
    __sub(path) { return flatten((path instanceof Path2D ? path : this.__path).__c, this.__s.m); }
    // painting
    fillRect(x, y, w, h) { if (!fin(x, y, w, h) || !w || !h) return; const p = new Path2D(); p.rect(x, y, w, h); this.__fill(flatten(p.__c, this.__s.m), false, this.__s.fs); }
    strokeRect(x, y, w, h) { if (!fin(x, y, w, h)) return; const p = new Path2D(); p.rect(x, y, w, h); this.__stroke(flatten(p.__c, this.__s.m), this.__s.ss); }
    clearRect(x, y, w, h) {
        if (!fin(x, y, w, h) || !w || !h) return;
        const p = new Path2D(), b = (p.rect(x, y, w, h), packPolys(flatten(p.__c, this.__s.m)));
        if (b) d.cvFill(this.__k, b, 0, new Float64Array([0, 0, 0, 0, 1]).buffer, 1, 6, this.__s.mask);
    }
    fill(a, b) { const path = a instanceof Path2D ? a : undefined, rule = path ? b : a; this.__fill(this.__sub(path), rule === 'evenodd', this.__s.fs); }
    stroke(path) { this.__stroke(this.__sub(path), this.__s.ss); }
    clip(a, b) { const path = a instanceof Path2D ? a : undefined, rule = path ? b : a, bf = packPolys(this.__sub(path)); this.__s.mask = bf ? d.cvClip(this.__k, bf, rule === 'evenodd' ? 1 : 0, this.__s.mask) : d.cvClip(this.__k, new Float64Array([0]).buffer, 0, undefined); }
    isPointInPath(a, x, y, r) { const path = a instanceof Path2D ? a : undefined; if (!path) { r = y; y = x; x = a; } return wind(this.__sub(path), x, y, r === 'evenodd'); }
    isPointInStroke(a, x, y) { const path = a instanceof Path2D ? a : undefined; if (!path) { y = x; x = a; } const s = this.__s, sc = Math.sqrt(Math.abs(s.m[0] * s.m[3] - s.m[1] * s.m[2])) || 1; return strokeGeom(this.__sub(path), s.lw * sc, s.cap, s.join, s.ml).some(q => wind([q], x, y, false)); }
    // path building on the current path
    beginPath() { this.__path = new Path2D(); }
    closePath() { this.__path.closePath(); } moveTo(x, y) { this.__path.moveTo(x, y); } lineTo(x, y) { this.__path.lineTo(x, y); }
    quadraticCurveTo(a, b, c, e) { this.__path.quadraticCurveTo(a, b, c, e); } bezierCurveTo(a, b, c, e, f, g2) { this.__path.bezierCurveTo(a, b, c, e, f, g2); }
    arc(x, y, r, a, b, c) { this.__path.arc(x, y, r, a, b, c); } arcTo(a, b, c, e, f) { this.__path.arcTo(a, b, c, e, f); } ellipse(a, b, c, e, f, g2, h, i) { this.__path.ellipse(a, b, c, e, f, g2, h, i); }
    rect(x, y, w, h) { this.__path.rect(x, y, w, h); } roundRect(x, y, w, h, r) { this.__path.roundRect(x, y, w, h, r); }
    // text (the browser's bitmap faces; no rotation or skew of glyphs)
    __text(t, x, y, st) {
        const s = this.__s, fi = fontInfo(s.font), m = s.m, sc = Math.sqrt(Math.abs(m[0] * m[3] - m[1] * m[2])) || 1;
        t = String(t).replace(/[\t\n\r\f]/g, ' ');
        if (!fin(x, y) || !t) return;
        const [w, asc, desc] = d.cvMeasure(t, fi.size * sc, fi.flags);
        const dir = s.dir === 'rtl', al = s.ta === 'start' ? (dir ? 'right' : 'left') : s.ta === 'end' ? (dir ? 'left' : 'right') : s.ta;
        const ux = al === 'center' ? x - w / sc / 2 : al === 'right' ? x - w / sc : x;
        const by = { top: asc, hanging: asc * 0.8, middle: (asc - desc) / 2, alphabetic: 0, ideographic: -desc, bottom: -desc }[s.tb] / sc;
        const dx = m[0] * ux + m[2] * (y + by) + m[4], dy = m[1] * ux + m[3] * (y + by) + m[5];
        d.cvText(this.__k, t, dx, dy, fi.size * sc, fi.flags, this.__paint(st), s.ga, this.__op(), s.mask);
    }
    fillText(t, x, y) { this.__text(t, x, y, this.__s.fs); }
    strokeText(t, x, y) { this.__text(t, x, y, this.__s.ss); }
    measureText(t) {
        const fi = fontInfo(this.__s.font), [w, asc, desc] = d.cvMeasure(String(t), fi.size, fi.flags), al = this.__s.ta, off = al === 'center' ? w / 2 : al === 'right' || al === 'end' ? w : 0;
        return new TextMetrics({ width: w, actualBoundingBoxLeft: off, actualBoundingBoxRight: w - off, fontBoundingBoxAscent: asc, fontBoundingBoxDescent: desc, actualBoundingBoxAscent: asc * 0.75, actualBoundingBoxDescent: 0, emHeightAscent: asc, emHeightDescent: desc, hangingBaseline: asc * 0.8, alphabeticBaseline: 0, ideographicBaseline: -desc });
    }
    // pixels
    drawImage(img, a, b, c, e, f, g2, h, i) {
        const src = img instanceof ImageBitmap ? img.__oc : img;
        if (!(src instanceof HTMLCanvasElement || src instanceof OffscreenCanvas)) return;    // ponytail: <img>/<video> pixels are not reachable from here; only canvas sources draw
        const sw0 = src.width, sh0 = src.height;
        if (!sw0 || !sh0) throw new DOMException('The image argument is a canvas element with a width or height of 0.', 'InvalidStateError');
        let sx = 0, sy = 0, sw = sw0, sh = sh0, dx, dy, dw, dh;
        if (arguments.length < 5) { dx = a; dy = b; dw = sw; dh = sh; } else if (arguments.length < 9) { dx = a; dy = b; dw = c; dh = e; } else { sx = a; sy = b; sw = c; sh = e; dx = f; dy = g2; dw = h; dh = i; }
        if (!fin(sx, sy, sw, sh, dx, dy, dw, dh) || !sw || !sh || !dw || !dh) return;
        const s = this.__s, F = mmul(s.m, [dw / sw, 0, 0, dh / sh, dx - sx * dw / sw, dy - sy * dh / sh]), inv = minv(F);
        if (!inv) return;
        const p = new Path2D(); p.rect(dx, dy, dw, dh);
        const b2 = packPolys(flatten(p.__c, s.m));
        if (b2) d.cvFill(this.__k, b2, 0, new Float64Array([3, cvId(src), ...inv, 0, 0, s.smooth ? 1 : 0]).buffer, s.ga, this.__op(), s.mask);
    }
    getImageData(x, y, w, h) {
        if (!fin(x, y, w, h)) throw new TypeError('The provided double value is non-finite.');
        if (!w || !h) throw idx();
        if (w < 0) { x += w; w = -w; } if (h < 0) { y += h; h = -h; }
        x = Math.floor(x); y = Math.floor(y); w = Math.floor(w); h = Math.floor(h);
        const buf = d.cvGet(this.__k, x, y, w, h);
        return new ImageData(buf ? new Uint8ClampedArray(buf) : new Uint8ClampedArray(w * h * 4), w, h);
    }
    createImageData(w, h) { return w instanceof ImageData ? new ImageData(w.width, w.height) : new ImageData(Math.abs(w), Math.abs(h)); }
    putImageData(im, dx, dy, ix = 0, iy = 0, iw = im.width, ih = im.height) {
        if (!(im instanceof ImageData)) throw new TypeError("Failed to execute 'putImageData' on 'CanvasRenderingContext2D': parameter 1 is not of type 'ImageData'.");
        if (!fin(dx, dy, ix, iy, iw, ih)) return;
        if (iw < 0) { ix += iw; iw = -iw; } if (ih < 0) { iy += ih; ih = -ih; }
        ix = Math.max(0, ix); iy = Math.max(0, iy); iw = Math.min(iw, im.width - ix); ih = Math.min(ih, im.height - iy);
        if (iw <= 0 || ih <= 0) return;
        let data = im.data;
        if (ix || iy || iw !== im.width || ih !== im.height) { const o = new Uint8ClampedArray(iw * ih * 4); for (let r = 0; r < ih; r++) o.set(data.subarray(((iy + r) * im.width + ix) * 4, ((iy + r) * im.width + ix + iw) * 4), r * iw * 4); data = o; }
        d.cvPut(this.__k, data.byteOffset || data.buffer.byteLength !== data.length ? data.slice().buffer : data.buffer, iw, ih, Math.floor(dx) + ix, Math.floor(dy) + iy);
    }
    drawFocusIfNeeded() {} scrollPathIntoView() {} beginLayer() {} endLayer() {}
}
const ctxOf = (c, type, opts) => {
    if (type !== '2d') return null;
    return c.__cx || Object.defineProperty(c, '__cx', { value: new CanvasRenderingContext2D(c) }).__cx;
};
const pngBlob = (c, cb) => {
    const u = c.toDataURL(), a = atob(u.slice(u.indexOf(',') + 1)), bytes = new Uint8Array(a.length);
    for (let i = 0; i < a.length; i++) bytes[i] = a.charCodeAt(i);
    const b = new g.Blob([], { type: 'image/png' }); Object.defineProperty(b, '__bin', { value: bytes }); return b;
};
class ImageBitmap { constructor(oc) { Object.defineProperty(this, '__oc', { value: oc }); this.width = oc.width; this.height = oc.height; } close() { this.width = this.height = 0; } }
class OffscreenCanvas extends EventTarget {
    constructor(w, h) { super(); this.width = w >>> 0; this.height = h >>> 0; }
    getContext(t, o) { return ctxOf(this, t, o); }
    toDataURL(t) { return this.width && this.height ? d.cvPNG(cvId(this)) : 'data:,'; }
    convertToBlob() { return Promise.resolve(pngBlob(this)); }
    transferToImageBitmap() { const o = new OffscreenCanvas(this.width, this.height); o.getContext('2d').drawImage(this, 0, 0); return new ImageBitmap(o); }
}
Object.assign(HTMLCanvasElement.prototype, {
    getContext(t, o) { return ctxOf(this, t, o); },
    toDataURL(t) { return this.width && this.height ? d.cvPNG(cvId(this)) : 'data:,'; },
    toBlob(cb) { if (typeof cb !== 'function') throw new TypeError("Failed to execute 'toBlob' on 'HTMLCanvasElement': The callback provided as parameter 1 is not a function."); setTimeout(() => cb(this.width && this.height ? pngBlob(this) : null), 0); },
    captureStream() { return null; }, transferControlToOffscreen() { return null; } });
const CVX = { CanvasRenderingContext2D, CanvasGradient, CanvasPattern, TextMetrics, ImageData, ImageBitmap, OffscreenCanvas, Path2D };
V.cvx = CVX;
reflect(HTMLMediaElement, { preload: 'preload', crossOrigin: 'crossorigin' }, { autoplay: 'autoplay', loop: 'loop', controls: 'controls', muted: 'muted' }, 0, { src: 'src' });
Object.assign(HTMLMediaElement.prototype, { play() { return Promise.resolve(); }, pause() {}, load() {}, canPlayType() { return ''; }, addTextTrack() {} });
Object.defineProperties(HTMLMediaElement.prototype, { paused: { get() { return true; } }, ended: { get() { return false; } }, readyState: { get() { return 0; } }, duration: { get() { return NaN; } },
    currentTime: { get() { return 0; }, set() {} }, volume: { get() { return 1; }, set() {} }, error: { get() { return null; } }, networkState: { get() { return 0; } } });
reflect(HTMLVideoElement, { poster: 'poster' }, { playsInline: 'playsinline' }, { width: 'width', height: 'height' });
reflect(HTMLIFrameElement, { name: 'name', allow: 'allow', loading: 'loading', sandbox: 'sandbox', referrerPolicy: 'referrerpolicy' }, { allowFullscreen: 'allowfullscreen' }, { width: 'width', height: 'height' }, { src: 'src' });
reflect(HTMLDialogElement, {}, { open: 'open' });
Object.assign(HTMLDialogElement.prototype, { show() { this.setAttribute('open', ''); }, showModal() { this.setAttribute('open', ''); }, close(r) { this.removeAttribute('open'); if (r !== undefined) this.returnValue = r; this.dispatchEvent(new Event('close')); } });
reflect(HTMLDetailsElement, {}, { open: 'open' });
for (const [c, p] of [['HTMLTableCellElement', { colSpan: 'colspan', rowSpan: 'rowspan' }], ['HTMLOListElement', { start: 'start' }], ['HTMLLIElement', { value: 'value' }], ['HTMLProgressElement', { max: 'max', value: 'value' }], ['HTMLMeterElement', { min: 'min', max: 'max', value: 'value' }]])
    reflect(g[c], 0, 0, p);
reflect(g.HTMLOptGroupElement, { label: 'label' }, { disabled: 'disabled' });
Object.defineProperties(HTMLTableRowElement.prototype, { cells: { get() { return coll([...this.children].filter(c => c.localName === 'td' || c.localName === 'th')); } },
    rowIndex: { get() { const t = this.closest('table'); return t ? [...t.querySelectorAll('tr')].indexOf(this) : -1; } } });
Object.defineProperties(HTMLTableElement.prototype, { rows: { get() { return coll(this.querySelectorAll('tr')); } }, tBodies: { get() { return coll(this.querySelectorAll('tbody')); } },
    tHead: { get() { return this.querySelector('thead'); } }, tFoot: { get() { return this.querySelector('tfoot'); } }, caption: { get() { return this.querySelector('caption'); } } });
HTMLTableElement.prototype.insertRow = function (i) { const tb = this.querySelector('tbody') || this.appendChild(document.createElement('tbody')); const r = document.createElement('tr'); const rows = tb.children; tb.insertBefore(r, i >= 0 && i < rows.length ? rows[i] : null); return r; };
HTMLTableElement.prototype.deleteRow = function (i) { const r = this.rows[i < 0 ? this.rows.length - 1 : i]; if (r) r.remove(); };
HTMLTableRowElement.prototype.insertCell = function (i) { const c = document.createElement('td'); const cs = this.cells; this.insertBefore(c, i >= 0 && i < cs.length ? cs[i] : null); return c; };
HTMLTableRowElement.prototype.deleteCell = function (i) { const c = this.cells[i < 0 ? this.cells.length - 1 : i]; if (c) c.remove(); };
g.HTMLElement = HTMLElement; g.HTMLUnknownElement = HTMLUnknownElement; g.SVGElement = SVGElement;

// ---------- document ----------
class Document extends Node {
    get documentElement() { return wrap(2); }
    get head() { return wrap(d.qsa(1, 'head', true)[0] || 0); }
    get body() { return wrap(d.qsa(1, 'body', true)[0] || 0); }
    set body(v) { const o = this.body; o ? this.documentElement.replaceChild(v, o) : this.documentElement.appendChild(v); }
    get title() { const t = this.querySelector('title'); return t ? t.textContent.replace(/\s+/g, ' ').trim() : ''; }
    set title(v) {
        let t = this.querySelector('title');
        if (!t) { t = this.createElement('title'); this.head.appendChild(t); }
        t.textContent = v;
    }
    get readyState() { return ['loading', 'interactive', 'complete'][d.ready()]; }
    get location() { return location; }
    set location(v) { location.href = v; }
    get URL() { return location.href; }
    get documentURI() { return location.href; }
    get baseURI() { const b = this.querySelector('base[href]'); return b ? b.href : location.href; }
    get domain() { return location.hostname; }
    get referrer() { return ''; }
    get characterSet() { return 'UTF-8'; } get charset() { return 'UTF-8'; } get inputEncoding() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get compatMode() { return 'CSS1Compat'; }
    get doctype() { return { name: 'html', publicId: '', systemId: '', nodeType: 10 }; }
    get designMode() { return 'off'; } set designMode(v) {}
    get dir() { return ''; } set dir(v) {}
    get implementation() { return impl; }
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get fullscreenElement() { return null; }
    get fullscreenEnabled() { return false; }
    get currentScript() { return wrap(d.curScript()); }
    get defaultView() { return g; }
    get activeElement() { const a = this.__active; return a && a.isConnected ? a : this.body; }
    get scrollingElement() { return this.documentElement; }
    get forms() { return coll(this.querySelectorAll('form')); }
    get images() { return coll(this.querySelectorAll('img')); }
    get links() { return coll(this.querySelectorAll('a[href], area[href]')); }
    get anchors() { return coll(this.querySelectorAll('a[name]')); }
    get scripts() { return coll(this.querySelectorAll('script')); }
    get embeds() { return coll([]); } get plugins() { return coll([]); }
    get styleSheets() { return coll([...this.querySelectorAll('style, link[rel~=stylesheet]')].map(e => e.sheet || new g.CSSStyleSheet())); }
    get adoptedStyleSheets() { return this.__ass || (this.__ass = []); } set adoptedStyleSheets(v) { adopt(this, v); }
    get fonts() { return { ready: Promise.resolve(), load: () => Promise.resolve([]), check: () => true, add() {}, forEach() {}, addEventListener() {} }; }
    get cookie() { return g.__cookieGet(); }
    set cookie(v) { g.__cookieSet(String(v)); }
    get lastModified() { return new Date().toString(); }
    hasFocus() { return true; }
    getElementsByName(n) { return coll(this.querySelectorAll('[name="' + String(n).replace(/"/g, '\\"') + '"]')); }
    createElement(t) { t = String(t).toLowerCase(); if (!/^[a-z_:\u0080-\uffff][^\s\/>]*$/.test(t)) throw new DOMException("The tag name provided ('" + t + "') is not a valid name.", 'InvalidCharacterError'); const c = t.indexOf('-') > 0 && CE.get(t); if (c) { V.upgrade = 0; return new c(); } return wrap(d.create(t)); }
    createElementNS(ns, t) { return this.createElement(String(t).replace(/^.*:/, '')); }
    createTextNode(s) { return wrap(d.createText(String(s))); }
    createComment(s) { return wrap(d.createComment(String(s))); }
    createCDATASection(s) { return wrap(d.createText(String(s))); }
    createDocumentFragment() { return wrap(d.createFrag()); }
    createEvent(t) { const m = { mouseevent: MouseEvent, mouseevents: MouseEvent, keyboardevent: KeyboardEvent, customevent: CustomEvent, uievent: UIEvent, uievents: UIEvent, focusevent: FocusEvent }; return new (m[String(t).toLowerCase()] || Event)(''); }
    createRange() { return new g.Range(); }
    createTreeWalker(root, what, filter) { return new g.TreeWalker(root, what, filter); }
    createNodeIterator(root, what, filter) { return new g.NodeIterator(root, what, filter); }
    importNode(n, deep) { return n.cloneNode(deep); }
    adoptNode(n) { return n; }
    getSelection() { return g.getSelection(); }
    elementFromPoint(x, y) { return wrap(d.hit(x | 0, y | 0)) || this.documentElement; }
    elementsFromPoint(x, y) { const e = this.elementFromPoint(x, y); const r = []; for (let n = e; n; n = n.parentElement) r.push(n); return r; }
    caretRangeFromPoint() { return null; }
    execCommand() { return false; } queryCommandSupported() { return false; } queryCommandState() { return false; }
    exitFullscreen() { return Promise.resolve(); }
    startViewTransition(cb) { const p = Promise.resolve(cb && cb()); return { finished: p, ready: p, updateCallbackDone: p, skipTransition() {} }; }
    open() { for (const e of [this.body, this.head]) while (e && e.firstChild) e.removeChild(e.firstChild); return this; }
    close() {}
    write(...a) { d.docwrite(a.join('')); }
    writeln(...a) { d.docwrite(a.join('') + '\n'); }
}
mix(Document.prototype, query);
Document.prototype.getElementById = function (id) { return wrap(d.byId(String(id))); };
g.HTMLDocument = Document;
g.XMLDocument = Document;
class DocumentType extends Node {}
// a detached "document" for createHTMLDocument (jQuery uses it to parse HTML safely): head/body elements under an <html>
const impl = {
    createHTMLDocument(title) {
        const html = document.createElement('html'), head = document.createElement('head'), body = document.createElement('body');
        html.appendChild(head); html.appendChild(body);
        if (title !== undefined) { const t = document.createElement('title'); t.textContent = title; head.appendChild(t); }
        const q = x => html.querySelector(x);
        return { nodeType: 9, nodeName: '#document', documentElement: html, head, body, defaultView: null, location: null, contentType: 'text/html', get title() { const t = q('title'); return t ? t.textContent : ''; },
            createElement: t => document.createElement(t), createElementNS: (n, t) => document.createElement(t), createTextNode: t => document.createTextNode(t), createComment: t => document.createComment(t), createDocumentFragment: () => document.createDocumentFragment(),
            querySelector: q, querySelectorAll: x => html.querySelectorAll(x), getElementById: x => html.getElementById(x), getElementsByTagName: x => html.getElementsByTagName(x), getElementsByClassName: x => html.getElementsByClassName(x),
            importNode: n => n.cloneNode(true), adoptNode: n => n, addEventListener() {}, removeEventListener() {}, get children() { return html.children; }, get firstChild() { return html; }, get childNodes() { return coll([html]); } };
    },
    createDocument() { return impl.createHTMLDocument(''); }, hasFeature() { return true; },
};


// ---------- window ----------
const document = wrap(1);
const loc = {
    get href() { return d.url(); }, set href(v) { go(String(v)); },
    get protocol() { return (g.__urlparts(d.url()) || {}).protocol || ''; },
    get hostname() { return (g.__urlparts(d.url()) || {}).hostname || ''; },
    get port() { return (g.__urlparts(d.url()) || {}).port || ''; },
    get host() { const m = g.__urlparts(d.url()) || {}; return (m.hostname || '') + (m.port ? ':' + m.port : ''); },
    get origin() { return this.protocol + '//' + this.host; },
    get pathname() { return (g.__urlparts(d.url()) || {}).pathname || '/'; },
    get search() { return (g.__urlparts(d.url()) || {}).search || ''; },
    get hash() { return (g.__urlparts(d.url()) || {}).hash || ''; },
    set hash(v) { go('#' + String(v).replace(/^#/, '')); },
    set search(v) { go(this.pathname + (v ? '?' + String(v).replace(/^\?/, '') : '') + this.hash); },
    set pathname(v) { go(String(v).replace(/^\/?/, '/') + this.search + this.hash); },
    assign(u) { go(String(u)); }, replace(u) { go(String(u), true); }, reload() { d.navigate(d.url()); },
    toString() { return d.url(); }, toJSON() { return d.url(); },
    ancestorOrigins: [],
};
const nohash = u => u.replace(/#.*$/, '');
function go(u, replace) {                       // navigation: a fragment-only change stays on the page
    const abs = d.resolve(u), cur = d.url();
    if (abs.includes('#') && nohash(abs) === nohash(cur)) { setHash(abs, replace); return; }
    if (/^javascript:/i.test(u)) { try { (0, eval)(u.slice(11)); } catch (e) { report(e); } return; }
    d.navigate(abs);
}
const H = { stack: [{ url: '', state: null }], i: 0 };
function setHash(abs, replace) {
    const old = d.url();
    if (abs === old) { d.setUrl(abs, true); return; }
    d.setUrl(abs, true);
    if (replace) H.stack[H.i].url = abs; else { H.stack.length = H.i + 1; H.stack.push({ url: abs, state: null }); H.i++; }
    setTimeout(() => dispatch(g, new HashChangeEvent('hashchange', { bubbles: false, oldURL: old, newURL: abs })), 0);
}
const history = {
    get length() { return H.stack.length; }, get state() { return H.stack[H.i].state; }, scrollRestoration: 'auto',
    pushState(s, t, u) { const abs = u == null ? d.url() : d.resolve(String(u)); H.stack.length = H.i + 1; H.stack.push({ url: abs, state: s === undefined ? null : s }); H.i++; d.setUrl(abs, false); },
    replaceState(s, t, u) { const abs = u == null ? d.url() : d.resolve(String(u)); H.stack[H.i] = { url: abs, state: s === undefined ? null : s }; d.setUrl(abs, false); },
    go(n) {
        n = n | 0;
        const j = H.i + n;
        if (!n) return d.navigate(d.url());
        if (j < 0 || j >= H.stack.length) { d.navigate(n < 0 ? 'vibe:back' : 'vibe:forward'); return; }
        const old = d.url(); H.i = j; const e = H.stack[j];
        setTimeout(() => { d.setUrl(e.url, false); dispatch(g, new PopStateEvent('popstate', { state: e.state })); if (nohash(old) === nohash(e.url) && old !== e.url) dispatch(g, new HashChangeEvent('hashchange', { oldURL: old, newURL: e.url })); }, 0);
    },
    back() { this.go(-1); }, forward() { this.go(1); },
};
H.stack[0].url = d.url();

function show(v, depth = 0, seen = []) {
    if (typeof v === 'string') return depth ? JSON.stringify(v) : v;
    if (typeof v === 'symbol') return v.toString();
    if (typeof v === 'bigint') return v + 'n';
    if (v instanceof Error) return depth ? '[' + v + ']' : v.stack ? v + '\n' + v.stack : String(v);
    if (v instanceof Node) return v.nodeType === 1 ? '<' + v.localName + (v.id ? '#' + v.id : '') + '>' : v.nodeName;
    if (typeof v === 'function') return '[Function ' + (v.name || 'anonymous') + ']';
    if (v === g) return '[Window]';
    if (v && typeof v === 'object') {
        if (seen.includes(v)) return '[Circular]';
        if (depth >= 3) return Array.isArray(v) ? '[Array]' : '[Object]';
        seen = seen.concat([v]);
        if (Array.isArray(v)) return '[' + v.slice(0, 20).map(x => show(x, depth + 1, seen)).join(', ') + (v.length > 20 ? ', ... ' + (v.length - 20) + ' more' : '') + ']';
        if (v instanceof Map) return 'Map(' + v.size + ') {' + [...v].slice(0, 10).map(([a, b]) => show(a, depth + 1, seen) + ' => ' + show(b, depth + 1, seen)).join(', ') + '}';
        if (v instanceof Set) return 'Set(' + v.size + ') {' + [...v].slice(0, 10).map(a => show(a, depth + 1, seen)).join(', ') + '}';
        if (v instanceof Date) return v.toISOString();
        if (v instanceof RegExp) return String(v);
        const k = Object.keys(v);
        return (v.constructor && v.constructor !== Object && v.constructor.name ? v.constructor.name + ' ' : '') + '{' + k.slice(0, 20).map(x => x + ': ' + show(v[x], depth + 1, seen)).join(', ') + (k.length > 20 ? ', ...' : '') + '}';
    }
    return String(v);
}
function fmt(a) {                               // printf-style %s %d %o in the first argument
    if (typeof a[0] === 'string' && a.length > 1 && /%[sdifoOjc]/.test(a[0])) {
        let i = 1;
        const s = a[0].replace(/%([sdifoOjc%])/g, (m, c) => c === '%' ? '%' : i >= a.length ? m : c === 'c' ? (i++, '') : c === 's' ? String(a[i++]) : c === 'd' || c === 'i' ? String(parseInt(a[i++])) : c === 'f' ? String(parseFloat(a[i++])) : show(a[i++], 1));
        return [s, ...a.slice(i)].map(x => show(x)).join(' ');
    }
    return a.map(x => show(x)).join(' ');
}
const out = k => (...a) => d.log(k, fmt(a));
let groupIndent = '';
const counts = new Map(), timers = new Map();
const console = { log: out(0), info: out(0), debug: out(0), warn: out(1), error: out(2), trace: (...a) => d.log(0, 'trace: ' + fmt(a) + '\n' + new Error().stack),
    assert: (c, ...a) => { if (!c) out(2)('Assertion failed:', ...a); }, clear() {}, dir: (o) => out(0)(o), dirxml: out(0), table: out(0),
    group: (...a) => { if (a.length) out(0)(...a); }, groupCollapsed: (...a) => { if (a.length) out(0)(...a); }, groupEnd() {},
    count: (l = 'default') => { counts.set(l, (counts.get(l) || 0) + 1); out(0)(l + ': ' + counts.get(l)); }, countReset: l => counts.delete(l || 'default'),
    time: (l = 'default') => timers.set(l, d.now()), timeEnd: (l = 'default') => { if (timers.has(l)) { out(0)(l + ': ' + (d.now() - timers.get(l)).toFixed(1) + ' ms'); timers.delete(l); } },
    timeLog: (l = 'default') => { if (timers.has(l)) out(0)(l + ': ' + (d.now() - timers.get(l)).toFixed(1) + ' ms'); }, timeStamp() {}, profile() {}, profileEnd() {} };

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
const rafs = [];
function raf(f) {                               // callbacks registered before a frame run together in it, in order
    if (typeof f !== 'function') throw new TypeError('requestAnimationFrame: not a function');
    const id = ++raf.n;
    rafs.push([id, f]);
    if (rafs.length === 1) timer(() => { const l = rafs.splice(0), t = d.now() - T0; for (const [i, fn] of l) if (!fn.__cancel) try { fn(t); } catch (e) { report(e); } }, 16, [], false);
    return id;
}
raf.n = 0;

V.wrap = wrap; V.go = go; V.show = show; V.d = d; V.timer = timer; V.W = W; V.document = document; V.T0 = T0; V.EventTarget = EventTarget; V.ET = EventTarget.prototype;
V.classes = { Event, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent, CustomEvent, ErrorEvent, MessageEvent, HashChangeEvent, PopStateEvent, ProgressEvent, PromiseRejectionEvent, StorageEvent,
    TransitionEvent, AnimationEvent, Animation, ClipboardEvent, DragEvent, TouchEvent, EventTarget, Node, CharacterData, Text, Comment, CDATASection, Element, Document, DocumentFragment, ShadowRoot, DocumentType, MutationObserver, DOMTokenList, DOMRect, Attr,
    HTMLAnchorElement, HTMLImageElement, HTMLInputElement, HTMLButtonElement, HTMLFormElement, HTMLSelectElement, HTMLOptionElement, HTMLTextAreaElement, HTMLLabelElement, HTMLScriptElement, HTMLLinkElement,
    HTMLStyleElement, HTMLMetaElement, HTMLBaseElement, HTMLTemplateElement, HTMLCanvasElement, HTMLMediaElement, HTMLVideoElement, HTMLAudioElement, HTMLIFrameElement, HTMLDialogElement, HTMLDetailsElement };

Object.assign(g, V.classes, V.cvx, {
    window: g, self: g, globalThis: g, top: g, parent: g, frames: g, document, location: loc, history, console, name: '', closed: false, length: 0, opener: null, frameElement: null,
    setTimeout: (f, ms, ...a) => timer(f, ms, a, false), setInterval: (f, ms, ...a) => timer(f, ms, a, true),
    clearTimeout: clear, clearInterval: clear,
    requestAnimationFrame: raf, cancelAnimationFrame: id => { for (const r of rafs) if (r[0] === id) r[1].__cancel = true; },
    requestIdleCallback: f => timer(() => f({ didTimeout: false, timeRemaining: () => 40 }), 1, [], false), cancelIdleCallback: clear,
    queueMicrotask: f => { Promise.resolve().then(() => { try { f(); } catch (e) { report(e); } }); },
    alert: m => d.alert(m === undefined ? '' : String(m)), confirm: m => { d.alert(String(m)); return true; }, prompt: (m, v) => { d.alert(String(m)); return v === undefined ? null : v; },
    print() {}, focus() {}, blur() {}, stop() {}, close() {}, moveTo() {}, resizeTo() {},
    open(u) { if (V.trusted() && u) { d.navigate(d.resolve(String(u))); } else d.log(1, 'window.open blocked'); return null; },
    postMessage(m, o) { setTimeout(() => dispatch(g, Object.assign(new MessageEvent('message', { data: m, origin: loc.origin }), { source: g })), 0); },
    reportError: e => report(e),
    getComputedStyle: (e, pseudo) => computedStyle(e),
    scrollTo(x, y) { if (x && typeof x === 'object') { y = x.top; x = x.left; } d.scrollTo(x || 0, y === undefined ? d.view()[0] : y); },
    scrollBy(x, y) { if (x && typeof x === 'object') { y = x.top; x = x.left; } d.scrollTo(0, d.view()[0] + (y || 0)); },
    scroll(x, y) { g.scrollTo(x, y); },
});
for (const k of ['addEventListener', 'removeEventListener', 'dispatchEvent']) g[k] = (...a) => EventTarget.prototype[k].apply(g, a);   // callable bare: addEventListener(...)
for (const k of Object.getOwnPropertyNames(EventTarget.prototype)) if (k.startsWith('on')) Object.defineProperty(g, k, Object.getOwnPropertyDescriptor(EventTarget.prototype, k));
Object.defineProperties(g, {
    innerWidth: { get() { return d.view()[1]; }, configurable: true }, innerHeight: { get() { return d.view()[2]; }, configurable: true },
    outerWidth: { get() { return d.view()[1]; }, configurable: true }, outerHeight: { get() { return d.view()[2] + 80; }, configurable: true },
    scrollX: { get() { return 0; }, configurable: true }, scrollY: { get() { return d.view()[0]; }, configurable: true },
    pageXOffset: { get() { return 0; }, configurable: true }, pageYOffset: { get() { return d.view()[0]; }, configurable: true },
    devicePixelRatio: { value: 1, writable: true, configurable: true }, screenX: { value: 0 }, screenY: { value: 0 },
});

// getComputedStyle: the cascade (css.c) for the properties it knows, tag defaults for the rest, the inline style on top
const BLOCKISH = new Set([...BLOCKS, 'body', 'html', 'head', 'table', 'canvas', 'video', 'iframe']);
const NONE = new Set(['head', 'script', 'style', 'title', 'meta', 'link', 'base', 'template', 'noscript', 'datalist', 'param', 'source', 'track']);
const px = v => v === -1 ? 'auto' : v <= -2 ? (-2 - v) + '%' : v + 'px';
const rgb = c => { const a = c >>> 24; return a === 0 && c === 0 ? 'rgba(0, 0, 0, 0)' : 'rgb(' + ((c >> 16) & 255) + ', ' + ((c >> 8) & 255) + ', ' + (c & 255) + ')'; };
function computedStyle(e) {
    const base = new Map();
    if (e && e.nodeType === 1) {
        const c = d.computed(e.__id);
        for (const k of Object.keys(c)) base.set(k, c[k]);
        for (const [k, v] of [['font-family', 'sans-serif'], ['font-style', 'normal'], ['line-height', 'normal'], ['white-space', 'normal'], ['text-decoration', 'none'], ['transform', 'none'], ['transition', 'all 0s ease 0s'],
                ['animation-name', 'none'], ['pointer-events', 'auto'], ['content', 'normal'], ['cursor', 'auto'], ['box-sizing', 'content-box']]) base.set(k, v);
        const r = rectOf(e);
        if (base.get('width') === 'auto' && r.w) base.set('width', r.w + 'px');
        if (base.get('height') === 'auto' && r.h) base.set('height', r.h + 'px');
        for (const [k, v] of parseStyle(e.getAttribute('style') || '')) if (!/^(color|background-color|display|width|height)$/.test(k) || !base.has(k)) base.set(k, v.replace(/\s*!important$/, ''));
    }
    const api = { getPropertyValue: n => { n = String(n); return n.startsWith('--') ? (e && e.nodeType === 1 ? d.cvar(e.__id, n) : '') : base.get(n.toLowerCase()) || ''; }, getPropertyPriority: () => '', item: i => [...base.keys()][i] || '', setProperty() {}, removeProperty() { return ''; } };
    return new Proxy({}, {
        get(t, k) { if (typeof k !== 'string') return undefined; if (k in api) return api[k]; if (k === 'length') return base.size; if (k === 'cssText') return [...base].map(([a, b]) => a + ': ' + b + ';').join(' ');
            if (k === 'cssFloat') return base.get('float'); return base.get(cssName(k)) ?? ''; },
        set() { return true; },
    });
}

// custom elements (the constructor runs through HTMLElement above); upgrade = existing elements by that tag
g.customElements = {
    define(name, ctor) {
        if (CE.has(name)) throw new DOMException("the name \"" + name + "\" has already been used with this registry", 'NotSupportedError');
        CE.set(name, ctor);
        for (const id of d.qsa(1, name, false)) { V.upgrade = id; try { const e = new ctor(); if (e.isConnected && e.connectedCallback) e.connectedCallback(); } catch (x) { report(x); } }
        const w = V.whenDefined[name]; if (w) w();
    },
    get(n) { return CE.get(n); }, getName(c) { for (const [n, k] of CE) if (k === c) return n; return null; },
    whenDefined(n) { return CE.has(n) ? Promise.resolve(CE.get(n)) : new Promise(r => { V.whenDefined[n] = () => r(CE.get(n)); }); },
    upgrade() {},
};
V.whenDefined = {};
})(globalThis);
)JS";
