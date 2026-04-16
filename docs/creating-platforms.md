# Creating Platforms

A *platform* is a host application that embeds CanDo and exposes a
scripting API to end-users — game engines, mod loaders, automation
frameworks, and similar tools.  This guide walks through the design
patterns with a minimal game-loop example.

For embedding fundamentals, see [embedding.md](embedding.md).

## Architecture

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
│  │  (one VM per tenant)     │   │
│  └────────────┬─────────────┘   │
│               │ dofile/dostring  │
│  ┌────────────▼─────────────┐   │
│  │      CanDo Scripts       │   │
│  │  (mods, game logic, UI)  │   │
│  └──────────────────────────┘   │
└─────────────────────────────────┘
```

The host controls what scripts can reach.  Only functions you register
with `cando_vm_register_native` are visible.

## Minimal game loop

```c
#include <cando.h>
#include <stdio.h>
#include <stdbool.h>

/* --- Platform APIs exposed to scripts --- */

static int native_draw_sprite(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 3 || !cando_is_number(args[0])
                  || !cando_is_number(args[1])
                  || !cando_is_string(args[2])) {
        cando_vm_error(vm, "draw.sprite: expected (x, y, name)");
        return -1;
    }
    double x         = args[0].as.number;
    double y         = args[1].as.number;
    const char *name = args[2].as.string->data;
    /* call your renderer here */
    printf("draw '%s' at (%.0f, %.0f)\n", name, x, y);
    return 0;
}

static int native_input_key(CandoVM *vm, int argc, CandoValue *args) {
    if (argc < 1 || !cando_is_string(args[0])) {
        cando_vm_error(vm, "input.key: expected key name");
        return -1;
    }
    const char *key = args[0].as.string->data;
    bool pressed = /* query your input system */ false;
    (void)key;
    cando_vm_push(vm, cando_bool(pressed));
    return 1;
}

/* --- Setup --- */

static void register_platform(CandoVM *vm) {
    cando_vm_register_native(vm, "draw.sprite", native_draw_sprite);
    cando_vm_register_native(vm, "input.key",   native_input_key);
}

/* --- Main loop --- */

int main(void) {
    CandoVM *vm = cando_open();

    /* Only load safe standard libraries */
    cando_open_mathlib(vm);
    cando_open_stringlib(vm);
    cando_open_arraylib(vm);
    cando_open_objectlib(vm);

    register_platform(vm);

    /* Load the game script (runs top-level code) */
    if (cando_dofile(vm, "game/main.cdo") != CANDO_OK) {
        fprintf(stderr, "load: %s\n", cando_errmsg(vm));
        cando_close(vm);
        return 1;
    }

    /* Retrieve lifecycle callbacks the script defined as globals */
    CandoValue fn_update, fn_draw;
    bool has_update = cando_vm_get_global(vm, "update", &fn_update);
    bool has_draw   = cando_vm_get_global(vm, "draw",   &fn_draw);

    double dt = 1.0 / 60.0;

    for (int frame = 0; frame < 100; frame++) {
        if (has_update) {
            CandoValue arg = cando_number(dt);
            cando_vm_call_value(vm, fn_update, &arg, 1);
        }
        if (has_draw) {
            cando_vm_call_value(vm, fn_draw, NULL, 0);
        }
    }

    cando_close(vm);
    return 0;
}
```

The game script (`game/main.cdo`):

```cando
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

## Registering platform APIs

Use dot notation in the registered name to create namespaces:

```c
cando_vm_register_native(vm, "audio.play",   native_audio_play);
cando_vm_register_native(vm, "audio.stop",   native_audio_stop);
cando_vm_register_native(vm, "audio.volume", native_audio_volume);
```

Scripts call them as `audio.play("bgm.ogg")`, `audio.stop()`, etc.

For larger APIs you can also define a wrapper layer in CanDo itself.
Load it with `cando_dofile` before loading user scripts:

```c
cando_dofile(vm, "platform/bindings.cdo");   /* validation, docs */
cando_dofile(vm, "user/mod.cdo");
```

## Lifecycle callbacks

The general pattern:

1. Run the script with `cando_dofile` — top-level code does setup.
2. Retrieve callback functions with `cando_vm_get_global`.
3. Call them with `cando_vm_call_value` in your host loop.

```c
CandoValue on_load, on_update, on_event;
bool has_load   = cando_vm_get_global(vm, "onLoad",   &on_load);
bool has_update = cando_vm_get_global(vm, "onUpdate", &on_update);
bool has_event  = cando_vm_get_global(vm, "onEvent",  &on_event);

/* Fire onLoad once */
if (has_load)
    cando_vm_call_value(vm, on_load, NULL, 0);

/* Fire onUpdate(dt) every frame */
if (has_update) {
    CandoValue arg = cando_number(delta_time);
    cando_vm_call_value(vm, on_update, &arg, 1);
}

/* Fire onEvent(name, data) on demand */
if (has_event) {
    CandoValue args[2] = {
        cando_string_value(cando_string_new("keydown", 7)),
        cando_string_value(cando_string_new("Space", 5)),
    };
    cando_vm_call_value(vm, on_event, args, 2);
    cando_value_release(args[0]);
    cando_value_release(args[1]);
}
```

