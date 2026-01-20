#include <stddef.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"

/*
** Thread-local storage for multi-state safety.
** Each thread gets its own cache to avoid corruption when multiple
** Lua states are used concurrently.
*/
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define CACHE_TLS _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define CACHE_TLS __thread
#elif defined(_MSC_VER)
#define CACHE_TLS __declspec(thread)
#else
#define CACHE_TLS  /* fallback: not thread-safe */
#endif

/*
** Macro to detect if TLS is actually available.
** Used for runtime warnings and introspection.
*/
#ifndef CACHE_TLS_AVAILABLE
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define CACHE_TLS_AVAILABLE 1
#elif defined(__GNUC__) || defined(__clang__)
#define CACHE_TLS_AVAILABLE 1
#elif defined(_MSC_VER)
#define CACHE_TLS_AVAILABLE 1
#else
#define CACHE_TLS_AVAILABLE 0
#endif
#endif

/*
** Source filename cache to avoid expensive lua_getinfo calls.
** Consecutive line hits usually come from the same function,
** so caching the last result provides significant speedup.
**
** We store the pointer directly instead of copying the string.
** This is safe because Lua interns all strings - the pointer
** returned by lua_getinfo points to Lua's interned string data
** which remains valid as long as the string is reachable.
*/
static CACHE_TLS struct {
    lua_Integer level;
    const char *filename;  /* Direct pointer to Lua interned string */
    int valid;
} source_cache = {0, NULL, 0};

/*
** Per-file cache for max line number, max hits, and table reference.
** Avoids Lua table operations on every line hit.
** Flushed when switching to a different file.
**
** We use pointer comparison for file switching detection since
** Lua strings are interned (same content = same pointer address).
**
** The file_data_ref caches a registry reference to the file_data table,
** allowing us to skip lua_getfield("data") and lua_getfield(filename)
** on same-file hits.
**
** We track runner.data via data_ptr (using lua_topointer) to detect
** when it's reassigned. This is an O(1) pointer comparison per call.
*/
static CACHE_TLS struct {
    const char *filename_ptr;  /* Pointer for O(1) comparison */
    int file_data_ref;         /* Registry reference to file_data table */
    const void *data_ptr;      /* Pointer to runner.data for change detection */
    lua_Integer max_line;
    lua_Integer max_hits;
    int dirty;  /* 1 if needs to be flushed to Lua */
} file_stats_cache = {NULL, LUA_NOREF, NULL, 0, 0, 0};

/*
** Line hit batch cache for tight loops.
** Instead of updating Lua tables on every line hit, we batch updates
** for the same line. This significantly reduces Lua API calls in tight loops.
*/
#define LINE_BATCH_SIZE 8
static CACHE_TLS struct {
    lua_Integer line;   /* Line number */
    lua_Integer count;  /* Accumulated hit count */
} line_batch[LINE_BATCH_SIZE];
static CACHE_TLS int line_batch_count = 0;

/*
** Flush line_batch to Lua table.
** Expects file_data table at stack index file_data_idx.
*/
static void flush_line_batch(lua_State *L, int file_data_idx) {
    int i;
    for (i = 0; i < line_batch_count; i++) {
        lua_Integer line = line_batch[i].line;
        lua_Integer batch_count = line_batch[i].count;
        lua_Integer current;

        lua_rawgeti(L, file_data_idx, line);
        current = lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_pushinteger(L, current + batch_count);
        lua_rawseti(L, file_data_idx, line);

        /* Update max_hits if needed. */
        if (current + batch_count > file_stats_cache.max_hits) {
            file_stats_cache.max_hits = current + batch_count;
            file_stats_cache.dirty = 1;
        }
    }
    line_batch_count = 0;
}

/*
** Flush file_stats_cache to Lua if dirty.
** Expects file_data table at stack index file_data_idx.
*/
static void flush_file_stats(lua_State *L, int file_data_idx) {
    if (!file_stats_cache.dirty) {
        return;
    }
    /* Update max in Lua table. */
    lua_pushinteger(L, file_stats_cache.max_line);
    lua_setfield(L, file_data_idx, "max");
    /* Update max_hits in Lua table. */
    lua_pushinteger(L, file_stats_cache.max_hits);
    lua_setfield(L, file_data_idx, "max_hits");
    file_stats_cache.dirty = 0;
}

