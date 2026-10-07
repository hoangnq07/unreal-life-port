/*
 * Emulation of the slice of the Android framework / Java side that libunity reaches through JNI.
 * Methods are keyed by "pkg/Class.method(sig)ret". Anything missing is reported by fakejni.c at runtime
 * ("JAVA UNIMPL call ..."), so this file grows by running the game and reading that log.
 */
#define _GNU_SOURCE
#include "android_native.h"
#include "fakejni.h"
#include "loader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

fobj *g_activity;

static const char *apk_path(void) { const char *p = getenv("UNITY_APK"); return p ? p : "game.apk"; }
static const char *data_dir(void) { const char *p = getenv("UNITY_DATA"); return p ? p : "userdata"; }
static const char *lib_dir(void) { const char *p = getenv("UNITY_LIBS"); return p ? p : "libs"; }

#define PKG "net.room6.unreallife"

/* explicit functions + tables registered in androidfw_init() */
typedef struct { const char *key; jfn fn; } mdef;
typedef struct { const char *key; jfield_fn fn; } fdef;

#define FN(name) static jvalue name(fobj *self, const jvalue *a)

static jvalue ret_null(fobj *s, const jvalue *a) { return jv_l(0); }
static jvalue ret_false(fobj *s, const jvalue *a) { return jv_z(0); }
static jvalue ret_true(fobj *s, const jvalue *a) { return jv_z(1); }
static jvalue ret_zero(fobj *s, const jvalue *a) { return jv_i(0); }
static jvalue ret_one(fobj *s, const jvalue *a) { return jv_i(1); }
static jvalue ret_void(fobj *s, const jvalue *a) { return JV_VOID; }
static jvalue ret_self(fobj *s, const jvalue *a) { return jv_l(s); }

static int ensure_dir(const char *dir) {
    if (!dir || !*dir) return 0;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 1 : 0;
}

/* ---- Context ---- */
FN(ctx_pkgname) { return jv_l(jstr(PKG)); }
FN(ctx_pkgcodepath) { return jv_l(jstr(apk_path())); }
FN(ctx_filesdir) {
    char p[1024]; snprintf(p, sizeof p, "%s/files", data_dir());
    ensure_dir(p);
    fobj *f = jnew("java/io/File"); f->name = strdup(p); return jv_l(f);
}
FN(ctx_cachedir) {
    char p[1024]; snprintf(p, sizeof p, "%s/cache", data_dir());
    ensure_dir(p);
    fobj *f = jnew("java/io/File"); f->name = strdup(p); return jv_l(f);
}
FN(ctx_obbdir) {
    char p[1024]; snprintf(p, sizeof p, "%s/obb", data_dir());
    ensure_dir(p);
    fobj *f = jnew("java/io/File"); f->name = strdup(p); return jv_l(f);
}
FN(file_abspath) { return jv_l(jstr(self && self->name ? self->name : "")); }
FN(file_exists) { struct stat st; return jv_z(self && self->name && stat(self->name, &st) == 0); }
FN(ctx_getappinfo) {
    fobj *ai = jnew("android/content/pm/ApplicationInfo");
    return jv_l(ai);
}
FN(ctx_sysservice) {
    const char *n = jstr_utf8((fobj *)a[0].l);
    so_log("getSystemService(%s)", n);
    if (!strcmp(n, "window")) return jv_l(jnew("android/view/WindowManager"));
    if (!strcmp(n, "display")) return jv_l(jnew("android/hardware/display/DisplayManager"));
    if (!strcmp(n, "audio")) return jv_l(jnew("android/media/AudioManager"));
    if (!strcmp(n, "clipboard")) return jv_l(jnew("android/content/ClipboardManager"));
    if (!strcmp(n, "connectivity")) return jv_l(jnew("android/net/ConnectivityManager"));
    if (!strcmp(n, "input")) return jv_l(jnew("android/hardware/input/InputManager"));
    return jv_l(0);
}
FN(ctx_getresources) { return jv_l(jnew("android/content/res/Resources")); }
FN(ctx_getassets) { return jv_l(jnew("android/content/res/AssetManager")); }
FN(ctx_getlooper) { return jv_l(jnew("android/os/Looper")); }

/* ---- Display / metrics ---- */
FN(wm_getdisplay) { return jv_l(jnew("android/view/Display")); }
FN(disp_w) { return jv_i(g_screen_w); }
FN(disp_h) { return jv_i(g_screen_h); }
FN(disp_rate) { return jv_f(60.0f); }
FN(disp_getmetrics) {
    fobj *m = (fobj *)a[0].l;
    if (m) { m->ival = 1; }
    return JV_VOID;
}
FN(res_getmetrics) { return jv_l(jnew("android/util/DisplayMetrics")); }
static jvalue dm_widthpx(fobj *s) { return jv_i(g_screen_w); }
static jvalue dm_heightpx(fobj *s) { return jv_i(g_screen_h); }
static jvalue dm_density(fobj *s) { return jv_f(1.0f); }
static jvalue dm_dpi(fobj *s) { return jv_i(160); }

/* ---- Build ---- */
static jvalue build_sdk(fobj *s) { return jv_i(29); }
static jvalue build_str_android(fobj *s) { return jv_l(jstr("10")); }
static jvalue build_str_rk(fobj *s) { return jv_l(jstr("rockchip")); }
static jvalue build_str_r36s(fobj *s) { return jv_l(jstr("R36S")); }

/* ---- Settings / misc ---- */
FN(str_empty) { return jv_l(jstr("")); }

/* ---- System/Locale/etc. often used by Unity through statics ---- */
FN(sys_getproperty) {
    const char *k = jstr_utf8((fobj *)a[0].l);
    if (!strcmp(k, "java.vm.version")) return jv_l(jstr("2.1.0"));
    return jv_l(0);
}
FN(locale_getdefault) { return jv_l(jnew("java/util/Locale")); }
FN(locale_tostr) { return jv_l(jstr("en_US")); }
FN(locale_lang) { return jv_l(jstr("en")); }

/* ======================= batch 2 ======================= */
static char *sb_text(fobj *s) { if (!s->name) s->name = strdup(""); return s->name; }
static void sb_add(fobj *s, const char *t) {
    char *o = sb_text(s);
    s->name = realloc(o, strlen(o) + strlen(t) + 1);
    strcat(s->name, t);
}
FN(sb_init) { return JV_VOID; }
FN(sb_init_str) { sb_text(self); sb_add(self, jstr_utf8((fobj *)a[0].l)); return JV_VOID; }
FN(sb_append_str) { sb_add(self, a[0].l ? jstr_utf8((fobj *)a[0].l) : "null"); return jv_l(self); }
FN(sb_append_int) { char b[32]; snprintf(b, sizeof b, "%d", a[0].i); sb_add(self, b); return jv_l(self); }
FN(sb_append_long) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)a[0].j); sb_add(self, b); return jv_l(self); }
FN(sb_append_char) { char b[2] = {(char)a[0].c, 0}; sb_add(self, b); return jv_l(self); }
FN(sb_tostring) { return jv_l(jstr(sb_text(self))); }
FN(str_length) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)jstr_utf8(self); *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return jv_i(n);
}
FN(str_tostring) { return jv_l(self ? self : (fobj *)jstr("")); }

/* Intent / Bundle */
FN(act_getintent) { return jv_l(jnew("android/content/Intent")); }
FN(intent_getstring) { return jv_l(0); }
FN(intent_getbool) { return jv_z(a[1].z); }
FN(intent_getint) { return jv_i(a[1].i); }
FN(bundle_getstring) { return jv_l(0); }
FN(bundle_getbool) { return jv_z(a[1].z); }
FN(bundle_getint) { return jv_i(a[1].i); }

/* AssetManager / Scanner */
FN(am_open) {
    so_log("AssetManager.open(%s)", jstr_utf8((fobj *)a[0].l));
    return jv_l(jnew("java/io/InputStream"));
}
FN(scanner_init) { return JV_VOID; }
FN(scanner_use) { return jv_l(self); }
FN(scanner_next) { return jv_l(jstr("")); }

/* PackageManager / ApplicationInfo / PackageInfo */
FN(ctx_getpm) { return jv_l(jnew("android/content/pm/PackageManager")); }
FN(pm_getappinfo) { return jv_l(jnew("android/content/pm/ApplicationInfo")); }
FN(pm_getpkginfo) { return jv_l(jnew("android/content/pm/PackageInfo")); }
static jvalue ai_native_dir(fobj *s) { return jv_l(jstr(lib_dir())); }
static jvalue ai_source_dir(fobj *s) { return jv_l(jstr(apk_path())); }
static jvalue ai_data_dir(fobj *s) { return jv_l(jstr(data_dir())); }
static jvalue ai_pkg(fobj *s) { return jv_l(jstr(PKG)); }
static jvalue f_null(fobj *s) { return jv_l(0); }
static jvalue f_zero(fobj *s) { return jv_i(0); }
static jvalue f_one(fobj *s) { return jv_i(1); }
static jvalue f_bundle(fobj *s) { return jv_l(jnew("android/os/Bundle")); }
static jvalue f_sdk29(fobj *s) { return jv_i(29); }
static jvalue f_ver_name(fobj *s) { return jv_l(jstr("1.0")); }
static jvalue f_str_location(fobj *s) { return jv_l(jstr("location")); }
static jvalue f_flags_none(fobj *s) { return jv_i(0); }
static jvalue f_get_meta(fobj *s) { return jv_i(128); }

/* SharedPreferences (in-memory; contents are not persisted yet) */
FN(ctx_getprefs) { return jv_l(jnew("android/content/SharedPreferences")); }
FN(prefs_edit) { return jv_l(jnew("android/content/SharedPreferences$Editor")); }
FN(prefs_getint) { return jv_i(a[1].i); }
FN(prefs_getlong) { return jv_j(a[1].j); }
FN(prefs_getbool) { return jv_z(a[1].z); }
FN(prefs_getfloat) { return jv_f(a[1].f); }
FN(prefs_getstring) { return jv_l(a[1].l); }
FN(prefs_getall) { return jv_l(jnew("java/util/Map")); }
FN(map_entryset) { return jv_l(jnew("java/util/Set")); }
FN(set_iterator) { return jv_l(jnew("java/util/Iterator")); }
FN(iter_hasnext) {
    int has = (self && self->ival < 2) ? 1 : 0;
    return jv_z(has);
}
FN(iter_next) {
    int idx = self ? (int)self->ival++ : 0;
    const char *name = (idx == 0) ? "bgm_bundle" : "volta_bundle";
    fobj *entry = jnew("java/util/Map$Entry");
    entry->name = strdup(name);
    entry->ival = idx;
    return jv_l(entry);
}
FN(entry_getkey) {
    const char *name = (self && self->name) ? self->name : "bgm_bundle";
    return jv_l(jstr(name));
}
FN(entry_getvalue) {
    const char *name = (self && self->name) ? self->name : "bgm_bundle";
    fobj *st = jnew("com/google/android/play/core/assetpacks/AssetPackState");
    st->name = strdup(name);
    return jv_l(st);
}

/* Looper / Handler / UI thread: posted work is dropped (there is no UI) */
FN(looper_main) { return jv_l(jnew("android/os/Looper")); }
FN(handler_init) { return JV_VOID; }
FN(handler_post) { return jv_z(1); }
FN(act_runui) { return JV_VOID; }

