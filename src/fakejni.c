/*
 * Fake JavaVM / JNIEnv. There is no Java here: Unity's native code only ever talks to Java through the
 * JNIEnv function table, so we answer those calls ourselves.
 *
 * Java methods/fields are emulated by C functions registered in androidfw.c under
 * "pkg/Class.method(sig)ret" keys. Anything missing is logged once ("JAVA UNIMPL ...") and returns 0/null,
 * so the missing surface is discovered by running the game.
 */
#define _GNU_SOURCE
#include "fakejni.h"
#include "loader.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jni_unimpl(int slot);
#include "jni_slots.inc"

#define MAXARGS 32

/* ========================================================================== */
/* classes / objects / strings / arrays                                         */
/* ========================================================================== */

#define MAX_CLASSES 1024
static fobj *g_classes[MAX_CLASSES];
static int g_nclasses;

static volatile int g_cls_lock;
fobj *jclass(const char *name) {
    while (__sync_lock_test_and_set(&g_cls_lock, 1)) { }
    for (int i = 0; i < g_nclasses; i++)
        if (strcmp(g_classes[i]->name, name) == 0) { fobj *r = g_classes[i]; __sync_lock_release(&g_cls_lock); return r; }
    fobj *c = calloc(1, sizeof *c);
    c->kind = K_CLASS;
    c->name = strdup(name);
    if (g_nclasses < MAX_CLASSES) g_classes[g_nclasses++] = c;
    __sync_lock_release(&g_cls_lock);
    return c;
}
fobj *jnew(const char *cn) {
    fobj *o = calloc(1, sizeof *o);
    o->kind = K_OBJECT;
    o->cls = jclass(cn);
    return o;
}
fobj *jstr(const char *s) {
    fobj *o = calloc(1, sizeof *o);
    o->kind = K_STRING;
    o->cls = jclass("java/lang/String");
    o->name = strdup(s ? s : "");
    return o;
}
const char *jstr_utf8(fobj *s) { return (s && s->kind == K_STRING) ? s->name : ""; }

static int elem_size(char e) {
    switch (e) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default: return 8;      /* J D L */
    }
}
fobj *jarray(char elem, int len) {
    fobj *o = calloc(1, sizeof *o);
    o->kind = K_ARRAY;
    o->cls = jclass("[array");
    o->elem = elem;
    o->len = len;
    o->data = calloc(len > 0 ? len : 1, elem_size(elem));
    return o;
}

static const char *parent_of(const char *n) {
    static const char *const tbl[][2] = {
        {"android/app/Activity", "android/content/ContextWrapper"},
        {"android/app/Application", "android/content/ContextWrapper"},
        {"android/content/ContextWrapper", "android/content/Context"},
        {"android/content/Context", "java/lang/Object"},
        {"com/unity3d/player/UnityPlayerActivity", "android/app/Activity"},
        {"com/unity3d/player/UnityPlayer", "android/widget/FrameLayout"},
        {"android/widget/FrameLayout", "android/view/ViewGroup"},
        {"android/view/ViewGroup", "android/view/View"},
        {"android/view/SurfaceView", "android/view/View"},
        {"android/view/View", "java/lang/Object"},
        {"android/view/MotionEvent", "android/view/InputEvent"},
        {"android/view/KeyEvent", "android/view/InputEvent"},
        {"android/view/InputEvent", "java/lang/Object"},
        {"android/view/InputDevice", "java/lang/Object"},
        {"android/view/InputDevice$MotionRange", "java/lang/Object"},
        {"android/hardware/input/InputManager", "java/lang/Object"},
        {"android/view/MotionEvent$PointerCoords", "java/lang/Object"},
        {"android/view/MotionEvent$PointerProperties", "java/lang/Object"},
        {"android/content/pm/ApplicationInfo", "android/content/pm/PackageItemInfo"},
        {"android/content/pm/PackageItemInfo", "java/lang/Object"},
        {"java/util/HashMap", "java/util/Map"},
        {"java/util/Map", "java/lang/Object"},
        {"java/util/ArrayList", "java/util/List"},
        {"java/util/List", "java/util/Collection"},
        {"java/util/Collection", "java/lang/Iterable"},
        {"java/util/Iterable", "java/lang/Object"},
        {"java/lang/String", "java/lang/CharSequence"},
        {"java/lang/CharSequence", "java/lang/Object"},
        {"java/lang/Integer", "java/lang/Number"},
        {"java/lang/Long", "java/lang/Number"},
        {"java/lang/Float", "java/lang/Number"},
        {"java/lang/Double", "java/lang/Number"},
        {"java/lang/Number", "java/lang/Object"},
        {NULL, NULL}};
    for (int i = 0; tbl[i][0]; i++)
        if (strcmp(tbl[i][0], n) == 0) return tbl[i][1];
    return NULL;
}

static int is_assignable_from(const char *sub, const char *sup) {
    if (!sub || !sup) return 0;
    if (strcmp(sub, sup) == 0) return 1;
    if (strcmp(sup, "java/lang/Object") == 0) return 1;
    for (const char *c = sub; c; c = parent_of(c)) {
        if (strcmp(c, sup) == 0) return 1;
    }
    return 0;
}