`cando_vm_call_value` returns the number of values the function pushed
onto the stack (0 if the value is not callable).  Errors from inside the
script are reported through `cando_errmsg`.

## Loading mods

For mod systems, each mod can either share a single VM or get its own:

### Shared VM (mods can interact)

```c
const char *mods[] = { "mods/combat.cdo", "mods/ui.cdo", NULL };
for (int i = 0; mods[i]; i++) {
    int rc = cando_dofile(vm, mods[i]);
    if (rc != CANDO_OK)
        fprintf(stderr, "[%s] %s\n", mods[i], cando_errmsg(vm));
}
```

### Isolated VMs (mods cannot interfere)

```c
CandoVM *mod_vms[MAX_MODS];
for (int i = 0; mods[i]; i++) {
    mod_vms[i] = cando_open();
    cando_open_mathlib(mod_vms[i]);
    register_platform(mod_vms[i]);

    if (cando_dofile(mod_vms[i], mods[i]) != CANDO_OK) {
        fprintf(stderr, "[%s] %s\n", mods[i], cando_errmsg(mod_vms[i]));
        cando_close(mod_vms[i]);
        mod_vms[i] = NULL;
    }
}
```

## Hot-reload

Reload a script during development without restarting the host:

```c
void reload_script(const char *path) {
    cando_close(vm);
    vm = cando_open();
    register_platform(vm);
    cando_open_mathlib(vm);
    cando_open_stringlib(vm);

    if (cando_dofile(vm, path) != CANDO_OK) {
        fprintf(stderr, "reload: %s\n", cando_errmsg(vm));
        cando_close(vm);
        vm = NULL;
        return;
    }
    /* re-fetch callbacks */
    cando_vm_get_global(vm, "update", &fn_update);
    cando_vm_get_global(vm, "draw",   &fn_draw);
}
```

Trigger on a filesystem watch event or a dev-mode key.  Because a new VM
starts with clean state, all global variables reset to their initial values
defined in the script.

## Sandboxing untrusted scripts

Only open the libraries you trust:

```c
CandoVM *sandbox = cando_open();

/* Safe: pure computation and data structures */
cando_open_mathlib(sandbox);
cando_open_stringlib(sandbox);
cando_open_arraylib(sandbox);
cando_open_objectlib(sandbox);
cando_open_jsonlib(sandbox);

/* NOT opened: file, os, process, net, http, https, include, eval */
```

Without those libraries the script has no access to the filesystem,
network, or shell.  It can only use math, strings, arrays, objects, and
JSON — plus whatever platform natives you register.

## Error recovery

Script errors never crash the host.  After any non-`CANDO_OK` result,
log the error and continue:

```c
int rc = cando_dofile(vm, "mod.cdo");
if (rc != CANDO_OK) {
    fprintf(stderr, "[mod] %s\n", cando_errmsg(vm));
    /* Host continues; VM is still usable */
}
```

For per-frame callback errors, check `cando_errmsg` after each call and
decide whether to skip the frame, disable the mod, or reload the script:

```c
for (;;) {
    CandoValue arg = cando_number(dt);
    int n = cando_vm_call_value(vm, fn_update, &arg, 1);
    if (n == 0) {
        /* fn_update was not callable or an error occurred */
        const char *err = cando_errmsg(vm);
        if (err[0] != '\0')
            fprintf(stderr, "update: %s (skipping frame)\n", err);
    }
    render_frame();
}
```

## Passing structured data

Convert C structs to CanDo objects through the bridge:

```c
#include "vm/bridge.h"
#include "object/object.h"
#include "object/string.h"
#include "object/value.h"

typedef struct { double x, y; } Vec2;

static CandoValue vec2_to_cando(CandoVM *vm, Vec2 v) {
    CandoValue val = cando_bridge_new_object(vm);
    CdoObject *o   = cando_bridge_resolve(vm, val.as.handle);

    CdoString *kx = cdo_string_intern("x", 1);
    CdoString *ky = cdo_string_intern("y", 1);
    cdo_object_rawset(o, kx, cdo_number(v.x), FIELD_NONE);
    cdo_object_rawset(o, ky, cdo_number(v.y), FIELD_NONE);
    cdo_string_release(kx);
    cdo_string_release(ky);

    return val;
}

static int native_get_position(CandoVM *vm, int argc, CandoValue *args) {
    Vec2 pos = get_player_position();
    cando_vm_push(vm, vec2_to_cando(vm, pos));
    return 1;
}
```

```cando
VAR pos = get_position();
print(pos.x, pos.y);
```