/* reflection / class loading */
FN(obj_getclass) {
    if (self) {
        if (self->kind == K_CLASS) return jv_l(jclass("java/lang/Class"));
        if (self->cls) return jv_l(self->cls);
    }
    return jv_l(jclass("java/lang/Object"));
}
FN(cls_getloader) { return jv_l(jnew("java/lang/ClassLoader")); }
FN(cl_findlib) {
    char p[1024];
    snprintf(p, sizeof p, "%s/lib%s.so", lib_dir(), jstr_utf8((fobj *)a[0].l));
    return jv_l(jstr(p));
}
FN(jnibridge_proxy) {
    fobj *o = jnew("java/lang/reflect/Proxy");
    o->ival = a[0].j;
    fobj *arr = a[1].l;
    if (arr && arr->len > 0) o->user = ((fobj **)arr->data)[0];
    return jv_l(o);
}
FN(ar_false) { return jv_z(0); }
FN(ctx_getobbdirs) { return jv_l(jarray('L', 0)); }

/* AlertDialog: Unity uses it to surface fatal errors, so print them */
FN(dlg_init) { return JV_VOID; }
FN(dlg_title) { so_log("ALERT title: %s", a[0].l ? jstr_utf8((fobj *)a[0].l) : "(null)"); return jv_l(self); }
FN(dlg_message) { so_log("ALERT message: %s", a[0].l ? jstr_utf8((fobj *)a[0].l) : "(null)"); return jv_l(self); }
FN(dlg_self) { return jv_l(self); }
FN(dlg_show) { so_log("ALERT shown"); return jv_l(self); }

static const mdef methods2[] = {
    {"java/lang/StringBuilder.<init>()V", sb_init},
    {"java/lang/StringBuilder.<init>(Ljava/lang/String;)V", sb_init_str},
    {"java/lang/StringBuilder.append(Ljava/lang/String;)Ljava/lang/StringBuilder;", sb_append_str},
    {"java/lang/StringBuilder.append(Ljava/lang/Object;)Ljava/lang/StringBuilder;", sb_append_str},
    {"java/lang/StringBuilder.append(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;", sb_append_str},
    {"java/lang/StringBuilder.append(I)Ljava/lang/StringBuilder;", sb_append_int},
    {"java/lang/StringBuilder.append(J)Ljava/lang/StringBuilder;", sb_append_long},
    {"java/lang/StringBuilder.append(C)Ljava/lang/StringBuilder;", sb_append_char},
    {"java/lang/StringBuilder.toString()Ljava/lang/String;", sb_tostring},
    {"java/lang/String.length()I", str_length},
    {"java/lang/String.toString()Ljava/lang/String;", str_tostring},
    {"android/app/Activity.getIntent()Landroid/content/Intent;", act_getintent},
    {"android/content/Intent.getStringExtra(Ljava/lang/String;)Ljava/lang/String;", intent_getstring},
    {"android/content/Intent.getBooleanExtra(Ljava/lang/String;Z)Z", intent_getbool},
    {"android/content/Intent.getIntExtra(Ljava/lang/String;I)I", intent_getint},
    {"android/content/Intent.hasExtra(Ljava/lang/String;)Z", ret_false},
    {"android/os/Bundle.getString(Ljava/lang/String;)Ljava/lang/String;", bundle_getstring},
    {"android/os/Bundle.getBoolean(Ljava/lang/String;Z)Z", bundle_getbool},
    {"android/os/Bundle.getInt(Ljava/lang/String;I)I", bundle_getint},
    {"android/os/Bundle.containsKey(Ljava/lang/String;)Z", ret_false},
    {"android/content/res/AssetManager.open(Ljava/lang/String;)Ljava/io/InputStream;", am_open},
    {"java/util/Scanner.<init>(Ljava/io/InputStream;Ljava/lang/String;)V", scanner_init},
    {"java/util/Scanner.useDelimiter(Ljava/lang/String;)Ljava/util/Scanner;", scanner_use},
    {"java/util/Scanner.next()Ljava/lang/String;", scanner_next},
    {"java/util/Scanner.hasNext()Z", ret_false},
    {"java/util/Scanner.close()V", ret_void},
    {"java/io/InputStream.close()V", ret_void},
    {"android/content/Context.getPackageManager()Landroid/content/pm/PackageManager;", ctx_getpm},
    {"android/content/pm/PackageManager.getApplicationInfo(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", pm_getappinfo},
    {"android/content/pm/PackageManager.getPackageInfo(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;", pm_getpkginfo},
    {"android/content/Context.getSharedPreferences(Ljava/lang/String;I)Landroid/content/SharedPreferences;", ctx_getprefs},
    {"android/content/SharedPreferences.edit()Landroid/content/SharedPreferences$Editor;", prefs_edit},
    {"android/content/SharedPreferences.getInt(Ljava/lang/String;I)I", prefs_getint},
    {"android/content/SharedPreferences.getLong(Ljava/lang/String;J)J", prefs_getlong},
    {"android/content/SharedPreferences.getBoolean(Ljava/lang/String;Z)Z", prefs_getbool},
    {"android/content/SharedPreferences.getFloat(Ljava/lang/String;F)F", prefs_getfloat},
    {"android/content/SharedPreferences.getString(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", prefs_getstring},
    {"android/content/SharedPreferences.contains(Ljava/lang/String;)Z", ret_false},
    {"android/content/SharedPreferences.getAll()Ljava/util/Map;", prefs_getall},
    {"java/util/Map.entrySet()Ljava/util/Set;", map_entryset},
    {"java/util/Set.iterator()Ljava/util/Iterator;", set_iterator},
    {"java/util/Iterator.hasNext()Z", iter_hasnext},
    {"java/util/Iterator.next()Ljava/lang/Object;", iter_next},
    {"java/util/Map$Entry.getKey()Ljava/lang/Object;", entry_getkey},
    {"java/util/Map$Entry.getValue()Ljava/lang/Object;", entry_getvalue},
    {"android/content/SharedPreferences$Editor.putInt(Ljava/lang/String;I)Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.putLong(Ljava/lang/String;J)Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.putBoolean(Ljava/lang/String;Z)Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.putFloat(Ljava/lang/String;F)Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.putString(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.remove(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.clear()Landroid/content/SharedPreferences$Editor;", ret_self},
    {"android/content/SharedPreferences$Editor.apply()V", ret_void},
    {"android/content/SharedPreferences$Editor.commit()Z", ret_true},
    {"android/os/Looper.getMainLooper()Landroid/os/Looper;", looper_main},
    {"android/os/Handler.<init>(Landroid/os/Looper;)V", handler_init},
    {"android/os/Handler.<init>()V", handler_init},
    {"android/os/Handler.post(Ljava/lang/Runnable;)Z", handler_post},
    {"android/os/Handler.postDelayed(Ljava/lang/Runnable;J)Z", handler_post},
    {"android/app/Activity.runOnUiThread(Ljava/lang/Runnable;)V", act_runui},
    {"java/lang/Object.<init>()V", ret_void},
    {"java/lang/Object.getClass()Ljava/lang/Class;", obj_getclass},
    {"java/lang/Object.getClass()Ljava/lang/Object;", obj_getclass},
    {"android/app/Activity.getClass()Ljava/lang/Object;", obj_getclass},
    {"android/app/Activity.getClass()Ljava/lang/Class;", obj_getclass},
    {"android/content/Context.getClass()Ljava/lang/Object;", obj_getclass},
    {"android/content/Context.getClass()Ljava/lang/Class;", obj_getclass},
    {"com/unity3d/player/UnityPlayerActivity.getClass()Ljava/lang/Object;", obj_getclass},
    {"com/unity3d/player/UnityPlayerActivity.getClass()Ljava/lang/Class;", obj_getclass},
    {"java/lang/Class.getClassLoader()Ljava/lang/ClassLoader;", cls_getloader},
    {"java/lang/ClassLoader.findLibrary(Ljava/lang/String;)Ljava/lang/String;", cl_findlib},
    {"bitter/jnibridge/JNIBridge.newInterfaceProxy(J[Ljava/lang/Class;)Ljava/lang/Object;", jnibridge_proxy},
    {"com/unity3d/player/UnityPlayer.initializeGoogleAr()Z", ar_false},
    {"android/content/Context.getObbDirs()[Ljava/io/File;", ctx_getobbdirs},
    {"android/app/AlertDialog$Builder.<init>(Landroid/content/Context;)V", dlg_init},
    {"android/app/AlertDialog$Builder.setTitle(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", dlg_title},
    {"android/app/AlertDialog$Builder.setMessage(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;", dlg_message},
    {"android/app/AlertDialog$Builder.setCancelable(Z)Landroid/app/AlertDialog$Builder;", dlg_self},
    {"android/app/AlertDialog$Builder.create()Landroid/app/AlertDialog;", dlg_self},
    {"android/app/AlertDialog$Builder.show()Landroid/app/AlertDialog;", dlg_show},
    {"android/app/AlertDialog.show()V", ret_void},
    {NULL, NULL}};

static const fdef fields2[] = {
    {"android/content/pm/ApplicationInfo.nativeLibraryDir", ai_native_dir},
    {"android/content/pm/ApplicationInfo.sourceDir", ai_source_dir},
    {"android/content/pm/ApplicationInfo.publicSourceDir", ai_source_dir},
    {"android/content/pm/ApplicationInfo.dataDir", ai_data_dir},
    {"android/content/pm/ApplicationInfo.splitPublicSourceDirs", f_null},
    {"android/content/pm/ApplicationInfo.splitSourceDirs", f_null},
    {"android/content/pm/ApplicationInfo.flags", f_flags_none},
    {"android/content/pm/ApplicationInfo.targetSdkVersion", f_sdk29},
    {"android/content/pm/PackageItemInfo.metaData", f_bundle},
    {"android/content/pm/PackageItemInfo.packageName", ai_pkg},
    {"android/content/pm/PackageInfo.versionCode", f_one},
    {"android/content/pm/PackageInfo.versionName", f_ver_name},
    {"android/content/pm/PackageInfo.packageName", ai_pkg},
    {NULL, NULL}};

static const fdef sfields2[] = {
    {"android/content/Context.LOCATION_SERVICE", f_str_location},
    {"android/content/Context.MODE_PRIVATE", f_zero},
    {"android/content/pm/PackageManager.GET_META_DATA", f_get_meta},
    {NULL, NULL}};


static const mdef methods[] = {
    {"android/content/Context.getPackageName()Ljava/lang/String;", ctx_pkgname},
    {"android/content/Context.getPackageCodePath()Ljava/lang/String;", ctx_pkgcodepath},
    {"android/content/Context.getFilesDir()Ljava/io/File;", ctx_filesdir},
    {"android/content/Context.getCacheDir()Ljava/io/File;", ctx_cachedir},
    {"android/content/Context.getObbDir()Ljava/io/File;", ctx_obbdir},
    {"android/content/Context.getExternalFilesDir(Ljava/lang/String;)Ljava/io/File;", ctx_filesdir},
    {"android/content/Context.getApplicationContext()Landroid/content/Context;", ret_self},
    {"android/content/Context.getApplicationInfo()Landroid/content/pm/ApplicationInfo;", ctx_getappinfo},
    {"android/content/Context.getSystemService(Ljava/lang/String;)Ljava/lang/Object;", ctx_sysservice},
    {"android/content/Context.getResources()Landroid/content/res/Resources;", ctx_getresources},
    {"android/content/Context.getAssets()Landroid/content/res/AssetManager;", ctx_getassets},
    {"android/content/Context.getMainLooper()Landroid/os/Looper;", ctx_getlooper},
    {"android/app/Activity.getWindowManager()Landroid/view/WindowManager;", ret_self},
    {"java/io/File.getAbsolutePath()Ljava/lang/String;", file_abspath},
    {"java/io/File.getPath()Ljava/lang/String;", file_abspath},
    {"java/io/File.exists()Z", file_exists},
    {"android/view/WindowManager.getDefaultDisplay()Landroid/view/Display;", wm_getdisplay},
    {"android/view/Display.getWidth()I", disp_w},
    {"android/view/Display.getHeight()I", disp_h},
    {"android/view/Display.getRefreshRate()F", disp_rate},
    {"android/view/Display.getMetrics(Landroid/util/DisplayMetrics;)V", disp_getmetrics},
    {"android/view/Display.getRealMetrics(Landroid/util/DisplayMetrics;)V", disp_getmetrics},
    {"android/content/res/Resources.getDisplayMetrics()Landroid/util/DisplayMetrics;", res_getmetrics},
    {"java/lang/System.getProperty(Ljava/lang/String;)Ljava/lang/String;", sys_getproperty},
    {"java/util/Locale.getDefault()Ljava/util/Locale;", locale_getdefault},
    {"java/util/Locale.toString()Ljava/lang/String;", locale_tostr},
    {"java/util/Locale.getLanguage()Ljava/lang/String;", locale_lang},
    {NULL, NULL}};

