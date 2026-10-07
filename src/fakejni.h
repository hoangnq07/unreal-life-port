#ifndef FAKEJNI_H
#define FAKEJNI_H

#include <stdint.h>

/* ---- minimal JNI type system ---- */
typedef void *jobject;
typedef union {
    uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j; float f; double d; void *l;
} jvalue;
typedef struct { const char *name; const char *signature; void *fnPtr; } JNINativeMethod;

/* ---- object model (used by the Android framework emulation) ---- */
enum { K_CLASS = 1, K_STRING, K_OBJECT, K_ARRAY, K_BUFFER };
typedef struct fobj {
    int kind;
    char *name;                 /* class name (K_CLASS) / utf8 text (K_STRING) */
    struct fobj *cls;           /* class of an object */
    char elem;                  /* K_ARRAY element type: Z B C S I J F D L */
    int len;                    /* K_ARRAY length / K_BUFFER capacity */
    void *data;                 /* K_ARRAY storage / K_BUFFER address */
    void *user;                 /* free for the emulation layer */
    int64_t ival;
} fobj;

typedef struct fmethod { fobj *cls; char *name; char *sig; int is_static; void *impl; int warned; } fmethod;

/* An emulated Java method: self is NULL for static methods. */
typedef jvalue (*jfn)(fobj *self, const jvalue *args);
/* An emulated field getter. */
typedef jvalue (*jfield_fn)(fobj *self);

void fakejni_init(void);
void *fakejni_vm(void);     /* JavaVM*  */
void *fakejni_env(void);    /* JNIEnv*  */

fobj *jclass(const char *name);
fobj *jnew(const char *class_name);                 /* plain object of a class */
fobj *jstr(const char *utf8);
fobj *jarray(char elem, int len);
const char *jstr_utf8(fobj *s);

/* key = "pkg/Class.method(sig)ret"   e.g. "android/content/Context.getPackageName()Ljava/lang/String;" */
void java_method(const char *key, jfn fn);
/* key = "pkg/Class.field" */
void java_field(const char *key, jfield_fn fn);
void java_static_field(const char *key, jfield_fn fn);

/* helpers to build return values */
static inline jvalue jv_l(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static inline jvalue jv_i(int32_t i) { jvalue v; v.j = 0; v.i = i; return v; }
static inline jvalue jv_j(int64_t j) { jvalue v; v.j = j; return v; }
static inline jvalue jv_z(int z) { jvalue v; v.j = 0; v.z = z ? 1 : 0; return v; }
static inline jvalue jv_f(float f) { jvalue v; v.j = 0; v.f = f; return v; }
static inline jvalue jv_d(double d) { jvalue v; v.d = d; return v; }
#define JV_VOID jv_l(0)

/* natives registered by the guest through RegisterNatives */
void *fakejni_find_native(const char *class_name, const char *method);

/* Android framework emulation (androidfw.c) */
void androidfw_init(void);
void looper_init(void);         /* Looper/Handler/Choreographer runtime (looper.c) */
extern fobj *g_activity;        /* the Context/Activity handed to UnityPlayer */

#endif