/* ---- UTF-8 <-> UTF-16 ---- */
static int utf16_len(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x80) { p += 1; n += 1; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; n += 1; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; n += 1; }
        else { p += 4; n += 2; }
    }
    return n;
}
static uint16_t *to_utf16(const char *s, int *outlen) {
    int n = utf16_len(s);
    uint16_t *r = malloc((n + 1) * 2);
    int i = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        uint32_t c;
        if (*p < 0x80) { c = *p; p += 1; }
        else if ((*p & 0xE0) == 0xC0) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        if (c >= 0x10000) { c -= 0x10000; r[i++] = 0xD800 | (c >> 10); r[i++] = 0xDC00 | (c & 0x3FF); }
        else r[i++] = (uint16_t)c;
    }
    r[i] = 0;
    if (outlen) *outlen = i;
    return r;
}
static char *from_utf16(const uint16_t *u, int n) {
    char *r = malloc(n * 3 + 1), *w = r;
    for (int i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00); }
        if (c < 0x80) *w++ = (char)c;
        else if (c < 0x800) { *w++ = 0xC0 | (c >> 6); *w++ = 0x80 | (c & 0x3F); }
        else if (c < 0x10000) { *w++ = 0xE0 | (c >> 12); *w++ = 0x80 | ((c >> 6) & 0x3F); *w++ = 0x80 | (c & 0x3F); }
        else { *w++ = 0xF0 | (c >> 18); *w++ = 0x80 | ((c >> 12) & 0x3F); *w++ = 0x80 | ((c >> 6) & 0x3F); *w++ = 0x80 | (c & 0x3F); }
    }
    *w = 0;
    return r;
}

/* ========================================================================== */
/* emulated Java registry                                                       */
/* ========================================================================== */

typedef struct { char *key; void *fn; } reg_t;
static reg_t *g_methods; static int g_nmethods, g_capm;
static reg_t *g_fields; static int g_nfields, g_capf;
static reg_t *g_sfields; static int g_nsfields, g_caps;

#define ADD(arr, n, cap, k, f)                                                                     \
    do {                                                                                           \
        if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 256; (arr) = realloc((arr), (cap) * sizeof(reg_t)); } \
        (arr)[(n)++] = (reg_t){strdup(k), (void *)(f)};                                           \
    } while (0)

void java_method(const char *key, jfn fn) { ADD(g_methods, g_nmethods, g_capm, key, fn); }
void java_field(const char *key, jfield_fn fn) { ADD(g_fields, g_nfields, g_capf, key, fn); }
void java_static_field(const char *key, jfield_fn fn) { ADD(g_sfields, g_nsfields, g_caps, key, fn); }

static void *lookup(reg_t *arr, int n, const char *cls, const char *name, const char *sig) {
    char key[512];
    /* 1. Exact signature match through class hierarchy */
    for (const char *c = cls; c; c = parent_of(c)) {
        snprintf(key, sizeof key, "%s.%s%s", c, name, sig ? sig : "");
        for (int i = 0; i < n; i++)
            if (strcmp(arr[i].key, key) == 0) return arr[i].fn;
    }
    snprintf(key, sizeof key, "java/lang/Object.%s%s", name, sig ? sig : "");
    for (int i = 0; i < n; i++)
        if (strcmp(arr[i].key, key) == 0) return arr[i].fn;

    /* 2. Match parameter part up to ')' (ignoring return type difference) */
    if (sig && strchr(sig, ')')) {
        size_t plen = (size_t)(strchr(sig, ')') - sig) + 1;
        for (const char *c = cls; c; c = parent_of(c)) {
            snprintf(key, sizeof key, "%s.%s%.*s", c, name, (int)plen, sig);
            size_t klen = strlen(key);
            for (int i = 0; i < n; i++)
                if (!strncmp(arr[i].key, key, klen)) return arr[i].fn;
        }
        snprintf(key, sizeof key, "java/lang/Object.%s%.*s", name, (int)plen, sig);
        size_t klen = strlen(key);
        for (int i = 0; i < n; i++)
            if (!strncmp(arr[i].key, key, klen)) return arr[i].fn;
    }

    /* 3. Fallback: match by method name prefix (for subclassed parameters or wildcard lookups) */
    for (const char *c = cls; c; c = parent_of(c)) {
        snprintf(key, sizeof key, "%s.%s(", c, name);
        size_t klen = strlen(key);
        for (int i = 0; i < n; i++)
            if (!strncmp(arr[i].key, key, klen)) return arr[i].fn;
    }
    snprintf(key, sizeof key, "java/lang/Object.%s(", name);
    size_t klen = strlen(key);
    for (int i = 0; i < n; i++)
        if (!strncmp(arr[i].key, key, klen)) return arr[i].fn;

    return NULL;
}

/* ========================================================================== */
/* registered natives                                                           */
/* ========================================================================== */