static const fdef fields[] = {
    {"android/util/DisplayMetrics.widthPixels", dm_widthpx},
    {"android/util/DisplayMetrics.heightPixels", dm_heightpx},
    {"android/util/DisplayMetrics.density", dm_density},
    {"android/util/DisplayMetrics.densityDpi", dm_dpi},
    {"android/util/DisplayMetrics.xdpi", dm_dpi},
    {"android/util/DisplayMetrics.ydpi", dm_dpi},
    {NULL, NULL}};

static const fdef sfields[] = {
    {"android/os/Build$VERSION.SDK_INT", build_sdk},
    {"android/os/Build$VERSION.RELEASE", build_str_android},
    {"android/os/Build.MANUFACTURER", build_str_rk},
    {"android/os/Build.MODEL", build_str_r36s},
    {"android/os/Build.DEVICE", build_str_r36s},
    {"android/os/Build.BRAND", build_str_rk},
    {NULL, NULL}};

/* ======================= batch 3 ======================= */
FN(file_getparent) {
    if (!self || !self->name) return jv_l(0);
    char *p = strdup(self->name), *s = strrchr(p, '/');
    if (!s) { free(p); return jv_l(0); }
    if (s == p) s[1] = 0; else *s = 0;
    fobj *r = jstr(p);
    free(p);
    return jv_l(r);
}
FN(file_getname) {
    const char *s = self && self->name ? strrchr(self->name, '/') : NULL;
    return jv_l(jstr(s ? s + 1 : (self && self->name ? self->name : "")));
}
FN(file_mkdirs) {
    const char *p = self && self->name ? self->name : ".";
    return jv_z(ensure_dir(p));
}
FN(file_isdir) { struct stat st; return jv_z(self && self->name && stat(self->name, &st) == 0 && S_ISDIR(st.st_mode)); }
FN(file_isfile) { struct stat st; return jv_z(self && self->name && stat(self->name, &st) == 0 && S_ISREG(st.st_mode)); }
#define CONST_I(fname, val) static jvalue fname(fobj *s) { (void)s; return jv_i(val); }
#define CONST_S(fname, val) static jvalue fname(fobj *s) { (void)s; return jv_l(jstr(val)); }
CONST_S(c_mounted, "mounted")
CONST_S(c_display_service, "display")
CONST_I(c_cutout_never, 2)
CONST_I(c_so_unspecified, -1)
CONST_I(c_so_landscape, 0)
CONST_I(c_so_portrait, 1)
CONST_I(c_so_sensor, 4)
CONST_I(c_so_sensor_landscape, 6)
CONST_I(c_so_sensor_portrait, 7)
CONST_I(c_so_reverse_landscape, 8)
CONST_I(c_so_reverse_portrait, 9)
CONST_I(c_so_full_sensor, 10)
FN(env_extstate) { return jv_l(jstr("mounted")); }
FN(str_equals) {
    fobj *o = (fobj *)a[0].l;
    return jv_z(o && o->kind == K_STRING && self && !strcmp(self->name, o->name));
}
FN(proc_setprio) { return JV_VOID; }
FN(dm_getdisplay) { return jv_l(jnew("android/view/Display")); }
FN(disp_id) { return jv_i(0); }
FN(disp_rotation) { return jv_i(0); }
FN(uri_encode) { return jv_l(a[0].l); }
FN(act_getwindow) { return jv_l(jnew("android/view/Window")); }
FN(win_decor) { return jv_l(jnew("android/view/View")); }
FN(win_attrs) { return jv_l(jnew("android/view/WindowManager$LayoutParams")); }
FN(act_getorient) { return jv_i(0); }

static const mdef methods3[] = {
    {"java/io/File.getParent()Ljava/lang/String;", file_getparent},
    {"java/io/File.getName()Ljava/lang/String;", file_getname},
    {"java/io/File.mkdirs()Z", file_mkdirs},
    {"java/io/File.mkdir()Z", file_mkdirs},
    {"java/io/File.isDirectory()Z", file_isdir},
    {"java/io/File.isFile()Z", file_isfile},
    {"java/io/File.canRead()Z", ret_true},
    {"java/io/File.canWrite()Z", ret_true},
    {"android/os/Environment.getExternalStorageState()Ljava/lang/String;", env_extstate},
    {"java/lang/String.equals(Ljava/lang/Object;)Z", str_equals},
    {"android/os/Process.setThreadPriority(II)V", proc_setprio},
    {"android/hardware/display/DisplayManager.getDisplay(I)Landroid/view/Display;", dm_getdisplay},
    {"android/view/Display.getDisplayId()I", disp_id},
    {"android/view/Display.getRotation()I", disp_rotation},
    {"android/net/Uri.encode(Ljava/lang/String;)Ljava/lang/String;", uri_encode},
    {"android/app/Activity.getWindow()Landroid/view/Window;", act_getwindow},
    {"android/view/Window.getDecorView()Landroid/view/View;", win_decor},
    {"android/view/Window.getAttributes()Landroid/view/WindowManager$LayoutParams;", win_attrs},
    {"android/view/View.getRootWindowInsets()Landroid/view/WindowInsets;", ret_null},
    {"android/view/View.setOnApplyWindowInsetsListener(Landroid/view/View$OnApplyWindowInsetsListener;)V", ret_void},
    {"android/view/View.addOnLayoutChangeListener(Landroid/view/View$OnLayoutChangeListener;)V", ret_void},
    {"android/app/Activity.getRequestedOrientation()I", act_getorient},
    {"android/app/Activity.setRequestedOrientation(I)V", ret_void},
    {"android/content/Intent.getExtras()Landroid/os/Bundle;", ret_null},
    {"java/lang/Error.<init>(Ljava/lang/String;)V", ret_void},
    {"java/lang/Error.setStackTrace([Ljava/lang/StackTraceElement;)V", ret_void},
    {"java/lang/StackTraceElement.<init>(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V", ret_void},
    {NULL, NULL}};

static const fdef fields3[] = {
    {"android/view/WindowManager$LayoutParams.layoutInDisplayCutoutMode", f_zero},
    {NULL, NULL}};

static const fdef sfields3[] = {
    {"android/os/Environment.MEDIA_MOUNTED", c_mounted},
    {"android/content/Context.DISPLAY_SERVICE", c_display_service},
    {"android/view/WindowManager$LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER", c_cutout_never},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED", c_so_unspecified},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE", c_so_landscape},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_PORTRAIT", c_so_portrait},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_SENSOR", c_so_sensor},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE", c_so_sensor_landscape},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT", c_so_sensor_portrait},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE", c_so_reverse_landscape},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT", c_so_reverse_portrait},
    {"android/content/pm/ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR", c_so_full_sensor},
    {NULL, NULL}};

/* ======================= batch 4 ======================= */
CONST_S(c_audio_service, "audio")
CONST_S(c_media_router, "media_router")
CONST_S(c_feat_lowlat, "android.hardware.audio.low_latency")
CONST_S(c_prop_rate, "android.media.property.OUTPUT_SAMPLE_RATE")
CONST_S(c_prop_frames, "android.media.property.OUTPUT_FRAMES_PER_BUFFER")
CONST_S(c_android_id, "android_id")
CONST_I(c_perm_granted, 0)
CONST_I(c_route_live_video, 2)
FN(am_getprop) {
    const char *k = jstr_utf8((fobj *)a[0].l);
    const char *val = "512";
    if (k && strstr(k, "SAMPLE_RATE")) val = "48000";
    so_log("AudioManager.getProperty(%s) -> %s", k ? k : "", val);
    return jv_l(jstr(val));
}
FN(pm_hasfeature) { return jv_z(0); }
FN(ctx_checkperm) { return jv_i(0); }
FN(ctx_contentresolver) { return jv_l(jnew("android/content/ContentResolver")); }
FN(secure_getstring) { return jv_l(jstr("unrealr36s0000000")); }
FN(inputdev_ids) {
    /* Cached: the emulated JNI never frees objects, so a fresh array on every call
     * would leak one allocation per poll. Rewired polls the device list regularly. */
    static fobj *arr;
    if (!arr) {
        arr = jarray('I', 1);
        if (arr && arr->data) ((int32_t *)arr->data)[0] = 1;
    }
    return jv_l(arr);
}
FN(ret_arg0) { return jv_l(a[0].l); }
FN(handlerthread_looper) { return jv_l(jnew("android/os/Looper")); }
FN(handler_obtainmsg) { return jv_l(jnew("android/os/Message")); }
FN(locale_country) { return jv_l(jstr("US")); }
FN(int_parseint) { return jv_i(atoi(jstr_utf8((fobj *)a[0].l))); }
CONST_I(c_flag_keep_screen_on, 128)
CONST_I(c_ui_fullscreen, 4)
CONST_I(c_ui_hide_nav, 2)
CONST_I(c_ui_immersive_sticky, 4096)
CONST_I(c_ui_layout_fullscreen, 1024)
CONST_I(c_ui_layout_hide_nav, 512)
CONST_I(c_ui_layout_stable, 256)
FN(method_tostring) { return jv_l(jstr(self && self->name ? self->name : "")); }

static const mdef methods4[] = {
    {"android/media/AudioManager.getProperty(Ljava/lang/String;)Ljava/lang/String;", am_getprop},
    {"android/media/AudioManager.isBluetoothA2dpOn()Z", ret_false},
    {"android/content/pm/PackageManager.hasSystemFeature(Ljava/lang/String;)Z", pm_hasfeature},
    {"android/content/Context.checkCallingOrSelfPermission(Ljava/lang/String;)I", ctx_checkperm},
    {"android/content/Context.getContentResolver()Landroid/content/ContentResolver;", ctx_contentresolver},
    {"android/provider/Settings$Secure.getString(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", secure_getstring},
    {"android/view/InputDevice.getDeviceIds()[I", inputdev_ids},
    {"android/view/View.onApplyWindowInsets(Landroid/view/WindowInsets;)Landroid/view/WindowInsets;", ret_arg0},
    {"android/view/WindowInsets.getDisplayCutout()Landroid/view/DisplayCutout;", ret_null},
    {"android/hardware/display/DisplayManager.registerDisplayListener(Landroid/hardware/display/DisplayManager$DisplayListener;Landroid/os/Handler;)V", ret_void},
    {"org/fmod/FMODAudioDevice.<init>()V", ret_void},
    {"org/fmod/FMODAudioDevice.start()V", ret_void},
    {"org/fmod/FMODAudioDevice.stop()V", ret_void},
    {"org/fmod/FMODAudioDevice.close()V", ret_void},
    {"org/fmod/FMODAudioDevice.startAudioRecord(III)I", ret_zero},
    {"org/fmod/FMODAudioDevice.stopAudioRecord()V", ret_void},
    {"java/lang/System.load(Ljava/lang/String;)V", ret_void},
    {"com/unity3d/player/UnityPlayer.hideSoftInput()V", ret_void},
    {"com/unity3d/player/UnityPlayer.addPhoneCallListener()V", ret_void},
    {"android/os/HandlerThread.<init>(Ljava/lang/String;)V", ret_void},
    {"java/lang/Thread.start()V", ret_void},
    {"android/os/HandlerThread.getLooper()Landroid/os/Looper;", handlerthread_looper},
    {"android/os/Handler.<init>(Landroid/os/Looper;Landroid/os/Handler$Callback;)V", ret_void},
    {"android/os/Handler.obtainMessage(I)Landroid/os/Message;", handler_obtainmsg},
    {"android/os/Message.sendToTarget()V", ret_void},
    {"java/util/Locale.getCountry()Ljava/lang/String;", locale_country},
    {"java/lang/Integer.parseInt(Ljava/lang/String;)I", int_parseint},
    {"android/view/Window.setFlags(II)V", ret_void},
    {"android/view/Window.addFlags(I)V", ret_void},
    {"android/view/Window.clearFlags(I)V", ret_void},
    {"android/view/View.getSystemUiVisibility()I", ret_zero},
    {"android/view/View.setSystemUiVisibility(I)V", ret_void},
    {"java/lang/reflect/Method.toString()Ljava/lang/String;", method_tostring},
    {NULL, NULL}};

