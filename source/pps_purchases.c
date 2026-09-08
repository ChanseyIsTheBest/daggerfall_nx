/* pps_purchases.c -- in-app content you own, and the music manager.
 *
 * MIT licensed. See LICENSE.
 *
 * THE STORE
 * ---------
 * Papa Pear Saga sells gold bars through Google Play. On Android the game asks
 * Play which products your account owns and the Java billing wrapper hands the
 * answer back to the engine:
 *
 *     GooglePlayIABv3Lib.onSetupFinished(response, billingSupported)
 *     GooglePlayIABv3Lib.onQueryPurchasesFinished(response, Purchase[])
 *
 * There is no Play Store here to ask, so the port reads purchases.txt and
 * makes the identical calls with the identical shapes. Nothing is patched, no
 * save file is edited, and the engine grants the content through its own
 * ordinary restore path.
 *
 * You are asserting what you own. The file ships with everything commented
 * out, so a fresh install restores nothing until you say otherwise.
 *
 * WHAT "OWNING" MEANS FOR A CONSUMABLE
 * ------------------------------------
 * These products are gold bar packs, which are consumables rather than
 * permanent unlocks like Osmos's light mode. Google Play would deliver an
 * unconsumed purchase once, the game would grant the bars, and the purchase
 * would then be consumed and never seen again.
 *
 * This port re-delivers every uncommented line on EVERY launch. That is a
 * deliberate choice and it is worth being clear about: leaving a line
 * uncommented grants those bars again each time you start the game. Comment it
 * out once you have what you wanted. The file is yours to manage.
 *
 * THE MUSIC MANAGER
 * -----------------
 * Sharing a file with the store because both are small and both are "things
 * the Java layer used to do for the engine". See the second half.
 */

#include <switch.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "pps_jni.h"
#include "pps_paths.h"
#include "pps_io.h"
#include "pps_platform.h"
#include "so_util.h"
#include "pps_music.h"
#include "pps_purchases.h"

/* ------------------------------------------------------------------ */
/* Product identifiers                                                 */
/* ------------------------------------------------------------------ */

/* These are the real ids, read out of libpapapearsaga.so's .rodata -- they are
 * what the engine itself asks the store about, not names invented from the
 * product table. Getting this wrong is silent: ProvideContent would name a
 * product the store has never heard of and nothing would unlock.
 *
 * The friendly aliases are a convenience; anything not listed is passed
 * through untouched, so a raw id always works. */
typedef struct { const char *alias, *sku, *what; } Sku;

static const Sku g_alias[] = {
  { "goldbars_xs",    "com.midasplayer.iap.papapearsaga.gold.bars.extra.small.package",       "Gold bars, extra small" },
  { "goldbars_s",     "com.midasplayer.iap.papapearsaga.gold.bars.small.package",             "Gold bars, small" },
  { "goldbars_m",     "com.midasplayer.iap.papapearsaga.gold.bars.medium.package",            "Gold bars, medium" },
  { "goldbars_l",     "com.midasplayer.iap.papapearsaga.gold.bars.large.package",             "Gold bars, large" },
  { "goldbars_xl",    "com.midasplayer.iap.papapearsaga.gold.bars.extra.large.package",       "Gold bars, extra large" },
  { "goldbars_xxl",   "com.midasplayer.iap.papapearsaga.gold.bars.extra.extra.large.package", "Gold bars, extra extra large" },
  { NULL, NULL, NULL }
};

#define MAX_OWNED 32
static char g_owned[MAX_OWNED][160];
static int  g_nowned;
static int  g_loaded;