typedef struct { char *cls, *name, *sig; void *fn; } native_rec;
#define MAX_NATIVES 512
static native_rec g_natives[MAX_NATIVES];
static int g_nnatives;

void *fakejni_find_native(const char *cls, const char *method) {
    for (int i = 0; i < g_nnatives; i++)
        if (!strcmp(g_natives[i].cls, cls) && !strcmp(g_natives[i].name, method)) return g_natives[i].fn;
    return NULL;
}

static void jni_unimpl(int slot) {
    static unsigned char seen[JNI_SLOTS];
    if (!seen[slot]) {
        seen[slot] = 1;
        so_log("JNI unimplemented slot %d: %s", slot, jni_slot_names[slot]);
    }
}

/* ========================================================================== */
/* method / field ids and invocation                                            */
/* ========================================================================== */

typedef struct ffield { fobj *cls; char *name; char *sig; int is_static; void *getter; } ffield;

static fmethod **g_fm;
static int g_nfm, g_capfm;
static volatile int g_fm_lock;
#define FM_LOCK() while (__sync_lock_test_and_set(&g_fm_lock, 1)) { }
#define FM_UNLOCK() __sync_lock_release(&g_fm_lock)

static fmethod *get_method_locked(fobj *cls, const char *name, const char *sig, int st) {
    for (int i = 0; i < g_nfm; i++)
        if (g_fm[i]->cls == cls && g_fm[i]->is_static == st && !strcmp(g_fm[i]->name, name) && !strcmp(g_fm[i]->sig, sig))
            return g_fm[i];
    /* adopt a placeholder created by FromReflectedMethod (it did not know the signature yet) */
    for (int i = 0; i < g_nfm; i++)
        if (g_fm[i]->cls == cls && !st && g_fm[i]->sig[0] == 0 && !strcmp(g_fm[i]->name, name)) {
            fmethod *m = g_fm[i];
            free(m->sig);
            m->sig = strdup(sig);
            m->impl = lookup(g_methods, g_nmethods, cls ? cls->name : "?", name, sig);
            if (so_verbose || !m->impl)
                so_log("JNI GetMethodID(%s.%s%s)%s", cls ? cls->name : "?", name, sig, m->impl ? "" : "   <-- not emulated");
            return m;
        }
    fmethod *m = calloc(1, sizeof *m);
    m->cls = cls;
    m->name = strdup(name);
    m->sig = strdup(sig);
    m->is_static = st;
    m->impl = lookup(g_methods, g_nmethods, cls ? cls->name : "?", name, sig);
    if (so_verbose || !m->impl)
        so_log("JNI Get%sMethodID(%s.%s%s)%s", st ? "Static" : "", cls ? cls->name : "?", name, sig,
               m->impl ? "" : "   <-- not emulated");
    if (g_nfm == g_capfm) { g_capfm = g_capfm ? g_capfm * 2 : 256; g_fm = realloc(g_fm, g_capfm * sizeof *g_fm); }
    g_fm[g_nfm++] = m;
    return m;
}
static fmethod *get_method(fobj *cls, const char *name, const char *sig, int st) {
    FM_LOCK();
    fmethod *m = get_method_locked(cls, name, sig, st);
    FM_UNLOCK();
    return m;
}
/* java.lang.reflect.Method -> jmethodID */
static fmethod *j_FromReflectedMethod(void *env, fobj *meth) {
    (void)env;
    if (!meth) return NULL;
    FM_LOCK();
    fobj *cls = (fobj *)meth->user;
    const char *name = meth->name ? meth->name : "";
    const char *sig = meth->data ? (const char *)meth->data : "";
    int st = (int)meth->ival;
    fmethod *m = get_method_locked(cls, name, sig, st);
    FM_UNLOCK();
    return m;
}
static fmethod *j_GetMethodID(void *env, fobj *cls, const char *n, const char *s) { (void)env; return get_method(cls, n, s, 0); }
static fmethod *j_GetStaticMethodID(void *env, fobj *cls, const char *n, const char *s) { (void)env; return get_method(cls, n, s, 1); }

static jvalue invoke(fmethod *m, fobj *self, const jvalue *a) {
    if (!m || !m->impl) {
        if (m && !m->warned) {
            m->warned = 1;
            so_log("JAVA UNIMPL call %s.%s%s", m->cls ? m->cls->name : "?", m->name, m->sig);
        }
        return jv_l(0);
    }
    return ((jfn)m->impl)(self, a);
}

/* Fill a jvalue array from a C va_list according to a JNI method signature. */
static void va_fill(const char *sig, va_list ap, jvalue *out) {
    int n = 0;
    const char *p = sig + 1;    /* skip '(' */
    while (*p && *p != ')' && n < MAXARGS) {
        out[n].j = 0;
        switch (*p) {
        case 'Z': case 'B': case 'C': case 'S': case 'I': out[n].i = va_arg(ap, int); p++; break;
        case 'J': out[n].j = va_arg(ap, int64_t); p++; break;
        case 'F': out[n].f = (float)va_arg(ap, double); p++; break;
        case 'D': out[n].d = va_arg(ap, double); p++; break;
        case 'L': out[n].l = va_arg(ap, void *); while (*p != ';') p++; p++; break;
        case '[': out[n].l = va_arg(ap, void *); while (*p == '[') p++; if (*p == 'L') { while (*p != ';') p++; } p++; break;
        default: p++; break;
        }
        n++;
    }
}

