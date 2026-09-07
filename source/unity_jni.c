/* unity_jni.c -- real handlers for the Unity Java classes the engine calls
 * Style/idioms follow cr3_nx's jni_fake.c. See unity_jni.h for how it plugs in.
 *
 * NOT compile-tested (no devkitA64 here). Method names/signatures are the
 * standard Android framework API, so they're concrete; the spots that need
 * checking on hardware are marked CHECK. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>      /* File.listFiles */
#include <errno.h>

#include "jni_fake.h"   /* jni_note_approx: the approximation ledger */
#include "unity_jni.h"
#include "util.h"

/* FakeID layout we read (must match jni_fake.c). Only the char[] fields. */
struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

extern int screen_width, screen_height;   /* real panel size (config.c) */
#define SCREEN_W_HANDHELD 720   /* fbstub45 PORTRAIT (stable) */
#define SCREEN_H_HANDHELD 1280
#define SCREEN_W_DOCKED   1080
#define SCREEN_H_DOCKED   1920
#define REFRESH_HZ        60

static char g_root[192];            /* e.g. /switch/zookeeper                 */
static char g_assets[208];          /* g_root "/assets"                      */

static int  has(const char *s, const char *sub){ return strstr(s,sub)!=NULL; }
static int  ret_is(const char *sig,const char *t) __attribute__((unused));
static int  ret_is(const char *sig,const char *t){ const char*p=strchr(sig,')'); return p && strstr(p+1,t)==p+1; }

/* --------------------------------------------------------------------------
 * stateful handle objects (streams / fds / prefs) -- jni_fake's objects are
 * label-only, so we carry our own. Opaque to the engine; recovered via recv.
 * -------------------------------------------------------------------------- */
enum { UJ_TAG = 0x554a4831 /*'UJH1'*/ };
enum { UJ_INPUTSTREAM, UJ_AFD, UJ_FD, UJ_PREFS, UJ_EDITOR, UJ_GENERIC,
       UJ_MAP, UJ_SET, UJ_ITER, UJ_ENTRY, UJ_BOXED, UJ_FILE };

typedef struct {
  uint32_t tag; int kind;
  FILE *fp;                 /* InputStream                                  */
  int   fd;                 /* AFD / FD                                     */
  long  off, len;           /* AFD region                                  */
  int   idx;                /* UJ_ITER cursor / UJ_ENTRY kv index           */
  char  btype;              /* UJ_BOXED: 'I' int 'L' long 'B' bool 'F' float*/
  long long bival;          /* UJ_BOXED int/long/bool payload               */
  double bfval;             /* UJ_BOXED float payload                       */
  char  path[512];          /* UJ_FILE: the real filesystem path            */
} UHandle;

static UHandle *uh_new(int kind){
  UHandle *h = calloc(1,sizeof *h);
  h->tag = UJ_TAG; h->kind = kind; h->fd = -1; return h;
}
/* ---- java.io.File with a REAL path ----------------------------------------
 * jni_fake's File objects were label-only, and every getAbsolutePath() answered
 * the data root regardless of which File was asked. That is fine for
 * getFilesDir() and useless for anything else: Clone Hero builds
 * Environment.getExternalStoragePublicDirectory(DIRECTORY_DOCUMENTS) then
 * new File(that, "Clone Hero"), mkdirs() it, and scans it for songs. Each of
 * those needs the File to remember where it points. */
static const char *managed_root(void);   /* defined below */
static UHandle *uh_file(const char *path){
  UHandle *h = uh_new(UJ_FILE);
  snprintf(h->path, sizeof h->path, "%s", path ? path : "");
  /* strip a trailing slash so dirname/basename behave */
  size_t L = strlen(h->path);
  while (L > 1 && h->path[L-1] == '/') h->path[--L] = 0;
  return h;
}
static UHandle *uh_file_join(const char *dir, const char *leaf){
  char buf[512];
  if (!leaf || !*leaf) return uh_file(dir);
  if (leaf[0] == '/' || strchr(leaf, ':')) return uh_file(leaf);   /* absolute */
  snprintf(buf, sizeof buf, "%s/%s", (dir && *dir) ? dir : managed_root(), leaf);
  return uh_file(buf);
}
static int is_uh_file(const void *p){
  const UHandle *h = p;
  return h && ((uintptr_t)h & 3) == 0 && h->tag == UJ_TAG && h->kind == UJ_FILE;
}
/* Accept either a UJ_FILE or a plain FakeString as "a path argument". */
static const char *path_of(void *p){
  if (is_uh_file(p)) return ((UHandle *)p)->path;
  return jni_string_utf(p);
}
static const char *file_parent(const UHandle *f, char *out, size_t n){
  snprintf(out, n, "%s", f->path);
  char *s = strrchr(out, '/');
  if (!s || s == out) return NULL;
  *s = 0;
  return out;
}
static const char *file_name(const UHandle *f){
  const char *s = strrchr(f->path, '/');
  return s ? s + 1 : f->path;
}

/* What Java class is this handle? Answers GetObjectClass so Unity's
 * reflection resolves methods against the right class (File.getAbsolutePath,
 * not Object.getAbsolutePath) and so the class-gated dispatch reaches us.
 * NULL for anything that is not one of our handles. */
const char *unity_class_of(void *obj){
  const UHandle *h = obj;
  if (!h || ((uintptr_t)h & 3) || (uintptr_t)h >= 0x8000000000ull || h->tag != UJ_TAG) return NULL;
  switch (h->kind) {
    case UJ_FILE:        return "java/io/File";
    case UJ_INPUTSTREAM: return "java/io/InputStream";
    case UJ_AFD:         return "android/content/res/AssetFileDescriptor";
    case UJ_FD:          return "java/io/FileDescriptor";
    case UJ_PREFS:       return "android/content/SharedPreferences";
    case UJ_EDITOR:      return "android/content/SharedPreferences$Editor";
    case UJ_MAP:         return "java/util/HashMap";
    case UJ_SET:         return "java/util/HashSet";
    case UJ_ITER:        return "java/util/Iterator";
    case UJ_ENTRY:       return "java/util/Map$Entry";
    case UJ_BOXED:
      switch (h->btype) {
        case 'F': return "java/lang/Float";
        case 'L': return "java/lang/Long";
        case 'B': return "java/lang/Boolean";
        default:  return "java/lang/Integer";
      }
    default:             return "java/lang/Object";
  }
}
/* Is this receiver one of ours, regardless of what class the caller resolved
 * the method against? Lets jni_fake route by RECEIVER before by class. */