static const fdef sfields4[] = {
    {"android/view/WindowManager$LayoutParams.FLAG_KEEP_SCREEN_ON", c_flag_keep_screen_on},
    {"android/view/View.SYSTEM_UI_FLAG_FULLSCREEN", c_ui_fullscreen},
    {"android/view/View.SYSTEM_UI_FLAG_HIDE_NAVIGATION", c_ui_hide_nav},
    {"android/view/View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY", c_ui_immersive_sticky},
    {"android/view/View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN", c_ui_layout_fullscreen},
    {"android/view/View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION", c_ui_layout_hide_nav},
    {"android/view/View.SYSTEM_UI_FLAG_LAYOUT_STABLE", c_ui_layout_stable},
    {"android/content/Context.AUDIO_SERVICE", c_audio_service},
    {"android/content/Context.MEDIA_ROUTER_SERVICE", c_media_router},
    {"android/content/pm/PackageManager.FEATURE_AUDIO_LOW_LATENCY", c_feat_lowlat},
    {"android/content/pm/PackageManager.PERMISSION_GRANTED", c_perm_granted},
    {"android/media/AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE", c_prop_rate},
    {"android/media/AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER", c_prop_frames},
    {"android/media/MediaRouter.ROUTE_TYPE_LIVE_VIDEO", c_route_live_video},
    {"android/provider/Settings$Secure.ANDROID_ID", c_android_id},
    {NULL, NULL}};

/* ======================= batch 5 ======================= */
CONST_S(c_power_service, "power")
CONST_S(c_vibrator_service, "vibrator")
CONST_S(c_build_tags, "release-keys")
CONST_S(c_build_id, "UNREALR36S")
CONST_S(c_build_incremental, "1")
static jvalue sf_current_activity(fobj *s) { (void)s; return jv_l(g_activity); }
FN(power_sustained) { return jv_z(0); }
FN(pm_installer) { return jv_l(jstr("com.android.vending")); }

FN(refl_get_method) {
    fobj *cls = (fobj *)a[0].l;
    fobj *name = (fobj *)a[1].l;
    fobj *sig = (fobj *)a[2].l;
    int is_static = a[3].z;
    const char *sname = jstr_utf8(name);
    const char *ssig = jstr_utf8(sig);
    fobj *m = jnew("java/lang/reflect/Method");
    m->name = strdup(sname ? sname : "");
    m->user = cls;
    m->data = strdup(ssig ? ssig : "");
    m->ival = is_static;
    so_log("[refl] getMethodID(%s, %s, %s, %d)", cls ? cls->name : "?", sname, ssig, is_static);
    return jv_l(m);
}
FN(refl_get_constructor) {
    fobj *cls = (fobj *)a[0].l;
    fobj *sig = (fobj *)a[1].l;
    const char *ssig = jstr_utf8(sig);
    fobj *m = jnew("java/lang/reflect/Constructor");
    m->name = strdup("<init>");
    m->user = cls;
    m->data = strdup(ssig ? ssig : "");
    m->ival = 0;
    so_log("[refl] getConstructorID(%s, %s)", cls ? cls->name : "?", ssig);
    return jv_l(m);
}
FN(refl_get_field) {
    fobj *cls = (fobj *)a[0].l;
    fobj *name = (fobj *)a[1].l;
    fobj *sig = (fobj *)a[2].l;
    int is_static = a[3].z;
    const char *sname = jstr_utf8(name);
    const char *ssig = jstr_utf8(sig);
    fobj *f = jnew("java/lang/reflect/Field");
    f->name = strdup(sname ? sname : "");
    f->user = cls;
    f->data = strdup(ssig ? ssig : "");
    f->ival = is_static;
    so_log("[refl] getFieldID(%s, %s, %s, %d)", cls ? cls->name : "?", sname, ssig, is_static);
    return jv_l(f);
}
FN(refl_new_proxy) {
    int id = a[0].i;
    fobj *cls = (fobj *)a[1].l;
    fobj *p = jnew(cls ? cls->name : "proxy");
    p->ival = id;
    p->user = cls;
    so_log("[refl] newProxyInstance(id=%d, cls=%s)", id, cls ? cls->name : "?");
    return jv_l(p);
}

FN(mr_get_route) { return jv_l(jnew("android/media/MediaRouter$RouteInfo")); }
FN(route_get_name) { return jv_l(jstr("Default")); }
FN(bool_init) { if (self) self->ival = a[0].z; return JV_VOID; }
FN(bool_val) { return jv_z(self ? (int)self->ival : 0); }
FN(map_init) { return JV_VOID; }
FN(map_put) { return jv_l(0); }

FN(class_forname) {
    fobj *s = (fobj *)a[0].l;
    const char *cn = jstr_utf8(s);
    so_log("[refl] Class.forName(%s)", cn ? cn : "?");
    if (!cn) return jv_l(0);
    char buf[256];
    strncpy(buf, cn, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char *p = buf; *p; p++) if (*p == '.') *p = '/';
    return jv_l(jclass(buf));
}

/* ========================================================================== */
/* Google Play Core: Play Asset Delivery (PAD)                                */
/* ========================================================================== */

static char g_assetpack_dir[640];

static void init_assetpack_dir(void) {
    if (g_assetpack_dir[0]) return;
    const char *apk = getenv("UNITY_APK") ? getenv("UNITY_APK") : "game.apk";
    char dir[640];
    strncpy(dir, apk, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        snprintf(g_assetpack_dir, sizeof g_assetpack_dir, "%s/assets/assetpack", dir);
    } else {
        snprintf(g_assetpack_dir, sizeof g_assetpack_dir, "assets/assetpack");
    }
}

FN(apm_get_instance) {
    init_assetpack_dir();
    so_log("[assetpack] AssetPackManagerFactory.getInstance -> ok");
    return jv_l(jnew("com/google/android/play/core/assetpacks/AssetPackManager"));
}

FN(apm_get_pack_location) {
    init_assetpack_dir();
    fobj *s = (fobj *)a[0].l;
    const char *pn = jstr_utf8(s);
    so_log("[assetpack] getPackLocation(%s)", pn ? pn : "?");
    fobj *loc = jnew("com/google/android/play/core/assetpacks/AssetPackLocation");
    loc->name = strdup(pn ? pn : "");
    return jv_l(loc);
}

FN(apl_storage_method) {
    init_assetpack_dir();
    char testpath[700];
    const char *pn = (self && self->name && self->name[0]) ? self->name : "bgm_bundle";
    snprintf(testpath, sizeof testpath, "%s/%s", g_assetpack_dir, pn);
    struct stat st;
    if (stat(testpath, &st) == 0) {
        so_log("[assetpack] packStorageMethod(%s) -> 0 (StorageFiles at %s)", pn, g_assetpack_dir);
        return jv_i(0); /* StorageFiles */
    }
    so_log("[assetpack] packStorageMethod(%s) -> 1 (ApkAssets from APK)", pn);
    return jv_i(1);     /* ApkAssets */
}

FN(apl_assets_path) {
    init_assetpack_dir();
    so_log("[assetpack] assetsPath() -> %s", g_assetpack_dir);
    return jv_l(jstr(g_assetpack_dir));
}

FN(apm_get_asset_location) {
    fobj *pack = (fobj *)a[0].l;
    fobj *asset = (fobj *)a[1].l;
    const char *pn = jstr_utf8(pack);
    const char *an = jstr_utf8(asset);
    so_log("[assetpack] getAssetLocation(pack=%s, asset=%s)", pn ? pn : "?", an ? an : "?");
    fobj *loc = jnew("com/google/android/play/core/assetpacks/AssetLocation");
    loc->name = strdup(an ? an : "");
    return jv_l(loc);
}

FN(obj_getname) {
    if (self && self->name) {
        char *n = strdup(self->name);
        for (char *p = n; *p; p++) if (*p == '/') *p = '.';
        fobj *r = jstr(n);
        free(n);
        return jv_l(r);
    }
    return jv_l(jstr("java.lang.Object"));
}
FN(sys_hashcode) {
    uintptr_t p = (uintptr_t)a[0].l;
    int h = (int)(p ^ (p >> 16));
    return jv_i(h != 0 ? h : 1);
}
FN(cls_isarray) {
    int is_arr = (self && self->name && self->name[0] == '[') ? 1 : 0;
    return jv_z(is_arr);
}
FN(obj_hashcode) {
    uintptr_t p = (uintptr_t)self;
    int h = (int)(p ^ (p >> 16));
    return jv_i(h != 0 ? h : 1);
}
FN(obj_equals) { return jv_z(self == (fobj *)a[0].l); }

FN(al_path) {
    init_assetpack_dir();
    char testpath[700];
    const char *an = (self && self->name) ? self->name : "";
    const char *short_name = strrchr(an, '/');
    short_name = short_name ? short_name + 1 : an;
    snprintf(testpath, sizeof testpath, "%s/%s", g_assetpack_dir, short_name);
    struct stat st;
    if (stat(testpath, &st) == 0) {
        so_log("[assetpack] AssetLocation.path(%s) -> direct file %s", an, testpath);
        return jv_l(jstr(testpath));
    }
    const char *apk = getenv("UNITY_APK") ? getenv("UNITY_APK") : "game.apk";
    so_log("[assetpack] AssetLocation.path() fallback -> %s", apk);
    return jv_l(jstr(apk));
}

FN(al_offset) {
    init_assetpack_dir();
    char testpath[700];
    const char *an = (self && self->name) ? self->name : "";
    const char *short_name = strrchr(an, '/');
    short_name = short_name ? short_name + 1 : an;
    snprintf(testpath, sizeof testpath, "%s/%s", g_assetpack_dir, short_name);
    struct stat st;
    if (stat(testpath, &st) == 0) {
        so_log("[assetpack] AssetLocation.offset(%s) -> 0 (direct file)", an);
        return jv_j(0);
    }
    int64_t off = 3548; /* default to bgm_bundle */
    if (strstr(an, "volta")) off = 176840672LL;
    so_log("[assetpack] AssetLocation.offset(%s) -> %lld", an, (long long)off);
    return jv_j(off);
}

FN(al_size) {
    init_assetpack_dir();
    char testpath[700];
    const char *an = (self && self->name) ? self->name : "";
    const char *short_name = strrchr(an, '/');
    short_name = short_name ? short_name + 1 : an;
    snprintf(testpath, sizeof testpath, "%s/%s", g_assetpack_dir, short_name);
    struct stat st;
    if (stat(testpath, &st) == 0) {
        so_log("[assetpack] AssetLocation.size(%s) -> %lld (direct file)", an, (long long)st.st_size);
        return jv_j((int64_t)st.st_size);
    }
    int64_t sz = 176837062LL; /* default to bgm_bundle */
    if (strstr(an, "volta")) sz = 5024731LL;
    so_log("[assetpack] AssetLocation.size(%s) -> %lld", an, (long long)sz);
    return jv_j(sz);
}

