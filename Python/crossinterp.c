
/* API for managing interactions between isolated interpreters */

#include "Python.h"
#include "pycore_ceval.h"         // _Py_simple_func
#include "pycore_crossinterp.h"   // struct _xid
#include "pycore_initconfig.h"    // _PyStatus_OK()
#include "pycore_pyerrors.h"      // _PyErr_Clear()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_weakref.h"       // _PyWeakref_GET_REF()


/***************************/
/* cross-interpreter calls */
/***************************/

int
_Py_CallInInterpreter(PyInterpreterState *interp,
                      _Py_simple_func func, void *arg)
{
    if (interp == _PyThreadState_GetCurrent()->interp) {
        return func(arg);
    }
    // XXX Emit a warning if this fails?
    _PyEval_AddPendingCall(interp, (_Py_pending_call_func)func, arg, 0);
    return 0;
}

int
_Py_CallInInterpreterAndRawFree(PyInterpreterState *interp,
                                _Py_simple_func func, void *arg)
{
    if (interp == _PyThreadState_GetCurrent()->interp) {
        int res = func(arg);
        PyMem_RawFree(arg);
        return res;
    }
    // XXX Emit a warning if this fails?
    _PyEval_AddPendingCall(interp, func, arg, _Py_PENDING_RAWFREE);
    return 0;
}


/**************************/
/* cross-interpreter data */
/**************************/

_PyCrossInterpreterData *
_PyCrossInterpreterData_New(void)
{
    _PyCrossInterpreterData *xid = PyMem_RawMalloc(
                                            sizeof(_PyCrossInterpreterData));
    if (xid == NULL) {
        PyErr_NoMemory();
    }
    return xid;
}

void
_PyCrossInterpreterData_Free(_PyCrossInterpreterData *xid)
{
    PyInterpreterState *interp = PyInterpreterState_Get();
    _PyCrossInterpreterData_Clear(interp, xid);
    PyMem_RawFree(xid);
}


/* exceptions */

static PyStatus
_init_not_shareable_error_type(PyInterpreterState *interp)
{
    const char *name = "_interpreters.NotShareableError";
    PyObject *base = PyExc_ValueError;
    PyObject *ns = NULL;
    PyObject *exctype = PyErr_NewException(name, base, ns);
    if (exctype == NULL) {
        PyErr_Clear();
        return _PyStatus_ERR("could not initialize NotShareableError");
    }

    interp->xi.PyExc_NotShareableError = exctype;
    return _PyStatus_OK();
}

static void
_fini_not_shareable_error_type(PyInterpreterState *interp)
{
    Py_CLEAR(interp->xi.PyExc_NotShareableError);
}

static PyObject *
_get_not_shareable_error_type(PyInterpreterState *interp)
{
    assert(interp->xi.PyExc_NotShareableError != NULL);
    return interp->xi.PyExc_NotShareableError;
}


/* defining cross-interpreter data */

static inline void
_xidata_init(_PyCrossInterpreterData *data)
{
    // If the value is being reused
    // then _xidata_clear() should have been called already.
    assert(data->data == NULL);
    assert(data->obj == NULL);
    *data = (_PyCrossInterpreterData){0};
    data->interpid = -1;
}

static inline void
_xidata_clear(_PyCrossInterpreterData *data)
{
    // _PyCrossInterpreterData only has two members that need to be
    // cleaned up, if set: "data" must be freed and "obj" must be decref'ed.
    // In both cases the original (owning) interpreter must be used,
    // which is the caller's responsibility to ensure.
    if (data->data != NULL) {
        if (data->free != NULL) {
            data->free(data->data);
        }
        data->data = NULL;
    }
    Py_CLEAR(data->obj);
}

void
_PyCrossInterpreterData_Init(_PyCrossInterpreterData *data,
                             PyInterpreterState *interp,
                             void *shared, PyObject *obj,
                             xid_newobjectfunc new_object)
{
    assert(data != NULL);
    assert(new_object != NULL);
    _xidata_init(data);
    data->data = shared;
    if (obj != NULL) {
        assert(interp != NULL);
        // released in _PyCrossInterpreterData_Clear()
        data->obj = Py_NewRef(obj);
    }
    // Ideally every object would know its owning interpreter.
    // Until then, we have to rely on the caller to identify it
    // (but we don't need it in all cases).
    data->interpid = (interp != NULL) ? interp->id : -1;
    data->new_object = new_object;
}

int
_PyCrossInterpreterData_InitWithSize(_PyCrossInterpreterData *data,
                                     PyInterpreterState *interp,
                                     const size_t size, PyObject *obj,
                                     xid_newobjectfunc new_object)
{
    assert(size > 0);
    // For now we always free the shared data in the same interpreter
    // where it was allocated, so the interpreter is required.
    assert(interp != NULL);
    _PyCrossInterpreterData_Init(data, interp, NULL, obj, new_object);
    data->data = PyMem_RawMalloc(size);
    if (data->data == NULL) {
        return -1;
    }
    data->free = PyMem_RawFree;
    return 0;
}

void
_PyCrossInterpreterData_Clear(PyInterpreterState *interp,
                              _PyCrossInterpreterData *data)
{
    assert(data != NULL);
    // This must be called in the owning interpreter.
    assert(interp == NULL
           || data->interpid == -1
           || data->interpid == interp->id);
    _xidata_clear(data);
}


/* using cross-interpreter data */

static int
_check_xidata(PyThreadState *tstate, _PyCrossInterpreterData *data)
{
    // data->data can be anything, including NULL, so we don't check it.

    // data->obj may be NULL, so we don't check it.

    if (data->interpid < 0) {
        _PyErr_SetString(tstate, PyExc_SystemError, "missing interp");
        return -1;
    }

    if (data->new_object == NULL) {
        _PyErr_SetString(tstate, PyExc_SystemError, "missing new_object func");
        return -1;
    }

    // data->free may be NULL, so we don't check it.

    return 0;
}

static crossinterpdatafunc _lookup_getdata_from_registry(
                                            PyInterpreterState *, PyObject *);

static crossinterpdatafunc
_lookup_getdata(PyInterpreterState *interp, PyObject *obj)
{
   /* Cross-interpreter objects are looked up by exact match on the class.
      We can reassess this policy when we move from a global registry to a
      tp_* slot. */
    return _lookup_getdata_from_registry(interp, obj);
}

crossinterpdatafunc
_PyCrossInterpreterData_Lookup(PyObject *obj)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return _lookup_getdata(interp, obj);
}