int unity_owns_recv(void *obj){ return unity_class_of(obj) != NULL; }

/* new File(a0[, a1]) from jni_fake's NewObject. Either argument may be one of
 * our File handles or a String; a lone relative path is anchored at the data
 * root, matching where a phone would put it relative to external storage. */
void *unity_new_file(void *a0, void *a1){
  const char *p0 = path_of(a0);
  if (a1) return uh_file_join(p0, path_of(a1));
  if (p0 && (p0[0] == '/' || strchr(p0, ':'))) return uh_file(p0);
  return uh_file_join(managed_root(), p0);
}

/* jni_fake.c free_ref() should call this for UJ_TAG; until then streams that
 * the engine forgets to close() leak. Optional hardening, see header note. */
void unity_handle_free(void *p){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG) return;
  if (h->fp) fclose(h->fp);
  if (h->fd>=0) close(h->fd);
  free(h);
}
static int is_uh(void *p,int kind){ UHandle*h=p; return h && h->tag==UJ_TAG && h->kind==kind; }

/* --------------------------------------------------------------------------
 * asset path: AssetManager.open("bin/Data/x") -> g_root/assets/bin/Data/x
 * (the engine's *direct* fopen of the data path is handled separately by the
 *  libc_shim fopen redirect + the Context path getters below.)
 * -------------------------------------------------------------------------- */
static void asset_path(char *out,size_t n,const char *name){
  while (name && (name[0]=='/' )) name++;
  snprintf(out,n,"%s/%s",g_assets,name?name:"");
}

/* Path handed to *managed* code (persistentDataPath / dataPath via the Context
 * getters below). It must be Unix-rooted ("/switch/zookeeper"): Mono/IL2CPP on
 * Android uses Unix path rules where ":" is NOT a root marker, so a "sdmc:/..."
 * path is treated as *relative* and Path.Combine() concatenates it after a
 * relative asset path -> "assets/bin/Data/sdmc:/switch/zookeeper". newlib then
 * sees the embedded "sdmc:" mid-path, mis-parses the device, and null-derefs
 * (Data Abort at the devoptab mkdir_r slot, +0x68). Stripping the device prefix
 * fixes this; newlib still resolves the device-less absolute path via the
 * default device (sdmc), set by our boot chdir, so file I/O is unaffected. Our
 * internal g_root/g_assets keep the explicit "sdmc:" prefix. */
static const char *managed_root(void){
  const char *c = strchr(g_root, ':');
  return (c && c[1] == '/') ? c + 1 : g_root;
}

/* ==========================================================================
 * SharedPreferences  (Unity PlayerPrefs == save data)  -> g_root/prefs.kv
 * flat "type\tkey\tvalue" lines; value url-ish escaped on tab/newline.
 * ========================================================================== */
typedef struct { char type; char *key; char *val; } KV;
static KV   *g_kv = NULL; static int g_kv_n=0, g_kv_cap=0; static int g_kv_dirty=0;

static char *prefs_file(char *buf,size_t n){ snprintf(buf,n,"%s/prefs.kv",g_root); return buf; }

static void kv_set(char type,const char*key,const char*val){
  for (int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)){
    g_kv[i].type=type; free(g_kv[i].val); g_kv[i].val=strdup(val); g_kv_dirty=1; return; }
  if (g_kv_n==g_kv_cap){ g_kv_cap=g_kv_cap?g_kv_cap*2:32; g_kv=realloc(g_kv,g_kv_cap*sizeof(KV)); }
  g_kv[g_kv_n].type=type; g_kv[g_kv_n].key=strdup(key); g_kv[g_kv_n].val=strdup(val);
  g_kv_n++; g_kv_dirty=1;
}
static KV *kv_get(const char*key){ for(int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)) return &g_kv[i]; return NULL; }
static void kv_remove(const char*key){
  for(int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)){
    free(g_kv[i].key);free(g_kv[i].val); g_kv[i]=g_kv[--g_kv_n]; g_kv_dirty=1; return; }
}
static void kv_clear(void){ for(int i=0;i<g_kv_n;i++){free(g_kv[i].key);free(g_kv[i].val);} g_kv_n=0; g_kv_dirty=1; }

static void esc(FILE*f,const char*s){ for(;*s;s++){ if(*s=='\\'||*s=='\t'||*s=='\n'){fputc('\\',f);
  fputc(*s=='\t'?'t':*s=='\n'?'n':'\\',f);} else fputc(*s,f);} }
static char *unesc(char*s){ char*o=s,*w=s; for(;*o;o++){ if(*o=='\\'&&o[1]){o++;
  *w++=(*o=='t')?'\t':(*o=='n')?'\n':*o;} else *w++=*o;} *w=0; return s; }

/* Seed DFU's OWN persistent-data-path override, once, if the player has not set
 * one. This is not a hack around the game -- DaggerfallUnityApplication exposes
 * exactly this key (dump.cs: customPersistentDataPathKey =
 * "DaggerfallUnity_CustomPersistentDataPath") for precisely this purpose.
 *
 * WHY IT IS NEEDED. Android's persistentDataPath is <HOME>/files, and DFU then
 * appends "DaggerfallUnity", so by default it looks for its StreamingAssets at
 *     <root>/files/DaggerfallUnity/assets/...
 * The first in-game boot logged, in order:
 *     DirectoryNotFoundException: .../files/DaggerfallUnity/assets/Text
 *     Fonts directory path .../files/DaggerfallUnity/assets/Fonts not found
 *     Mod system is enabled but directory .../assets/Mods doesn't exist
 * while assets/Text, assets/Fonts and the rest sat correctly at <root>/assets,
 * unpacked from the APK exactly as the README asks.
 *
 * Pointing the override at <root> makes DFU look at <root>/assets, <root>/Saves
 * and <root>/settings.ini -- next to the .nro, which is the layout this port
 * documents and the one the player already has.
 *
 * Seeded only when ABSENT, so anything the player sets in-game wins and this
 * never fights them. */