FN(apm_get_pack_states) {
    so_log("[assetpack] getPackStates() -> completed Task");
    fobj *task = jnew("com/google/android/play/core/tasks/Task");
    task->user = jnew("com/google/android/play/core/assetpacks/AssetPackStates");
    return jv_l(task);
}

static void dispatch_proxy_call(fobj *proxy, const char *method, fobj *arg) {
    if (!proxy || !proxy->ival) return;
    typedef fobj *(*proxy_fn)(void *, void *, int, fobj *, fobj *);
    static proxy_fn s_invoke;
    if (!s_invoke) {
        s_invoke = (proxy_fn)fakejni_find_native("com/unity3d/player/ReflectionHelper", "nativeProxyInvoke");
    }
    if (!s_invoke) {
        so_log("[proxy] ReflectionHelper.nativeProxyInvoke not found");
        return;
    }
    void *env = fakejni_env();
    fobj *mname = jstr(method);
    fobj *args = jarray('L', arg ? 1 : 0);
    if (arg) ((fobj **)args->data)[0] = arg;
    so_log("[proxy] calling %s on proxy id=%d", method, (int)proxy->ival);
    s_invoke(env, jclass("com/unity3d/player/ReflectionHelper"), (int)proxy->ival, mname, args);
}

static fobj *g_assetpack_listener = NULL;

static void notify_pack_state(const char *name) {
    if (!g_assetpack_listener) return;
    fobj *st = jnew("com/google/android/play/core/assetpacks/AssetPackState");
    st->name = strdup(name);
    dispatch_proxy_call(g_assetpack_listener, "onStateUpdate", st);
}

static void notify_all_packs_completed(void) {
    notify_pack_state("bgm_bundle");
    notify_pack_state("volta_bundle");
}

FN(apm_fetch) {
    so_log("[assetpack] fetch() -> completed Task");
    notify_all_packs_completed();
    fobj *task = jnew("com/google/android/play/core/tasks/Task");
    task->user = jnew("com/google/android/play/core/assetpacks/AssetPackStates");
    return jv_l(task);
}

FN(task_is_complete) { return jv_z(1); }
FN(task_is_successful) { return jv_z(1); }
FN(task_get_result) { return jv_l(self ? self->user : NULL); }

FN(task_add_success) {
    fobj *listener = (fobj *)a[0].l;
    if (a[1].l) listener = (fobj *)a[1].l;
    so_log("[assetpack] Task.addOnSuccessListener listener=%p", listener);
    if (listener) {
        dispatch_proxy_call(listener, "onSuccess", self ? self->user : NULL);
    }
    return jv_l(self);
}

FN(task_add_complete) {
    fobj *listener = (fobj *)a[0].l;
    if (a[1].l) listener = (fobj *)a[1].l;
    so_log("[assetpack] Task.addOnCompleteListener listener=%p", listener);
    if (listener) {
        dispatch_proxy_call(listener, "onComplete", self);
    }
    return jv_l(self);
}

FN(apm_register_listener) {
    fobj *listener = (fobj *)a[0].l;
    so_log("[assetpack] AssetPackManager.registerListener listener=%p", listener);
    if (listener) {
        g_assetpack_listener = listener;
        notify_all_packs_completed();
    }
    return JV_VOID;
}

FN(apm_task_ret) {
    fobj *task = jnew("com/google/android/play/core/tasks/Task");
    task->user = jnew("com/google/android/play/core/assetpacks/AssetPackStates");
    return jv_l(task);
}

FN(map_get_entry) {
    fobj *k = (fobj *)a[0].l;
    const char *name = jstr_utf8(k);
    if (!name || !name[0]) name = "bgm_bundle";
    fobj *st = jnew("com/google/android/play/core/assetpacks/AssetPackState");
    st->name = strdup(name);
    return jv_l(st);
}
FN(map_contains_key) { return jv_z(1); }

FN(list_init) { return JV_VOID; }
FN(list_add) { return jv_z(1); }
FN(list_size) {
    if (self && self->name && !strcmp(self->name, "motion_ranges")) {
        return jv_i(8);
    }
    return jv_i(2);
}
FN(list_get) {
    int idx = a[0].i;
    if (self && self->name && !strcmp(self->name, "motion_ranges")) {
        static const int s_axes[8] = {0, 1, 11, 14, 15, 16, 17, 18};
        if (idx >= 0 && idx < 8) {
            fobj *mr = jnew("android/view/InputDevice$MotionRange");
            mr->ival = s_axes[idx];
            return jv_l(mr);
        }
        return jv_l(0);
    }
    const char *name = (idx == 0) ? "bgm_bundle" : "volta_bundle";
    return jv_l(jstr(name));
}

FN(aps_pack_states) {
    fobj *map = jnew("java/util/HashMap");
    return jv_l(map);
}
FN(aps_total_bytes) { return jv_j(181861793LL); }

FN(state_name) { return jv_l(jstr(self && self->name ? self->name : "bgm_bundle")); }
FN(state_status) { return jv_i(4); } /* COMPLETED */
FN(state_error_code) { return jv_i(0); }
FN(state_bytes_downloaded) {
    int64_t sz = 176837062LL;
    if (self && self->name && strstr(self->name, "volta")) sz = 5024731LL;
    return jv_j(sz);
}
FN(state_total_bytes) {
    int64_t sz = 176837062LL;
    if (self && self->name && strstr(self->name, "volta")) sz = 5024731LL;
    return jv_j(sz);
}
FN(state_pct) { return jv_i(100); }

/* ========================================================================== */
/* Android Input Events (KeyEvent / MotionEvent)                              */
/* ========================================================================== */

typedef struct {
    int is_motion;
    int action;
    int keycode;
    int source;
    int device_id;
    float x;
    float y;
    float axes[32];
    int64_t time_ms;
} android_input_event_t;

FN(ke_get_action) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_i(ev ? ev->action : 0);
}
FN(ke_get_keycode) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_i(ev ? ev->keycode : 0);
}
FN(ke_get_repeat) { return jv_i(0); }
FN(ke_get_scancode) { return jv_i(0); }
FN(ke_get_time) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_j(ev ? ev->time_ms : 0);
}
FN(ie_get_source) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && ev->source) return jv_i(ev->source);
    if (ev && ev->is_motion) return jv_i(0x1002); /* SOURCE_TOUCHSCREEN */
    return jv_i(0x00000401 | 0x00000201); /* SOURCE_GAMEPAD | SOURCE_DPAD */
}
FN(ke_get_device_id) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && ev->device_id) return jv_i(ev->device_id);
    if (ev && ev->is_motion) return jv_i(0); /* touchscreen */
    return jv_i(1); /* Microsoft X-Box 360 pad */
}
FN(ie_get_device) {
    fobj *dev = jnew("android/view/InputDevice");
    dev->ival = 1;
    return jv_l(dev);
}
FN(ie_is_from_source) {
    int src = a[0].i;
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    int ev_src = 0x00000401 | 0x00000201; /* SOURCE_GAMEPAD | SOURCE_DPAD */
    if (ev && ev->source) ev_src = ev->source;
    else if (ev && ev->is_motion) ev_src = 0x1002;
    return jv_z((ev_src & src) == src);
}
FN(ke_is_gamepad_button) {
    int kc = a[0].i;
    return jv_z((kc >= 96 && kc <= 110) || (kc >= 188 && kc <= 203) || (kc >= 19 && kc <= 22));
}
FN(ke_get_chars) { return jv_l(jstr("")); }
FN(ke_get_unicode) { return jv_i(0); }
FN(ke_get_metastate) { return jv_i(0); }

/* ---- KeyCharacterMap ---- */
FN(kcm_load) {
    return jv_l(jnew("android/view/KeyCharacterMap"));
}
FN(kcm_get) {
    int kc = a[0].i;
    if (kc >= 29 && kc <= 54) return jv_i('a' + (kc - 29));
    if (kc >= 7 && kc <= 16) return jv_i('0' + (kc - 7));
    if (kc == 62) return jv_i(' ');
    if (kc == 66) return jv_i('\n');
    return jv_i(0);
}

/* ---- InputDevice$MotionRange ---- */
FN(mr_get_axis) { return jv_i(self ? (int)self->ival : 0); }
FN(mr_get_source) { return jv_i(0x01000010 | 0x00000401); /* JOYSTICK | GAMEPAD */ }
FN(mr_get_min) {
    int ax = self ? (int)self->ival : 0;
    return jv_f((ax == 17 || ax == 18) ? 0.0f : -1.0f);
}
FN(mr_get_max) { return jv_f(1.0f); }
FN(mr_get_flat) { return jv_f(0.0f); }
FN(mr_get_fuzz) { return jv_f(0.0f); }
FN(mr_get_range) {
    int ax = self ? (int)self->ival : 0;
    return jv_f((ax == 17 || ax == 18) ? 1.0f : 2.0f);
}

/* ---- InputDevice ---- */
FN(id_get_device) {
    fobj *dev = jnew("android/view/InputDevice");
    dev->ival = a[0].i ? a[0].i : 1;
    return jv_l(dev);
}
FN(id_get_name) { return jv_l(jstr("Microsoft X-Box 360 pad")); }
FN(id_get_descriptor) { return jv_l(jstr("9e048e02000000000000000000000000")); }
FN(id_get_id) { return jv_i(self && self->ival ? (int)self->ival : 1); }
FN(id_get_sources) { return jv_i(0x01000010 | 0x00000401 | 0x00000201); }
FN(id_get_keyboard_type) { return jv_i(0); /* KEYBOARD_TYPE_NONE */ }
FN(id_is_virtual) { return jv_z(0); }
FN(id_get_motion_range) {
    int ax = a[0].i;
    if (ax == 0 || ax == 1 || ax == 11 || ax == 14 || ax == 15 || ax == 16 || ax == 17 || ax == 18) {
        fobj *mr = jnew("android/view/InputDevice$MotionRange");
        mr->ival = ax;
        return jv_l(mr);
    }
    return jv_l(0);
}
FN(id_get_motion_ranges) {
    static fobj *list;                      /* cached: see inputdev_ids - no GC here */
    if (!list) {
        list = jnew("java/util/ArrayList");
        list->name = "motion_ranges";
    }
    return jv_l(list);
}
FN(id_get_vendor_id) { return jv_i(0x045e); /* Microsoft */ }
FN(id_get_product_id) { return jv_i(0x028e); /* Xbox 360 */ }
FN(id_get_controller_number) { return jv_i(1); }
FN(id_supports_source) {
    int src = a[0].i;
    int dev_src = 0x01000010 | 0x00000401 | 0x00000201; /* JOYSTICK | GAMEPAD | DPAD */
    return jv_z((dev_src & src) == src);
}
FN(id_is_from_source) {
    int src = a[0].i;
    int dev_src = 0x01000010 | 0x00000401 | 0x00000201; /* JOYSTICK | GAMEPAD | DPAD */
    return jv_z((dev_src & src) == src);
}
FN(id_get_device_ids) {
    static fobj *arr;                       /* cached: see inputdev_ids - no GC here */
    if (!arr) {
        arr = jarray('I', 1);
        if (arr && arr->data) ((int32_t *)arr->data)[0] = 1;
    }
    return jv_l(arr);
}
FN(id_haskeys) {
    fobj *keys = (fobj *)a[0].l;
    int len = (keys && keys->kind == K_ARRAY) ? keys->len : 0;
    fobj *arr = jarray('Z', len);
    if (arr && arr->data && len > 0) memset(arr->data, 1, len);
    return jv_l(arr);
}

