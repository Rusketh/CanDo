/*
 * src/controls/ctl_treeview.h -- TreeView native methods.
 *
 * Phase 3.1 of the rewrite (REWRITE_PLAN.md / API_SPEC.md §7.3).
 * Wraps Win32's SysTreeView32.
 *
 * v1 surface uses opaque numeric handles (HTREEITEM cast through
 * uintptr_t -> double) rather than first-class Node instances.
 * Scripts pass the handle from addNode back to follow-up calls
 * (expandNode, removeNode, getNodeText, ...).  A future Phase 3.1b
 * can layer Node instance objects on top without breaking the v1 API.
 */

#ifndef CANDO_FORMS_CONTROLS_TREEVIEW_H
#define CANDO_FORMS_CONTROLS_TREEVIEW_H

#ifndef FORMS_MODULE_TEST_BUILD

#include "../core/cando_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TreeView.addNode(parentHandle | NULL, text) -> handle (number).
 * Inserts as the last child of `parent`, or at the root when
 * parent is NULL / 0.  Returns 0 on failure. */
int native_tree_add_node(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.removeNode(handle).  No-op if handle is 0. */
int native_tree_remove_node(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.clearNodes() -- TVM_DELETEITEM with TVI_ROOT. */
int native_tree_clear_nodes(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.getSelectedNode() -> handle (number).  0 if none. */
int native_tree_get_selected_node(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.setSelectedNode(handle). */
int native_tree_set_selected_node(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.expandNode(handle) / collapseNode(handle). */
int native_tree_expand_node(CandoVM *vm, int argc, CandoValue *args);
int native_tree_collapse_node(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.getNodeText(handle) -> string.  NULL on bad handle. */
int native_tree_get_node_text(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.setNodeText(handle, text). */
int native_tree_set_node_text(CandoVM *vm, int argc, CandoValue *args);

/* TreeView.getNodeCount() -> total number of items in the tree. */
int native_tree_get_node_count(CandoVM *vm, int argc, CandoValue *args);

#ifdef __cplusplus
}
#endif

#endif /* !FORMS_MODULE_TEST_BUILD */

#endif /* CANDO_FORMS_CONTROLS_TREEVIEW_H */
