/*
 * src/core/manager.h -- forms-module manager-thread state.
 *
 * Today this header exposes only the public state-machine enum.  The
 * rest of the manager (Win32 message-only window, command queue,
 * window-class registration, manager_thread_main, mgr_shutdown,
 * ensure_manager, etc.) is still inlined in forms_module.c because
 * it's deeply coupled to the Win32 backend.  Phase 1 of the rewrite
 * splits the backend into src/backend/win32_*.c, at which point the
 * remaining manager state can move into manager.{c,h} alongside it.
 *
 * Pulled out separately now so other core TUs (and the upcoming
 * dispatch TU) can read the state values without dragging the entire
 * Win32 manager surface in.
 */

#ifndef CANDO_FORMS_CORE_MANAGER_H
#define CANDO_FORMS_CORE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle of the dedicated UI thread that owns every HWND we
 * create.  Stored in a global atomic_int that the main thread spins
 * on during ensure_manager() / mgr_shutdown(). */
typedef enum {
    MGR_UNSTARTED = 0,
    MGR_STARTING,
    MGR_RUNNING,
    MGR_INIT_FAILED,
    MGR_STOPPING,
    MGR_STOPPED,
} ManagerState;

#ifdef __cplusplus
}
#endif

#endif /* CANDO_FORMS_CORE_MANAGER_H */