FN(me_get_action) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_i(ev ? ev->action : 0);
}
FN(me_get_action_masked) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_i(ev ? (ev->action & 0xff) : 0);
}
FN(me_get_action_index) { return jv_i(0); }
FN(me_get_x) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_f(ev ? ev->x : 0.0f);
}
FN(me_get_y) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_f(ev ? ev->y : 0.0f);
}
FN(me_get_pointer_count) { return jv_i(1); }
FN(me_get_pointer_id) { return jv_i(0); }
FN(me_get_time) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    return jv_j(ev ? ev->time_ms : 0);
}
FN(me_get_source) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && ev->source) return jv_i(ev->source);
    return jv_i(0x1002); /* TOUCHSCREEN */
}
FN(me_get_device_id) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && ev->device_id) return jv_i(ev->device_id);
    return jv_i(0);
}
FN(me_get_button_state) { return jv_i(0); }
FN(me_get_axis) {
    int axis = a[0].i;
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && axis >= 0 && axis < 32) return jv_f(ev->axes[axis]);
    return jv_f(0.0f);
}
FN(pc_get_axis) {
    int axis = a[0].i;
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && axis >= 0 && axis < 32) return jv_f(ev->axes[axis]);
    return jv_f(0.0f);
}
FN(me_get_pressure) { return jv_f(1.0f); }
FN(me_get_size) { return jv_f(1.0f); }
FN(me_get_history_size) { return jv_i(0); }
FN(me_get_tool_type) { return jv_i(1); /* TOOL_TYPE_FINGER */ }
static jvalue pc_get_x(fobj *self) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && (ev->source & 0x01000000)) return jv_f(ev->axes[0]);
    return jv_f(ev ? ev->x : 0.0f);
}
static jvalue pc_get_y(fobj *self) {
    android_input_event_t *ev = self ? (android_input_event_t *)self->user : NULL;
    if (ev && (ev->source & 0x01000000)) return jv_f(ev->axes[1]);
    return jv_f(ev ? ev->y : 0.0f);
}
static jvalue pc_get_pressure(fobj *self) { return jv_f(1.0f); }
static jvalue pc_get_size(fobj *self) { return jv_f(1.0f); }
static jvalue pp_get_id(fobj *self) { return jv_i((int)(self ? self->ival : 0)); }
static jvalue pp_get_tool_type(fobj *self) { return jv_i(1); }

FN(me_get_pointer_coords) {
    fobj *out = (fobj *)a[1].l;
    if (out) {
        out->user = self ? self->user : NULL;
        out->ival = a[0].i;
    }
    return JV_VOID;
}
FN(me_get_pointer_properties) {
    fobj *out = (fobj *)a[1].l;
    if (out) {
        out->user = self ? self->user : NULL;
        out->ival = a[0].i;
    }
    return JV_VOID;
}
static android_input_event_t s_input_pool[128];
static uint32_t s_input_pool_head = 0;

static android_input_event_t *alloc_input_event(void) {
    android_input_event_t *ev = &s_input_pool[(s_input_pool_head++) % 128];
    memset(ev, 0, sizeof(*ev));
    return ev;
}

FN(me_obtain) {
    fobj *src = (fobj *)a[0].l;
    fobj *clone = jnew("android/view/MotionEvent");
    if (src && src->user) {
        android_input_event_t *data = alloc_input_event();
        memcpy(data, src->user, sizeof(android_input_event_t));
        clone->user = data;
    }
    return jv_l(clone);
}
FN(me_recycle) {
    /* No-op with static ring pool: avoids use-after-free and memory fragmentation */
    return JV_VOID;
}

extern fobj *g_unity_player;
static float s_gamepad_axes[32];

void android_inject_key(int keycode, int action) {
    typedef int (*inject_fn)(void *, void *, fobj *);
    static inject_fn s_inject;
    if (!s_inject) {
        s_inject = (inject_fn)fakejni_find_native("com/unity3d/player/UnityPlayer", "nativeInjectEvent");
    }
    void *player = g_unity_player ? g_unity_player : g_activity;
    if (!s_inject || !player) return;

    fobj *ev = jnew("android/view/KeyEvent");
    android_input_event_t *data = alloc_input_event();
    data->is_motion = 0;
    data->action = action;
    data->keycode = keycode;
    data->source = 0x00000401 | 0x00000201; /* SOURCE_GAMEPAD | SOURCE_DPAD */
    data->device_id = 1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    data->time_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    ev->user = data;
    s_inject(fakejni_env(), player, ev);
}

void android_inject_touch(float x, float y, int action) {
    typedef int (*inject_fn)(void *, void *, fobj *);
    static inject_fn s_inject;
    if (!s_inject) {
        s_inject = (inject_fn)fakejni_find_native("com/unity3d/player/UnityPlayer", "nativeInjectEvent");
    }
    void *player = g_unity_player ? g_unity_player : g_activity;
    if (!s_inject || !player) return;

    fobj *ev = jnew("android/view/MotionEvent");
    android_input_event_t *data = alloc_input_event();
    data->is_motion = 1;
    data->action = action;
    data->source = 0x1002; /* TOUCHSCREEN */
    data->device_id = 0;
    data->x = x;
    data->y = y;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    data->time_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    ev->user = data;
    int r = s_inject(fakejni_env(), player, ev);
    if (so_verbose) so_log("[input] inject_touch(x=%.1f, y=%.1f, act=%d) -> %d", x, y, action, r);
}

void android_inject_axes(int count, const int *axes, const float *vals) {
    typedef int (*inject_fn)(void *, void *, fobj *);
    static inject_fn s_inject;
    if (!s_inject) {
        s_inject = (inject_fn)fakejni_find_native("com/unity3d/player/UnityPlayer", "nativeInjectEvent");
    }
    void *player = g_unity_player ? g_unity_player : g_activity;
    if (!s_inject || !player) return;

    for (int i = 0; i < count; i++) {
        int ax = axes[i];
        if (ax >= 0 && ax < 32) {
            s_gamepad_axes[ax] = vals[i];
        }
    }

    fobj *ev = jnew("android/view/MotionEvent");
    android_input_event_t *data = alloc_input_event();
    data->is_motion = 1;
    data->action = 2; /* ACTION_MOVE */
    data->source = 0x01000010 | 0x00000401; /* SOURCE_JOYSTICK | SOURCE_GAMEPAD */
    data->device_id = 1;
    data->x = s_gamepad_axes[0];
    data->y = s_gamepad_axes[1];
    memcpy(data->axes, s_gamepad_axes, sizeof(data->axes));
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    data->time_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    ev->user = data;
    int r = s_inject(fakejni_env(), player, ev);
    if (so_verbose) so_log("[input] inject_axes(count=%d) -> %d", count, r);
}

void android_inject_axis(int axis, float val) {
    int ax = axis;
    float v = val;
    android_inject_axes(1, &ax, &v);
}

