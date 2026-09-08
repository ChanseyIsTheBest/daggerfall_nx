/* pps_purchases.h -- in-app content you own. MIT licensed.
 *
 * Reads <gamedir>/purchases.txt and delivers each entitlement through the
 * engine's own restore path -- the same GooglePlayIABv3Lib.onQueryPurchases-
 * Finished(0, Purchase[]) call Google Play's restore flow would produce.
 *
 * Gold bars are consumable, so every uncommented line is delivered again on
 * every launch. See the long note at the top of the .c file.
 */
#ifndef PPS_PURCHASES_H
#define PPS_PURCHASES_H

/* Create purchases.txt with everything commented out, if it is not already
 * there. Never overwrites. Returns 1 if it wrote one. */
int pps_purchases_ensure_file(void);

/* How many entitlements purchases.txt lists. 0 means there is nothing to
 * restore and main.c can skip the store handshake entirely. */
int pps_purchases_count(void);

/* The Purchase[] to hand to onQueryPurchasesFinished. */
void *pps_purchases_build_array(void);

/* Make the two calls Google Play's restore flow would have made. Call after
 * create() and after the engine has been told it is active -- the store
 * callbacks land on a queue that only drains once it is. */
struct so_module;
void pps_purchases_deliver(struct so_module *mod, void *env, void *thiz);

#endif
