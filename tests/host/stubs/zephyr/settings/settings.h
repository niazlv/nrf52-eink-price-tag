/* Host stand-in for <zephyr/settings/settings.h>.
 *
 * settings_save_one() records what was written (name + bytes) so a test can
 * assert on it; SETTINGS_STATIC_HANDLER_DEFINE captures the module's load
 * handler into settings_stub_set so a test can replay a stored blob through
 * it — that is how the old-layout migrations get exercised. */
#ifndef STUB_ZEPHYR_SETTINGS_H
#define STUB_ZEPHYR_SETTINGS_H

#include <stddef.h>
#include <sys/types.h>

typedef ssize_t (*settings_read_cb)(void *cb_arg, void *data, size_t len);

int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_name_steq(const char *name, const char *key, const char **next);

typedef int (*settings_stub_set_fn)(const char *name, size_t len,
				    settings_read_cb read_cb, void *cb_arg);
extern settings_stub_set_fn settings_stub_set;

/* What the last settings_save_one() wrote. */
extern char   settings_stub_last_name[32];
extern unsigned char settings_stub_last_value[256];
extern size_t settings_stub_last_len;
extern int    settings_stub_save_count;

/* Feed a blob to the captured handler as settings_load() would. */
int settings_stub_load(const char *key, const void *blob, size_t len);

#define SETTINGS_STATIC_HANDLER_DEFINE(_hname, _tree, _get, _set, _commit, _export) \
	static __attribute__((constructor)) void settings_stub_register_##_hname(void) \
	{ settings_stub_set = _set; }

#endif