static void trim(char *s) {
  char *p = s;
  size_t n;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static const char *resolve_alias(const char *in) {
  int i;
  for (i = 0; g_alias[i].alias; i++)
    if (!strcmp(in, g_alias[i].alias)) return g_alias[i].sku;
  return in;   /* a raw product id, passed through */
}

static const char *g_template =
"# Papa Pear Saga -- in-app content you own\n"
"#\n"
"# This file was created for you on first boot. Uncomment the lines for what\n"
"# you actually bought (delete the leading '#'), then restart the game.\n"
"#\n"
"# On Android the game asks Google Play which in-app products your account\n"
"# owns and unlocks them through its normal restore path:\n"
"#\n"
"#     GooglePlayIABv3Lib.onQueryPurchasesFinished(0, Purchase[])\n"
"#\n"
"# There is no Play Store on this console to ask, so the port reads this list\n"
"# and makes the identical call. Nothing is patched and no save data is\n"
"# edited -- the engine grants the content itself, exactly as it would after a\n"
"# restore on your phone.\n"
"#\n"
"# IMPORTANT: gold bars are CONSUMABLE, not a permanent unlock. Every\n"
"# uncommented line below is delivered again on EVERY launch, so leaving one\n"
"# uncommented keeps granting those bars each time you start the game.\n"
"# Comment it out again once you have what you wanted.\n"
"#\n"
"# List only what you actually purchased.\n"
"\n"
"# --- gold bar packs --------------------------------------------------------\n"
"#goldbars_xs           # extra small\n"
"#goldbars_s            # small\n"
"#goldbars_m            # medium\n"
"#goldbars_l            # large\n"
"#goldbars_xl           # extra large\n"
"#goldbars_xxl          # extra extra large\n"
"\n"
"# --- raw identifiers -------------------------------------------------------\n"
"# Anything not named above is passed to the engine untouched, so you can use\n"
"# the game's own product ids directly:\n"
"#com.midasplayer.iap.papapearsaga.gold.bars.medium.package\n";

int pps_purchases_ensure_file(void) {
  char path[FS_MAX_PATH];
  FILE *f;
  snprintf(path, sizeof(path), "%s/purchases.txt", pps_root());
  f = fopen_locked(path, "r");
  if (f) { fclose_locked(f); return 0; }        /* never overwrite */
  f = fopen_locked(path, "w");
  if (!f) { LOGW("could not create purchases.txt"); return -1; }
  fputs(g_template, f);
  fclose_locked(f);
  LOGB("wrote a fresh purchases.txt (everything commented out)");
  return 1;
}

static void load_owned(void) {
  char path[FS_MAX_PATH];
  char line[256];
  FILE *f;
  if (g_loaded) return;
  g_loaded = 1;
  g_nowned = 0;

  if (!cfg_purchases_enabled()) {
    LOGB("purchases disabled in config.txt; not reading purchases.txt");
    return;
  }

  snprintf(path, sizeof(path), "%s/purchases.txt", pps_root());
  f = fopen_locked(path, "r");
  if (!f) return;

  while (fgets(line, sizeof(line), f) && g_nowned < MAX_OWNED) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    trim(line);
    if (!line[0]) continue;
    snprintf(g_owned[g_nowned], sizeof(g_owned[0]), "%s", resolve_alias(line));
    g_nowned++;
  }
  fclose_locked(f);

  if (g_nowned) {
    int i;
    LOGB("purchases.txt: %d entitlement(s) to restore", g_nowned);
    for (i = 0; i < g_nowned; i++) LOGB("   %s", g_owned[i]);
  }
}

int pps_purchases_count(void) { load_owned(); return g_nowned; }

/* Build the Purchase[] the engine expects back from a query.
 *
 * Every field the engine reads has to be present, because it parses the
 * original JSON blob rather than trusting the accessors alone -- the getters
 * exist but getOriginalJson is what its verifier looks at. The JSON is the
 * shape Play returns; purchaseState 0 is "purchased". */