#define DEF_CALL(T, RT, F)                                                                          \
    static RT jc_##T(void *e, fobj *o, fmethod *m, ...) {                                           \
        jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap);         \
        return (RT)invoke(m, o, a).F; }                                                             \
    static RT jc_##T##V(void *e, fobj *o, fmethod *m, va_list ap) {                                 \
        jvalue a[MAXARGS]; va_fill(m->sig, ap, a); return (RT)invoke(m, o, a).F; }                  \
    static RT jc_##T##A(void *e, fobj *o, fmethod *m, const jvalue *a) { return (RT)invoke(m, o, a).F; } \
    static RT jn_##T(void *e, fobj *o, fobj *c, fmethod *m, ...) {                                  \
        jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap);         \
        return (RT)invoke(m, o, a).F; }                                                             \
    static RT jn_##T##V(void *e, fobj *o, fobj *c, fmethod *m, va_list ap) {                        \
        jvalue a[MAXARGS]; va_fill(m->sig, ap, a); return (RT)invoke(m, o, a).F; }                  \
    static RT jn_##T##A(void *e, fobj *o, fobj *c, fmethod *m, const jvalue *a) { return (RT)invoke(m, o, a).F; } \
    static RT js_##T(void *e, fobj *c, fmethod *m, ...) {                                           \
        jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap);         \
        return (RT)invoke(m, NULL, a).F; }                                                          \
    static RT js_##T##V(void *e, fobj *c, fmethod *m, va_list ap) {                                 \
        jvalue a[MAXARGS]; va_fill(m->sig, ap, a); return (RT)invoke(m, NULL, a).F; }               \
    static RT js_##T##A(void *e, fobj *c, fmethod *m, const jvalue *a) { return (RT)invoke(m, NULL, a).F; }

DEF_CALL(Object, void *, l)
DEF_CALL(Boolean, uint8_t, z)
DEF_CALL(Byte, int8_t, b)
DEF_CALL(Char, uint16_t, c)
DEF_CALL(Short, int16_t, s)
DEF_CALL(Int, int32_t, i)
DEF_CALL(Long, int64_t, j)
DEF_CALL(Float, float, f)
DEF_CALL(Double, double, d)

/* void: no return value */
static void jc_Void(void *e, fobj *o, fmethod *m, ...) {
    jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap); invoke(m, o, a); }
static void jc_VoidV(void *e, fobj *o, fmethod *m, va_list ap) { jvalue a[MAXARGS]; va_fill(m->sig, ap, a); invoke(m, o, a); }
static void jc_VoidA(void *e, fobj *o, fmethod *m, const jvalue *a) { invoke(m, o, a); }
static void jn_Void(void *e, fobj *o, fobj *c, fmethod *m, ...) {
    jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap); invoke(m, o, a); }
static void jn_VoidV(void *e, fobj *o, fobj *c, fmethod *m, va_list ap) { jvalue a[MAXARGS]; va_fill(m->sig, ap, a); invoke(m, o, a); }
static void jn_VoidA(void *e, fobj *o, fobj *c, fmethod *m, const jvalue *a) { invoke(m, o, a); }
static void js_Void(void *e, fobj *c, fmethod *m, ...) {
    jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap); invoke(m, NULL, a); }
static void js_VoidV(void *e, fobj *c, fmethod *m, va_list ap) { jvalue a[MAXARGS]; va_fill(m->sig, ap, a); invoke(m, NULL, a); }
static void js_VoidA(void *e, fobj *c, fmethod *m, const jvalue *a) { invoke(m, NULL, a); }

/* NewObject: allocate + call <init> */
static fobj *j_AllocObject(void *e, fobj *cls) {
    fobj *o = calloc(1, sizeof *o);
    o->kind = K_OBJECT;
    o->cls = cls;
    return o;
}
static fobj *j_NewObjectA(void *e, fobj *cls, fmethod *m, const jvalue *a) {
    fobj *o = j_AllocObject(e, cls);
    invoke(m, o, a);
    return o;
}
static fobj *j_NewObjectV(void *e, fobj *cls, fmethod *m, va_list ap) {
    jvalue a[MAXARGS]; va_fill(m->sig, ap, a);
    return j_NewObjectA(e, cls, m, a);
}
static fobj *j_NewObject(void *e, fobj *cls, fmethod *m, ...) {
    jvalue a[MAXARGS]; va_list ap; va_start(ap, m); va_fill(m->sig, ap, a); va_end(ap);
    return j_NewObjectA(e, cls, m, a);
}