static const char *get_source_filename(lua_State *L, lua_Integer level) {
    lua_Debug ar;
    const char *result;

    /* Fast path: return cached filename if level matches. */
    if (source_cache.valid && source_cache.level == level) {
        return source_cache.filename;
    }

    if (!lua_getstack(L, level - 1, &ar)) {
        /* Cache negative result. */
        source_cache.level = level;
        source_cache.filename = NULL;
        source_cache.valid = 1;
        return NULL;
    }

    lua_getinfo(L, "S", &ar);

    if (ar.source[0] == '@') {
        result = ar.source + 1;
    } else {
        /*
        ** Ignore Lua code loaded from raw strings,
        ** unless runner.configuration.codefromstrings is true.
        */
        int codefromstrings;
        lua_getfield(L, lua_upvalueindex(1), "configuration");
        lua_getfield(L, -1, "codefromstrings");
        codefromstrings = lua_toboolean(L, -1);
        lua_pop(L, 2);
        result = codefromstrings ? ar.source : NULL;
    }

    /*
    ** Cache the pointer directly.
    ** Lua interns all strings, so this pointer remains valid
    ** as long as the string is reachable (which it is, via runner.data).
    */
    source_cache.level = level;
    source_cache.filename = result;
    source_cache.valid = 1;

    return result;
}