void *pps_purchases_build_array(void) {
  void *arr;
  int i;
  load_owned();
  if (g_nowned <= 0) return jni_make_object_array(0);

  arr = jni_make_object_array(g_nowned);
  for (i = 0; i < g_nowned; i++) {
    /* Sized from the maxima rather than guessed: 132 bytes of literal, a
     * 95-char token twice and a 159-char product id. The precision specifiers
     * below are what let the compiler prove that -- without them GCC assumes
     * each %s is unbounded and warns about a truncation that cannot happen. */
    char json[768];
    char token[96];
    /* A DISTINCT object per entry. jni_make_object interns one handle per
     * class, which is right for stateless platform objects and wrong here --
     * every Purchase would otherwise carry the last entry's fields. */
    void *purchase = jni_make_unique_object("com/king/store/billingutil/Purchase");
    if (!purchase) continue;

    snprintf(token, sizeof(token), "papapear-nx-%s-%d", pps_device_uuid(), i);

    /* The engine's verifier reads getOriginalJson rather than trusting the
     * accessors, so the blob has to be well formed and has to agree with
     * them. purchaseState 0 is "purchased". */
    snprintf(json, sizeof(json),
             "{\"orderId\":\"%.95s\","
             "\"packageName\":\"com.midasplayer.apps.papapearsaga\","
             "\"productId\":\"%.159s\",\"purchaseTime\":0,"
             "\"purchaseState\":0,\"purchaseToken\":\"%.95s\"}",
             token, g_owned[i], token);

    /* Keyed by the Java FIELD names, not the accessor names.
     *
     * This is the correction that made the restore path work at all. The
     * accessors -- getSku, getToken, getOriginalJson -- appear NOWHERE in
     * libpapapearsaga.so, but mSku, mToken, mOrderId, mSignature,
     * mOriginalJson, mPurchaseState and mPurchaseTime all do. So the engine's
     * onQueryPurchasesFinished does not call methods on these objects: it does
     * GetFieldID(cls, "mSku", ...) and reads the field directly.
     *
     * Answering the accessors instead was a silent no-op. Every Purchase came
     * back with an empty sku, the engine matched none of them against its
     * product table, and nothing unlocked -- with no error, because an empty
     * string is a perfectly valid answer to a question nobody asked. */
    jni_object_set_prop(purchase, "mSku",          g_owned[i]);
    jni_object_set_prop(purchase, "mToken",        token);
    jni_object_set_prop(purchase, "mOrderId",      token);
    jni_object_set_prop(purchase, "mItemType",     "inapp");
    jni_object_set_prop(purchase, "mOriginalJson", json);
    jni_object_set_prop(purchase, "mSignature",    "");
    jni_object_set_prop(purchase, "mPackageName",  "com.midasplayer.apps.papapearsaga");
    jni_object_set_prop(purchase, "mDeveloperPayload", "");
    /* PURCHASED. Play's other states -- 1 CANCELED, 2 REFUNDED -- make the
     * engine discard the entitlement, so this one value decides whether any of
     * this has an effect. Read back through GetIntField on "mPurchaseState". */
    jni_object_set_prop(purchase, "mPurchaseState", "0");
    jni_object_set_prop(purchase, "mPurchaseTime",  "0");
    jni_object_array_set(arr, i, purchase);
  }
  LOGB("store: delivering %d purchase(s)", g_nowned);
  return arr;
}

/* ------------------------------------------------------------------ */
/* Store dispatch                                                      */
/* ------------------------------------------------------------------ */

int pps_purchases_call(const char *cls, const char *name, const char *sig,
                       void *recv, const PpsArgs *a, PpsArgs *out) {
  (void)sig; (void)a;

  /* Purchase / SkuDetails accessors, answered from the properties attached
   * when the object was built. */
  if (!strcmp(cls, "com/king/store/billingutil/Purchase")) {
    const char *v = NULL;
    /* The accessor arm is kept even though this build reads fields instead:
     * it costs nothing, and the two spellings differ only by an "m" and a
     * "get", so a build that used methods would work unchanged. */
    if      (!strcmp(name, "getSku"))          v = jni_object_get_prop(recv, "mSku");
    else if (!strcmp(name, "getToken"))        v = jni_object_get_prop(recv, "mToken");
    else if (!strcmp(name, "getOrderId"))      v = jni_object_get_prop(recv, "mOrderId");
    else if (!strcmp(name, "getItemType"))     v = jni_object_get_prop(recv, "mItemType");
    else if (!strcmp(name, "getOriginalJson")) v = jni_object_get_prop(recv, "mOriginalJson");
    else if (!strcmp(name, "getPackageName"))  v = "com.midasplayer.apps.papapearsaga";
    else if (!strcmp(name, "getSignature"))    v = "";
    else if (!strcmp(name, "getDeveloperPayload")) v = "";
    if (v) { out->o[0] = jni_make_string(v); return 1; }

    if (!strcmp(name, "getPurchaseState")) { out->i[0] = 0; return 1; }  /* purchased */
    if (!strcmp(name, "getPurchaseTime"))  { out->i[0] = 0; return 1; }
  }

  if (!strcmp(cls, "com/king/store/billingutil/SkuDetails")) {
    const char *sku = jni_object_get_prop(recv, "mSku");
    if (!strcmp(name, "getSku"))   { out->o[0] = jni_make_string(sku ? sku : ""); return 1; }
    if (!strcmp(name, "getType"))  { out->o[0] = jni_make_string("inapp"); return 1; }
    if (!strcmp(name, "getPrice")) { out->o[0] = jni_make_string("--"); return 1; }
    if (!strcmp(name, "getTitle")) { out->o[0] = jni_make_string(sku ? sku : ""); return 1; }
    if (!strcmp(name, "getDescription")) { out->o[0] = jni_make_string(""); return 1; }
    if (!strcmp(name, "getPriceCurrencyCode")) { out->o[0] = jni_make_string("USD"); return 1; }
    if (!strcmp(name, "getPriceAmountMicros")) { out->i[0] = 0; return 1; }
  }

  /* The library object itself. The engine calls these to drive the store; the
   * answers it gets here are what put it in "the store exists but has nothing
   * to sell" mode, which is the only honest state on this console.
   *
   * launchPurchaseFlow deliberately does nothing rather than reporting an
   * error: an error opens the game's "could not contact store" popup, which
   * is a worse experience than the button simply not doing anything. */
  if (!strcmp(cls, "com/king/store/GooglePlayIABv3Lib")) {
    if (!strcmp(name, "isBillingSupported")) { out->i[0] = 0; return 1; }
    if (strstr(name, "launchPurchase") || strstr(name, "purchase")) {
      LOGB("store: a purchase was requested; there is no store here");
      return 1;
    }
    if (strstr(name, "consume") || strstr(name, "dispose") ||
        strstr(name, "startSetup") || strstr(name, "query")) {
      return 1;
    }
  }
  return 0;
}