static void prefs_seed_dfu_datapath(void) {
  static const char *KEY = "DaggerfallUnity_CustomPersistentDataPath";
  if (kv_get(KEY)) return;                    /* player/game already chose */
  /* Use this file's OWN helper, not nx_data_root.h/libc_shim.h. unity_jni.c
   * includes neither, keeps its own g_root (set at line ~807 from data_root,
   * before prefs_load() runs on the next line), and already uses managed_root()
   * for getPackageCodePath. It strips the "sdmc:" prefix the same way
   * managed_path() does. */
  const char *want = managed_root();             /* "/switch/<folder>" */
  if (!want || !*want) return;
  kv_set('s', KEY, want);
  debugPrintf("[prefs] seeded %s = %s\n", KEY, want);
  debugPrintf("[prefs]   -> DFU will use <root>/assets, <root>/Saves, "
              "<root>/settings.ini\n");
}

static void prefs_load_file(void);

/* prefs_load() = load the file, THEN seed. Two things went wrong when the seed
 * lived inside the loader: it sat after an early `return` taken when prefs.kv
 * does not exist (i.e. it never ran on a first boot, the only boot that needs
 * it), and the loader ends with `g_kv_dirty=0`, which would have discarded the
 * seeded entry instead of writing it out. */
static void prefs_load(void){
  prefs_load_file();
  prefs_seed_dfu_datapath();
}

static void prefs_load_file(void){
  char p[256]; FILE*f=fopen(prefs_file(p,sizeof p),"rb");
  if(!f){ debugPrintf("[prefs] load: no save file at %s (errno=%d)\n", p, errno); return; }
  /* Lines of ANY length. A Rewired ControllerMap saved to PlayerPrefs is an
   * XML document of several KB on one line; fgets() into a 2 KB buffer
   * returned its first 2047 bytes as one record and the remainder as garbage
   * records. Rewired then read back a truncated map and logged
   *   "Error creating KeyboardMap from XML. Failed to parse XML string."
   * (boot 25) -- and fell back to defaults, losing every saved mapping. */
  size_t cap = 4096; char *line = malloc(cap); size_t len = 0; int c; int truncated_seen = 0;
  int dropped_xml = 0;
  if (!line) { fclose(f); return; }
  for (;;) {
    c = fgetc(f);
    if (c != EOF && c != '\n') {
      if (len + 1 >= cap) { cap *= 2; char *nl = realloc(line, cap); if (!nl) break; line = nl; }
      line[len++] = (char)c;
      continue;
    }
    line[len] = 0;
    if (len) {
      if (len > 2047) truncated_seen++;
      char type=line[0]; char *k=line+2;            /* "T\tkey\tval"          */
      char *t1=strchr(k,'\t');
      if (t1) {
        *t1=0; char*v=t1+1;
        const char *key = unesc(k), *val = unesc(v);
        /* DISCARD A TRUNCATED XML VALUE instead of serving it.
         *
         * Rewired stores its controller maps as multi-KB XML in PlayerPrefs.
         * Until boot 25 this loader read them through a 2 KB fgets buffer and
         * returned the first 2047 bytes; worse, the game then FLUSHED that
         * truncated value back, so the file on the SD card is permanently
         * corrupt and a fixed loader still reads corrupt data -- boot 31 still
         * logs "Error creating KeyboardMap from XML" and only L/R/START work,
         * because every other binding lives in the map that will not parse.
         * A complete document ends with '>'. Anything that starts like XML and
         * does not is dropped: Rewired then sees the key as absent and writes
         * fresh defaults, which heals the file without the user deleting it. */
        size_t vl = strlen(val);
        if (vl >= 5 && !memcmp(val, "<?xml", 5) && val[vl-1] != '>') {
          dropped_xml++;
          if (dropped_xml <= 4)
            debugPrintf("[prefs] dropping truncated XML value (%u bytes, ends 0x%02x) for '%.70s'\n",
                        (unsigned)vl, (unsigned char)val[vl-1], key);
          continue;
        }
        kv_set(type, key, val);
      }
    }
    len = 0;
    if (c == EOF) break;
  }
  free(line);
  if (truncated_seen) debugPrintf("[prefs] load: %d entries longer than the old 2 KB line buffer (would have been truncated before)\n", truncated_seen);
  if (dropped_xml) debugPrintf("[prefs] load: dropped %d truncated XML value(s) -- Rewired will rewrite them\n", dropped_xml);
  fclose(f); g_kv_dirty=0;
  debugPrintf("[prefs] load: %d entries from %s\n", g_kv_n, p);
}
static void prefs_flush(void){
  if(!g_kv_dirty){ debugPrintf("[prefs] flush: nothing dirty (%d entries)\n", g_kv_n); return; }
  char p[256]; FILE*f=fopen(prefs_file(p,sizeof p),"wb");
  if(!f){ debugPrintf("[prefs] flush FAILED: fopen(%s,wb) errno=%d\n", p, errno); return; }
  for(int i=0;i<g_kv_n;i++){ fputc(g_kv[i].type,f); fputc('\t',f);
    esc(f,g_kv[i].key); fputc('\t',f); esc(f,g_kv[i].val); fputc('\n',f); }
  fclose(f); g_kv_dirty=0;
  debugPrintf("[prefs] flush: wrote %d entries to %s\n", g_kv_n, p);
}

