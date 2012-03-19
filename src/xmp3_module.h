/**
 * xmp3 - XMPP Proxy
 * Copyright (c) 2012 Drexel University
 *
 * @file xmp3_module.h
 * Defines structures and functions for XMP3 extension modules.
 */

#pragma once

#include <stdbool.h>

// Forward declarations
struct xmpp_server;
struct xmp3_modules;

/** Called to instantiate a new intance of the module. */
typedef void* (*xmp3_module_new)(void);

/** Called when the module should be deleted. */
typedef void (*xmp3_module_del)(void *module);

/** Called for each key=value pair in the config file for this module. */
typedef bool (*xmp3_module_conf)(void *module, const char *key,
                                 const char *value);

/** Called when the server is starting. */
typedef bool (*xmp3_module_start)(void *module, struct xmpp_server *server);

/** Called when the server is stopping. */
typedef bool (*xmp3_module_stop)(void *module);

/**
 * Structure representing an XMP3 extension module.
 *
 * Each module should define a global symbol named "XMP3_MODULE" containing
 * this structure, with the methods filled in.  XMP3 will call these functions
 * at the appropriate time.
 *
 * To load a module, define a section in a config file with the name of the
 * shared object containing the extension.  See the example config file.
 */
struct xmp3_module {
    xmp3_module_new mod_new;
    xmp3_module_del mod_del;
    xmp3_module_conf mod_conf;
    xmp3_module_start mod_start;
    xmp3_module_stop mod_stop;
};

struct xmp3_modules* xmp3_modules_new(void);

void xmp3_modules_del(struct xmp3_modules *modules);

bool xmp3_modules_load(struct xmp3_modules *modules, const char *path,
                       const char *name);

bool xmp3_modules_config(struct xmp3_modules *modules, const char *name,
                         const char *key, const char *value);

bool xmp3_modules_start(struct xmp3_modules *modules,
                        struct xmpp_server *server);

bool xmp3_modules_stop(struct xmp3_modules *modules);