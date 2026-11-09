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
for (const n of ['NodeList', 'HTMLCollection', 'HTMLOptionsCollection', 'HTMLFormControlsCollection']) g[n] = Object.assign(function () { throw new TypeError('Illegal constructor'); }, { prototype: ListProto });
g.CSSStyleDeclaration = function CSSStyleDeclaration() {}; g.NamedNodeMap = function NamedNodeMap() {}; g.Window = function Window() {};
V.coll = coll;

// ---------- mutation observers ----------
const OBS = new Set();
function record(rec) {
    if (!OBS.size) return;
    for (const o of OBS) {
        const opt = o.__opt;
        for (const t of o.__targets) {
            const hit = rec.target === t || (opt.subtree && t.contains(rec.target));
            if (!hit) continue;
            if (rec.type === 'attributes' && !(opt.attributes || opt.attributeFilter || opt.attributeOldValue)) continue;
            if (rec.type === 'attributes' && opt.attributeFilter && !opt.attributeFilter.includes(rec.attributeName)) continue;
            if (rec.type === 'childList' && !opt.childList) continue;
            if (rec.type === 'characterData' && !(opt.characterData || opt.characterDataOldValue)) continue;
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

class Node extends EventTarget {
    get nodeType() { return d.type(this.__id); }
    get nodeName() { const t = d.tag(this.__id); return this.nodeType === 1 ? t.toUpperCase() : t; }
    get parentNode() { return wrap(d.parent(this.__id)); }
    get parentElement() { const p = this.parentNode; return p && p.nodeType === 1 ? p : null; }
    get firstChild() { return wrap(d.first(this.__id)); }
    get lastChild() { return wrap(d.last(this.__id)); }
    get nextSibling() { return wrap(d.next(this.__id)); }
    get previousSibling() { return wrap(d.prev(this.__id)); }
    get childNodes() { const r = []; for (let c = d.first(this.__id); c; c = d.next(c)) r.push(wrap(c)); return coll(r); }
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
    contains(n) { for (; n; n = n.parentNode) if (n === this) return true; return false; }
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
const query = {
    querySelector(s) { return wrap(d.qsa(this.__id, String(s), true)[0] || 0); },
    querySelectorAll(s) { return coll(d.qsa(this.__id, String(s), false).map(wrap)); },
    getElementsByTagName(t) { t = String(t); return t === '*' ? this.querySelectorAll('*') : this.querySelectorAll(t.toLowerCase()); },
    getElementsByTagNameNS(ns, t) { return this.getElementsByTagName(t); },
    getElementsByClassName(c) { c = String(c).trim(); return c ? this.querySelectorAll(c.split(/\s+/).map(x => '.' + CSS.escape(x)).join('')) : coll([]); },
    get children() { const r = []; for (let c = d.first(this.__id); c; c = d.next(c)) if (d.type(c) === 1) r.push(wrap(c)); return coll(r); },
    get firstElementChild() { for (let c = d.first(this.__id); c; c = d.next(c)) if (d.type(c) === 1) return wrap(c); return null; },
    get lastElementChild() { for (let c = d.last(this.__id); c; c = d.prev(c)) if (d.type(c) === 1) return wrap(c); return null; },
    get childElementCount() { let n = 0; for (let c = d.first(this.__id); c; c = d.next(c)) if (d.type(c) === 1) n++; return n; },
    getElementById(id) { id = String(id); for (const e of d.qsa(this.__id, '[id]', false)) if (d.getAttr(e, 'id') === id) return wrap(e); return null; },
};
function mix(dst, src) { Object.defineProperties(dst, Object.getOwnPropertyDescriptors(src)); }

class DocumentFragment extends Node {
    get adoptedStyleSheets() { return this.__ass || []; } set adoptedStyleSheets(v) { this.__ass = v; }
    get innerHTML() { return d.html(this.__id, false); }
    set innerHTML(v) { d.setHTML(this.__id, v == null ? '' : String(v)); }
}
mix(DocumentFragment.prototype, query);
class ShadowRoot extends DocumentFragment {
    get mode() { return d.smode(this.__id) === 2 ? 'closed' : 'open'; } get host() { return wrap(d.host(this.__id)); } get delegatesFocus() { return false; }
    get adoptedStyleSheets() { return this.__ass || []; } set adoptedStyleSheets(v) { this.__ass = v; }
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
        has(t, k) { return typeof k === 'string' && (k in api || parse().has(cssName(k))); },
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
    matches(s) { return d.matches(this.__id, String(s)); }
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
    animate() { return { finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, finish() {}, onfinish: null, addEventListener() {} }; }
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
class SVGElement extends Element { get ownerSVGElement() { return null; } get viewportElement() { return null; } get className() { return { baseVal: this.getAttribute('class') || '', animVal: this.getAttribute('class') || '' }; } }
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
g.SVGSVGElement = SVGElement;

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
const noop = () => {};
const ctx2d = c => new Proxy({ canvas: c, fillStyle: '#000', strokeStyle: '#000', lineWidth: 1, font: '10px sans-serif', globalAlpha: 1 }, {
    get(t, k) { if (k in t) return t[k]; if (k === 'measureText') return s => ({ width: String(s).length * 6, actualBoundingBoxAscent: 8, actualBoundingBoxDescent: 2 });
        if (k === 'getImageData' || k === 'createImageData') return (x, y, w, h) => ({ width: w || 1, height: h || 1, data: new Uint8ClampedArray(4 * (w || 1) * (h || 1)) });
        if (/^create/.test(k)) return () => ({ addColorStop: noop }); if (k === 'getLineDash') return () => []; if (k === 'isPointInPath') return () => false; return noop; },
    set(t, k, v) { t[k] = v; return true; } });
Object.assign(HTMLCanvasElement.prototype, { getContext(t) { return t === '2d' ? (this.__cx || Object.defineProperty(this, '__cx', { value: ctx2d(this) }).__cx) : null; },
    toDataURL() { return 'data:,'; }, toBlob(cb) { setTimeout(() => cb(null), 0); }, captureStream() { return null; }, transferControlToOffscreen() { return null; } });
reflect(HTMLMediaElement, { preload: 'preload', crossOrigin: 'crossorigin' }, { autoplay: 'autoplay', loop: 'loop', controls: 'controls', muted: 'muted' }, 0, { src: 'src' });
Object.assign(HTMLMediaElement.prototype, { play() { return Promise.resolve(); }, pause() {}, load() {}, canPlayType() { return ''; }, addTextTrack() {} });
Object.defineProperties(HTMLMediaElement.prototype, { paused: { get() { return true; } }, ended: { get() { return false; } }, readyState: { get() { return 0; } }, duration: { get() { return NaN; } },
    currentTime: { get() { return 0; }, set() {} }, volume: { get() { return 1; }, set() {} }, error: { get() { return null; } }, networkState: { get() { return 0; } } });
reflect(HTMLVideoElement, { poster: 'poster' }, { playsInline: 'playsinline' }, { width: 'width', height: 'height' });
reflect(HTMLIFrameElement, { name: 'name', allow: 'allow', loading: 'loading', sandbox: 'sandbox', referrerPolicy: 'referrerpolicy' }, { allowFullscreen: 'allowfullscreen' }, { width: 'width', height: 'height' }, { src: 'src' });
Object.defineProperties(HTMLIFrameElement.prototype, { contentWindow: { get() { return null; } }, contentDocument: { get() { return null; } } });
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
    get adoptedStyleSheets() { return this.__ass || []; } set adoptedStyleSheets(v) { this.__ass = v; }
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

V.go = go; V.show = show; V.d = d; V.timer = timer; V.W = W; V.document = document; V.T0 = T0; V.EventTarget = EventTarget; V.ET = EventTarget.prototype;
V.classes = { Event, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent, CustomEvent, ErrorEvent, MessageEvent, HashChangeEvent, PopStateEvent, ProgressEvent, PromiseRejectionEvent, StorageEvent,
    TransitionEvent, AnimationEvent, ClipboardEvent, DragEvent, TouchEvent, EventTarget, Node, CharacterData, Text, Comment, CDATASection, Element, Document, DocumentFragment, ShadowRoot, DocumentType, MutationObserver, DOMTokenList, DOMRect, Attr,
    HTMLAnchorElement, HTMLImageElement, HTMLInputElement, HTMLButtonElement, HTMLFormElement, HTMLSelectElement, HTMLOptionElement, HTMLTextAreaElement, HTMLLabelElement, HTMLScriptElement, HTMLLinkElement,
    HTMLStyleElement, HTMLMetaElement, HTMLBaseElement, HTMLTemplateElement, HTMLCanvasElement, HTMLMediaElement, HTMLVideoElement, HTMLAudioElement, HTMLIFrameElement, HTMLDialogElement, HTMLDetailsElement };

Object.assign(g, V.classes, {
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
    const api = { getPropertyValue: n => base.get(String(n).toLowerCase()) || '', getPropertyPriority: () => '', item: i => [...base.keys()][i] || '', setProperty() {}, removeProperty() { return ''; } };
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