/* ==========================================================================
 * getAll() boxed values: turn a stored KV into the Java object Unity expects.
 * Strings come back as a native FakeString (Unity reads them via
 * GetStringUTFChars); primitives come back as our UJ_BOXED handle, which
 * jni_fake.c recognises by receiver for IsInstanceOf + intValue/longValue/
 * booleanValue/floatValue. Type char matches kv_set(): S/I/L/B/F.
 * ========================================================================== */
static void *uh_box_from_kv(const KV *kv){
  if (!kv) return jni_make_string("");
  switch (kv->type){
    case 'I': case 'L': case 'B': {
      UHandle *h = uh_new(UJ_BOXED);
      h->btype = kv->type;
      h->bival = strtoll(kv->val, NULL, 10);
      if (kv->type=='B') h->bival = (kv->val[0]=='1'||kv->val[0]=='t'||kv->val[0]=='T') ? 1 : 0;
      return h;
    }
    case 'F': {
      UHandle *h = uh_new(UJ_BOXED);
      h->btype = 'F'; h->bfval = strtod(kv->val, NULL);
      return h;
    }
    default: /* 'S' and anything else -> string */
      return jni_make_string(kv->val);
  }
}

/* Receiver-keyed accessors used by jni_fake.c (see unity_jni.h). */
int unity_is_boxed(void *p){
  UHandle *h = p; return (h && h->tag==UJ_TAG && h->kind==UJ_BOXED) ? 1 : 0;
}
uint64_t unity_boxed_int(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0;
  if (h->btype=='F') return (uint64_t)(long long)h->bfval;
  return (uint64_t)h->bival;
}
float unity_boxed_float(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0.0f;
  return (h->btype=='F') ? (float)h->bfval : (float)h->bival;
}
/* 1/0 if obj is one of our boxed primitives and matches/!matches clazz; -1 if
 * obj is not ours (jni_fake.c then applies its own rules). */
int unity_isinstance(void *p, const char *clazz){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG || h->kind!=UJ_BOXED) return -1;
  if (!clazz) return 0;
  switch (h->btype){
    case 'I': return strstr(clazz,"Integer") ? 1 : 0;
    case 'L': return strstr(clazz,"Long")    ? 1 : 0;
    case 'F': return strstr(clazz,"Float")   ? 1 : 0;
    case 'B': return strstr(clazz,"Boolean") ? 1 : 0;
  }
  return 0;
}

/* ==========================================================================
 * class ownership + dispatch
 * ========================================================================== */
int unity_owns_class(const char *cls){
  return has(cls,"java/io/File") || has(cls,"os/Environment") || has(cls,"app/Activity") ||
         has(cls,"AssetManager") || has(cls,"java/io/InputStream") ||
         has(cls,"AssetFileDescriptor") || has(cls,"java/io/FileDescriptor") ||
         has(cls,"SharedPreferences") || has(cls,"SharedPreferences$Editor") ||
         has(cls,"java/util/Map") || has(cls,"java/util/Set") ||
         has(cls,"java/util/Iterator") || has(cls,"java/util/HashMap") ||
         has(cls,"view/Display") || has(cls,"DisplayManager") ||
         has(cls,"res/Configuration") || has(cls,"res/Resources") ||
         has(cls,"DisplayMetrics") || has(cls,"content/Context") || has(cls,"app/Activity") ||
         has(cls,"unity3d/player/UnityPlayer");
}

