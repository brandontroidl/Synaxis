/*
 * pycompat.h — Python 3.10+ compatibility for Synaxis mod-python
 *
 * Synaxis requires Python 3.10 or later. Python 2 is not supported.
 *
 * The original X3 mod-python used Python 2 APIs (PyString, PyInt,
 * PyCObject, Py_InitModule). This header maps those calls to their
 * Python 3 equivalents so mod-python.c compiles without rewriting
 * every call site.
 */
#ifndef SYNAXIS_PYCOMPAT_H
#define SYNAXIS_PYCOMPAT_H

#include <Python.h>

#if PY_MAJOR_VERSION < 3 || (PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION < 10)
  #error "Synaxis mod-python requires Python 3.10 or later"
#endif

/* PyString removed in Python 3 — use PyUnicode */
#define PyString_FromString     PyUnicode_FromString
#define PyString_AsString       PyUnicode_AsUTF8
#define PyString_AS_STRING      PyUnicode_AsUTF8
#define PyString_Check          PyUnicode_Check
#define PyString_FromFormat     PyUnicode_FromFormat

/* PyInt merged into PyLong in Python 3 */
#define PyInt_FromLong          PyLong_FromLong
#define PyInt_AsLong            PyLong_AsLong
#define PyInt_Check             PyLong_Check

/* PyCObject removed in Python 3.2 — use PyCapsule */
#define PyCObject_FromVoidPtr(p, d)  PyCapsule_New((p), NULL, NULL)
#define PyCObject_AsVoidPtr(o)       PyCapsule_GetPointer((o), NULL)

/* Module initialization — Python 3 uses PyModule_Create */
static struct PyModuleDef EmbModule = {
    PyModuleDef_HEAD_INIT,
    "_svc", NULL, -1, NULL
};

#define Py_InitModule(name, methods) \
    ( EmbModule.m_methods = (methods), PyModule_Create(&EmbModule) )

#endif /* SYNAXIS_PYCOMPAT_H */