static int l_debug_hook(lua_State *L) {
    const char *filename;
    lua_Integer line_nr;
    lua_Integer steps_after_save, save_step_size;
    lua_Integer level;

    line_nr = luaL_checkinteger(L, 2);
    level = luaL_optinteger(L, 3, 2);
    lua_settop(L, 0);

    /* Fast path: if we've confirmed initialization, skip the Lua check. */
    if (!lua_tointeger(L, lua_upvalueindex(5))) {
        lua_getfield(L, lua_upvalueindex(1), "initialized");
        if (!lua_toboolean(L, -1)) {
            return 0;
        }
        lua_pop(L, 1);
        /* Cache that we've confirmed initialization. */
        lua_pushinteger(L, 1);
        lua_replace(L, lua_upvalueindex(5));
    }

    filename = get_source_filename(L, level);

    if (filename == NULL) {
        return 0;
    }

    /*
    ** Get runner.data and check if it was reassigned using lua_topointer.
    ** This is O(1) pointer comparison, done every call for correctness.
    */
    lua_getfield(L, lua_upvalueindex(1), "data");
    {
        const void *data_ptr = lua_topointer(L, -1);
        if (data_ptr != file_stats_cache.data_ptr) {
            /* runner.data changed - flush and invalidate cache. */
            if (file_stats_cache.file_data_ref != LUA_NOREF) {
                /* Flush pending batched hits to old file_data before invalidating. */
                lua_rawgeti(L, LUA_REGISTRYINDEX, file_stats_cache.file_data_ref);
                flush_line_batch(L, lua_gettop(L));
                flush_file_stats(L, lua_gettop(L));
                lua_pop(L, 1);
                luaL_unref(L, LUA_REGISTRYINDEX, file_stats_cache.file_data_ref);
                file_stats_cache.file_data_ref = LUA_NOREF;
            }
            line_batch_count = 0;  /* Clear batch even if no ref */
            file_stats_cache.filename_ptr = NULL;
            file_stats_cache.dirty = 0;
            file_stats_cache.data_ptr = data_ptr;
        }
    }

    /*
    ** Fast path: if same file as last hit, use cached registry reference.
    ** This skips lua_getfield(filename) - we still need runner.data for new files.
    */
    if (file_stats_cache.filename_ptr == filename &&
        file_stats_cache.file_data_ref != LUA_NOREF) {
        /* Pop runner.data, get file_data directly from registry. */
        lua_pop(L, 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, file_stats_cache.file_data_ref);
    } else {
        /* Slow path: different file or no cache. */
        lua_getfield(L, -1, filename);

        if (!lua_istable(L, -1)) {
            /* New or ignored file. */
            lua_pop(L, 1);
            lua_getfield(L, lua_upvalueindex(2), filename);

            if (lua_toboolean(L, -1)) {
                /* Ignored file. */
                return 0;
            }

            lua_pop(L, 1);
            /* New file, have to call runner.file_included. */
            lua_getfield(L, lua_upvalueindex(1), "file_included");
            lua_pushstring(L, filename);
            lua_call(L, 1, 1);

            if (!lua_toboolean(L, -1)) {
                /* Remember this file as ignored. */
                lua_pushboolean(L, 1);
                lua_setfield(L, lua_upvalueindex(2), filename);
                return 0;
            }

            lua_pop(L, 1);

            /* Construct file data table for this new file. */
            lua_newtable(L);
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "max");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "max_hits");

            /* Copy file data table and save it as data[filename]. */
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, filename);
        }

        /*
        ** File changed: flush old cache and initialize for new file.
        */
        if (file_stats_cache.filename_ptr != filename) {
            /* Flush batched hits and file stats to old file_data. */
            if (file_stats_cache.file_data_ref != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, file_stats_cache.file_data_ref);
                flush_line_batch(L, lua_gettop(L));
                flush_file_stats(L, lua_gettop(L));
                lua_pop(L, 1);
                luaL_unref(L, LUA_REGISTRYINDEX, file_stats_cache.file_data_ref);
            }
            line_batch_count = 0;  /* Clear batch for new file */
            /* Store new file_data reference in registry. */
            lua_pushvalue(L, -1);  /* file_data is at top of stack */
            file_stats_cache.file_data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
            /* Update cache for new file. */
            file_stats_cache.filename_ptr = filename;
            /* Read current values from Lua. */
            lua_getfield(L, -1, "max");
            file_stats_cache.max_line = lua_tointeger(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "max_hits");
            file_stats_cache.max_hits = lua_tointeger(L, -1);
            lua_pop(L, 1);
            file_stats_cache.dirty = 0;
        }

        /* Pop runner.data, keep file_data at top. */
        lua_remove(L, -2);
    }

    /* Update max line number in cache. */
    if (line_nr > file_stats_cache.max_line) {
        file_stats_cache.max_line = line_nr;
        file_stats_cache.dirty = 1;
    }

    /*
    ** Batch line hits to reduce Lua API calls in tight loops.
    ** Check if this line is already in the batch.
    */
    {
        int i;
        int found = 0;
        for (i = 0; i < line_batch_count; i++) {
            if (line_batch[i].line == line_nr) {
                line_batch[i].count++;
                found = 1;
                break;
            }
        }
        if (!found) {
            /* Line not in batch - flush if full, then add. */
            if (line_batch_count >= LINE_BATCH_SIZE) {
                flush_line_batch(L, lua_gettop(L));
            }
            line_batch[line_batch_count].line = line_nr;
            line_batch[line_batch_count].count = 1;
            line_batch_count++;
        }
    }

    /* Handle runner.tick. */
    lua_getfield(L, lua_upvalueindex(1), "tick");

    if (!lua_toboolean(L, -1)) {
        return 0;
    }

    /* Use cached savestepsize from upvalue 4, or cache it on first access. */
    save_step_size = lua_tointeger(L, lua_upvalueindex(4));
    if (save_step_size == 0) {
        lua_getfield(L, lua_upvalueindex(1), "configuration");
        lua_getfield(L, -1, "savestepsize");
        save_step_size = lua_tointeger(L, -1);
        lua_pop(L, 2);
        /* Cache the value for subsequent calls. */
        lua_pushinteger(L, save_step_size);
        lua_replace(L, lua_upvalueindex(4));
    }

    steps_after_save = lua_tointeger(L, lua_upvalueindex(3)) + 1;

    if (steps_after_save == save_step_size) {
        int paused;
        steps_after_save = 0;

        /* Unless runner.paused, save data. */
        lua_getfield(L, lua_upvalueindex(1), "paused");
        paused = lua_toboolean(L, -1);
        lua_pop(L, 1);

        if (!paused) {
            /* Flush batched hits and file stats cache before saving. */
            if (file_stats_cache.file_data_ref != LUA_NOREF) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, file_stats_cache.file_data_ref);
                flush_line_batch(L, lua_gettop(L));
                flush_file_stats(L, lua_gettop(L));
                lua_pop(L, 1);
            }
            lua_getfield(L, lua_upvalueindex(1), "save_stats");
            lua_call(L, 0, 0);
        }
    }

    lua_pushinteger(L, steps_after_save);
    lua_replace(L, lua_upvalueindex(3));
    return 0;
}

int l_new_hook(lua_State *L) {
    /*
    ** Upvalues for the debug hook:
    ** 1: runner module (already on the stack),
    ** 2: ignored files set
    ** 3: steps counter
    ** 4: cached savestepsize (0 means not yet cached)
    ** 5: initialized flag (0 = not cached, 1 = confirmed initialized)
    */
    lua_settop(L, 1);
    lua_newtable(L);
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);  /* cached savestepsize, 0 = needs refresh */
    lua_pushinteger(L, 0);  /* initialized flag, 0 = not yet confirmed */
    lua_pushcclosure(L, l_debug_hook, 5);
    return 1;
}

int luaopen_cluacov_hook(lua_State *L) {
#if !CACHE_TLS_AVAILABLE
    fprintf(stderr,
        "Warning: cluacov.hook compiled without thread-local storage support.\n"
        "         Multi-threaded Lua usage may cause incorrect coverage data.\n");
#endif
    lua_newtable(L);
    lua_pushcfunction(L, l_new_hook);
    lua_setfield(L, -2, "new");
    /* Export TLS availability for introspection */
    lua_pushboolean(L, CACHE_TLS_AVAILABLE);
    lua_setfield(L, -2, "tls_available");
    return 1;
}