/* ---- object-returning calls --------------------------------------------- */
void *unity_dispatch_object(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;

  /* AssetManager.open(name) -> InputStream ; openFd(name) -> AssetFileDescriptor */
  if (has(cls,"AssetManager")){
    if (has(m,"openFd") || has(m,"openNonAssetFd")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[320]; asset_path(path,sizeof path,name);
      int fd = open(path,O_RDONLY);
      debugPrintf("[io] JNI AssetManager.openFd(%s) -> %s [%s]\n", name, fd>=0?"ok":"MISSING", path);
      if(fd<0){ return NULL; /* CHECK: engine expects exception; NULL usually ok */ }
      struct stat st; fstat(fd,&st);
      UHandle*h=uh_new(UJ_AFD); h->fd=fd; h->off=0; h->len=st.st_size; return h;
    }
    if (has(m,"open")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[320]; asset_path(path,sizeof path,name);
      FILE*fp=fopen(path,"rb");
      debugPrintf("[io] JNI AssetManager.open(%s) -> %s [%s]\n", name, fp?"ok":"MISSING", path);
      if(!fp) return NULL;
      UHandle*h=uh_new(UJ_INPUTSTREAM); h->fp=fp; return h;
    }
    if (has(m,"list")) return jni_make_object("String[]"); /* CHECK: empty array */
    return jni_make_object("AssetManager");
  }

  /* AssetFileDescriptor.getFileDescriptor() -> FileDescriptor (carries the fd) */
  if (has(cls,"AssetFileDescriptor")){
    if (has(m,"getFileDescriptor") || has(m,"getParcelFileDescriptor")){
      UHandle*a=recv; UHandle*fd=uh_new(UJ_FD); fd->fd = is_uh(a,UJ_AFD)?a->fd:-1; return fd;
    }
    return jni_make_object("AssetFileDescriptor");
  }

  /* SharedPreferences.edit() -> Editor ; getString -> String ; getAll -> Map */
  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    if (has(m,"edit")) return uh_new(UJ_EDITOR);
    if (has(m,"getString")){
      const char *key = jni_string_utf(va_arg(va,void*));
      KV*kv=kv_get(key);
      /* Log the RESULT, not just the fact of the call. Boot 16 re-ran the whole
       * asset unpack because PlayerPrefs "saVersion" did not read back as the
       * game expected, and nothing in the log could say whether the key was
       * missing, empty, or present with a different value. Bouncemasters' note:
       * "a key alone cannot tell you whether a save contains anything". */
      debugPrintf("[prefs] getString '%.90s' -> %s%u bytes%s%.40s%s\n", key,
                  kv ? "" : "<MISSING> ", kv ? (unsigned)strlen(kv->val) : 0u,
                  kv ? " \"" : "", kv ? kv->val : "", kv ? "\"" : "");
      return jni_make_string(kv?kv->val: "");
    }
    if (has(m,"getAll")){                 /* Unity PlayerPrefs LOAD entry point */
      debugPrintf("[prefs] getAll() -> Map of %d entries\n", g_kv_n);
      return uh_new(UJ_MAP);
    }
    if (has(m,"getStringSet")) return jni_make_object("Set");
    return jni_make_object("SharedPreferences");
  }

  /* ---- getAll() Map iteration: Map.entrySet/keySet -> Set -> Iterator -> ----
   * Entry{getKey,getValue}. The whole chain just walks the live g_kv list; the
   * Iterator carries a cursor, each Entry captures one index. Map.get(key) is
   * also handled in case Unity takes the keySet()+get() path on some build. */
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    if (has(m,"entrySet") || has(m,"keySet")) return uh_new(UJ_SET);
    if (has(m,"get")){ const char*k=jni_string_utf(va_arg(va,void*)); return uh_box_from_kv(kv_get(k)); }
    return uh_new(UJ_MAP);
  }
  if (has(cls,"java/util/Set")){
    if (has(m,"iterator")){ UHandle*it=uh_new(UJ_ITER); it->idx=0; return it; }
    return uh_new(UJ_SET);
  }
  if (has(cls,"java/util/Iterator")){
    if (has(m,"next")){                   /* return current entry, advance cursor */
      UHandle*it=recv;
      int i = is_uh(it,UJ_ITER) ? it->idx : 0;
      if (is_uh(it,UJ_ITER)) it->idx++;
      UHandle*e=uh_new(UJ_ENTRY); e->idx=i; return e;
    }
    return jni_make_object("java/util/Iterator");
  }
  if (has(cls,"java/util/Map") && has(cls,"Entry")){   /* java/util/Map$Entry */
    UHandle*e=recv; int i = is_uh(e,UJ_ENTRY) ? e->idx : -1;
    if (i<0 || i>=g_kv_n) return jni_make_string("");
    if (has(m,"getKey"))   return jni_make_string(g_kv[i].key);
    if (has(m,"getValue")) return uh_box_from_kv(&g_kv[i]);
    return jni_make_string("");
  }
  if (has(cls,"SharedPreferences$Editor")){
    /* putX all return the Editor (chained: editor.putInt(k,v).apply()), so the
     * engine reaches them via CallObjectMethod -> HERE, not the int path. All
     * four primitive puts MUST kv_set here or the write is silently dropped
     * (this was the save bug: int/bool prefs, incl. Unity's storage-version
     * marker, never persisted -> "Upgrading PlayerPrefs storage" every launch).
     * An EMPTY key means the key-encoding path (String([B)/Uri.encode) produced
     * nothing; storing under "" makes every such pref collide into one slot and
     * corrupts Screenmanager resolution -> bad res -> crash. Skip those. */
    if (has(m,"putString")){ const char*k=jni_string_utf(va_arg(va,void*));
      const char*v=jni_string_utf(va_arg(va,void*));
      if(!k[0]){ debugPrintf("[prefs] putString SKIP empty key\n"); return recv; }
      kv_set('S',k,v);
      debugPrintf("[prefs] putString '%s' = %u bytes%s\"%.48s%s\n", k,
                  (unsigned)strlen(v), *v ? " " : " <EMPTY> ", v,
                  strlen(v) > 48 ? "...\"" : "\"");
      return recv; }
    if (has(m,"putInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); char b[32]; snprintf(b,sizeof b,"%d",v);
      if(!k[0]){ debugPrintf("[prefs] putInt SKIP empty key (=%d)\n",v); return recv; }
      kv_set('I',k,b); debugPrintf("[prefs] putInt '%s'=%d\n",k,v); return recv; }
    if (has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      if(!k[0]){ debugPrintf("[prefs] putLong SKIP empty key\n"); return recv; }
      kv_set('L',k,b); debugPrintf("[prefs] putLong '%s'=%lld\n",k,v); return recv; }
    if (has(m,"putFloat")){ const char*k=jni_string_utf(va_arg(va,void*));
      double v=va_arg(va,double); char b[32]; snprintf(b,sizeof b,"%.9g",v);
      if(!k[0]){ debugPrintf("[prefs] putFloat SKIP empty key\n"); return recv; }
      kv_set('F',k,b); debugPrintf("[prefs] putFloat '%s'=%g\n",k,v); return recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int);
      if(!k[0]){ debugPrintf("[prefs] putBoolean SKIP empty key (=%d)\n",v); return recv; }
      kv_set('B',k,v?"1":"0"); debugPrintf("[prefs] putBoolean '%s'=%d\n",k,v); return recv; }
    if (has(m,"remove")){ const char*k=jni_string_utf(va_arg(va,void*)); kv_remove(k); return recv; }
    if (has(m,"clear")){ kv_clear(); return recv; }
    return recv;
  }

  /* Display / DisplayManager / Resources / Context: object getters */
  if (has(cls,"DisplayManager") && has(m,"getDisplay")) return jni_make_object("Display");
  if (has(cls,"res/Resources")){
    if (has(m,"getConfiguration")) return jni_make_object("Configuration");
    if (has(m,"getDisplayMetrics")) return jni_make_object("DisplayMetrics");
    return jni_make_object("Resources");
  }
  /* ---- java.io.File: real paths --------------------------------------------
   * Receiver-routed: the engine resolves File methods via GetObjectClass, which
   * may report java/lang/Object, so trust the handle kind over id->cls. */
  if (is_uh_file(recv)) {
    UHandle *f = recv; char buf[512];
    if (has(m,"getAbsolutePath")||has(m,"getPath")||has(m,"getCanonicalPath")||has(m,"toString"))
      return jni_make_string(f->path);
    if (has(m,"getName"))   return jni_make_string(file_name(f));
    if (has(m,"getParent")) { const char *pp = file_parent(f, buf, sizeof buf);
                              return pp ? jni_make_string(pp) : NULL; }
    if (has(m,"getParentFile")) { const char *pp = file_parent(f, buf, sizeof buf);
                                  return pp ? uh_file(pp) : NULL; }
    if (has(m,"getAbsoluteFile")||has(m,"getCanonicalFile")) return uh_file(f->path);
    if (has(m,"toURI")) { snprintf(buf, sizeof buf, "file://%s", f->path); return jni_make_string(buf); }
    /* listFiles() / list(): Clone Hero's song scanner. Return File objects (or
     * names) for every entry. The variants that take a filter get the unfiltered
     * list; the C# side re-checks anyway. */
    if (has(m,"listFiles")||!strcmp(m,"list")) {
      int names = !strcmp(m,"list");
      DIR *d = opendir(f->path);
      if (!d) return NULL;                     /* Java: null when not a directory */
      int cap = 64, cnt = 0; void **tmp = malloc(cap * sizeof *tmp);
      struct dirent *de;
      while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        if (cnt == cap) { cap *= 2; tmp = realloc(tmp, cap * sizeof *tmp); }
        if (names) tmp[cnt++] = jni_make_string(de->d_name);
        else { snprintf(buf, sizeof buf, "%s/%s", f->path, de->d_name); tmp[cnt++] = uh_file(buf); }
      }
      closedir(d);
      void *arr = jni_make_object_array(cnt);
      for (int i = 0; i < cnt; i++) jni_object_array_set(arr, i, tmp[i]);
      free(tmp);
      return arr;
    }
  }
  /* ---- android.os.Environment: public storage roots ---------------------- */
  if (has(cls,"os/Environment")) {
    if (has(m,"getExternalStorageDirectory")||has(m,"getRootDirectory")||has(m,"getDataDirectory"))
      return uh_file(managed_root());
    if (has(m,"getExternalStoragePublicDirectory")||has(m,"getExternalStorageDirectory")) {
      /* (String type) -> <root>/<type>. DIRECTORY_DOCUMENTS -> "Documents", so
       * Clone Hero's library lands in <root>/Documents/Clone Hero -- the same
       * relative layout it uses on a phone, just under our folder. */
      void *type = va_arg(va, void *);
      return uh_file_join(managed_root(), jni_string_utf(type));
    }
    if (has(m,"getDownloadCacheDirectory")) return uh_file_join(managed_root(), "cache");
  }
  if (has(cls,"content/Context") || has(cls,"app/Activity")){   /* Activity IS-A Context */
    /* Context identity + loader. The activity is a perfectly good Context and
     * Application, so answer with itself rather than a fresh opaque object that
     * would not compare equal to it. The ClassLoader is jni_fake's singleton --
     * the engine hands it back to FindClass paths that expect the same one. */
    if (has(m,"getApplicationContext") || has(m,"getBaseContext") || has(m,"getApplication"))
      return recv ? recv : jni_make_object("android/app/Activity");
    if (has(m,"getClassLoader")) return jni_classloader_obj();
    if (has(m,"getIntent"))      return jni_make_object("android/content/Intent");
    if (has(m,"getWindow"))      return jni_make_object("android/view/Window");
    if (has(m,"getContentResolver")) return jni_make_object("android/content/ContentResolver");
    if (has(m,"getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
    /* path getters -> REAL File handles under our staged dir, so both
     * getAbsolutePath() and later mkdirs()/exists()/listFiles() work. */
    if (has(m,"getFilesDir"))          return uh_file_join(managed_root(), "files");
    if (has(m,"getCacheDir"))          return uh_file_join(managed_root(), "cache");
    if (has(m,"getDataDir"))           return uh_file(managed_root());
    if (has(m,"getExternalFilesDir")) {              /* (String type|null) */
      void *type = va_arg(va, void *);
      const char *t = jni_string_utf(type);
      if (t && *t) { char b[512]; snprintf(b, sizeof b, "files/%s", t); return uh_file_join(managed_root(), b); }
      return uh_file_join(managed_root(), "files");
    }
    if (has(m,"getExternalCacheDir"))  return uh_file_join(managed_root(), "cache");
    if (has(m,"getObbDir"))            return uh_file_join(managed_root(), "obb");
    if (has(m,"getNoBackupFilesDir"))  return uh_file_join(managed_root(), "files");
    /* Clone Hero's real package id. The inherited value was Zookeeper's; the
     * game may fold this into paths (Android/data/<pkg>/) or pref namespaces. */
    if (has(m,"getPackageName")) return jni_make_string("com.srylain.CloneHero");
    if (has(m,"getPackageCodePath")||has(m,"getPackageResourcePath")) return jni_make_string(managed_root());
    if (has(m,"getAssets")) return jni_make_object("AssetManager");
    if (has(m,"getResources")) return jni_make_object("Resources");
    if (has(m,"getSystemService")) return jni_make_object("Service");
    return jni_make_object("Context");
  }
  /* A File the engine resolved by class but whose receiver we did not create
   * (a label-only object from an older path): fall back to the root. */
  if (has(cls,"java/io/File") && (has(m,"getAbsolutePath")||has(m,"getPath")||has(m,"toString")))
    return jni_make_string(managed_root());

  /* UnityPlayer host queries that return objects -> benign */
  if (has(cls,"UnityPlayer")) return jni_make_object("UnityPlayer");

  if (has(cls,"unity3d/player/UnityPlayer")) {
    /* Engine startup queries. Both are String-typed and both mean "none" here:
     * we were not launched from a URL, and there is no HTTP proxy. NULL is the
     * Java answer for "no launch URL"; "" keeps the proxy parser quiet. */
    if (has(m,"getLaunchURL"))            return NULL;
    if (has(m,"getNetworkProxySettings")) return jni_make_string("");
  }

  /* The most important ledger site in the tree. An opaque handle where a File
   * or a String belonged -- and because it never returns NULL, nothing
   * downstream can tell it apart from a real object. It will not show up as a
   * NULL-return bug; it shows up as a wrong stored value much later. */
  jni_note_approx("OPAQUE-OBJ", cls, id->name, id->sig);
  return jni_make_object(cls); /* default: opaque handle, never NULL */
}

/* ---- int / boolean / long calls ----------------------------------------- */
/* mkdir -p for File.mkdirs(): create every missing component. */
static int mkdirs_p(const char *path){
  char tmp[512]; snprintf(tmp, sizeof tmp, "%s", path);
  for (char *c = tmp + 1; *c; c++) {
    if (*c == '/') { *c = 0; if (strchr(tmp, ':') && !strchr(strchr(tmp,':')+1,'/')) { *c='/'; continue; }
                     mkdir(tmp, 0777); *c = '/'; }
  }
  return mkdir(tmp, 0777) == 0 || errno == EEXIST;
}

uint64_t unity_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  {
    const char *cls = id->cls, *m = id->name;
    /* ---- android.os.Environment: storage state ------------------------------
     * Clone Hero's Android-11+ path:
     *   if (SDK_INT >= 30 && !Environment.isExternalStorageManager())
     *       startActivity(MANAGE_APP_ALL_FILES_ACCESS_PERMISSION intent)  // and wait
     * The Settings screen does not exist on this platform and the wait never
     * ends. We have unrestricted access to our own folder, so the truthful
     * answer is yes -- and it is the only answer that lets the game proceed to
     * its unpack step. This handler used to live in jni_fake.c and was never
     * reached once os/Environment routed here; the default 0 said "no". */
    if (has(cls,"os/Environment")) {
      if (has(m,"isExternalStorageManager") || has(m,"isExternalStorageLegacy") ||
          has(m,"isExternalStorageEmulated") || has(m,"isExternalStorageRemovable")) return 1;
      if (has(m,"isExternalStorageRemovable")) return 0;
    }
    /* ---- Context / Activity: permissions ----------------------------------
     * PackageManager.PERMISSION_GRANTED == 0. Explicit, not by way of the
     * default, so a future reordering cannot silently flip it. */
    if (has(cls,"content/Context") || has(cls,"app/Activity")) {
      if (has(m,"checkSelfPermission") || has(m,"checkCallingOrSelfPermission") ||
          has(m,"checkPermission") || has(m,"checkCallingPermission")) return 0;
      if (has(m,"shouldShowRequestPermissionRationale")) return 0;   /* never nag */
      if (has(m,"isFinishing") || has(m,"isDestroyed") || has(m,"isChangingConfigurations")) return 0;
      if (has(m,"hasWindowFocus")) return 1;
      if (has(m,"getTaskId")) return 1;
    }
  }
  /* ---- java.io.File predicates and mutators -------------------------------
   * Receiver-routed (see unity_dispatch_object). These are what Clone Hero
   * uses to set up its library folder: exists() -> mkdirs() -> isDirectory()
   * -> canWrite(). Every one used to fall through to a default 0, so the game
   * believed its folder could not be created. */
  if (is_uh_file(recv)) {
    UHandle *f = recv; struct stat st; const char *m = id->name;
    int have = stat(f->path, &st) == 0;
    if (has(m,"exists"))      return have;
    if (has(m,"isDirectory")) return have && S_ISDIR(st.st_mode);
    if (has(m,"isFile"))      return have && S_ISREG(st.st_mode);
    if (has(m,"isAbsolute"))  return f->path[0]=='/' || strchr(f->path,':') != NULL;
    if (has(m,"isHidden"))    return file_name(f)[0] == '.';
    if (has(m,"canRead"))     return have;
    if (has(m,"canWrite")||has(m,"canExecute")) return 1;   /* our folder, always */
    if (has(m,"mkdirs"))      return mkdirs_p(f->path);
    if (has(m,"mkdir"))       return mkdir(f->path, 0777) == 0 || errno == EEXIST;
    if (has(m,"createNewFile")) {
      if (have) return 0;                       /* Java: false if it already exists */
      FILE *fp = fopen(f->path, "wb"); if (!fp) return 0; fclose(fp); return 1;
    }
    if (has(m,"delete"))      return have && (S_ISDIR(st.st_mode) ? rmdir(f->path) : unlink(f->path)) == 0;
    if (has(m,"length"))      return have ? (uint64_t)st.st_size : 0;
    if (has(m,"lastModified")) return have ? (uint64_t)st.st_mtime * 1000ULL : 0;
    if (has(m,"hashCode"))    { uint32_t h = 0; for (const char *c=f->path;*c;c++) h=h*31+(unsigned char)*c; return h; }
    if (has(m,"equals"))      { void *o = va_arg(va, void *); return is_uh_file(o) && !strcmp(f->path, ((UHandle*)o)->path); }
    if (has(m,"renameTo"))    { void *o = va_arg(va, void *); return is_uh_file(o) && rename(f->path, ((UHandle*)o)->path) == 0; }
    if (has(m,"setWritable")||has(m,"setReadable")||has(m,"setExecutable")||has(m,"setLastModified")) return 1;
    if (has(m,"getFreeSpace")||has(m,"getUsableSpace")||has(m,"getTotalSpace")) return 4ULL<<30; /* 4 GB: plenty */
  }
  const char *cls=id->cls, *m=id->name;

  /* getAll() iteration: Iterator.hasNext + a few Map predicates. (Integer/Long/
   * Boolean unboxing is routed by RECEIVER in jni_fake.c, not here.) */
  if (has(cls,"java/util/Iterator") && has(m,"hasNext")){
    UHandle*it=recv; return (uint64_t)((is_uh(it,UJ_ITER) && it->idx < g_kv_n) ? 1 : 0);
  }
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    if (has(m,"size"))    return (uint64_t)g_kv_n;
    if (has(m,"isEmpty")) return (uint64_t)(g_kv_n==0);
    if (has(m,"containsKey")){ const char*k=jni_string_utf(va_arg(va,void*)); return (uint64_t)(kv_get(k)?1:0); }
  }

  /* InputStream.read() / read([B) / read([B,off,len) / available / skip */
  if (has(cls,"java/io/InputStream")){
    UHandle*h=recv; if(!is_uh(h,UJ_INPUTSTREAM)||!h->fp) return (uint64_t)-1;
    if (has(m,"available")){ long cur=ftell(h->fp); fseek(h->fp,0,SEEK_END);
      long end=ftell(h->fp); fseek(h->fp,cur,SEEK_SET); return (uint64_t)(end-cur); }
    if (has(m,"skip")){ long nskip=(long)va_arg(va,long long); fseek(h->fp,nskip,SEEK_CUR); return (uint64_t)nskip; }
    if (has(m,"close")){ fclose(h->fp); h->fp=NULL; return 0; }
    if (has(m,"read")){
      if (strstr(id->sig,"([B")){                     /* read(byte[][,off,len]) */
        void *arr = va_arg(va,void*);
        int alen=0; char *buf = jni_bytearray_data(arr,&alen);
        int off=0, len=alen;
        if (strstr(id->sig,"([BII)")){ off=va_arg(va,int); len=va_arg(va,int); }
        size_t got=fread(buf+off,1,(size_t)len,h->fp);
        return got? (uint64_t)got : (uint64_t)-1;     /* -1 == EOF, per InputStream */
      }
      int c=fgetc(h->fp); return (uint64_t)(c==EOF? -1 : c); /* read() one byte */
    }
    return 0;
  }

  /* SharedPreferences getters (Int/Long/Boolean + contains) */
  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    if (has(m,"contains")){ const char*k=jni_string_utf(va_arg(va,void*)); int r=kv_get(k)?1:0;
      debugPrintf("[prefs] contains '%s' -> %d\n", k, r); return r; }
    if (has(m,"getInt")||has(m,"getLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long def=(long long)va_arg(va,long long); KV*kv=kv_get(k);
      return (uint64_t)(kv? strtoll(kv->val,NULL,10) : def); }
    if (has(m,"getBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int def=va_arg(va,int); KV*kv=kv_get(k); return (uint64_t)(kv? (kv->val[0]=='1'||kv->val[0]=='t') : def); }
    return 0;
  }
  /* Editor.putInt/Long/Boolean(...)Z?  most return the Editor (object), but
   * commit() returns Z. Route the primitive puts here since the key+value are
   * primitive-shaped, then have the engine ignore the int return. CHECK. */
  if (has(cls,"SharedPreferences$Editor")){
    if (has(m,"commit")){ debugPrintf("[prefs] commit\n"); prefs_flush(); return 1; }
    if (has(m,"putInt")||has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=(long long)va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      kv_set(has(m,"putLong")?'L':'I',k,b); return (uint64_t)(uintptr_t)recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); kv_set('B',k,v?"1":"0"); return (uint64_t)(uintptr_t)recv; }
    return (uint64_t)(uintptr_t)recv;
  }

  /* AssetFileDescriptor.getStartOffset()/getLength()/getDeclaredLength() (long) */
  if (has(cls,"AssetFileDescriptor")){
    UHandle*a=recv;
    if (has(m,"getStartOffset")) return (uint64_t)(is_uh(a,UJ_AFD)?a->off:0);
    if (has(m,"getLength")||has(m,"getDeclaredLength")) return (uint64_t)(is_uh(a,UJ_AFD)?a->len:0);
    return 0;
  }
  /* FileDescriptor: some engines read the raw int via a field, not a call.
   * If Unity calls FileDescriptor.getInt$()/getFd(), hand back the fd. CHECK. */
  if (has(cls,"java/io/FileDescriptor")){ UHandle*f=recv; return (uint64_t)(is_uh(f,UJ_FD)?(unsigned)f->fd:0); }

  /* Display / DisplayMetrics / Configuration ints */
  if (has(cls,"view/Display")||has(cls,"DisplayMetrics")||has(cls,"DisplayManager")){
    /* real panel size (landscape for PvZ); already docked/handheld aware */
    if (has(m,"getWidth")||has(m,"WidthPixels")||has(m,"getRawWidth"))  return screen_width;
    if (has(m,"getHeight")||has(m,"HeightPixels")||has(m,"getRawHeight"))return screen_height;
    if (has(m,"orientation")) return 2;   /* Configuration.ORIENTATION_LANDSCAPE */
    if (has(m,"getRotation")) return 0;   /* Surface.ROTATION_0 */
    if (has(m,"getDisplayId")) return 0;
    return 0;
  }
  return 0;
}