/* ================================================================== */
/* MusicManager                                                        */
/* ================================================================== */

/* On Android this wrapped a MediaPlayer: the engine handed over the raw bytes
 * of a track with LoadResource(name, byte[]) and Java decoded and streamed it.
 *
 * Here the sound effects already work -- they go through the engine's own
 * OpenSL ES path, which opensles.c backs -- and only the music came through
 * Java. Rather than build a second audio pipeline alongside that one, this
 * reports the music system as present but disabled: IsEnabled() false makes
 * the engine stop calling Play and stop expecting position updates, and it
 * leaves the SFX mixer untouched.
 *
 * That is a real gap and it is listed as one in the README. The honest reason
 * it is a gap rather than a bug is that the alternative is worse: a partial
 * implementation that reports a position it is not really at makes the engine
 * fight its own sequencer, and a track that starts and never reports finishing
 * hangs the level-complete transition waiting for a music cue.
 *
 * The shape to fill it in is known: decode the byte[] with a Vorbis decoder
 * and feed it as a second OpenSL buffer-queue player, then answer
 * GetTimePosition from the frames consumed. */
int pps_music_call(const char *name, const char *sig,
                   const PpsArgs *a, PpsArgs *out) {
  (void)sig;

  /* This used to be a table of typed zeros, which is why the port was silent.
   * The engine is not asking Java to "manage" anything abstract here: it hands
   * over the raw bytes of an Ogg Vorbis file and expects them decoded and
   * played. pps_music.c does that. See the header comment there for why music
   * takes this path and sound effects do not. */

  /* LoadResource(String name, byte[] data) -> int handle.
   *
   * Confirmed against the binary: the class path "com/king/core/MusicManager"
   * and the signature "(Ljava/lang/String;[B)I" are both in its .rodata. */
  if (!strcmp(name, "LoadResource")) {
    const char *res = (a->count > 0) ? jni_string_utf(a->o[0]) : NULL;
    int len = 0;
    void *bytes = (a->count > 1) ? jni_bytearray_data(a->o[1], &len) : NULL;
    out->i[0] = pps_music_load(res, bytes, len);
    return 1;
  }
  if (!strcmp(name, "ReleaseResource")) {
    if (a->count > 0) pps_music_release((int)a->i[0]);
    return 1;
  }

  /* Play(int handle, int loopCount, float volume). The float is in a->f[], not
   * a->i[] -- on aarch64 the integer and floating-point arguments come from two
   * separate register save areas, which is exactly why PpsArgs unpacks them by
   * signature rather than letting a handler walk a raw va_list. */
  if (!strcmp(name, "Play")) {
    const int handle = (a->count > 0) ? (int)a->i[0] : 0;
    const int loops  = (a->count > 1) ? (int)a->i[1] : 0;
    const float vol  = (a->count > 2) ? (float)a->f[2] : 1.0f;
    pps_music_play(handle, loops, vol);
    return 1;
  }

  if (!strcmp(name, "Stop"))    { pps_music_stop();    return 1; }
  if (!strcmp(name, "Suspend")) { pps_music_suspend(); return 1; }
  if (!strcmp(name, "Resume"))  { pps_music_resume();  return 1; }

  if (!strcmp(name, "SetVolume")) {
    if (a->count > 0) pps_music_set_volume((float)a->f[0]);
    return 1;
  }
  if (!strcmp(name, "SetEnabled")) {
    if (a->count > 0) pps_music_set_enabled((int)a->i[0]);
    return 1;
  }

  if (!strcmp(name, "IsEnabled")) { out->i[0] = pps_music_is_enabled(); return 1; }
  if (!strcmp(name, "IsPlaying")) { out->i[0] = pps_music_is_playing(); return 1; }
  if (!strcmp(name, "GetLoopCount")) { out->i[0] = pps_music_loop_count(); return 1; }
  if (!strcmp(name, "GetTimeLength"))   { out->f[0] = pps_music_length();   return 1; }
  if (!strcmp(name, "GetTimePosition")) { out->f[0] = pps_music_position(); return 1; }

  /* No other app is playing, and we are always "the device speaker" -- the
   * engine uses these to decide whether to duck its own music for a podcast
   * or a headset. Neither situation exists here. */
  if (!strcmp(name, "IsExternalMusicPlaying")) { out->i[0] = 0; return 1; }
  if (!strcmp(name, "IsUsingDeviceSpeaker") ||
      !strcmp(name, "IsUsingDeviceSpeakerInternal")) { out->i[0] = 1; return 1; }
  if (!strcmp(name, "GetHardwareOutputVolume")) {
    out->f[0] = (double)cfg_master_volume(); return 1;
  }

  /* Update() is the engine's audio tick. The mix is callback-driven so there
   * is nothing to do, but it must be ACCEPTED rather than fall through -- the
   * fallthrough would log an unhandled call every single frame. */
  if (!strcmp(name, "Update")) return 1;

  /* The _OnUiThread variants are the Java implementation's own internals; the
   * engine never calls them, but answering them costs nothing if a different
   * build does. */
  if (strstr(name, "_OnUiThread")) return 1;

  return 0;
}