static const mdef methods5[] = {
    {"android/view/KeyEvent.getAction()I", ke_get_action},
    {"android/view/KeyEvent.getKeyCode()I", ke_get_keycode},
    {"android/view/KeyEvent.getRepeatCount()I", ke_get_repeat},
    {"android/view/KeyEvent.getScanCode()I", ke_get_scancode},
    {"android/view/KeyEvent.getEventTime()J", ke_get_time},
    {"android/view/KeyEvent.getDownTime()J", ke_get_time},
    {"android/view/KeyEvent.getSource()I", ie_get_source},
    {"android/view/KeyEvent.getDeviceId()I", ke_get_device_id},
    {"android/view/KeyEvent.getCharacters()Ljava/lang/String;", ke_get_chars},
    {"android/view/KeyEvent.getUnicodeChar()I", ke_get_unicode},
    {"android/view/KeyEvent.getMetaState()I", ke_get_metastate},
    {"android/view/KeyEvent.getFlags()I", ret_zero},
    {"android/view/KeyEvent.isGamepadButton(I)Z", ke_is_gamepad_button},
    {"android/view/KeyEvent.isModifierKey(I)Z", ret_false},
    {"android/view/KeyEvent.isModifierKey()Z", ret_false},
    {"android/view/KeyEvent.isPrintingKey()Z", ret_false},
    {"android/view/KeyEvent.isSystem()Z", ret_false},
    {"android/view/KeyEvent.isFromSource(I)Z", ie_is_from_source},
    {"android/view/KeyEvent.recycle()V", me_recycle},

    {"android/view/InputEvent.getSource()I", ie_get_source},
    {"android/view/InputEvent.getDeviceId()I", ke_get_device_id},
    {"android/view/InputEvent.getEventTime()J", ke_get_time},
    {"android/view/InputEvent.getDevice()Landroid/view/InputDevice;", ie_get_device},
    {"android/view/InputEvent.isFromSource(I)Z", ie_is_from_source},
    {"android/view/InputEvent.getKeyCode()I", ret_zero},
    {"android/view/InputEvent.getCharacters()Ljava/lang/String;", ke_get_chars},
    {"android/view/KeyEvent.getDevice()Landroid/view/InputDevice;", ie_get_device},
    {"android/view/MotionEvent.getKeyCode()I", ret_zero},
    {"android/view/MotionEvent.getCharacters()Ljava/lang/String;", ke_get_chars},

    {"android/hardware/input/InputManager.getInputDevice(I)Landroid/view/InputDevice;", id_get_device},
    {"android/hardware/input/InputManager.getInputDeviceIds()[I", id_get_device_ids},
    {"android/hardware/input/InputManager.registerInputDeviceListener(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V", ret_void},
    {"android/hardware/input/InputManager.unregisterInputDeviceListener(Landroid/hardware/input/InputManager$InputDeviceListener;)V", ret_void},

    {"android/view/KeyCharacterMap.load(I)Landroid/view/KeyCharacterMap;", kcm_load},
    {"android/view/KeyCharacterMap.get(II)I", kcm_get},

    {"android/view/InputDevice.getDevice(I)Landroid/view/InputDevice;", id_get_device},
    {"android/view/InputDevice.getName()Ljava/lang/String;", id_get_name},
    {"android/view/InputDevice.getDescriptor()Ljava/lang/String;", id_get_descriptor},
    {"android/view/InputDevice.getId()I", id_get_id},
    {"android/view/InputDevice.getSources()I", id_get_sources},
    {"android/view/InputDevice.getKeyboardType()I", id_get_keyboard_type},
    {"android/view/InputDevice.isVirtual()Z", id_is_virtual},
    {"android/view/InputDevice.isExternal()Z", ret_true},
    {"android/view/InputDevice.hasKeys([I)[Z", id_haskeys},
    {"android/view/InputDevice.getMotionRange(I)Landroid/view/InputDevice$MotionRange;", id_get_motion_range},
    {"android/view/InputDevice.getMotionRange(II)Landroid/view/InputDevice$MotionRange;", id_get_motion_range},
    {"android/view/InputDevice.getMotionRanges()Ljava/util/List;", id_get_motion_ranges},
    {"android/view/InputDevice.getVendorId()I", id_get_vendor_id},
    {"android/view/InputDevice.getProductId()I", id_get_product_id},
    {"android/view/InputDevice.getControllerNumber()I", id_get_controller_number},
    {"android/view/InputDevice.supportsSource(I)Z", id_supports_source},
    {"android/view/InputDevice.isFromSource(I)Z", id_is_from_source},
    {"android/view/InputDevice.getDeviceIds()[I", id_get_device_ids},
    {"android/view/InputDevice.getVibrator()Landroid/os/Vibrator;", ret_null},
    {"android/view/InputDevice.hasMicrophone()Z", ret_false},

    {"android/view/View$OnSystemUiVisibilityChangeListener.run()V", ret_void},

    {"android/view/MotionEvent.obtain(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", me_obtain},
    {"android/view/MotionEvent.obtainNoHistory(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", me_obtain},
    {"android/view/MotionEvent.recycle()V", me_recycle},
    {"android/view/MotionEvent.getAction()I", me_get_action},
    {"android/view/MotionEvent.getActionMasked()I", me_get_action_masked},
    {"android/view/MotionEvent.getActionIndex()I", me_get_action_index},
    {"android/view/MotionEvent.getX()F", me_get_x},
    {"android/view/MotionEvent.getY()F", me_get_y},
    {"android/view/MotionEvent.getX(I)F", me_get_x},
    {"android/view/MotionEvent.getY(I)F", me_get_y},
    {"android/view/MotionEvent.getPointerCount()I", me_get_pointer_count},
    {"android/view/MotionEvent.getPointerId(I)I", me_get_pointer_id},
    {"android/view/MotionEvent.getEventTime()J", me_get_time},
    {"android/view/MotionEvent.getDownTime()J", me_get_time},
    {"android/view/MotionEvent.getSource()I", me_get_source},
    {"android/view/MotionEvent.getDeviceId()I", me_get_device_id},
    {"android/view/MotionEvent.getButtonState()I", me_get_button_state},
    {"android/view/MotionEvent.getAxisValue(I)F", me_get_axis},
    {"android/view/MotionEvent.getAxisValue(II)F", me_get_axis},
    {"android/view/MotionEvent.getPressure()F", me_get_pressure},
    {"android/view/MotionEvent.getPressure(I)F", me_get_pressure},
    {"android/view/MotionEvent.getSize()F", me_get_size},
    {"android/view/MotionEvent.getSize(I)F", me_get_size},
    {"android/view/MotionEvent.getHistorySize()I", me_get_history_size},
    {"android/view/MotionEvent.getToolType(I)I", me_get_tool_type},
    {"android/view/MotionEvent.getPointerCoords(ILandroid/view/MotionEvent$PointerCoords;)V", me_get_pointer_coords},
    {"android/view/MotionEvent.getPointerProperties(ILandroid/view/MotionEvent$PointerProperties;)V", me_get_pointer_properties},
    {"android/view/MotionEvent$PointerCoords.getAxisValue(I)F", pc_get_axis},
    {"android/view/InputDevice$MotionRange.getAxis()I", mr_get_axis},
    {"android/view/InputDevice$MotionRange.getSource()I", mr_get_source},
    {"android/view/InputDevice$MotionRange.getMin()F", mr_get_min},
    {"android/view/InputDevice$MotionRange.getMax()F", mr_get_max},
    {"android/view/InputDevice$MotionRange.getFlat()F", mr_get_flat},
    {"android/view/InputDevice$MotionRange.getFuzz()F", mr_get_fuzz},
    {"android/view/InputDevice$MotionRange.getRange()F", mr_get_range},
    {"android/os/PowerManager.isSustainedPerformanceModeSupported()Z", power_sustained},
    {"android/os/Vibrator.hasVibrator()Z", ret_false},
    {"android/content/pm/PackageManager.getInstallerPackageName(Ljava/lang/String;)Ljava/lang/String;", pm_installer},
    {"com/unity3d/player/ReflectionHelper.getMethodID(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Method;", refl_get_method},
    {"com/unity3d/player/ReflectionHelper.getConstructorID(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Constructor;", refl_get_constructor},
    {"com/unity3d/player/ReflectionHelper.getFieldID(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;Z)Ljava/lang/reflect/Field;", refl_get_field},
    {"com/unity3d/player/ReflectionHelper.newProxyInstance(ILjava/lang/Class;)Ljava/lang/Object;", refl_new_proxy},
    {"com/unity3d/player/ReflectionHelper.endUnityLaunch()V", ret_void},
    {"android/content/Context.getExternalCacheDir()Ljava/io/File;", ctx_cachedir},
    {"android/media/MediaRouter.getSelectedRoute(I)Landroid/media/MediaRouter$RouteInfo;", mr_get_route},
    {"android/media/MediaRouter$RouteInfo.getName(Landroid/content/Context;)Ljava/lang/CharSequence;", route_get_name},
    {"java/lang/Boolean.<init>(Z)V", bool_init},
    {"java/lang/Boolean.booleanValue()Z", bool_val},
    {"java/util/HashMap.<init>()V", map_init},
    {"java/util/HashMap.<init>(I)V", map_init},
    {"java/util/HashMap.put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", map_put},
    {"java/util/HashMap.get(Ljava/lang/Object;)Ljava/lang/Object;", map_get_entry},
    {"java/util/HashMap.containsKey(Ljava/lang/Object;)Z", map_contains_key},
    {"java/util/Map.put(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", map_put},
    {"java/util/Map.get(Ljava/lang/Object;)Ljava/lang/Object;", map_get_entry},
    {"java/util/Map.containsKey(Ljava/lang/Object;)Z", map_contains_key},
    {"java/util/ArrayList.<init>()V", list_init},
    {"java/util/ArrayList.<init>(I)V", list_init},
    {"java/util/ArrayList.add(Ljava/lang/Object;)Z", list_add},
    {"java/util/ArrayList.size()I", list_size},
    {"java/util/ArrayList.get(I)Ljava/lang/Object;", list_get},
    {"java/util/List.add(Ljava/lang/Object;)Z", list_add},
    {"java/util/List.size()I", list_size},
    {"java/util/List.get(I)Ljava/lang/Object;", list_get},
    {"com/unity3d/player/UnityWebRequest.<init>(JLjava/lang/String;Ljava/util/Map;Ljava/lang/String;ZI)V", ret_void},
    {"com/unity3d/player/UnityWebRequest.setupTransferSettings(JZZ)V", ret_void},
    {"com/unity3d/player/UnityWebRequest.run()V", ret_void},
    {"com/unity3d/player/UnityWebRequest.clearCookieCache(Ljava/lang/String;Ljava/lang/String;)V", ret_void},
    {"android/view/View$OnSystemUiVisibilityChangeListener.onSystemUiVisibilityChange(I)V", ret_void},
    {"android/os/Handler$Callback.handleMessage(Landroid/os/Message;)Z", ret_false},
    {"java/lang/Runnable.run()V", ret_void},
    {"java/lang/Class.forName(Ljava/lang/String;)Ljava/lang/Class;", class_forname},
    {"java/lang/Class.forName(Ljava/lang/String;)Ljava/lang/Object;", class_forname},
    {"java/lang/Object.getName()Ljava/lang/String;", obj_getname},
    {"java/lang/Class.getName()Ljava/lang/String;", obj_getname},
    {"java/lang/System.identityHashCode(Ljava/lang/Object;)I", sys_hashcode},
    {"java/lang/Object.isArray()Z", cls_isarray},
    {"java/lang/Class.isArray()Z", cls_isarray},
    {"java/lang/String.toString()Ljava/lang/String;", str_tostring},
    {"java/lang/Object.toString()Ljava/lang/String;", str_tostring},
    {"java/lang/Object.hashCode()I", obj_hashcode},
    {"java/lang/Object.equals(Ljava/lang/Object;)Z", obj_equals},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Landroid/content/Context;)Lcom/google/android/play/core/assetpacks/AssetPackManager;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Landroid/content/Context;)Ljava/lang/Object;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Lcom/unity3d/player/UnityPlayerActivity;)Lcom/google/android/play/core/assetpacks/AssetPackManager;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Lcom/unity3d/player/UnityPlayerActivity;)Ljava/lang/Object;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Landroid/app/Activity;)Lcom/google/android/play/core/assetpacks/AssetPackManager;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Landroid/app/Activity;)Ljava/lang/Object;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManagerFactory.getInstance(Ljava/lang/Object;)Ljava/lang/Object;", apm_get_instance},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getPackLocation(Ljava/lang/String;)Lcom/google/android/play/core/assetpacks/AssetPackLocation;", apm_get_pack_location},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getPackLocation(Ljava/lang/String;)Ljava/lang/Object;", apm_get_pack_location},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getPackLocation(Ljava/lang/Object;)Ljava/lang/Object;", apm_get_pack_location},
    {"com/google/android/play/core/assetpacks/AssetPackLocation.packStorageMethod()I", apl_storage_method},
    {"com/google/android/play/core/assetpacks/AssetPackLocation.assetsPath()Ljava/lang/String;", apl_assets_path},
    {"com/google/android/play/core/assetpacks/AssetPackLocation.assetsPath()Ljava/lang/Object;", apl_assets_path},
    {"com/google/android/play/core/assetpacks/AssetPackLocation.path()Ljava/lang/String;", apl_assets_path},
    {"com/google/android/play/core/assetpacks/AssetPackLocation.path()Ljava/lang/Object;", apl_assets_path},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getAssetLocation(Ljava/lang/String;Ljava/lang/String;)Lcom/google/android/play/core/assetpacks/AssetLocation;", apm_get_asset_location},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getAssetLocation(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;", apm_get_asset_location},
    {"com/google/android/play/core/assetpacks/AssetLocation.path()Ljava/lang/String;", al_path},
    {"com/google/android/play/core/assetpacks/AssetLocation.offset()J", al_offset},
    {"com/google/android/play/core/assetpacks/AssetLocation.size()J", al_size},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getPackStates(Ljava/util/List;)Lcom/google/android/play/core/tasks/Task;", apm_get_pack_states},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getPackStates(Ljava/util/List;)Ljava/lang/Object;", apm_get_pack_states},
    {"com/google/android/play/core/assetpacks/AssetPackManager.getPackStates(Ljava/lang/Object;)Ljava/lang/Object;", apm_get_pack_states},
    {"com/google/android/play/core/assetpacks/AssetPackManager.fetch(Ljava/util/List;)Lcom/google/android/play/core/tasks/Task;", apm_fetch},
    {"com/google/android/play/core/assetpacks/AssetPackManager.fetch(Ljava/util/List;)Ljava/lang/Object;", apm_fetch},
    {"com/google/android/play/core/assetpacks/AssetPackManager.fetch(Ljava/lang/Object;)Ljava/lang/Object;", apm_fetch},
    {"com/google/android/play/core/assetpacks/AssetPackManager.cancel(Ljava/util/List;)Lcom/google/android/play/core/tasks/Task;", apm_task_ret},
    {"com/google/android/play/core/assetpacks/AssetPackManager.cancel(Ljava/lang/Object;)Ljava/lang/Object;", apm_task_ret},
    {"com/google/android/play/core/assetpacks/AssetPackManager.removePack(Ljava/lang/String;)Lcom/google/android/play/core/tasks/Task;", apm_task_ret},
    {"com/google/android/play/core/assetpacks/AssetPackManager.removePack(Ljava/lang/Object;)Ljava/lang/Object;", apm_task_ret},
    {"com/google/android/play/core/assetpacks/AssetPackManager.showCellularDataConfirmation(Landroid/app/Activity;)Lcom/google/android/play/core/tasks/Task;", apm_task_ret},
    {"com/google/android/play/core/assetpacks/AssetPackManager.showCellularDataConfirmation(Ljava/lang/Object;)Ljava/lang/Object;", apm_task_ret},
    {"com/google/android/play/core/assetpacks/AssetPackManager.registerListener(Lcom/google/android/play/core/assetpacks/AssetPackStateUpdateListener;)V", apm_register_listener},
    {"com/google/android/play/core/assetpacks/AssetPackManager.registerListener(Ljava/lang/Object;)V", apm_register_listener},
    {"com/google/android/play/core/assetpacks/AssetPackManager.unregisterListener(Lcom/google/android/play/core/assetpacks/AssetPackStateUpdateListener;)V", ret_void},
    {"com/google/android/play/core/assetpacks/AssetPackManager.unregisterListener(Ljava/lang/Object;)V", ret_void},
    {"com/google/android/play/core/assetpacks/AssetPackManager.clearListeners()V", ret_void},
    {"com/google/android/play/core/tasks/Task.isComplete()Z", task_is_complete},
    {"com/google/android/play/core/tasks/Task.isSuccessful()Z", task_is_successful},
    {"com/google/android/play/core/tasks/Task.getResult()Ljava/lang/Object;", task_get_result},
    {"com/google/android/play/core/tasks/Task.getResult(Ljava/lang/Class;)Ljava/lang/Object;", task_get_result},
    {"com/google/android/play/core/tasks/Task.addOnSuccessListener(Lcom/google/android/play/core/tasks/OnSuccessListener;)Lcom/google/android/play/core/tasks/Task;", task_add_success},
    {"com/google/android/play/core/tasks/Task.addOnSuccessListener(Lcom/google/android/play/core/tasks/OnSuccessListener;)Ljava/lang/Object;", task_add_success},
    {"com/google/android/play/core/tasks/Task.addOnSuccessListener(Ljava/util/concurrent/Executor;Lcom/google/android/play/core/tasks/OnSuccessListener;)Lcom/google/android/play/core/tasks/Task;", task_add_success},
    {"com/google/android/play/core/tasks/Task.addOnSuccessListener(Ljava/util/concurrent/Executor;Lcom/google/android/play/core/tasks/OnSuccessListener;)Ljava/lang/Object;", task_add_success},
    {"com/google/android/play/core/tasks/Task.addOnSuccessListener(Ljava/lang/Object;)Ljava/lang/Object;", task_add_success},
    {"com/google/android/play/core/tasks/Task.addOnSuccessListener(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", task_add_success},
    {"com/google/android/play/core/tasks/Task.addOnCompleteListener(Lcom/google/android/play/core/tasks/OnCompleteListener;)Lcom/google/android/play/core/tasks/Task;", task_add_complete},
    {"com/google/android/play/core/tasks/Task.addOnCompleteListener(Lcom/google/android/play/core/tasks/OnCompleteListener;)Ljava/lang/Object;", task_add_complete},
    {"com/google/android/play/core/tasks/Task.addOnCompleteListener(Ljava/util/concurrent/Executor;Lcom/google/android/play/core/tasks/OnCompleteListener;)Lcom/google/android/play/core/tasks/Task;", task_add_complete},
    {"com/google/android/play/core/tasks/Task.addOnCompleteListener(Ljava/util/concurrent/Executor;Lcom/google/android/play/core/tasks/OnCompleteListener;)Ljava/lang/Object;", task_add_complete},
    {"com/google/android/play/core/tasks/Task.addOnCompleteListener(Ljava/lang/Object;)Ljava/lang/Object;", task_add_complete},
    {"com/google/android/play/core/tasks/Task.addOnCompleteListener(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", task_add_complete},
    {"com/google/android/play/core/tasks/Task.addOnFailureListener(Lcom/google/android/play/core/tasks/OnFailureListener;)Lcom/google/android/play/core/tasks/Task;", ret_self},
    {"com/google/android/play/core/tasks/Task.addOnFailureListener(Ljava/util/concurrent/Executor;Lcom/google/android/play/core/tasks/OnFailureListener;)Lcom/google/android/play/core/tasks/Task;", ret_self},
    {"com/google/android/play/core/tasks/Task.addOnFailureListener(Ljava/lang/Object;)Ljava/lang/Object;", ret_self},
    {"com/google/android/play/core/tasks/Task.addOnFailureListener(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", ret_self},
    {"com/google/android/play/core/tasks/Task.getException()Ljava/lang/Exception;", ret_null},
    {"com/google/android/play/core/assetpacks/AssetPackStates.packStates()Ljava/util/Map;", aps_pack_states},
    {"com/google/android/play/core/assetpacks/AssetPackStates.packStates()Ljava/lang/Object;", aps_pack_states},
    {"com/google/android/play/core/assetpacks/AssetPackStates.totalBytes()J", aps_total_bytes},
    {"com/google/android/play/core/assetpacks/AssetPackState.name()Ljava/lang/String;", state_name},
    {"com/google/android/play/core/assetpacks/AssetPackState.name()Ljava/lang/Object;", state_name},
    {"com/google/android/play/core/assetpacks/AssetPackState.status()I", state_status},
    {"com/google/android/play/core/assetpacks/AssetPackState.errorCode()I", state_error_code},
    {"com/google/android/play/core/assetpacks/AssetPackState.bytesDownloaded()J", state_bytes_downloaded},
    {"com/google/android/play/core/assetpacks/AssetPackState.totalBytesToDownload()J", state_total_bytes},
    {"com/google/android/play/core/assetpacks/AssetPackState.transferProgressPercentage()I", state_pct},
    {NULL, NULL}};

