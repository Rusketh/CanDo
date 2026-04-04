/*
 * bridge.h -- Conversion layer between CandoValue (VM) and CdoValue (objects).
 *
 * The VM uses CandoValue with HandleIndex for TYPE_OBJECT; the object layer
 * uses CdoValue with raw CdoObject*.  This bridge provides helpers to convert
 * between the two representations via the VM's CandoHandleTable.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CANDO_BRIDGE_H
#define CANDO_BRIDGE_H

#include "vm.h"
#include "../object/object.h"
#include "../object/array.h"

/*
 * cando_bridge_resolve -- resolve a HandleIndex to CdoObject*.
 * Asserts that h is a valid, live handle.
 */
CdoObject *cando_bridge_resolve(CandoVM *vm, HandleIndex h);

/*
 * cando_bridge_new_object -- allocate a plain CdoObject, register in the
 * handle table, and return a CandoValue of TYPE_OBJECT.
 */
CandoValue cando_bridge_new_object(CandoVM *vm);

/*
 * cando_bridge_new_array -- allocate a CdoObject array, register in the
 * handle table, and return a CandoValue of TYPE_OBJECT.
 */
CandoValue cando_bridge_new_array(CandoVM *vm);

/*
 * cando_bridge_intern_key -- intern a CandoString's data as a CdoString
 * suitable for use as an object field key.
 * Returns a retained CdoString*; caller must call cdo_string_release().
 */
CdoString *cando_bridge_intern_key(CandoString *cs);

/*
 * cando_bridge_to_cando -- convert an object-layer CdoValue to a VM CandoValue.
 * For object subtypes a new handle is allocated in vm->handles.
 */
CandoValue cando_bridge_to_cando(CandoVM *vm, CdoValue v);

/*
 * cando_bridge_to_cdo -- convert a VM CandoValue to an object-layer CdoValue.
 * For TYPE_OBJECT the handle is resolved to CdoObject*.
 */
CdoValue cando_bridge_to_cdo(CandoVM *vm, CandoValue v);

#endif /* CANDO_BRIDGE_H */