/* ------------------------------------------------------------------ */
/* Delivering the restore                                              */
/* ------------------------------------------------------------------ */

/* The engine's side of Google Play's restore flow, driven from outside.
 *
 * On Android, GooglePlayIABv3Lib set up a billing client, asked Play what the
 * account owned, and delivered the answer through two native callbacks. Both
 * are exported by name from libpapapearsaga.so, so this port makes exactly the
 * same two calls with exactly the same shapes. Nothing is patched, no save
 * file is edited, and the engine unlocks the content through its own code.
 *
 * onSetupFinished(int responseCode, int billingSupported)
 *     0 and 1: "the billing service came up and billing is available". The
 *     engine gates the query below on having seen this, so the order is not
 *     optional.
 *
 * onQueryPurchasesFinished(int responseCode, Purchase[] purchases)
 *     0 plus the array. The engine walks it, reads the mSku FIELD off each
 *     and grants the matching product. Those accessors are answered by
 *     pps_purchases_call through the JNI net -- see above -- from the
 *     properties attached to each object here.
 */
typedef void (*fn_setup)(void *env, void *thiz, int response, int supported);
typedef void (*fn_query)(void *env, void *thiz, int response, void *array);

void pps_purchases_deliver(so_module *mod, void *env, void *thiz) {
  fn_setup setup;
  fn_query query;
  void *array;

  setup = (fn_setup)so_try_find_addr_rx(mod,
            "Java_com_king_store_GooglePlayIABv3Lib_onSetupFinished");
  query = (fn_query)so_try_find_addr_rx(mod,
            "Java_com_king_store_GooglePlayIABv3Lib_onQueryPurchasesFinished");

  if (!setup || !query) {
    /* Not fatal, and worth saying plainly rather than failing: a different
     * build of the game may not carry this store at all, and the game is
     * perfectly playable without any of it. */
    LOGW("this build has no GooglePlayIABv3Lib entry points; "
         "purchases.txt cannot be delivered");
    return;
  }

  array = pps_purchases_build_array();
  if (!array) { LOGW("could not build the Purchase array"); return; }

  LOGB("store: onSetupFinished(0, 1)");
  setup(env, thiz, 0, 1);

  LOGB("store: onQueryPurchasesFinished(0, Purchase[%d])", g_nowned);
  query(env, thiz, 0, array);
}
