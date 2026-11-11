#pragma once
#include <glib.h>

/* Call before GTK/GApplication starts threads. Never changes child environments. */
gboolean runtime_init_schemas(void);