/* ---- void calls --------------------------------------------------------- */
void unity_dispatch_void(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;
  if (has(cls,"java/io/InputStream") && has(m,"close")){ UHandle*h=recv;
    if(is_uh(h,UJ_INPUTSTREAM)&&h->fp){fclose(h->fp);h->fp=NULL;} return; }
  if (has(cls,"AssetFileDescriptor") && has(m,"close")){ UHandle*a=recv;
    if(is_uh(a,UJ_AFD)&&a->fd>=0){close(a->fd);a->fd=-1;} return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"apply")){ debugPrintf("[prefs] apply\n"); prefs_flush(); return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"putString")){ /* if routed here as void */
    const char*k=jni_string_utf(va_arg(va,void*)); const char*v=jni_string_utf(va_arg(va,void*));
    kv_set('S',k,v); return; }
  if (has(cls,"UnityPlayer")){
    /* setOrientation/lowMemory/configurationChanged/etc. -> no-op */
    return;
  }
  (void)recv;(void)va;
}

/* ========================================================================== */
void unity_jni_init(const char *data_root){
  snprintf(g_root,sizeof g_root,"%s",data_root && *data_root ? data_root : "/switch/zookeeper");
  snprintf(g_assets,sizeof g_assets,"%s/assets",g_root);
  prefs_load();
  /* caller (jni_fake.c jni_init) should also intern every UNITY_JNI_CLASSES[]
   * name so FindClass returns non-NULL classes. */
}