/* fields */
/* GetFieldID/GetStaticFieldID may be called repeatedly (per object, per frame in a tight
 * loop). The emulated JNI has no GC, so a fresh ffield per call leaks. ffield is immutable
 * once built, so returning a cached one for the same (class, name, sig, static) is safe. */
#define MAX_FIELD_CACHE 2048
static ffield *g_field_cache[MAX_FIELD_CACHE];
static int g_nfield_cache;

static ffield *get_field(fobj *cls, const char *name, const char *sig, int st) {
    for (int i = 0; i < g_nfield_cache; i++) {
        ffield *f = g_field_cache[i];
        if (f->cls == cls && f->is_static == st &&
            strcmp(f->name, name ? name : "") == 0 && strcmp(f->sig, sig ? sig : "") == 0)
            return f;
    }
    ffield *f = calloc(1, sizeof *f);
    f->cls = cls;
    f->name = strdup(name ? name : "");
    f->sig = strdup(sig ? sig : "");
    f->is_static = st;
    f->getter = st ? lookup(g_sfields, g_nsfields, cls ? cls->name : "?", name, NULL)
                   : lookup(g_fields, g_nfields, cls ? cls->name : "?", name, NULL);
    if (so_verbose || !f->getter)
        so_log("JNI Get%sFieldID(%s.%s %s)%s", st ? "Static" : "", cls ? cls->name : "?", name, sig ? sig : "",
               f->getter ? "" : "   <-- not emulated");
    if (g_nfield_cache < MAX_FIELD_CACHE) g_field_cache[g_nfield_cache++] = f;
    return f;
}
static ffield *j_GetFieldID(void *e, fobj *c, const char *n, const char *s) { (void)e; return get_field(c, n, s, 0); }
static ffield *j_GetStaticFieldID(void *e, fobj *c, const char *n, const char *s) { (void)e; return get_field(c, n, s, 1); }
static ffield *j_FromReflectedField(void *env, fobj *field) {
    (void)env;
    if (!field) return NULL;
    fobj *cls = (fobj *)field->user;
    const char *name = field->name ? field->name : "";
    const char *sig = field->data ? (const char *)field->data : "";
    int st = (int)field->ival;
    return get_field(cls, name, sig, st);
}
static fobj *j_ToReflectedMethod(void *env, fobj *cls, fmethod *m, uint8_t isStatic) {
    (void)env;
    if (!m) return NULL;
    fobj *o = jnew("java/lang/reflect/Method");
    o->name = strdup(m->name ? m->name : "");
    o->user = cls ? cls : m->cls;
    o->data = strdup(m->sig ? m->sig : "");
    o->ival = isStatic;
    return o;
}
static fobj *j_ToReflectedField(void *env, fobj *cls, ffield *f, uint8_t isStatic) {
    (void)env;
    if (!f) return NULL;
    fobj *o = jnew("java/lang/reflect/Field");
    o->name = strdup(f->name ? f->name : "");
    o->user = cls ? cls : f->cls;
    o->data = strdup(f->sig ? f->sig : "");
    o->ival = isStatic;
    return o;
}

static jvalue read_field(ffield *f, fobj *o) {
    if (!f || !f->getter) {
        so_log("JAVA UNIMPL field %s.%s %s", f && f->cls ? f->cls->name : "?", f ? f->name : "?", f ? f->sig : "?");
        return jv_l(0);
    }
    return ((jfield_fn)f->getter)(o);
}
#define DEF_FIELD(T, RT, F)                                                                          \
    static RT jg_##T(void *e, fobj *o, ffield *f) { return (RT)read_field(f, o).F; }                \
    static RT jgs_##T(void *e, fobj *c, ffield *f) { return (RT)read_field(f, NULL).F; }
DEF_FIELD(Object, void *, l)
DEF_FIELD(Boolean, uint8_t, z)
DEF_FIELD(Byte, int8_t, b)
DEF_FIELD(Char, uint16_t, c)
DEF_FIELD(Short, int16_t, s)
DEF_FIELD(Int, int32_t, i)
DEF_FIELD(Long, int64_t, j)
DEF_FIELD(Float, float, f)
DEF_FIELD(Double, double, d)
static void j_SetField(void) { }    /* writes to emulated fields are ignored */

/* ========================================================================== */
/* misc JNIEnv functions                                                        */
/* ========================================================================== */