static inline void
_set_xid_lookup_failure(PyInterpreterState *interp,
                        PyObject *obj, const char *msg)
{
    PyObject *exctype = _get_not_shareable_error_type(interp);
    assert(exctype != NULL);
    if (msg != NULL) {
        assert(obj == NULL);
        PyErr_SetString(exctype, msg);
    }
    else if (obj == NULL) {
        PyErr_SetString(exctype,
                        "object does not support cross-interpreter data");
    }
    else {
        PyErr_Format(exctype,
                     "%S does not support cross-interpreter data", obj);
    }
}

int
_PyObject_CheckCrossInterpreterData(PyObject *obj)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    crossinterpdatafunc getdata = _lookup_getdata(interp, obj);
    if (getdata == NULL) {
        if (!PyErr_Occurred()) {
            _set_xid_lookup_failure(interp, obj, NULL);
        }
        return -1;
    }
    return 0;
}

int
_PyObject_GetCrossInterpreterData(PyObject *obj, _PyCrossInterpreterData *data)
{
    PyThreadState *tstate = _PyThreadState_GetCurrent();
#ifdef Py_DEBUG
    // The caller must hold the GIL
    _Py_EnsureTstateNotNULL(tstate);
#endif
    PyInterpreterState *interp = tstate->interp;

    // Reset data before re-populating.
    *data = (_PyCrossInterpreterData){0};
    data->interpid = -1;

    // Call the "getdata" func for the object.
    Py_INCREF(obj);
    crossinterpdatafunc getdata = _lookup_getdata(interp, obj);
    if (getdata == NULL) {
        Py_DECREF(obj);
        if (!PyErr_Occurred()) {
            _set_xid_lookup_failure(interp, obj, NULL);
        }
        return -1;
    }
    int res = getdata(tstate, obj, data);
    Py_DECREF(obj);
    if (res != 0) {
        return -1;
    }

    // Fill in the blanks and validate the result.
    data->interpid = interp->id;
    if (_check_xidata(tstate, data) != 0) {
        (void)_PyCrossInterpreterData_Release(data);
        return -1;
    }

    return 0;
}

PyObject *
_PyCrossInterpreterData_NewObject(_PyCrossInterpreterData *data)
{
    return data->new_object(data);
}

static int
_call_clear_xidata(void *data)
{
    _xidata_clear((_PyCrossInterpreterData *)data);
    return 0;
}

static int
_xidata_release(_PyCrossInterpreterData *data, int rawfree)
{
    if ((data->data == NULL || data->free == NULL) && data->obj == NULL) {
        // Nothing to release!
        if (rawfree) {
            PyMem_RawFree(data);
        }
        else {
            data->data = NULL;
        }
        return 0;
    }

    // Switch to the original interpreter.
    PyInterpreterState *interp = _PyInterpreterState_LookUpID(data->interpid);
    if (interp == NULL) {
        // The interpreter was already destroyed.
        // This function shouldn't have been called.
        // XXX Someone leaked some memory...
        assert(PyErr_Occurred());
        if (rawfree) {
            PyMem_RawFree(data);
        }
        return -1;
    }

    // "Release" the data and/or the object.
    if (rawfree) {
        return _Py_CallInInterpreterAndRawFree(interp, _call_clear_xidata, data);
    }
    else {
        return _Py_CallInInterpreter(interp, _call_clear_xidata, data);
    }
}

int
_PyCrossInterpreterData_Release(_PyCrossInterpreterData *data)
{
    return _xidata_release(data, 0);
}

int
_PyCrossInterpreterData_ReleaseAndRawFree(_PyCrossInterpreterData *data)
{
    return _xidata_release(data, 1);
}


/* registry of {type -> crossinterpdatafunc} */

/* For now we use a global registry of shareable classes.  An
   alternative would be to add a tp_* slot for a class's
   crossinterpdatafunc. It would be simpler and more efficient. */

static inline struct _xidregistry *
_get_global_xidregistry(_PyRuntimeState *runtime)
{
    return &runtime->xi.registry;
}

static inline struct _xidregistry *
_get_xidregistry(PyInterpreterState *interp)
{
    return &interp->xi.registry;
}

static inline struct _xidregistry *
_get_xidregistry_for_type(PyInterpreterState *interp, PyTypeObject *cls)
{
    struct _xidregistry *registry = _get_global_xidregistry(interp->runtime);
    if (cls->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        registry = _get_xidregistry(interp);
    }
    return registry;
}

static int
_xidregistry_add_type(struct _xidregistry *xidregistry,
                      PyTypeObject *cls, crossinterpdatafunc getdata)
{
    struct _xidregitem *newhead = PyMem_RawMalloc(sizeof(struct _xidregitem));
    if (newhead == NULL) {
        return -1;
    }
    *newhead = (struct _xidregitem){
        // We do not keep a reference, to avoid keeping the class alive.
        .cls = cls,
        .refcount = 1,
        .getdata = getdata,
    };
    if (cls->tp_flags & Py_TPFLAGS_HEAPTYPE) {
        // XXX Assign a callback to clear the entry from the registry?
        newhead->weakref = PyWeakref_NewRef((PyObject *)cls, NULL);
        if (newhead->weakref == NULL) {
            PyMem_RawFree(newhead);
            return -1;
        }
    }
    newhead->next = xidregistry->head;
    if (newhead->next != NULL) {
        newhead->next->prev = newhead;
    }
    xidregistry->head = newhead;
    return 0;
}

static struct _xidregitem *
_xidregistry_remove_entry(struct _xidregistry *xidregistry,
                          struct _xidregitem *entry)
{
    struct _xidregitem *next = entry->next;
    if (entry->prev != NULL) {
        assert(entry->prev->next == entry);
        entry->prev->next = next;
    }
    else {
        assert(xidregistry->head == entry);
        xidregistry->head = next;
    }
    if (next != NULL) {
        next->prev = entry->prev;
    }
    Py_XDECREF(entry->weakref);
    PyMem_RawFree(entry);
    return next;
}

static void
_xidregistry_clear(struct _xidregistry *xidregistry)
{
    struct _xidregitem *cur = xidregistry->head;
    xidregistry->head = NULL;
    while (cur != NULL) {
        struct _xidregitem *next = cur->next;
        Py_XDECREF(cur->weakref);
        PyMem_RawFree(cur);
        cur = next;
    }
}

static void
_xidregistry_lock(struct _xidregistry *registry)
{
    if (registry->mutex != NULL) {
        PyThread_acquire_lock(registry->mutex, WAIT_LOCK);
    }
}

static void
_xidregistry_unlock(struct _xidregistry *registry)
{
    if (registry->mutex != NULL) {
        PyThread_release_lock(registry->mutex);
    }
}

