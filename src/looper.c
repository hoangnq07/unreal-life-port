/*
 * Minimal android.os.Looper / Handler / HandlerThread / Message / Choreographer runtime.
 *
 * Unity's native side (UnityChoreographer, runOnUiThread, ...) creates Java HandlerThreads, posts
 * messages and frame callbacks and then *waits* for them to run, so these cannot just be ignored.
 * Callbacks passed from native code are JNIBridge interface proxies; running one means calling the
 * native bitter.jnibridge.JNIBridge.invoke(ptr, Class, Method, Object[]) that libunity registered.
 */
#define _GNU_SOURCE
#include "fakejni.h"
#include "loader.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct task {
    struct task *next;
    int64_t when;       /* CLOCK_MONOTONIC ns */
    int kind;           /* 0 = message, 1 = runnable, 2 = frame callback */
    fobj *handler;
    fobj *msg;
    fobj *target;       /* runnable / frame callback proxy */
} task;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    task *head;
    int started;
    fobj *obj;          /* the Java Looper object */
    char name[48];
} looper_t;

static __thread looper_t *t_looper;
static fobj *g_main_looper_obj;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- calling a JNIBridge interface proxy ---- */
static fobj *proxy_call(fobj *proxy, const char *mname, fobj *args) {
    typedef fobj *(*inv_fn)(void *, void *, int64_t, fobj *, fobj *, fobj *);
    static inv_fn inv;
    if (!inv) inv = (inv_fn)fakejni_find_native("bitter/jnibridge/JNIBridge", "invoke");
    if (!inv) { so_log("JNIBridge.invoke native is not registered"); return NULL; }
    if (!proxy || !proxy->ival) return NULL;
    fobj *decl = proxy->user ? (fobj *)proxy->user : jclass("java/lang/Object");
    fobj *m = jnew("java/lang/reflect/Method");
    m->name = strdup(mname);
    m->user = decl;
    return inv(fakejni_env(), jclass("bitter/jnibridge/JNIBridge"), proxy->ival, decl, m, args);
}

static fobj *box(const char *cls, int64_t v) {
    fobj *o = jnew(cls);
    o->ival = v;
    return o;
}

static void run_task(task *t) {
    if (t->kind == 1) {
        proxy_call(t->target, "run", jarray('L', 0));
    } else if (t->kind == 2) {
        fobj *args = jarray('L', 1);
        ((fobj **)args->data)[0] = box("java/lang/Long", now_ns());
        proxy_call(t->target, "doFrame", args);
    } else {
        fobj *cb = t->handler ? (fobj *)t->handler->data : NULL;
        if (cb) {
            fobj *args = jarray('L', 1);
            ((fobj **)args->data)[0] = t->msg;
            proxy_call(cb, "handleMessage", args);
        }
    }
}

static void *loop_thread(void *p) {
    looper_t *l = p;
    t_looper = l;
    pthread_setname_np(pthread_self(), l->name);
    pthread_mutex_lock(&l->mu);
    for (;;) {
        task *best = NULL, **bp = NULL;
        for (task **pp = &l->head; *pp; pp = &(*pp)->next)
            if (!best || (*pp)->when < best->when) { best = *pp; bp = pp; }
        if (!best) { pthread_cond_wait(&l->cv, &l->mu); continue; }
        int64_t now = now_ns();
        if (best->when > now) {
            int64_t at = best->when;
            struct timespec ts = {at / 1000000000LL, at % 1000000000LL};
            pthread_cond_timedwait(&l->cv, &l->mu, &ts);
            continue;
        }
        *bp = best->next;
        pthread_mutex_unlock(&l->mu);
        run_task(best);
        free(best);
        pthread_mutex_lock(&l->mu);
    }
    return NULL;
}

static void looper_start(looper_t *l) {
    if (l->started) return;
    l->started = 1;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_t th;
    pthread_create(&th, &at, loop_thread, l);
    pthread_detach(th);
}

static fobj *looper_obj_new(const char *name) {
    looper_t *l = calloc(1, sizeof *l);
    pthread_mutex_init(&l->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&l->cv, &ca);
    snprintf(l->name, sizeof l->name, "%s", name);
    fobj *o = jnew("android/os/Looper");
    o->user = l;
    l->obj = o;
    return o;
}

static fobj *main_looper(void) {
    if (!g_main_looper_obj) {
        g_main_looper_obj = looper_obj_new("AndroidMain");
        looper_start(g_main_looper_obj->user);
    }
    return g_main_looper_obj;
}