static int j_GetVersion(void *env) { (void)env; return 0x00010006; }
static fobj *j_FindClass(void *env, const char *name) {
    (void)env;
    if (so_verbose) so_log("JNI FindClass(%s)", name);
    return jclass(name);
}
static fobj *j_Ref(void *env, fobj *o) { (void)env; return o; }
static void j_DeleteRef(void *env, fobj *o) { (void)env; (void)o; }
static int j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_Null(void *env) { (void)env; return NULL; }
static void j_Void0(void *env) { (void)env; }
static int j_ret0(void) { return 0; }
static int j_IsSameObject(void *e, fobj *a, fobj *b) { (void)e; return a == b; }
static int j_ret1(void) { return 1; }
static uint8_t j_IsAssignableFrom(void *env, fobj *sub, fobj *sup) {
    (void)env;
    if (!sub || (uintptr_t)sub < 0x10000 || ((uintptr_t)sub & 7) != 0 ||
        !sup || (uintptr_t)sup < 0x10000 || ((uintptr_t)sup & 7) != 0 ||
        !sub->name || !sup->name) return 0;
    return is_assignable_from(sub->name, sup->name) ? 1 : 0;
}
static uint8_t j_IsInstanceOf(void *env, fobj *obj, fobj *clazz) {
    (void)env;
    if (!obj || (uintptr_t)obj < 0x10000 || ((uintptr_t)obj & 7) != 0 ||
        !clazz || (uintptr_t)clazz < 0x10000 || ((uintptr_t)clazz & 7) != 0 ||
        !clazz->name) return 0;
    const char *sub = NULL;
    if (obj->cls && obj->cls->name) sub = obj->cls->name;
    else if (obj->kind == K_CLASS && obj->name) sub = "java/lang/Class";
    else if (obj->kind == K_STRING) sub = "java/lang/String";
    if (!sub) return 0;
    return is_assignable_from(sub, clazz->name) ? 1 : 0;
}
static fobj *j_GetObjectClass(void *env, fobj *o) { (void)env; return (o && o->cls) ? o->cls : jclass("java/lang/Object"); }
static fobj *j_GetSuperclass(void *env, fobj *c) { (void)env; const char *p = c ? parent_of(c->name) : NULL; return jclass(p ? p : "java/lang/Object"); }
static int j_Throw(void *env, fobj *t) { (void)env; (void)t; so_log("JNI Throw called"); return 0; }
static int j_ThrowNew(void *env, fobj *c, const char *msg) { (void)env; so_log("JNI ThrowNew(%s, %s)", c ? c->name : "?", msg); return 0; }
static void j_FatalError(void *env, const char *msg) { (void)env; so_log("JNI FatalError: %s", msg); abort(); }

static int j_RegisterNatives(void *env, fobj *cls, const JNINativeMethod *m, int n) {
    (void)env;
    const char *cn = cls ? cls->name : "?";
    for (int i = 0; i < n; i++) {
        if (so_verbose) so_log("JNI RegisterNatives %s.%s%s -> %p", cn, m[i].name, m[i].signature, m[i].fnPtr);
        if (g_nnatives < MAX_NATIVES)
            g_natives[g_nnatives++] = (native_rec){strdup(cn), strdup(m[i].name), strdup(m[i].signature), m[i].fnPtr};
    }
    return 0;
}

static void *g_vm_ptr;
static int j_GetJavaVM(void *env, void **vm) { (void)env; *vm = &g_vm_ptr; return 0; }

/* strings */
static fobj *j_NewStringUTF(void *env, const char *s) { (void)env; return jstr(s); }
static fobj *j_NewString(void *env, const uint16_t *u, int n) {
    (void)env;
    char *s = from_utf16(u, n);
    fobj *r = jstr(s);
    free(s);
    return r;
}
static int j_GetStringLength(void *env, fobj *s) { (void)env; return s ? utf16_len(s->name) : 0; }
static int j_GetStringUTFLength(void *env, fobj *s) { (void)env; return s ? (int)strlen(s->name) : 0; }
static const char *j_GetStringUTFChars(void *env, fobj *s, uint8_t *isCopy) {
    (void)env;
    if (isCopy) *isCopy = 0;
    return s ? s->name : "";
}
static void j_ReleaseStringUTFChars(void *env, fobj *s, const char *c) { (void)env; (void)s; (void)c; }
static const uint16_t *j_GetStringChars(void *env, fobj *s, uint8_t *isCopy) {
    (void)env;
    if (isCopy) *isCopy = 1;
    return to_utf16(s ? s->name : "", NULL);
}
static void j_ReleaseStringChars(void *env, fobj *s, const uint16_t *c) { (void)env; (void)s; free((void *)c); }
static void j_GetStringRegion(void *env, fobj *s, int start, int len, uint16_t *buf) {
    (void)env;
    int n; uint16_t *u = to_utf16(s ? s->name : "", &n);
    memcpy(buf, u + start, (size_t)len * 2);
    free(u);
}
static void j_GetStringUTFRegion(void *env, fobj *s, int start, int len, char *buf) {
    (void)env;
    int n; uint16_t *u = to_utf16(s ? s->name : "", &n);
    char *r = from_utf16(u + start, len);
    strcpy(buf, r);
    free(r); free(u);
}

/* arrays */
static int j_GetArrayLength(void *env, fobj *a) { (void)env; return a ? a->len : 0; }
static fobj *j_NewObjectArray(void *env, int len, fobj *cls, fobj *init) {
    (void)env; (void)cls;
    fobj *a = jarray('L', len);
    for (int i = 0; i < len; i++) ((fobj **)a->data)[i] = init;
    return a;
}
static fobj *j_GetObjectArrayElement(void *env, fobj *a, int i) { (void)env; return (a && i >= 0 && i < a->len) ? ((fobj **)a->data)[i] : NULL; }
static void j_SetObjectArrayElement(void *env, fobj *a, int i, fobj *v) { (void)env; if (a && i >= 0 && i < a->len) ((fobj **)a->data)[i] = v; }