static const fdef fields5[] = {
    {"android/view/MotionEvent$PointerCoords.x", pc_get_x},
    {"android/view/MotionEvent$PointerCoords.y", pc_get_y},
    {"android/view/MotionEvent$PointerCoords.pressure", pc_get_pressure},
    {"android/view/MotionEvent$PointerCoords.size", pc_get_size},
    {"android/view/MotionEvent$PointerProperties.id", pp_get_id},
    {"android/view/MotionEvent$PointerProperties.toolType", pp_get_tool_type},
    {NULL, NULL}};

static jvalue c_vol_down(fobj *s) { (void)s; return jv_i(25); }
static jvalue c_vol_up(fobj *s) { (void)s; return jv_i(24); }
static jvalue c_zoom_in(fobj *s) { (void)s; return jv_i(168); }
static jvalue c_zoom_out(fobj *s) { (void)s; return jv_i(169); }
static jvalue c_camera(fobj *s) { (void)s; return jv_i(27); }

static const fdef sfields5[] = {
    {"com/unity3d/player/UnityPlayer.currentActivity", sf_current_activity},
    {"android/content/Context.POWER_SERVICE", c_power_service},
    {"android/content/Context.VIBRATOR_SERVICE", c_vibrator_service},
    {"android/os/Build.TAGS", c_build_tags},
    {"android/os/Build.ID", c_build_id},
    {"android/os/Build$VERSION.INCREMENTAL", c_build_incremental},
    {"android/view/KeyEvent.KEYCODE_VOLUME_DOWN", c_vol_down},
    {"android/view/KeyEvent.KEYCODE_VOLUME_UP", c_vol_up},
    {"android/view/KeyEvent.KEYCODE_ZOOM_IN", c_zoom_in},
    {"android/view/KeyEvent.KEYCODE_ZOOM_OUT", c_zoom_out},
    {"android/view/KeyEvent.KEYCODE_CAMERA", c_camera},
    {NULL, NULL}};

/* Audio / PackageManager / List / FMOD helpers */
FN(audio_getproperty) {
    const char *k = jstr_utf8((fobj *)a[0].l);
    const char *val = "512";
    if (k && (strstr(k, "OUTPUT_SAMPLE_RATE") || strstr(k, "sample_rate")))
        val = "48000";
    else if (k && (strstr(k, "OUTPUT_FRAMES_PER_BUFFER") || strstr(k, "frames_per_buffer")))
        val = "512";
    so_log("AudioManager.getProperty(%s) -> %s", k ? k : "", val);
    return jv_l(jstr(val));
}
static jvalue audio_prop_frames(fobj *s) { (void)s; return jv_l(jstr("android.media.property.OUTPUT_FRAMES_PER_BUFFER")); }
static jvalue audio_prop_rate(fobj *s) { (void)s; return jv_l(jstr("android.media.property.OUTPUT_SAMPLE_RATE")); }
static jvalue audio_get_volume(fobj *s, const jvalue *a) { (void)s; (void)a; return jv_i(15); }

FN(pm_hassystemfeature) {
    const char *feat = jstr_utf8((fobj *)a[0].l);
    so_log("PackageManager.hasSystemFeature(%s) -> 1", feat ? feat : "");
    return jv_z(1);
}

FN(list_iterator) {
    fobj *it = jnew("java/util/Iterator");
    it->ival = 999; /* empty list */
    return jv_l(it);
}

static const mdef methods6[] = {
    {"android/media/AudioManager.getProperty(Ljava/lang/String;)Ljava/lang/String;", audio_getproperty},
    {"android/media/AudioManager.getProperty(Ljava/lang/Object;)Ljava/lang/Object;", audio_getproperty},
    {"android/media/AudioManager.getStreamVolume(I)I", audio_get_volume},
    {"android/media/AudioManager.getStreamMaxVolume(I)I", audio_get_volume},
    {"android/media/AudioManager.isMusicActive()Z", ret_false},
    {"android/media/AudioManager.requestAudioFocus(Landroid/media/AudioManager$OnAudioFocusChangeListener;II)I", ret_one},
    {"android/media/AudioManager.requestAudioFocus(Ljava/lang/Object;II)I", ret_one},
    {"android/media/AudioManager.abandonAudioFocus(Landroid/media/AudioManager$OnAudioFocusChangeListener;)I", ret_one},
    {"android/media/AudioManager.abandonAudioFocus(Ljava/lang/Object;)I", ret_one},
    {"android/media/AudioManager.getMode()I", ret_zero},

    {"android/content/pm/PackageManager.hasSystemFeature(Ljava/lang/String;)Z", pm_hassystemfeature},
    {"android/content/pm/PackageManager.hasSystemFeature(Ljava/lang/Object;)Z", pm_hassystemfeature},

    {"java/util/List.iterator()Ljava/util/Iterator;", list_iterator},
    {"java/util/List.size()I", ret_zero},
    {"java/util/List.isEmpty()Z", ret_true},
    {"java/util/Iterator.hasNext()Z", ret_false},
    {"java/util/Iterator.next()Ljava/lang/Object;", ret_null},
    {"android/view/MotionEvent.getTouchMajor()F", me_get_pressure},
    {"android/view/MotionEvent.getTouchMajor(I)F", me_get_pressure},
    {"android/view/MotionEvent.getTouchMinor()F", me_get_pressure},
    {"android/view/MotionEvent.getTouchMinor(I)F", me_get_pressure},

    {"org/fmod/FMODAudioDevice.<init>()V", ret_void},
    {"org/fmod/FMODAudioDevice.startAudioRecord(III)I", ret_zero},
    {"org/fmod/FMODAudioDevice.stopAudioRecord()V", ret_void},
    {"org/fmod/FMODAudioDevice.start()V", ret_void},
    {"org/fmod/FMODAudioDevice.stop()V", ret_void},
    {"org/fmod/FMODAudioDevice.close()V", ret_void},
    {NULL, NULL}};

static const fdef sfields6[] = {
    {"android/media/AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER", audio_prop_frames},
    {"android/media/AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE", audio_prop_rate},
    {NULL, NULL}};

void androidfw_init(void) {
    looper_init();
    for (const mdef *m = methods6; m->key; m++) java_method(m->key, m->fn);
    for (const fdef *f = sfields6; f->key; f++) java_static_field(f->key, f->fn);
    for (const mdef *m = methods5; m->key; m++) java_method(m->key, m->fn);
    for (const fdef *f = fields5; f->key; f++) java_field(f->key, f->fn);
    for (const fdef *f = sfields5; f->key; f++) java_static_field(f->key, f->fn);
    for (const mdef *m = methods4; m->key; m++) java_method(m->key, m->fn);
    for (const fdef *f = sfields4; f->key; f++) java_static_field(f->key, f->fn);
    (void)ret_null; (void)ret_false; (void)ret_true; (void)ret_zero; (void)ret_void; (void)str_empty;
    (void)lib_dir;
    for (const mdef *m = methods; m->key; m++) java_method(m->key, m->fn);
    for (const mdef *m = methods3; m->key; m++) java_method(m->key, m->fn);
    for (const fdef *f = fields3; f->key; f++) java_field(f->key, f->fn);
    for (const fdef *f = sfields3; f->key; f++) java_static_field(f->key, f->fn);
    for (const mdef *m = methods2; m->key; m++) java_method(m->key, m->fn);
    for (const fdef *f = fields2; f->key; f++) java_field(f->key, f->fn);
    for (const fdef *f = sfields2; f->key; f++) java_static_field(f->key, f->fn);
    for (const fdef *f = fields; f->key; f++) java_field(f->key, f->fn);
    for (const fdef *f = sfields; f->key; f++) java_static_field(f->key, f->fn);
    g_activity = jnew("com/unity3d/player/UnityPlayerActivity");
}
