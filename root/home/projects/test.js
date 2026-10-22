// test.js - QuickJS regression test for VibeOS: run `qjs test.js` (prints one line per failure and a summary)
import * as std from "std";
import * as os from "os";

let pass = 0, fail = 0;
function check(name, got, want) {
    const ok = typeof want === "function" ? want(got) : Object.is(got, want) || JSON.stringify(got) === JSON.stringify(want);
    if (ok) pass++;
    else { fail++; print(`FAIL ${name}: got ${JSON.stringify(got)}, want ${typeof want === "function" ? want : JSON.stringify(want)}`); }
}

// ---- language ----
check("arith", 7 * 6, 42);
check("int32", (2 ** 31) | 0, -2147483648);
check("bigint", 2n ** 64n, 18446744073709551616n);
check("float", 0.1 + 0.2, 0.30000000000000004);
check("toFixed", (1.005).toFixed(2), "1.00");
check("toPrecision", (123.456).toPrecision(4), "123.5");
check("exp", (1e21).toString(), "1e+21");
check("parseFloat", parseFloat("3.14159e2"), 314.159);
check("number tostring", (255).toString(16), "ff");
check("destructure", (([a, , b] = [1, 2, 3]) => a + b)(), 4);
check("spread", Math.max(...[3, 9, 2]), 9);
check("class", (() => { class A { #x = 2; get x() { return this.#x; } } class B extends A { y() { return this.x * 3; } } return new B().y(); })(), 6);
check("generator", [...(function* () { yield 1; yield* [2, 3]; })()], [1, 2, 3]);
check("closure", (() => { const f = []; for (let i = 0; i < 3; i++) f.push(() => i); return f.map(g => g()); })(), [0, 1, 2]);
check("optional chain", ({ a: null })?.a?.b ?? "d", "d");
check("template", `${1 + 1}px`, "2px");
check("label break", (() => { let n = 0; outer: for (;;) { for (;;) { n++; if (n > 3) break outer; } } return n; })(), 4);
check("try/finally", (() => { try { throw new Error("x"); } catch (e) { return e.message; } finally { } })(), "x");
check("symbol", typeof Symbol.iterator, "symbol");
check("proxy", new Proxy({}, { get: (t, k) => k + "!" }).hi, "hi!");
check("weakmap", (() => { const w = new WeakMap(), k = {}; w.set(k, 5); return w.get(k); })(), 5);

// ---- builtins ----
check("sort", [5, 1, 4, 10].sort((a, b) => a - b), [1, 4, 5, 10]);
check("map/filter/reduce", [1, 2, 3, 4].map(x => x * x).filter(x => x > 1).reduce((a, b) => a + b), 29);
check("string", "Hello VibeOS".toUpperCase().split(" ").reverse().join("-"), "VIBEOS-HELLO");
check("padStart", "7".padStart(3, "0"), "007");
check("unicode", "héllo".normalize("NFD").length, 6);
check("json", JSON.parse(JSON.stringify({ a: [1, { b: "c" }], d: true })), { a: [1, { b: "c" }], d: true });
check("regexp", "2026-10-22".replace(/(\d+)-(\d+)-(\d+)/, "$3/$2/$1"), "22/10/2026");
check("regexp named", /(?<y>\d{4})/.exec("in 1999").groups.y, "1999");
check("regexp sticky", [..."a1b22c333".matchAll(/\d+/g)].map(m => m[0]), ["1", "22", "333"]);
check("Math.sqrt", Math.sqrt(2), 1.4142135623730951);
check("Math.cbrt", Math.cbrt(27), 3);
check("Math.expm1", Math.expm1(1e-10), 1.00000000005e-10);
check("Math.log1p", Math.log1p(1e-10), 9.999999999500001e-11);   // = glibc
check("Math.hypot", Math.hypot(3, 4), 5);
check("Math.asinh", Math.asinh(1), x => Math.abs(x - 0.881373587019543) < 1e-15);
check("Math.atanh", Math.atanh(0.5), x => Math.abs(x - 0.5493061443340549) < 1e-15);
check("Math.round", Math.round(-2.5), -2);
check("Math.min -0", Object.is(Math.min(0, -0), -0), true);
check("Math.fround", Math.fround(5.5), 5.5);
check("Math.clz32", Math.clz32(1), 31);
check("Uint8ClampedArray", Array.from(new Uint8ClampedArray([1.5, 2.5, 300, -5])), [2, 2, 255, 0]);
check("typed arrays", new Float64Array(new Uint8Array([1, 2, 3]).buffer.slice(0, 0)).length, 0);
check("Map/Set", new Set([1, 2, 2, 3]).size + new Map([[1, 2]]).get(1), 5);
check("Date.UTC", new Date(Date.UTC(2026, 9, 22, 12, 0, 0)).toISOString(), "2026-10-22T12:00:00.000Z");
check("Date now", Date.now() > 1.7e12, true);
check("Intl-free toLocale", typeof (1234.5).toLocaleString(), "string");

// ---- std / os ----
const tmp = "/tmp/qjs_test.txt";
const f = std.open(tmp, "w");
f.puts("line one\nline two\n");
f.close();
check("std.loadFile", std.loadFile(tmp), "line one\nline two\n");
const g = std.open(tmp, "r");
check("getline", g.getline(), "line one");
g.close();
check("os.stat size", os.stat(tmp)[0].size, 18);
check("os.readdir", os.readdir("/tmp")[0].includes("qjs_test.txt"), true);
check("os.rename", os.rename(tmp, tmp + ".2"), 0);
check("os.remove", os.remove(tmp + ".2"), 0);
check("os.getcwd", typeof os.getcwd()[0], "string");
check("std.getenv", typeof std.getenv("PATH"), "string");
check("std.sprintf", std.sprintf("%05.1f|%-3d|%s", 3.14159, 7, "x"), "003.1|7  |x");
check("std.popen", (() => { const p = std.popen("echo hi from sh", "r"); const s = p.readAsString(); p.close(); return s; })(), "hi from sh\n");
check("std.evalScript", std.evalScript("6 * 7"), 42);
check("os.platform", typeof os.platform, "string");
check("os.exec unsupported", (() => { try { os.exec(["true"]); return "ran"; } catch (e) { return "throws"; } })(), "throws");

// ---- async: promises, timers ----
const t0 = os.now();
let order = [];
Promise.resolve().then(() => order.push("micro"));
os.setTimeout(() => order.push("t20"), 20);
os.setTimeout(() => order.push("t5"), 5);
order.push("sync");
(async () => {
    await new Promise(r => os.setTimeout(r, 60));
    check("timer order", order, ["sync", "micro", "t5", "t20"]);
    check("timer elapsed", os.now() - t0 >= 55, true);
    check("await", await (async () => 5)(), 5);
    print(`qjs test: ${pass} passed, ${fail} failed`);
    std.exit(fail ? 1 : 0);
})();