#define DEF_ARR(T, E)                                                                                \
    static fobj *jna_##T(void *env, int n) { (void)env; return jarray(E, n); }                      \
    static void *jga_##T(void *env, fobj *a, uint8_t *c) { (void)env; if (c) *c = 0; return a ? a->data : NULL; } \
    static void jra_##T(void *env, fobj *a, void *p, int mode) { (void)env; (void)a; (void)p; (void)mode; } \
    static void jgr_##T(void *env, fobj *a, int s, int l, void *buf) { (void)env; memcpy(buf, (char *)a->data + (size_t)s * elem_size(E), (size_t)l * elem_size(E)); } \
    static void jsr_##T(void *env, fobj *a, int s, int l, const void *buf) { (void)env; memcpy((char *)a->data + (size_t)s * elem_size(E), buf, (size_t)l * elem_size(E)); }
DEF_ARR(Boolean, 'Z')
DEF_ARR(Byte, 'B')
DEF_ARR(Char, 'C')
DEF_ARR(Short, 'S')
DEF_ARR(Int, 'I')
DEF_ARR(Long, 'J')
DEF_ARR(Float, 'F')
DEF_ARR(Double, 'D')
static void *j_GetPrimitiveArrayCritical(void *env, fobj *a, uint8_t *c) { return jga_Byte(env, a, c); }
static void j_ReleasePrimitiveArrayCritical(void *env, fobj *a, void *p, int m) { (void)env; (void)a; (void)p; (void)m; }

/* direct byte buffers */
static fobj *j_NewDirectByteBuffer(void *env, void *addr, int64_t cap) {
    (void)env;
    fobj *o = calloc(1, sizeof *o);
    o->kind = K_BUFFER;
    o->cls = jclass("java/nio/DirectByteBuffer");
    o->data = addr;
    o->len = (int)cap;
    return o;
}
static void *j_GetDirectBufferAddress(void *env, fobj *b) { (void)env; return b ? b->data : NULL; }
static int64_t j_GetDirectBufferCapacity(void *env, fobj *b) { (void)env; return b ? b->len : -1; }

/* ========================================================================== */
/* JavaVM                                                                       */
/* ========================================================================== */

static const void *g_env_ptr;   /* points at the function table */
static int vm_Destroy(void *vm) { (void)vm; return 0; }
static int vm_Attach(void *vm, void **penv, void *args) { (void)vm; (void)args; *penv = &g_env_ptr; return 0; }
static int vm_Detach(void *vm) { (void)vm; return 0; }
static int vm_GetEnv(void *vm, void **penv, int ver) { (void)vm; (void)ver; *penv = &g_env_ptr; return 0; }
static void *vm_table[8];