static struct _xidregitem *
_xidregistry_find_type(struct _xidregistry *xidregistry, PyTypeObject *cls)
{
    struct _xidregitem *cur = xidregistry->head;
    while (cur != NULL) {
        if (cur->weakref != NULL) {
            // cur is/was a heap type.
            PyObject *registered = _PyWeakref_GET_REF(cur->weakref);
            if (registered == NULL) {
                // The weakly ref'ed object was freed.
                cur = _xidregistry_remove_entry(xidregistry, cur);
                continue;
            }
            assert(PyType_Check(registered));
            assert(cur->cls == (PyTypeObject *)registered);
            assert(cur->cls->tp_flags & Py_TPFLAGS_HEAPTYPE);
            Py_DECREF(registered);
        }
        if (cur->cls == cls) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

int
_PyCrossInterpreterData_RegisterClass(PyTypeObject *cls,
                                      crossinterpdatafunc getdata)
{
    if (!PyType_Check(cls)) {
        PyErr_Format(PyExc_ValueError, "only classes may be registered");
        return -1;
    }
    if (getdata == NULL) {
        PyErr_Format(PyExc_ValueError, "missing 'getdata' func");
        return -1;
    }

    int res = 0;
    PyInterpreterState *interp = _PyInterpreterState_GET();
    struct _xidregistry *xidregistry = _get_xidregistry_for_type(interp, cls);
    _xidregistry_lock(xidregistry);

    struct _xidregitem *matched = _xidregistry_find_type(xidregistry, cls);
    if (matched != NULL) {
        assert(matched->getdata == getdata);
        matched->refcount += 1;
        goto finally;
    }

    res = _xidregistry_add_type(xidregistry, cls, getdata);

finally:
    _xidregistry_unlock(xidregistry);
    return res;
}

int
_PyCrossInterpreterData_UnregisterClass(PyTypeObject *cls)
{
    int res = 0;
    PyInterpreterState *interp = _PyInterpreterState_GET();
    struct _xidregistry *xidregistry = _get_xidregistry_for_type(interp, cls);
    _xidregistry_lock(xidregistry);

    struct _xidregitem *matched = _xidregistry_find_type(xidregistry, cls);
    if (matched != NULL) {
        assert(matched->refcount > 0);
        matched->refcount -= 1;
        if (matched->refcount == 0) {
            (void)_xidregistry_remove_entry(xidregistry, matched);
        }
        res = 1;
    }

    _xidregistry_unlock(xidregistry);
    return res;
}

static crossinterpdatafunc
_lookup_getdata_from_registry(PyInterpreterState *interp, PyObject *obj)
{
    PyTypeObject *cls = Py_TYPE(obj);

    struct _xidregistry *xidregistry = _get_xidregistry_for_type(interp, cls);
    _xidregistry_lock(xidregistry);

    struct _xidregitem *matched = _xidregistry_find_type(xidregistry, cls);
    crossinterpdatafunc func = matched != NULL ? matched->getdata : NULL;

    _xidregistry_unlock(xidregistry);
    return func;
}

/* cross-interpreter data for builtin types */

struct _shared_bytes_data {
    char *bytes;
    Py_ssize_t len;
};

static PyObject *
_new_bytes_object(_PyCrossInterpreterData *data)
{
    struct _shared_bytes_data *shared = (struct _shared_bytes_data *)(data->data);
    return PyBytes_FromStringAndSize(shared->bytes, shared->len);
}

static int
_bytes_shared(PyThreadState *tstate, PyObject *obj,
              _PyCrossInterpreterData *data)
{
    if (_PyCrossInterpreterData_InitWithSize(
            data, tstate->interp, sizeof(struct _shared_bytes_data), obj,
            _new_bytes_object
            ) < 0)
    {
        return -1;
    }
    struct _shared_bytes_data *shared = (struct _shared_bytes_data *)data->data;
    if (PyBytes_AsStringAndSize(obj, &shared->bytes, &shared->len) < 0) {
        _PyCrossInterpreterData_Clear(tstate->interp, data);
        return -1;
    }
    return 0;
}

struct _shared_str_data {
    int kind;
    const void *buffer;
    Py_ssize_t len;
};

static PyObject *
_new_str_object(_PyCrossInterpreterData *data)
{
    struct _shared_str_data *shared = (struct _shared_str_data *)(data->data);
    return PyUnicode_FromKindAndData(shared->kind, shared->buffer, shared->len);
}

static int
_str_shared(PyThreadState *tstate, PyObject *obj,
            _PyCrossInterpreterData *data)
{
    if (_PyCrossInterpreterData_InitWithSize(
            data, tstate->interp, sizeof(struct _shared_str_data), obj,
            _new_str_object
            ) < 0)
    {
        return -1;
    }
    struct _shared_str_data *shared = (struct _shared_str_data *)data->data;
    shared->kind = PyUnicode_KIND(obj);
    shared->buffer = PyUnicode_DATA(obj);
    shared->len = PyUnicode_GET_LENGTH(obj);
    return 0;
}

static PyObject *
_new_long_object(_PyCrossInterpreterData *data)
{
    return PyLong_FromSsize_t((Py_ssize_t)(data->data));
}

static int
_long_shared(PyThreadState *tstate, PyObject *obj,
             _PyCrossInterpreterData *data)
{
    /* Note that this means the size of shareable ints is bounded by
     * sys.maxsize.  Hence on 32-bit architectures that is half the
     * size of maximum shareable ints on 64-bit.
     */
    Py_ssize_t value = PyLong_AsSsize_t(obj);
    if (value == -1 && PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_OverflowError)) {
            PyErr_SetString(PyExc_OverflowError, "try sending as bytes");
        }
        return -1;
    }
    _PyCrossInterpreterData_Init(data, tstate->interp, (void *)value, NULL,
            _new_long_object);
    // data->obj and data->free remain NULL
    return 0;
}

static PyObject *
_new_float_object(_PyCrossInterpreterData *data)
{
    double * value_ptr = data->data;
    return PyFloat_FromDouble(*value_ptr);
}

static int
_float_shared(PyThreadState *tstate, PyObject *obj,
             _PyCrossInterpreterData *data)
{
    if (_PyCrossInterpreterData_InitWithSize(
            data, tstate->interp, sizeof(double), NULL,
            _new_float_object
            ) < 0)
    {
        return -1;
    }
    double *shared = (double *)data->data;
    *shared = PyFloat_AsDouble(obj);
    return 0;
}

static PyObject *
_new_none_object(_PyCrossInterpreterData *data)
{
    // XXX Singleton refcounts are problematic across interpreters...
    return Py_NewRef(Py_None);
}