static void enqueue(looper_t *l, task *t) {
    pthread_mutex_lock(&l->mu);
    t->next = NULL;
    task **pp = &l->head;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    pthread_cond_signal(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

static looper_t *looper_of_handler(fobj *h) { return h && h->user ? ((fobj *)h->user)->user : NULL; }

static void post_task(fobj *handler, int kind, fobj *msg, fobj *target, int64_t delay_ms) {
    looper_t *l = looper_of_handler(handler);
    if (!l) return;
    task *t = calloc(1, sizeof *t);
    t->kind = kind; t->handler = handler; t->msg = msg; t->target = target;
    t->when = now_ns() + delay_ms * 1000000LL;
    enqueue(l, t);
}

/* ======================= Java-visible API ======================= */
#define FN(name) static jvalue name(fobj *self, const jvalue *a)

/* HandlerThread */
FN(ht_init) { self->user = looper_obj_new(a[0].l ? jstr_utf8((fobj *)a[0].l) : "HandlerThread"); return JV_VOID; }
FN(ht_getlooper) { return jv_l(self->user); }
FN(thread_start) {
    if (self && self->user && self->cls && !strcmp(self->cls->name, "android/os/HandlerThread"))
        looper_start(((fobj *)self->user)->user);
    return JV_VOID;
}

/* Looper */
FN(looper_getmain) { return jv_l(main_looper()); }
FN(looper_mylooper) { return jv_l(t_looper ? t_looper->obj : NULL); }
FN(looper_quit) { return JV_VOID; }

/* Handler */
FN(h_init_looper) { self->user = a[0].l; return JV_VOID; }
FN(h_init_none) { self->user = t_looper ? t_looper->obj : main_looper(); return JV_VOID; }
FN(h_init_looper_cb) { self->user = a[0].l; self->data = a[1].l; return JV_VOID; }
FN(h_init_cb) { self->user = main_looper(); self->data = a[0].l; return JV_VOID; }
FN(h_post) { post_task(self, 1, NULL, a[0].l, 0); return jv_z(1); }
FN(h_postdelayed) { post_task(self, 1, NULL, a[0].l, a[1].j); return jv_z(1); }
FN(h_nop_void) { return JV_VOID; }
FN(h_false) { return jv_z(0); }
FN(h_getlooper) { return jv_l(self->user); }
FN(h_sendmsg) { fobj *m = a[0].l; post_task(self, 0, m, NULL, 0); return jv_z(1); }
FN(h_sendmsg_delayed) { fobj *m = a[0].l; post_task(self, 0, m, NULL, a[1].j); return jv_z(1); }
FN(h_sendempty) {
    fobj *m = jnew("android/os/Message");
    m->ival = (uint32_t)a[0].i;
    m->user = self;
    post_task(self, 0, m, NULL, 0);
    return jv_z(1);
}
FN(h_sendempty_delayed) {
    fobj *m = jnew("android/os/Message");
    m->ival = (uint32_t)a[0].i;
    m->user = self;
    post_task(self, 0, m, NULL, a[1].j);
    return jv_z(1);
}
FN(h_obtain0) { fobj *m = jnew("android/os/Message"); m->user = self; return jv_l(m); }
FN(h_obtain1) { fobj *m = jnew("android/os/Message"); m->ival = (uint32_t)a[0].i; m->user = self; return jv_l(m); }
FN(h_obtain2) {
    fobj *m = jnew("android/os/Message");
    m->ival = (uint32_t)a[0].i; m->data = a[1].l; m->user = self;
    return jv_l(m);
}

/* Message */
FN(msg_send) { if (self->user) post_task((fobj *)self->user, 0, self, NULL, 0); return JV_VOID; }
static jvalue msg_what(fobj *s) { return jv_i((int32_t)(s->ival & 0xffffffff)); }
static jvalue msg_arg1(fobj *s) { return jv_i((int32_t)(s->ival >> 32)); }
static jvalue msg_arg2(fobj *s) { return jv_i(s->len); }
static jvalue msg_obj(fobj *s) { return jv_l(s->data); }

/* Choreographer: deliver frame callbacks ~60 Hz on the calling thread's looper */
FN(choreo_get) { return jv_l(jnew("android/view/Choreographer")); }
static void post_frame(fobj *cb, int64_t delay_ms) {
    looper_t *l = t_looper ? t_looper : (looper_t *)main_looper()->user;
    task *t = calloc(1, sizeof *t);
    t->kind = 2; t->target = cb;
    t->when = now_ns() + (delay_ms > 0 ? delay_ms : 16) * 1000000LL;
    enqueue(l, t);
}
FN(choreo_post) { post_frame(a[0].l, 0); return JV_VOID; }
FN(choreo_post_delayed) { post_frame(a[0].l, a[1].j); return JV_VOID; }

/* Activity.runOnUiThread */
FN(act_runui) {
    fobj *h = jnew("android/os/Handler");
    h->user = main_looper();
    post_task(h, 1, NULL, a[0].l, 0);
    return JV_VOID;
}

/* boxed primitives + reflection used by the JNIBridge */
FN(box_long) { return jv_j(self->ival); }
FN(box_int) { return jv_i((int32_t)self->ival); }
FN(box_bool) { return jv_z(self->ival != 0); }
FN(box_float) { jvalue v; v.j = 0; memcpy(&v.f, &self->ival, sizeof(float)); return v; }
FN(box_double) { jvalue v; memcpy(&v.d, &self->ival, sizeof(double)); return v; }
FN(method_getname) { return jv_l(jstr(self->name ? self->name : "")); }
FN(method_getdecl) { return jv_l(self->user); }
FN(class_getname) {
    char *n = strdup(self && self->name ? self->name : ""), *p;
    for (p = n; *p; p++) if (*p == '/') *p = '.';
    fobj *r = jstr(n);
    free(n);
    return jv_l(r);
}
FN(proxy_new) {
    fobj *o = jnew("java/lang/reflect/Proxy");
    o->ival = a[0].j;
    fobj *arr = a[1].l;
    if (arr && arr->len > 0) o->user = ((fobj **)arr->data)[0];
    return jv_l(o);
}
FN(ret_self_o) { return jv_l(self); }

typedef struct { const char *key; jfn fn; } ldef;
static const ldef L[] = {
    {"android/os/HandlerThread.<init>(Ljava/lang/String;)V", ht_init},
    {"android/os/HandlerThread.getLooper()Landroid/os/Looper;", ht_getlooper},
    {"java/lang/Thread.start()V", thread_start},
    {"android/os/Looper.getMainLooper()Landroid/os/Looper;", looper_getmain},
    {"android/os/Looper.myLooper()Landroid/os/Looper;", looper_mylooper},
    {"android/os/Looper.quit()V", looper_quit},
    {"android/os/Looper.quitSafely()V", looper_quit},
    {"android/os/Handler.<init>(Landroid/os/Looper;)V", h_init_looper},
    {"android/os/Handler.<init>()V", h_init_none},
    {"android/os/Handler.<init>(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", h_init_looper_cb},
    {"android/os/Handler.<init>(Landroid/os/Handler$Callback;)V", h_init_cb},
    {"android/os/Handler.post(Ljava/lang/Runnable;)Z", h_post},
    {"android/os/Handler.postDelayed(Ljava/lang/Runnable;J)Z", h_postdelayed},
    {"android/os/Handler.removeCallbacks(Ljava/lang/Runnable;)V", h_nop_void},
    {"android/os/Handler.removeCallbacksAndMessages(Ljava/lang/Object;)V", h_nop_void},
    {"android/os/Handler.removeMessages(I)V", h_nop_void},
    {"android/os/Handler.hasMessages(I)Z", h_false},
    {"android/os/Handler.getLooper()Landroid/os/Looper;", h_getlooper},
    {"android/os/Handler.sendMessage(Landroid/os/Message;)Z", h_sendmsg},
    {"android/os/Handler.sendMessageDelayed(Landroid/os/Message;J)Z", h_sendmsg_delayed},
    {"android/os/Handler.sendEmptyMessage(I)Z", h_sendempty},
    {"android/os/Handler.sendEmptyMessageDelayed(IJ)Z", h_sendempty_delayed},
    {"android/os/Handler.obtainMessage()Landroid/os/Message;", h_obtain0},
    {"android/os/Handler.obtainMessage(I)Landroid/os/Message;", h_obtain1},
    {"android/os/Handler.obtainMessage(ILjava/lang/Object;)Landroid/os/Message;", h_obtain2},
    {"android/os/Message.sendToTarget()V", msg_send},
    {"android/view/Choreographer.getInstance()Landroid/view/Choreographer;", choreo_get},
    {"android/view/Choreographer.postFrameCallback(Landroid/view/Choreographer$FrameCallback;)V", choreo_post},
    {"android/view/Choreographer.postFrameCallbackDelayed(Landroid/view/Choreographer$FrameCallback;J)V", choreo_post_delayed},
    {"android/view/Choreographer.removeFrameCallback(Landroid/view/Choreographer$FrameCallback;)V", h_nop_void},
    {"android/app/Activity.runOnUiThread(Ljava/lang/Runnable;)V", act_runui},
    {"java/lang/Long.longValue()J", box_long},
    {"java/lang/Integer.intValue()I", box_int},
    {"java/lang/Boolean.booleanValue()Z", box_bool},
    {"java/lang/Float.floatValue()F", box_float},
    {"java/lang/Double.doubleValue()D", box_double},
    {"java/lang/reflect/Method.getName()Ljava/lang/String;", method_getname},
    {"java/lang/reflect/Method.getDeclaringClass()Ljava/lang/Class;", method_getdecl},
    {"java/lang/Class.getName()Ljava/lang/String;", class_getname},
    {"bitter/jnibridge/JNIBridge.newInterfaceProxy(J[Ljava/lang/Class;)Ljava/lang/Object;", proxy_new},
    {NULL, NULL}};

typedef struct { const char *key; jfield_fn fn; } lfdef;
static const lfdef LF[] = {
    {"android/os/Message.what", msg_what},
    {"android/os/Message.arg1", msg_arg1},
    {"android/os/Message.arg2", msg_arg2},
    {"android/os/Message.obj", msg_obj},
    {NULL, NULL}};

void looper_init(void) {
    for (const ldef *d = L; d->key; d++) java_method(d->key, d->fn);
    for (const lfdef *f = LF; f->key; f++) java_field(f->key, f->fn);
    (void)ret_self_o;
}
