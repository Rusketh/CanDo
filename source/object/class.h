/*
 * class.h -- CLASS keyword scaffolding for Cando.
 *
 * cdo_class_new() constructs a class object with:
 *   __type  = type_name  (FIELD_STATIC -- immutable class identity)
 *   __index = proto      (prototype chain for instance lookup)
 *
 * The __call constructor must be attached separately by the compiler.
 *
 * Must compile with gcc -std=c11.
 */

#ifndef CDO_CLASS_H
#define CDO_CLASS_H

#include "object.h"

/* -----------------------------------------------------------------------
 * Class construction
 * --------------------------------------------------------------------- */

/*
 * cdo_class_new -- allocate a class scaffold object.
 *
 * type_name: the class's string type tag (stored as FIELD_STATIC __type).
 *            May be NULL if the class is anonymous.
 * proto:     the prototype object for __index inheritance.
 *            May be NULL if the class has no parent.
 *
 * Returns a plain CdoObject (OBJ_OBJECT kind) with __type and optionally
 * __index pre-populated.  The caller adds __call to complete the class.
 */
CdoObject *cdo_class_new(CdoString *type_name, CdoObject *proto);

#endif /* CDO_CLASS_H */