#define SET(slot, fn) (jni_table[slot] = (void *)(fn))
#define REG_CALLS(T)                                                                                 \
    SET(JNI_Call##T##Method, jc_##T); SET(JNI_Call##T##MethodV, jc_##T##V); SET(JNI_Call##T##MethodA, jc_##T##A); \
    SET(JNI_CallNonvirtual##T##Method, jn_##T); SET(JNI_CallNonvirtual##T##MethodV, jn_##T##V);       \
    SET(JNI_CallNonvirtual##T##MethodA, jn_##T##A);                                                  \
    SET(JNI_CallStatic##T##Method, js_##T); SET(JNI_CallStatic##T##MethodV, js_##T##V);               \
    SET(JNI_CallStatic##T##MethodA, js_##T##A);
#define REG_FIELD(T) SET(JNI_Get##T##Field, jg_##T); SET(JNI_GetStatic##T##Field, jgs_##T); \
    SET(JNI_Set##T##Field, j_SetField); SET(JNI_SetStatic##T##Field, j_SetField);
#define REG_ARR(T)                                                                                   \
    SET(JNI_New##T##Array, jna_##T); SET(JNI_Get##T##ArrayElements, jga_##T);                        \
    SET(JNI_Release##T##ArrayElements, jra_##T); SET(JNI_Get##T##ArrayRegion, jgr_##T);              \
    SET(JNI_Set##T##ArrayRegion, jsr_##T);

void fakejni_init(void) {
    SET(JNI_GetVersion, j_GetVersion);
    SET(JNI_FindClass, j_FindClass);
    SET(JNI_GetSuperclass, j_GetSuperclass);
    SET(JNI_IsAssignableFrom, j_IsAssignableFrom);
    SET(JNI_Throw, j_Throw);
    SET(JNI_ThrowNew, j_ThrowNew);
    SET(JNI_FatalError, j_FatalError);
    SET(JNI_ExceptionOccurred, j_Null);
    SET(JNI_ExceptionDescribe, j_Void0);
    SET(JNI_ExceptionClear, j_Void0);
    SET(JNI_ExceptionCheck, j_ExceptionCheck);
    SET(JNI_NewGlobalRef, j_Ref);
    SET(JNI_NewLocalRef, j_Ref);
    SET(JNI_NewWeakGlobalRef, j_Ref);
    SET(JNI_DeleteGlobalRef, j_DeleteRef);
    SET(JNI_DeleteLocalRef, j_DeleteRef);
    SET(JNI_DeleteWeakGlobalRef, j_DeleteRef);
    SET(JNI_IsSameObject, j_IsSameObject);
    SET(JNI_PushLocalFrame, j_ret0);
    SET(JNI_PopLocalFrame, j_Ref);
    SET(JNI_EnsureLocalCapacity, j_ret0);
    SET(JNI_AllocObject, j_AllocObject);
    SET(JNI_NewObject, j_NewObject);
    SET(JNI_NewObjectV, j_NewObjectV);
    SET(JNI_NewObjectA, j_NewObjectA);
    SET(JNI_GetObjectClass, j_GetObjectClass);
    SET(JNI_IsInstanceOf, j_IsInstanceOf);
    SET(JNI_GetMethodID, j_GetMethodID);
    SET(JNI_GetStaticMethodID, j_GetStaticMethodID);
    SET(JNI_FromReflectedMethod, j_FromReflectedMethod);
    SET(JNI_FromReflectedField, j_FromReflectedField);
    SET(JNI_ToReflectedMethod, j_ToReflectedMethod);
    SET(JNI_ToReflectedField, j_ToReflectedField);
    SET(JNI_GetFieldID, j_GetFieldID);
    SET(JNI_GetStaticFieldID, j_GetStaticFieldID);
    REG_CALLS(Object) REG_CALLS(Boolean) REG_CALLS(Byte) REG_CALLS(Char) REG_CALLS(Short)
    REG_CALLS(Int) REG_CALLS(Long) REG_CALLS(Float) REG_CALLS(Double) REG_CALLS(Void)
    REG_FIELD(Object) REG_FIELD(Boolean) REG_FIELD(Byte) REG_FIELD(Char) REG_FIELD(Short)
    REG_FIELD(Int) REG_FIELD(Long) REG_FIELD(Float) REG_FIELD(Double)
    SET(JNI_NewString, j_NewString);
    SET(JNI_GetStringLength, j_GetStringLength);
    SET(JNI_GetStringChars, j_GetStringChars);
    SET(JNI_ReleaseStringChars, j_ReleaseStringChars);
    SET(JNI_NewStringUTF, j_NewStringUTF);
    SET(JNI_GetStringUTFLength, j_GetStringUTFLength);
    SET(JNI_GetStringUTFChars, j_GetStringUTFChars);
    SET(JNI_ReleaseStringUTFChars, j_ReleaseStringUTFChars);
    SET(JNI_GetStringRegion, j_GetStringRegion);
    SET(JNI_GetStringUTFRegion, j_GetStringUTFRegion);
    SET(JNI_GetStringCritical, j_GetStringChars);
    SET(JNI_ReleaseStringCritical, j_ReleaseStringChars);
    SET(JNI_GetArrayLength, j_GetArrayLength);
    SET(JNI_NewObjectArray, j_NewObjectArray);
    SET(JNI_GetObjectArrayElement, j_GetObjectArrayElement);
    SET(JNI_SetObjectArrayElement, j_SetObjectArrayElement);
    REG_ARR(Boolean) REG_ARR(Byte) REG_ARR(Char) REG_ARR(Short) REG_ARR(Int) REG_ARR(Long) REG_ARR(Float) REG_ARR(Double)
    SET(JNI_GetPrimitiveArrayCritical, j_GetPrimitiveArrayCritical);
    SET(JNI_ReleasePrimitiveArrayCritical, j_ReleasePrimitiveArrayCritical);
    SET(JNI_RegisterNatives, j_RegisterNatives);
    SET(JNI_UnregisterNatives, j_ret0);
    SET(JNI_MonitorEnter, j_ret0);
    SET(JNI_MonitorExit, j_ret0);
    SET(JNI_GetJavaVM, j_GetJavaVM);
    SET(JNI_NewDirectByteBuffer, j_NewDirectByteBuffer);
    SET(JNI_GetDirectBufferAddress, j_GetDirectBufferAddress);
    SET(JNI_GetDirectBufferCapacity, j_GetDirectBufferCapacity);
    SET(JNI_GetObjectRefType, j_ret1);
    g_env_ptr = jni_table;

    vm_table[3] = (void *)vm_Destroy;
    vm_table[4] = (void *)vm_Attach;
    vm_table[5] = (void *)vm_Detach;
    vm_table[6] = (void *)vm_GetEnv;
    vm_table[7] = (void *)vm_Attach;
    g_vm_ptr = vm_table;
}

void *fakejni_vm(void) { return &g_vm_ptr; }
void *fakejni_env(void) { return &g_env_ptr; }
