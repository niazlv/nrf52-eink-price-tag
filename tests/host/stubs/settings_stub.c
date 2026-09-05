#include <zephyr/settings/settings.h>
#include <string.h>

settings_stub_set_fn settings_stub_set;
char   settings_stub_last_name[32];
unsigned char settings_stub_last_value[256];
size_t settings_stub_last_len;
int    settings_stub_save_count;

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	strncpy(settings_stub_last_name, name, sizeof(settings_stub_last_name) - 1);
	settings_stub_last_name[sizeof(settings_stub_last_name) - 1] = '\0';
	settings_stub_last_len = val_len < sizeof(settings_stub_last_value)
				 ? val_len : sizeof(settings_stub_last_value);
	memcpy(settings_stub_last_value, value, settings_stub_last_len);
	settings_stub_save_count++;
	return 0;
}

/* The modules only ever call this with next == NULL, i.e. exact match. */
int settings_name_steq(const char *name, const char *key, const char **next)
{
	if (next) {
		*next = NULL;
	}
	return strcmp(name, key) == 0;
}

struct blob { const unsigned char *p; size_t len; };

static ssize_t read_blob(void *cb_arg, void *data, size_t len)
{
	struct blob *b = cb_arg;
	size_t n = len < b->len ? len : b->len;

	memcpy(data, b->p, n);
	return (ssize_t)n;
}

int settings_stub_load(const char *key, const void *blob, size_t len)
{
	struct blob b = { blob, len };

	return settings_stub_set ? settings_stub_set(key, len, read_blob, &b) : -1;
}