static int
_none_shared(PyThreadState *tstate, PyObject *obj,
             _PyCrossInterpreterData *data)
{
    _PyCrossInterpreterData_Init(data, tstate->interp, NULL, NULL,
            _new_none_object);
    // data->data, data->obj and data->free remain NULL
    return 0;
}

static PyObject *
_new_bool_object(_PyCrossInterpreterData *data)
{
    if (data->data){
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static int
_bool_shared(PyThreadState *tstate, PyObject *obj,
             _PyCrossInterpreterData *data)
{
    _PyCrossInterpreterData_Init(data, tstate->interp,
            (void *) (Py_IsTrue(obj) ? (uintptr_t) 1 : (uintptr_t) 0), NULL,
            _new_bool_object);
    // data->obj and data->free remain NULL
    return 0;
}

static void
_register_builtins_for_crossinterpreter_data(struct _xidregistry *xidregistry)
{
    // None
    if (_xidregistry_add_type(xidregistry, (PyTypeObject *)PyObject_Type(Py_None), _none_shared) != 0) {
        Py_FatalError("could not register None for cross-interpreter sharing");
    }

    // int
    if (_xidregistry_add_type(xidregistry, &PyLong_Type, _long_shared) != 0) {
        Py_FatalError("could not register int for cross-interpreter sharing");
    }

    // bytes
    if (_xidregistry_add_type(xidregistry, &PyBytes_Type, _bytes_shared) != 0) {
        Py_FatalError("could not register bytes for cross-interpreter sharing");
    }

    // str
    if (_xidregistry_add_type(xidregistry, &PyUnicode_Type, _str_shared) != 0) {
        Py_FatalError("could not register str for cross-interpreter sharing");
    }

    // bool
    if (_xidregistry_add_type(xidregistry, &PyBool_Type, _bool_shared) != 0) {
        Py_FatalError("could not register bool for cross-interpreter sharing");
    }

    // float
    if (_xidregistry_add_type(xidregistry, &PyFloat_Type, _float_shared) != 0) {
        Py_FatalError("could not register float for cross-interpreter sharing");
    }
}

/* registry lifecycle */

static void
_xidregistry_init(struct _xidregistry *registry)
{
    if (registry->initialized) {
        return;
    }
    registry->initialized = 1;

    if (registry->global) {
        // We manage the mutex lifecycle in pystate.c.
        assert(registry->mutex != NULL);

        // Registering the builtins is cheap so we don't bother doing it lazily.
        assert(registry->head == NULL);
        _register_builtins_for_crossinterpreter_data(registry);
    }
    else {
        // Within an interpreter we rely on the GIL instead of a separate lock.
        assert(registry->mutex == NULL);

        // There's nothing else to initialize.
    }
}

static void
_xidregistry_fini(struct _xidregistry *registry)
{
    if (!registry->initialized) {
        return;
    }
    registry->initialized = 0;

    _xidregistry_clear(registry);

    if (registry->global) {
        // We manage the mutex lifecycle in pystate.c.
        assert(registry->mutex != NULL);
    }
    else {
        // There's nothing else to finalize.

        // Within an interpreter we rely on the GIL instead of a separate lock.
        assert(registry->mutex == NULL);
    }
}


/*************************/
/* convenience utilities */
/*************************/

static const char *
_copy_raw_string(const char *str)
{
    char *copied = PyMem_RawMalloc(strlen(str)+1);
    if (copied == NULL) {
        return NULL;
    }
    strcpy(copied, str);
    return copied;
}

static const char *
_copy_string_obj_raw(PyObject *strobj)
{
    const char *str = PyUnicode_AsUTF8(strobj);
    if (str == NULL) {
        return NULL;
    }

    char *copied = PyMem_RawMalloc(strlen(str)+1);
    if (copied == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    strcpy(copied, str);
    return copied;
}

static int
_release_xid_data(_PyCrossInterpreterData *data, int rawfree)
{
    PyObject *exc = PyErr_GetRaisedException();
    int res = rawfree
        ? _PyCrossInterpreterData_Release(data)
        : _PyCrossInterpreterData_ReleaseAndRawFree(data);
    if (res < 0) {
        /* The owning interpreter is already destroyed. */
        _PyCrossInterpreterData_Clear(NULL, data);
        // XXX Emit a warning?
        PyErr_Clear();
    }
    PyErr_SetRaisedException(exc);
    return res;
}


/* exception snapshots */

static int
_exc_type_name_as_utf8(PyObject *exc, const char **p_typename)
{
    // XXX Use PyObject_GetAttrString(Py_TYPE(exc), '__name__')?
    PyObject *nameobj = PyUnicode_FromString(Py_TYPE(exc)->tp_name);
    if (nameobj == NULL) {
        assert(PyErr_Occurred());
        *p_typename = "unable to format exception type name";
        return -1;
    }
    const char *name = PyUnicode_AsUTF8(nameobj);
    if (name == NULL) {
        assert(PyErr_Occurred());
        Py_DECREF(nameobj);
        *p_typename = "unable to encode exception type name";
        return -1;
    }
    name = _copy_raw_string(name);
    Py_DECREF(nameobj);
    if (name == NULL) {
        *p_typename = "out of memory copying exception type name";
        return -1;
    }
    *p_typename = name;
    return 0;
}

static int
_exc_msg_as_utf8(PyObject *exc, const char **p_msg)
{
    PyObject *msgobj = PyObject_Str(exc);
    if (msgobj == NULL) {
        assert(PyErr_Occurred());
        *p_msg = "unable to format exception message";
        return -1;
    }
    const char *msg = PyUnicode_AsUTF8(msgobj);
    if (msg == NULL) {
        assert(PyErr_Occurred());
        Py_DECREF(msgobj);
        *p_msg = "unable to encode exception message";
        return -1;
    }
    msg = _copy_raw_string(msg);
    Py_DECREF(msgobj);
    if (msg == NULL) {
        assert(PyErr_ExceptionMatches(PyExc_MemoryError));
        *p_msg = "out of memory copying exception message";
        return -1;
    }
    *p_msg = msg;
    return 0;
}

static void
_Py_excinfo_Clear(_Py_excinfo *info)
{
    if (info->type != NULL) {
        PyMem_RawFree((void *)info->type);
    }
    if (info->msg != NULL) {
        PyMem_RawFree((void *)info->msg);
    }
    *info = (_Py_excinfo){ NULL };
}

static const char *
_Py_excinfo_InitFromException(_Py_excinfo *info, PyObject *exc)
{
    assert(exc != NULL);

    // Extract the exception type name.
    const char *typename = NULL;
    if (_exc_type_name_as_utf8(exc, &typename) < 0) {
        assert(typename != NULL);
        return typename;
    }

    // Extract the exception message.
    const char *msg = NULL;
    if (_exc_msg_as_utf8(exc, &msg) < 0) {
        assert(msg != NULL);
        return msg;
    }

    info->type = typename;
    info->msg = msg;
    return NULL;
}

static void
_Py_excinfo_Apply(_Py_excinfo *info, PyObject *exctype)
{
    if (info->type != NULL) {
        if (info->msg != NULL) {
            PyErr_Format(exctype, "%s: %s",  info->type, info->msg);
        }
        else {
            PyErr_SetString(exctype, info->type);
        }
    }
    else if (info->msg != NULL) {
        PyErr_SetString(exctype, info->msg);
    }
    else {
        PyErr_SetNone(exctype);
    }
}


/***************************/
/* short-term data sharing */
/***************************/

/* error codes */

static int
_PyXI_ApplyErrorCode(_PyXI_errcode code, PyInterpreterState *interp)
{
    assert(!PyErr_Occurred());
    switch (code) {
    case _PyXI_ERR_NO_ERROR:  // fall through
    case _PyXI_ERR_UNCAUGHT_EXCEPTION:
        // There is nothing to apply.
#ifdef Py_DEBUG
        Py_UNREACHABLE();
#endif
        return 0;
    case _PyXI_ERR_OTHER:
        // XXX msg?
        PyErr_SetNone(PyExc_RuntimeError);
        break;
    case _PyXI_ERR_NO_MEMORY:
        PyErr_NoMemory();
        break;
    case _PyXI_ERR_ALREADY_RUNNING:
        assert(interp != NULL);
        assert(_PyInterpreterState_IsRunningMain(interp));
        _PyInterpreterState_FailIfRunningMain(interp);
        break;
    case _PyXI_ERR_MAIN_NS_FAILURE:
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to get __main__ namespace");
        break;
    case _PyXI_ERR_APPLY_NS_FAILURE:
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to apply namespace to __main__");
        break;
    case _PyXI_ERR_NOT_SHAREABLE:
        _set_xid_lookup_failure(interp, NULL, NULL);
        break;
    default:
#ifdef Py_DEBUG
        Py_UNREACHABLE();
#else
        PyErr_Format(PyExc_RuntimeError, "unsupported error code %d", code);
#endif
    }
    assert(PyErr_Occurred());
    return -1;
}

/* shared exceptions */

static const char *
_PyXI_InitExceptionInfo(_PyXI_exception_info *info,
                        PyObject *excobj, _PyXI_errcode code)
{
    if (info->interp == NULL) {
        info->interp = PyInterpreterState_Get();
    }

    const char *failure = NULL;
    if (code == _PyXI_ERR_UNCAUGHT_EXCEPTION) {
        // There is an unhandled exception we need to propagate.
        failure = _Py_excinfo_InitFromException(&info->uncaught, excobj);
        if (failure != NULL) {
            // We failed to initialize info->uncaught.
            // XXX Print the excobj/traceback?  Emit a warning?
            // XXX Print the current exception/traceback?
            if (PyErr_ExceptionMatches(PyExc_MemoryError)) {
                info->code = _PyXI_ERR_NO_MEMORY;
            }
            else {
                info->code = _PyXI_ERR_OTHER;
            }
            PyErr_Clear();
        }
        else {
            info->code = code;
        }
        assert(info->code != _PyXI_ERR_NO_ERROR);
    }
    else {
        // There is an error code we need to propagate.
        assert(excobj == NULL);
        assert(code != _PyXI_ERR_NO_ERROR);
        info->code = code;
        _Py_excinfo_Clear(&info->uncaught);
    }
    return failure;
}

void
_PyXI_ApplyExceptionInfo(_PyXI_exception_info *info, PyObject *exctype)
{
    if (exctype == NULL) {
        exctype = PyExc_RuntimeError;
    }
    if (info->code == _PyXI_ERR_UNCAUGHT_EXCEPTION) {
        // Raise an exception that proxies the propagated exception.
        _Py_excinfo_Apply(&info->uncaught, exctype);
    }
    else if (info->code == _PyXI_ERR_NOT_SHAREABLE) {
        // Propagate the exception directly.
        _set_xid_lookup_failure(info->interp, NULL, info->uncaught.msg);
    }
    else {
        // Raise an exception corresponding to the code.
        assert(info->code != _PyXI_ERR_NO_ERROR);
        (void)_PyXI_ApplyErrorCode(info->code, info->interp);
        if (info->uncaught.type != NULL || info->uncaught.msg != NULL) {
            // __context__ will be set to a proxy of the propagated exception.
            PyObject *exc = PyErr_GetRaisedException();
            _Py_excinfo_Apply(&info->uncaught, exctype);
            PyObject *exc2 = PyErr_GetRaisedException();
            PyException_SetContext(exc, exc2);
            PyErr_SetRaisedException(exc);
        }
    }
    assert(PyErr_Occurred());
}

/* shared namespaces */

/* Shared namespaces are expected to have relatively short lifetimes.
   This means dealloc of a shared namespace will normally happen "soon".
   Namespace items hold cross-interpreter data, which must get released.
   If the namespace/items are cleared in a different interpreter than
   where the items' cross-interpreter data was set then that will cause
   pending calls to be used to release the cross-interpreter data.
   The tricky bit is that the pending calls can happen sufficiently
   later that the namespace/items might already be deallocated.  This is
   a problem if the cross-interpreter data is allocated as part of a
   namespace item.  If that's the case then we must ensure the shared
   namespace is only cleared/freed *after* that data has been released. */

typedef struct _sharednsitem {
    const char *name;
    _PyCrossInterpreterData *data;
    // We could have a "PyCrossInterpreterData _data" field, so it would
    // be allocated as part of the item and avoid an extra allocation.
    // However, doing so adds a bunch of complexity because we must
    // ensure the item isn't freed before a pending call might happen
    // in a different interpreter to release the XI data.
} _PyXI_namespace_item;

static int
_sharednsitem_is_initialized(_PyXI_namespace_item *item)
{
    if (item->name != NULL) {
        return 1;
    }
    return 0;
}

static int
_sharednsitem_init(_PyXI_namespace_item *item, PyObject *key)
{
    item->name = _copy_string_obj_raw(key);
    if (item->name == NULL) {
        assert(!_sharednsitem_is_initialized(item));
        return -1;
    }
    item->data = NULL;
    assert(_sharednsitem_is_initialized(item));
    return 0;
}

static int
_sharednsitem_has_value(_PyXI_namespace_item *item, int64_t *p_interpid)
{
    if (item->data == NULL) {
        return 0;
    }
    if (p_interpid != NULL) {
        *p_interpid = item->data->interpid;
    }
    return 1;
}

static int
_sharednsitem_set_value(_PyXI_namespace_item *item, PyObject *value)
{
    assert(_sharednsitem_is_initialized(item));
    assert(item->data == NULL);
    item->data = PyMem_RawMalloc(sizeof(_PyCrossInterpreterData));
    if (item->data == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    if (_PyObject_GetCrossInterpreterData(value, item->data) != 0) {
        PyMem_RawFree(item->data);
        item->data = NULL;
        // The caller may want to propagate PyExc_NotShareableError
        // if currently switched between interpreters.
        return -1;
    }
    return 0;
}

static void
_sharednsitem_clear_value(_PyXI_namespace_item *item)
{
    _PyCrossInterpreterData *data = item->data;
    if (data != NULL) {
        item->data = NULL;
        int rawfree = 1;
        (void)_release_xid_data(data, rawfree);
    }
}

static void
_sharednsitem_clear(_PyXI_namespace_item *item)
{
    if (item->name != NULL) {
        PyMem_RawFree((void *)item->name);
        item->name = NULL;
    }
    _sharednsitem_clear_value(item);
}

static int
_sharednsitem_copy_from_ns(struct _sharednsitem *item, PyObject *ns)
{
    assert(item->name != NULL);
    assert(item->data == NULL);
    PyObject *value = PyDict_GetItemString(ns, item->name);  // borrowed
    if (value == NULL) {
        if (PyErr_Occurred()) {
            return -1;
        }
        // When applied, this item will be set to the default (or fail).
        return 0;
    }
    if (_sharednsitem_set_value(item, value) < 0) {
        return -1;
    }
    return 0;
}

static int
_sharednsitem_apply(_PyXI_namespace_item *item, PyObject *ns, PyObject *dflt)
{
    PyObject *name = PyUnicode_FromString(item->name);
    if (name == NULL) {
        return -1;
    }
    PyObject *value;
    if (item->data != NULL) {
        value = _PyCrossInterpreterData_NewObject(item->data);
        if (value == NULL) {
            Py_DECREF(name);
            return -1;
        }
    }
    else {
        value = Py_NewRef(dflt);
    }
    int res = PyDict_SetItem(ns, name, value);
    Py_DECREF(name);
    Py_DECREF(value);
    return res;
}

struct _sharedns {
    Py_ssize_t len;
    _PyXI_namespace_item *items;
};

static _PyXI_namespace *
_sharedns_new(void)
{
    _PyXI_namespace *ns = PyMem_RawCalloc(sizeof(_PyXI_namespace), 1);
    if (ns == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    *ns = (_PyXI_namespace){ 0 };
    return ns;
}

static int
_sharedns_is_initialized(_PyXI_namespace *ns)
{
    if (ns->len == 0) {
        assert(ns->items == NULL);
        return 0;
    }

    assert(ns->len > 0);
    assert(ns->items != NULL);
    assert(_sharednsitem_is_initialized(&ns->items[0]));
    assert(ns->len == 1
           || _sharednsitem_is_initialized(&ns->items[ns->len - 1]));
    return 1;
}

#define HAS_COMPLETE_DATA 1
#define HAS_PARTIAL_DATA 2

static int
_sharedns_has_xidata(_PyXI_namespace *ns, int64_t *p_interpid)
{
    // We expect _PyXI_namespace to always be initialized.
    assert(_sharedns_is_initialized(ns));
    int res = 0;
    _PyXI_namespace_item *item0 = &ns->items[0];
    if (!_sharednsitem_is_initialized(item0)) {
        return 0;
    }
    int64_t interpid0 = -1;
    if (!_sharednsitem_has_value(item0, &interpid0)) {
        return 0;
    }
    if (ns->len > 1) {
        // At this point we know it is has at least partial data.
        _PyXI_namespace_item *itemN = &ns->items[ns->len-1];
        if (!_sharednsitem_is_initialized(itemN)) {
            res = HAS_PARTIAL_DATA;
            goto finally;
        }
        int64_t interpidN = -1;
        if (!_sharednsitem_has_value(itemN, &interpidN)) {
            res = HAS_PARTIAL_DATA;
            goto finally;
        }
        assert(interpidN == interpid0);
    }
    res = HAS_COMPLETE_DATA;
    *p_interpid = interpid0;

finally:
    return res;
}

static void
_sharedns_clear(_PyXI_namespace *ns)
{
    if (!_sharedns_is_initialized(ns)) {
        return;
    }

    // If the cross-interpreter data were allocated as part of
    // _PyXI_namespace_item (instead of dynamically), this is where
    // we would need verify that we are clearing the items in the
    // correct interpreter, to avoid a race with releasing the XI data
    // via a pending call.  See _sharedns_has_xidata().
    for (Py_ssize_t i=0; i < ns->len; i++) {
        _sharednsitem_clear(&ns->items[i]);
    }
    PyMem_RawFree(ns->items);
    ns->items = NULL;
    ns->len = 0;
}

static void
_sharedns_free(_PyXI_namespace *ns)
{
    _sharedns_clear(ns);
    PyMem_RawFree(ns);
}

static int
_sharedns_init(_PyXI_namespace *ns, PyObject *names)
{
    assert(!_sharedns_is_initialized(ns));
    assert(names != NULL);
    Py_ssize_t len = PyDict_CheckExact(names)
        ? PyDict_Size(names)
        : PySequence_Size(names);
    if (len < 0) {
        return -1;
    }
    if (len == 0) {
        PyErr_SetString(PyExc_ValueError, "empty namespaces not allowed");
        return -1;
    }
    assert(len > 0);

    // Allocate the items.
    _PyXI_namespace_item *items =
            PyMem_RawCalloc(sizeof(struct _sharednsitem), len);
    if (items == NULL) {
        PyErr_NoMemory();
        return -1;
    }

    // Fill in the names.
    Py_ssize_t i = -1;
    if (PyDict_CheckExact(names)) {
        Py_ssize_t pos = 0;
        for (i=0; i < len; i++) {
            PyObject *key;
            if (!PyDict_Next(names, &pos, &key, NULL)) {
                // This should not be possible.
                assert(0);
                goto error;
            }
            if (_sharednsitem_init(&items[i], key) < 0) {
                goto error;
            }
        }
    }
    else if (PySequence_Check(names)) {
        for (i=0; i < len; i++) {
            PyObject *key = PySequence_GetItem(names, i);
            if (key == NULL) {
                goto error;
            }
            int res = _sharednsitem_init(&items[i], key);
            Py_DECREF(key);
            if (res < 0) {
                goto error;
            }
        }
    }
    else {
        PyErr_SetString(PyExc_NotImplementedError,
                        "non-sequence namespace not supported");
        goto error;
    }

    ns->items = items;
    ns->len = len;
    assert(_sharedns_is_initialized(ns));
    return 0;

error:
    for (Py_ssize_t j=0; j < i; j++) {
        _sharednsitem_clear(&items[j]);
    }
    PyMem_RawFree(items);
    assert(!_sharedns_is_initialized(ns));
    return -1;
}

void
_PyXI_FreeNamespace(_PyXI_namespace *ns)
{
    if (!_sharedns_is_initialized(ns)) {
        return;
    }

    int64_t interpid = -1;
    if (!_sharedns_has_xidata(ns, &interpid)) {
        _sharedns_free(ns);
        return;
    }

    if (interpid == PyInterpreterState_GetID(_PyInterpreterState_GET())) {
        _sharedns_free(ns);
    }
    else {
        // If we weren't always dynamically allocating the cross-interpreter
        // data in each item then we would need to using a pending call
        // to call _sharedns_free(), to avoid the race between freeing
        // the shared namespace and releasing the XI data.
        _sharedns_free(ns);
    }
}

_PyXI_namespace *
_PyXI_NamespaceFromNames(PyObject *names)
{
    if (names == NULL || names == Py_None) {
        return NULL;
    }

    _PyXI_namespace *ns = _sharedns_new();
    if (ns == NULL) {
        return NULL;
    }

    if (_sharedns_init(ns, names) < 0) {
        PyMem_RawFree(ns);
        if (PySequence_Size(names) == 0) {
            PyErr_Clear();
        }
        return NULL;
    }

    return ns;
}

#ifndef NDEBUG
static int _session_is_active(_PyXI_session *);
#endif
static void _propagate_not_shareable_error(_PyXI_session *);

int
_PyXI_FillNamespaceFromDict(_PyXI_namespace *ns, PyObject *nsobj,
                            _PyXI_session *session)
{
    // session must be entered already, if provided.
    assert(session == NULL || _session_is_active(session));
    assert(_sharedns_is_initialized(ns));
    for (Py_ssize_t i=0; i < ns->len; i++) {
        _PyXI_namespace_item *item = &ns->items[i];
        if (_sharednsitem_copy_from_ns(item, nsobj) < 0) {
            _propagate_not_shareable_error(session);
            // Clear out the ones we set so far.
            for (Py_ssize_t j=0; j < i; j++) {
                _sharednsitem_clear_value(&ns->items[j]);
            }
            return -1;
        }
    }
    return 0;
}

// All items are expected to be shareable.
static _PyXI_namespace *
_PyXI_NamespaceFromDict(PyObject *nsobj, _PyXI_session *session)
{
    // session must be entered already, if provided.
    assert(session == NULL || _session_is_active(session));
    if (nsobj == NULL || nsobj == Py_None) {
        return NULL;
    }
    if (!PyDict_CheckExact(nsobj)) {
        PyErr_SetString(PyExc_TypeError, "expected a dict");
        return NULL;
    }

    _PyXI_namespace *ns = _sharedns_new();
    if (ns == NULL) {
        return NULL;
    }

    if (_sharedns_init(ns, nsobj) < 0) {
        if (PyDict_Size(nsobj) == 0) {
            PyMem_RawFree(ns);
            PyErr_Clear();
            return NULL;
        }
        goto error;
    }

    if (_PyXI_FillNamespaceFromDict(ns, nsobj, session) < 0) {
        goto error;
    }

    return ns;

error:
    assert(PyErr_Occurred()
           || (session != NULL && session->exc_override != NULL));
    _sharedns_free(ns);
    return NULL;
}

int
_PyXI_ApplyNamespace(_PyXI_namespace *ns, PyObject *nsobj, PyObject *dflt)
{
    for (Py_ssize_t i=0; i < ns->len; i++) {
        if (_sharednsitem_apply(&ns->items[i], nsobj, dflt) != 0) {
            return -1;
        }
    }
    return 0;
}


/**********************/
/* high-level helpers */
/**********************/

/* enter/exit a cross-interpreter session */

static void
_enter_session(_PyXI_session *session, PyInterpreterState *interp)
{
    // Set here and cleared in _exit_session().
    assert(!session->own_init_tstate);
    assert(session->init_tstate == NULL);
    assert(session->prev_tstate == NULL);
    // Set elsewhere and cleared in _exit_session().
    assert(!session->running);
    assert(session->main_ns == NULL);
    // Set elsewhere and cleared in _capture_current_exception().
    assert(session->exc_override == NULL);
    // Set elsewhere and cleared in _PyXI_ApplyCapturedException().
    assert(session->exc == NULL);

    // Switch to interpreter.
    PyThreadState *tstate = PyThreadState_Get();
    PyThreadState *prev = tstate;
    if (interp != tstate->interp) {
        tstate = PyThreadState_New(interp);
        tstate->_whence = _PyThreadState_WHENCE_EXEC;
        // XXX Possible GILState issues?
        session->prev_tstate = PyThreadState_Swap(tstate);
        assert(session->prev_tstate == prev);
        session->own_init_tstate = 1;
    }
    session->init_tstate = tstate;
    session->prev_tstate = prev;
}

static void
_exit_session(_PyXI_session *session)
{
    PyThreadState *tstate = session->init_tstate;
    assert(tstate != NULL);
    assert(PyThreadState_Get() == tstate);

    // Release any of the entered interpreters resources.
    if (session->main_ns != NULL) {
        Py_CLEAR(session->main_ns);
    }

    // Ensure this thread no longer owns __main__.
    if (session->running) {
        _PyInterpreterState_SetNotRunningMain(tstate->interp);
        assert(!PyErr_Occurred());
        session->running = 0;
    }

    // Switch back.
    assert(session->prev_tstate != NULL);
    if (session->prev_tstate != session->init_tstate) {
        assert(session->own_init_tstate);
        session->own_init_tstate = 0;
        PyThreadState_Clear(tstate);
        PyThreadState_Swap(session->prev_tstate);
        PyThreadState_Delete(tstate);
    }
    else {
        assert(!session->own_init_tstate);
    }
    session->prev_tstate = NULL;
    session->init_tstate = NULL;
}

#ifndef NDEBUG
static int
_session_is_active(_PyXI_session *session)
{
    return (session->init_tstate != NULL);
}
#endif

static void
_propagate_not_shareable_error(_PyXI_session *session)
{
    if (session == NULL) {
        return;
    }
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (PyErr_ExceptionMatches(_get_not_shareable_error_type(interp))) {
        // We want to propagate the exception directly.
        session->_exc_override = _PyXI_ERR_NOT_SHAREABLE;
        session->exc_override = &session->_exc_override;
    }
}

static void
_capture_current_exception(_PyXI_session *session)
{
    assert(session->exc == NULL);
    if (!PyErr_Occurred()) {
        assert(session->exc_override == NULL);
        return;
    }

    // Handle the exception override.
    _PyXI_errcode *override = session->exc_override;
    session->exc_override = NULL;
    _PyXI_errcode errcode = override != NULL
        ? *override
        : _PyXI_ERR_UNCAUGHT_EXCEPTION;

    // Pop the exception object.
    PyObject *excval = NULL;
    if (errcode == _PyXI_ERR_UNCAUGHT_EXCEPTION) {
        // We want to actually capture the current exception.
        excval = PyErr_GetRaisedException();
    }
    else if (errcode == _PyXI_ERR_ALREADY_RUNNING) {
        // We don't need the exception info.
        PyErr_Clear();
    }
    else {
        // We could do a variety of things here, depending on errcode.
        // However, for now we simply capture the exception and save
        // the errcode.
        excval = PyErr_GetRaisedException();
    }

    // Capture the exception.
    _PyXI_exception_info *exc = &session->_exc;
    *exc = (_PyXI_exception_info){
        .interp = session->init_tstate->interp,
    };
    const char *failure;
    if (excval == NULL) {
        failure = _PyXI_InitExceptionInfo(exc, NULL, errcode);
    }
    else {
        failure = _PyXI_InitExceptionInfo(exc, excval,
                                          _PyXI_ERR_UNCAUGHT_EXCEPTION);
        if (failure == NULL && override != NULL) {
            exc->code = errcode;
        }
    }

    // Handle capture failure.
    if (failure != NULL) {
        // XXX Make this error message more generic.
        fprintf(stderr,
                "RunFailedError: script raised an uncaught exception (%s)",
                failure);
        exc = NULL;
    }

    // a temporary hack  (famous last words)
    if (excval != NULL) {
        // XXX Store the traceback info (or rendered traceback) on
        // _PyXI_excinfo, attach it to the exception when applied,
        // and teach PyErr_Display() to print it.
#ifdef Py_DEBUG
        // XXX Drop this once _Py_excinfo picks up the slack.
        PyErr_Display(NULL, excval, NULL);
#endif
        Py_DECREF(excval);
    }

    // Finished!
    assert(!PyErr_Occurred());
    session->exc = exc;
}

void
_PyXI_ApplyCapturedException(_PyXI_session *session, PyObject *excwrapper)
{
    assert(!PyErr_Occurred());
    assert(session->exc != NULL);
    _PyXI_ApplyExceptionInfo(session->exc, excwrapper);
    assert(PyErr_Occurred());
    session->exc = NULL;
}

int
_PyXI_HasCapturedException(_PyXI_session *session)
{
    return session->exc != NULL;
}

int
_PyXI_Enter(_PyXI_session *session,
            PyInterpreterState *interp, PyObject *nsupdates)
{
    // Convert the attrs for cross-interpreter use.
    _PyXI_namespace *sharedns = NULL;
    if (nsupdates != NULL) {
        sharedns = _PyXI_NamespaceFromDict(nsupdates, NULL);
        if (sharedns == NULL && PyErr_Occurred()) {
            assert(session->exc == NULL);
            return -1;
        }
    }

    // Switch to the requested interpreter (if necessary).
    _enter_session(session, interp);
    _PyXI_errcode errcode = _PyXI_ERR_UNCAUGHT_EXCEPTION;

    // Ensure this thread owns __main__.
    if (_PyInterpreterState_SetRunningMain(interp) < 0) {
        // In the case where we didn't switch interpreters, it would
        // be more efficient to leave the exception in place and return
        // immediately.  However, life is simpler if we don't.
        errcode = _PyXI_ERR_ALREADY_RUNNING;
        goto error;
    }
    session->running = 1;

    // Cache __main__.__dict__.
    PyObject *main_mod = PyUnstable_InterpreterState_GetMainModule(interp);
    if (main_mod == NULL) {
        errcode = _PyXI_ERR_MAIN_NS_FAILURE;
        goto error;
    }
    PyObject *ns = PyModule_GetDict(main_mod);  // borrowed
    Py_DECREF(main_mod);
    if (ns == NULL) {
        errcode = _PyXI_ERR_MAIN_NS_FAILURE;
        goto error;
    }
    session->main_ns = Py_NewRef(ns);

    // Apply the cross-interpreter data.
    if (sharedns != NULL) {
        if (_PyXI_ApplyNamespace(sharedns, ns, NULL) < 0) {
            errcode = _PyXI_ERR_APPLY_NS_FAILURE;
            goto error;
        }
        _PyXI_FreeNamespace(sharedns);
    }

    errcode = _PyXI_ERR_NO_ERROR;
    assert(!PyErr_Occurred());
    return 0;

error:
    assert(PyErr_Occurred());
    // We want to propagate all exceptions here directly (best effort).
    assert(errcode != _PyXI_ERR_UNCAUGHT_EXCEPTION);
    session->exc_override = &errcode;
    _capture_current_exception(session);
    _exit_session(session);
    if (sharedns != NULL) {
        _PyXI_FreeNamespace(sharedns);
    }
    return -1;
}

void
_PyXI_Exit(_PyXI_session *session)
{
    _capture_current_exception(session);
    _exit_session(session);
}


/*********************/
/* runtime lifecycle */
/*********************/

PyStatus
_PyXI_Init(PyInterpreterState *interp)
{
    PyStatus status;

    // Initialize the XID registry.
    if (_Py_IsMainInterpreter(interp)) {
        _xidregistry_init(_get_global_xidregistry(interp->runtime));
    }
    _xidregistry_init(_get_xidregistry(interp));

    // Initialize exceptions (heap types).
    status = _init_not_shareable_error_type(interp);
    if (_PyStatus_EXCEPTION(status)) {
        return status;
    }

    return _PyStatus_OK();
}

// _PyXI_Fini() must be called before the interpreter is cleared,
// since we must clear some heap objects.

void
_PyXI_Fini(PyInterpreterState *interp)
{
    // Finalize exceptions (heap types).
    _fini_not_shareable_error_type(interp);

    // Finalize the XID registry.
    _xidregistry_fini(_get_xidregistry(interp));
    if (_Py_IsMainInterpreter(interp)) {
        _xidregistry_fini(_get_global_xidregistry(interp->runtime));
    }
}
