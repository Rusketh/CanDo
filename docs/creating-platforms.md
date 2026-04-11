# Creating Platforms with CanDo

A *platform* is a host application that exposes a CanDo-based scripting API
to end-users — game engines, mod loaders, automation frameworks, and similar
tools.  This document shows how to build one, using a minimal game loop
(love2d-style) as the running example.

---

## Table of Contents

1. [Architecture overview](#architecture-overview)
2. [Minimal game loop](#minimal-game-loop)
3. [Registering platform APIs](#registering-platform-apis)
4. [Lifecycle callbacks](#lifecycle-callbacks)
5. [Loading mods and plugins](#loading-mods-and-plugins)
6. [Hot-reload](#hot-reload)
7. [Sandboxing untrusted scripts](#sandboxing-untrusted-scripts)
8. [Resource management with finalizers](#resource-management-with-finalizers)
9. [Passing complex data structures](#passing-complex-data-structures)
10. [Error recovery](#error-recovery)

---

## Architecture overview

```
┌─────────────────────────────────┐
│         Host Application        │
│  (C/C++ — your game or tool)    │
│                                 │
│  ┌──────────────────────────┐   │
│  │      Platform API        │   │
│  │  registered as natives   │   │
│  └────────────┬─────────────┘   │
│               │ cando_vm_*      │
│  ┌────────────▼─────────────┐   │
│  │        CandoVM           │   │
│  │  (one VM per "tenant")   │   │
│  └────────────┬─────────────┘   │
│               │ dofile/dostring  │
│  ┌────────────▼─────────────┐   │
│  │      CanDo Scripts       │   │
│  │  (mods, game logic, UI)  │   │
│  └──────────────────────────┘   │
└─────────────────────────────────┘
```

The host controls what native functions are visible.  Scripts can only call
what you explicitly register.

---

## Minimal game loop

```c
#include <cando.h>
#include <stdio.h>
#include <stdbool.h>

/* --- Platform APIs exposed to scripts --- */

static int native_draw_sprite(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 3) { cando_vm_error(vm, "draw.sprite: x, y, name required"); return -1; }
    double x    = args[0].as.number;
    double y    = args[1].as.number;
    const char *name = args[2].as.string->data;
    /* call your renderer here */
    printf("draw sprite '%s' at (%.0f, %.0f)\n", name, x, y);
    return 0;
}

static int native_input_key(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1) { cando_vm_error(vm, "input.key: name required"); return -1; }
    const char *key = args[0].as.string->data;
    bool pressed = /* query your input system */ false;
    cando_vm_push(vm, cando_bool(pressed));
    return 1;
}

/* --- Platform setup --- */

static void register_platform_api(CandoVM *vm) {
    /* Expose draw.* namespace */
    cando_vm_register_native(vm, "draw.sprite", native_draw_sprite);

    /* Expose input.* namespace */
    cando_vm_register_native(vm, "input.key", native_input_key);
}

/* --- Game loop --- */

int main(void) {
    CandoVM *vm = cando_open();

    /* Load only safe standard libraries */
    cando_open_mathlib(vm);
    cando_open_stringlib(vm);
    cando_open_arraylib(vm);
    cando_open_objectlib(vm);

    /* Expose platform APIs */
    register_platform_api(vm);

    /* Load the game script — runs the top-level "load" section */
    if (cando_dofile(vm, "game/main.cdo") != CANDO_OK) {
        fprintf(stderr, "load error: %s\n", cando_errmsg(vm));
        cando_close(vm);
        return 1;
    }

    /* Retrieve lifecycle callbacks defined by the script */
    CandoValue fn_update, fn_draw;
    cando_vm_get_global(vm, "update", &fn_update);
    cando_vm_get_global(vm, "draw",   &fn_draw);

    /* Main loop */
    bool running = true;
    double delta = 1.0 / 60.0;  /* simulate 60 fps */
    int frame = 0;

    while (running && frame < 100) {
        /* Call update(dt) */
        CandoValue dt_arg = cando_number(delta);
        if (cando_vm_call_value(vm, fn_update, &dt_arg, 1) < 0) {
            fprintf(stderr, "update error: %s\n", cando_errmsg(vm));
            running = false;
        }

        /* Call draw() */
        if (cando_vm_call_value(vm, fn_draw, NULL, 0) < 0) {
            fprintf(stderr, "draw error: %s\n", cando_errmsg(vm));
            running = false;
        }

        frame++;
    }

    cando_close(vm);
    return 0;
}
```

The game script (`game/main.cdo`):
```cando
// game/main.cdo — entry point loaded by the platform

VAR x = 100;
VAR y = 100;

FUNCTION update(dt) {
    IF input.key("right") { x = x + 200 * dt; }
    IF input.key("left")  { x = x - 200 * dt; }
}

FUNCTION draw() {
    draw.sprite(x, y, "player");
}
```

---

## Registering platform APIs

Any C function registered with `cando_vm_register_native` becomes a global
in CanDo.  Use dot notation in the name to create namespaces:

```c
// All registered as globals in the VM:
cando_vm_register_native(vm, "audio.play",  native_audio_play);
cando_vm_register_native(vm, "audio.stop",  native_audio_stop);
cando_vm_register_native(vm, "audio.volume",native_audio_volume);
```

In CanDo they appear as `audio.play(...)`, `audio.stop()`, etc.

### Bundling a module at runtime

For larger APIs, define them in a `.cdo` file and load it before loading
user scripts:

```c
// Load platform bindings defined in CanDo itself
cando_dofile(vm, "platform/bindings.cdo");

// Then load the user's script
cando_dofile(vm, "user/mod.cdo");
```

`bindings.cdo` can wrap the native functions to provide argument validation,
documentation, and error messages that feel native to CanDo.

---

## Lifecycle callbacks

The pattern is:

1. Load the script with `cando_dofile` — this runs the top-level code (setup)
2. Look up callback functions with `cando_vm_get_global`
3. Call them repeatedly with `cando_vm_call_value`

```c
/* After dofile: retrieve callbacks */
CandoValue on_load, on_update, on_event;
cando_vm_get_global(vm, "onLoad",   &on_load);
cando_vm_get_global(vm, "onUpdate", &on_update);
cando_vm_get_global(vm, "onEvent",  &on_event);

/* Fire onLoad() once */
cando_vm_call_value(vm, on_load, NULL, 0);

/* Fire onUpdate(dt) every frame */
CandoValue dt = cando_number(delta_time);
cando_vm_call_value(vm, on_update, &dt, 1);

/* Fire onEvent(eventName, data) when events occur */
CandoValue event_args[2] = {
    cando_string_new("keydown"),
    cando_string_new("Space")
};
cando_vm_call_value(vm, on_event, event_args, 2);
```

### Checking if a callback exists

```c
CandoValue cb;
if (cando_vm_get_global(vm, "onResume", &cb)) {
    cando_vm_call_value(vm, cb, NULL, 0);
}
```

---

## Loading mods and plugins

For mod systems where users can supply their own scripts:

```c
void load_mod(CandoVM *vm, const char *mod_path) {
    int rc = cando_dofile(vm, mod_path);
    if (rc == CANDO_ERR_FILE)
        fprintf(stderr, "mod not found: %s\n", mod_path);
    else if (rc == CANDO_ERR_PARSE)
        fprintf(stderr, "mod syntax error [%s]: %s\n", mod_path, cando_errmsg(vm));
    else if (rc == CANDO_ERR_RUNTIME)
        fprintf(stderr, "mod runtime error [%s]: %s\n", mod_path, cando_errmsg(vm));
}

/* Load all mods in a directory */
const char *mods[] = { "mods/combat.cdo", "mods/ui.cdo", NULL };
for (int i = 0; mods[i]; i++)
    load_mod(vm, mods[i]);
```

For isolation, give each mod its own VM:

```c
/* Isolated VMs — mods cannot interfere with each other */
for (int i = 0; mods[i]; i++) {
    CandoVM *mod_vm = cando_open();
    /* Expose only the APIs relevant to this mod */
    cando_open_mathlib(mod_vm);
    register_platform_api(mod_vm);
    load_mod(mod_vm, mods[i]);
    /* Store mod_vm for later callbacks */
}
```

---

## Hot-reload

Reload a script during runtime without restarting the host:

```c
static CandoVM *script_vm = NULL;

void reload_script(const char *path) {
    if (script_vm) {
        cando_close(script_vm);
    }
    script_vm = cando_open();
    register_platform_api(script_vm);
    cando_open_mathlib(script_vm);

    if (cando_dofile(script_vm, path) != CANDO_OK) {
        fprintf(stderr, "reload error: %s\n", cando_errmsg(script_vm));
        cando_close(script_vm);
        script_vm = NULL;
    } else {
        printf("script reloaded: %s\n", path);
    }
}
```

Trigger `reload_script` on a filesystem watch event or a keyboard shortcut
during development.

---

## Sandboxing untrusted scripts

To run scripts from untrusted sources, only open the safe standard libraries:

```c
CandoVM *sandbox_vm = cando_open();

/* Safe: pure computation and data manipulation */
cando_open_mathlib(sandbox_vm);
cando_open_stringlib(sandbox_vm);
cando_open_arraylib(sandbox_vm);
cando_open_objectlib(sandbox_vm);
cando_open_jsonlib(sandbox_vm);

/* NOT opened: filelib, processlib, netlib, oslib, includelib */

/* Register only the platform APIs you want to expose */
cando_vm_register_native(sandbox_vm, "game.getScore", native_get_score);
```

Scripts in this sandbox cannot read files, run processes, or make network
requests — they can only use the math, string, array, object, and JSON
utilities, plus whatever your platform exposes.

---

## Resource management with finalizers

If a CanDo script holds a reference to a C resource (texture, sound, socket),
you need to free it when the script-side object is garbage collected.

CanDo supports `__gc` meta-methods on objects.  Register a native function
and set it as `__gc` on the object:

```c
typedef struct TextureHandle {
    int   id;
    int   width, height;
} TextureHandle;

static int native_texture_load(CandoVM *vm, int argc, CandoValue *args) {
    const char *path = args[0].as.string->data;

    /* Allocate a CanDo object to hold the texture */
    CandoValue obj = cando_bridge_new_object(vm);
    CdoObject *o   = cando_bridge_resolve(vm, obj.as.handle);

    /* Store texture ID in an object field */
    /* ... set fields using cdo_object_set ... */

    cando_vm_push(vm, obj);
    return 1;
}
```

See [c-api.md](c-api.md) and [writing-extensions.md](writing-extensions.md)
for details on working with the object layer.

---

## Passing complex data structures

### C struct → CanDo object

```c
typedef struct Vec2 { double x, y; } Vec2;

CandoValue vec2_to_cando(CandoVM *vm, Vec2 v) {
    CandoValue obj = cando_bridge_new_object(vm);
    CdoObject *o   = cando_bridge_resolve(vm, obj.as.handle);

    CdoString *kx = cando_bridge_intern_key(cando_string_new("x"));
    CdoString *ky = cando_bridge_intern_key(cando_string_new("y"));

    CdoValue cx = { .type = CDO_NUMBER, .as.number = v.x };
    CdoValue cy = { .type = CDO_NUMBER, .as.number = v.y };

    cdo_object_set(o, kx, cx);
    cdo_object_set(o, ky, cy);

    cdo_string_release(kx);
    cdo_string_release(ky);

    return obj;
}

/* Pass a Vec2 to CanDo via a native function */
static int native_get_position(CandoVM *vm, int argc, CandoValue *args) {
    Vec2 pos = get_player_position();  /* your C function */
    cando_vm_push(vm, vec2_to_cando(vm, pos));
    return 1;
}
```

In CanDo:
```cando
VAR pos = get_position();
print(pos.x);  // 100.5
print(pos.y);  // 200.0
```

---

## Error recovery

Recover from script errors without crashing the host:

```c
int rc = cando_dofile(vm, "mod.cdo");
if (rc != CANDO_OK) {
    /* Log the error */
    fprintf(stderr, "[mod error] %s\n", cando_errmsg(vm));

    /* Continue running — the host is unaffected */
    /* Optionally disable this mod and continue */
    return;
}
```

For callback errors during the game loop:

```c
for (;;) {
    CandoValue dt = cando_number(delta);
    int rc = cando_vm_call_value(vm, fn_update, &dt, 1);
    if (rc < 0) {
        fprintf(stderr, "update error: %s (skipping frame)\n", cando_errmsg(vm));
        /* Reset error state and continue */
    }
    render_frame();
}
```

`cando_errmsg` always returns a valid string; after logging the error you can
continue using the same VM — the error state is cleared on the next successful
execution.
