//
// DRNSF - An unofficial Crash Bandicoot level editor
// Copyright (C) 2017-2020  DRNSF contributors
//
// See the AUTHORS.md file for more details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

// Specify that we will be using the Py_ssize_t-enabled API for certain Python
// functions.
#define PY_SSIZE_T_CLEAN

// Specify that we will be using the "limited" Python API from PEP 384. This is
// important for ABI compatibility across minor revisions of the Python library
// on Windows.
#define Py_LIMITED_API 0x03030000

// According to Python documentation, the Python header should be included
// before any system and standard headers. This may preclude precompiled header
// support for 'common.hh' in this file on some C++ implementations.
//
// On MSVC, we disable _DEBUG to prevent Py_DEBUG and other similar macros from
// being defined, as they are incompatible with Py_LIMITED_API.
#if defined(_MSC_VER) && defined(_DEBUG)
#undef _DEBUG
#include <Python.h>
#define _DEBUG
#else
#include <Python.h>
#endif

#include "common.hh"
#include "scripting.hh"
#include "res.hh"
#include "edit.hh"
#include <thread>

DRNSF_DECLARE_EMBED(drnsf_py);

namespace drnsf {
namespace scripting {

// (s-var) s_init_state
// The initialization state of the scripting runtime.
//
// Possible values:
//   - none:      The runtime has not been initialized yet.
//   - ready:     The runtime initialized successfully and is ready for use.
//   - failed:    The runtime is currently initializing or has failed to do so.
//   - finished:  The runtime was initialized and then shutdown.
static enum class init_state {
    none,
    ready,
    failed,
    finished
} s_init_state = init_state::none;

// (s-var) s_lockcount
// The number of locks (see `lock' and `unlock') currently held on the runtime
// by the main thread.
static int s_lockcount = 0;

// (s-var) s_main_threadstate
// The main python thread state.
static PyThreadState *s_main_threadstate;

// (s-var) s_moduledef
// The definition for the "drnsf" module.
static PyModuleDef s_moduledef = {
    PyModuleDef_HEAD_INIT,
    "drnsf",
    nullptr,
    sizeof(engine_impl *),
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

// (s-var) s_dict
// An owning pointer to the dict of the "drnsf" module.
static PyObject *s_dict;

// (s-var) s_nonnative_dict
// An owning pointer to the dictionary for the "drnsf._nonnative" module.
static PyObject *s_nonnative_dict;

// (struct) engine_impl
// Internal implementation type for `scripting::engine'. Members are kept in
// this struct rather than the engine class so that the types need not be
// declared in `scripting.hh'. This is important for Python C-API types, which
// are not available in any other translation units.
struct engine_impl
{
    // (var) m_ctx
    // A pointer to the editor context associated with this engine, if any. This
    // may be null.
    edit::context *m_ctx;

    // (var) m_project_stack
    // FIXME explain
    std::vector<std::shared_ptr<res::project>> m_project_stack;

    // (var) m_interp
    // A pointer to the subinterpreter thread associated with this engine. Each
    // engine object has its own subinterpreter.
    PyThreadState *m_interp;

    // (s-func) get
    // Returns a pointer to the engine_impl associated with the current python
    // subinterpreter, or null if not available.
    static engine_impl *get()
    {
        auto module = PyState_FindModule(&s_moduledef);
        if (!module)
            return nullptr;

        auto pp = PyModule_GetState(module);
        if (!pp)
            return nullptr;

        return *static_cast<engine_impl **>(pp);
    }
};

// (s-func) drnsf_module_init
// Initializes a new instance of the "drnsf" builtin module. This is used with
// `PyImport_AppendInittab'.
//
// The initial "drnsf" module is created manually during runtime init; this
// function is used to create new "drnsf" modules when "import drnsf" is used
// in a Python subinterpreter (e.g. one made by `scripting::engine').
PyObject *drnsf_module_init()
{
    auto module = PyModule_Create(&s_moduledef);
    if (!module) {
        return nullptr;
    }

    auto dict = PyModule_GetDict(module);
    if (!dict) {
        Py_DECREF(module);
        return nullptr;
    }

    // Copy the existing dict into the new module dict.
    Py_ssize_t p = 0;
    PyObject *k;
    PyObject *v;
    while (PyDict_Next(s_dict, &p, &k, &v)) {
        if (PyDict_SetItem(dict, k, v) == -1) {
            Py_DECREF(module);
            return nullptr;
        }
    }

    return module;
}

namespace {

// (internal type) conversion_error
// Thrown by `from_python' when the object cannot be converted to the output
// native type.
class conversion_error : public std::runtime_error {
public:
    using runtime_error::runtime_error;
};

#define SLOT_FN(x) { (Py_##x), reinterpret_cast<void *>(x) }

#define GETSET_RO(name) \
    { \
        const_cast<char *>(#name), \
        reinterpret_cast<getter>(get_##name), \
        nullptr, \
        nullptr, \
        nullptr \
    }

#define GETSET_RW(name) \
    { \
        const_cast<char *>(#name), \
        reinterpret_cast<getter>(get_##name), \
        reinterpret_cast<setter>(set_##name), \
        nullptr, \
        nullptr \
    }

#define METHOD(name, flags) \
    { \
        #name, \
        reinterpret_cast<PyCFunction>(mth_##name), \
        flags, \
        nullptr \
    }

#define DECLARE_GETTER(type, name) \
    static PyObject *get_##name(type *self, void *) noexcept

#define DECLARE_SETTER(type, name) \
    static int set_##name(type *self, PyObject *value, void *) noexcept

#define DEFINE_GETTER(type, name) \
    PyObject *type::get_##name(type *self, void *) noexcept

#define DEFINE_SETTER(type, name) \
    int type::set_##name(type *self, PyObject *value, void *) noexcept

#define DECLARE_METHOD_NOARGS(type, name) \
    static PyObject *mth_##name(type *self, PyObject *) noexcept

#define DECLARE_METHOD_O(type, name) \
    static PyObject *mth_##name(type *self, PyObject *arg) noexcept

#define DEFINE_METHOD_NOARGS(type, name) \
    PyObject *type::mth_##name(type *self, PyObject *) noexcept

#define DEFINE_METHOD_O(type, name) \
    PyObject *type::mth_##name(type *self, PyObject *arg) noexcept

// (internal type) scr_base
// Base type for all scripting types defined below.
struct scr_base : PyObject {
};

// (internal type) scr_project
// Scripting type for "Project".
struct scr_project : scr_base {
    using native_type = std::shared_ptr<res::project>;

    static inline PyTypeObject *type;

    native_type proj_p;

    static native_type from_python(PyObject *obj)
    {
        if (obj == Py_None)
            return nullptr;

        if (Py_TYPE(obj) != type)
            throw conversion_error("scr_project: incompatible type");

        return static_cast<scr_project *>(obj)->proj_p;
    }

    static PyObject *to_python(native_type value) noexcept
    {
        if (!value)
            Py_RETURN_NONE;

        if (value->m_scripthandle.p) {
            auto obj = static_cast<scr_project *>(value->m_scripthandle.p);
            Py_INCREF(obj);
            return obj;
        }

        try {
            auto obj = new scr_project();
            obj->ob_refcnt = 1;
            obj->ob_type = type;
            obj->proj_p = value;
            obj->proj_p->m_scripthandle.p = obj;
            return obj;
        } catch (std::bad_alloc &) {
            return PyErr_NoMemory();
        }
    }

    static PyObject *tp_new(
        PyObject *subtype,
        PyObject *args,
        PyObject *kwds) noexcept
    {
        if (PyTuple_Size(args) != 0) {
            PyErr_SetString(PyExc_TypeError, "drnsf.Project() takes no parameters");
            return nullptr;
        }
        if (kwds && PyMapping_Size(kwds) != 0) {
            PyErr_SetString(PyExc_TypeError, "drnsf.Project() takes no parameters");
            return nullptr;
        }
        try {
            return to_python(std::make_shared<res::project>());
        } catch (std::bad_alloc &) {
            return PyErr_NoMemory();
        }
    }

    static void tp_dealloc(scr_project *self) noexcept
    {
        self->proj_p->m_scripthandle = {};
        delete self;
    }

    DECLARE_GETTER(scr_project, root);

    static void install()
    {
        if (type)
            return;

        static PyGetSetDef getset[] = {
            GETSET_RO(root),
            {}
        };

        static PyType_Slot slots[] = {
            SLOT_FN(tp_new),
            SLOT_FN(tp_dealloc),
            { Py_tp_getset, getset },
            {}
        };

        static PyType_Spec spec = {
            "drnsf.Project",
            sizeof(scr_project),
            0,
            0,
            slots
        };

        auto type_o = PyType_FromSpec(&spec);
        type = reinterpret_cast<PyTypeObject *>(type_o);
        if (!type) {
            PyErr_Print();
            std::abort();
        }

        if (PyDict_SetItemString(s_dict, "Project", type_o) == -1) {
            PyErr_Print();
            std::abort();
        }
    }
};

// (internal type) scr_atom
// Scripting type for "Atom".
struct scr_atom : scr_base {
    using native_type = res::atom;

    static inline PyTypeObject *type;

    native_type v;

    static native_type from_python(PyObject *obj)
    {
        if (obj == Py_None)
            return nullptr;

        if (Py_TYPE(obj) != type)
            throw conversion_error("scr_atom: incompatible type");

        return static_cast<scr_atom *>(obj)->v;
    }

    static PyObject *to_python(native_type value) noexcept
    {
        if (!value)
            Py_RETURN_NONE;

        auto obj = new(std::nothrow) scr_atom();
        if (!obj)
            return PyErr_NoMemory();
        obj->ob_refcnt = 1;
        obj->ob_type = type;
        obj->v = value;
        return obj;
    }

    static PyObject *tp_new(
        PyObject *subtype,
        PyObject *args,
        PyObject *kwds) noexcept
    {
        const char *kwdnames[] = {
            "path",
            "project",
            nullptr
        };
        const char *path;
        scr_project *proj;
        if (PyArg_ParseTupleAndKeywords(
            args, kwds,
            "sO!:Atom",
            const_cast<char **>(kwdnames),
            &path, scr_project::type, &proj)) {
                try {
                    auto obj = std::make_unique<scr_atom>();
                    obj->ob_refcnt = 1;
                    obj->ob_type = type;
                    obj->v = proj->proj_p->get_asset_root();
                    while (*path) {
                        // Check for a leading slash
                        if (path[0] != '/') {
                            PyErr_SetString(
                                PyExc_ValueError,
                                "Atom(): path must have leading slash"
                            );
                            return nullptr;
                        }

                        // Remove the leading slash
                        path++;

                        auto next = std::strchr(path, '/');
                        if (!next) {
                            next = path + std::strlen(path);
                        }

                        if (path == next) {
                            PyErr_SetString(
                                PyExc_ValueError,
                                "Atom(): path segments may not be zero-length"
                            );
                            return nullptr;
                        }

                        for (const char *p = path; p < next; p++) {
                            if (!res::atom::is_valid_char(*p)) {
                                PyErr_SetString(
                                    PyExc_ValueError,
                                    "Atom(): invalid character in path"
                                );
                                return nullptr;
                            }
                        }

                        obj->v /= { path, size_t(next - path) };
                        path = next;
                    }
                    return obj.release();
                } catch (std::bad_alloc &) {
                    return PyErr_NoMemory();
                }
        }

        return nullptr;
    }

    static void tp_dealloc(scr_atom *self) noexcept
    {
        delete self;
    }

    static PyObject *tp_repr(scr_atom *self) noexcept
    {
        if (self->v.get_depth() == 0) {
            return PyUnicode_FromString("<drnsf.Atom root>");
        } else {
            return PyUnicode_FromFormat(
                "<drnsf.Atom \"%s\">",
                self->v.path().c_str()
            );
        }
    }

    static PyObject *tp_str(scr_atom *self) noexcept
    {
        if (self->v.get_depth() == 0) {
            return PyUnicode_FromString("(root)");
        } else {
            return PyUnicode_FromString(self->v.path().c_str());
        }
    }

    static PyObject *tp_richcompare(PyObject *lhs, PyObject *rhs, int op)
    {
        if (op != Py_EQ && op != Py_NE)
            Py_RETURN_NOTIMPLEMENTED;

        bool equal;
        try {
            equal = from_python(lhs) == from_python(rhs);
        } catch (conversion_error &) {
            Py_RETURN_NOTIMPLEMENTED;
        }

        PyObject *result;
        if (op == Py_EQ)
            result = equal ? Py_True : Py_False;
        else
            result = !equal ? Py_True : Py_False;

        Py_INCREF(result);
        return result;
    }

    static PyObject *nb_true_divide(scr_atom *self, PyObject *rhs)
    {
        if (Py_TYPE(rhs) != &PyUnicode_Type)
            Py_RETURN_NOTIMPLEMENTED;

        auto bytes = PyUnicode_AsUTF8String(rhs);
        if (!bytes)
            return nullptr;
        DRNSF_ON_EXIT { Py_DECREF(bytes); };

        std::string_view str(
            PyBytes_AsString(bytes),
            PyBytes_Size(bytes)
        );
        if (str.size() == 0) {
            PyErr_SetString(
                PyExc_ValueError,
                "divide: atom name may not be zero-length"
            );
            return nullptr;
        }
        for (auto c : str) {
            if (!res::atom::is_valid_char(c)) {
                PyErr_SetString(
                    PyExc_ValueError,
                    "divide: invalid character in name"
                );
                return nullptr;
            }
        }

        return to_python(self->v / str);
    }

    DECLARE_GETTER(scr_atom, parent);
    DECLARE_GETTER(scr_atom, basename);
    DECLARE_GETTER(scr_atom, dirname);
    DECLARE_GETTER(scr_atom, path);
    DECLARE_GETTER(scr_atom, asset);

    DECLARE_METHOD_NOARGS(scr_atom, firstchild);
    DECLARE_METHOD_NOARGS(scr_atom, nextsibling);

    static void install()
    {
        if (type)
            return;

        static PyGetSetDef getset[] = {
            GETSET_RO(parent),
            GETSET_RO(basename),
            GETSET_RO(dirname),
            GETSET_RO(path),
            GETSET_RO(asset),
            {}
        };

        static PyMethodDef methods[] = {
            METHOD(firstchild, METH_NOARGS),
            METHOD(nextsibling, METH_NOARGS),
            {}
        };

        static PyType_Slot slots[] = {
            SLOT_FN(tp_new),
            SLOT_FN(tp_dealloc),
            SLOT_FN(tp_repr),
            SLOT_FN(tp_str),
            SLOT_FN(tp_richcompare),
            SLOT_FN(nb_true_divide),
            { Py_tp_getset, getset },
            { Py_tp_methods, methods },
            {}
        };

        static PyType_Spec spec = {
            "drnsf.Atom",
            sizeof(scr_atom),
            0,
            0,
            slots
        };

        auto type_o = PyType_FromSpec(&spec);
        type = reinterpret_cast<PyTypeObject *>(type_o);
        if (!type) {
            PyErr_Print();
            std::abort();
        }

        if (PyDict_SetItemString(s_dict, "Atom", type_o) == -1) {
            PyErr_Print();
            std::abort();
        }
    }
};

// (internal type) scr_specificasset
// Scripting type for various asset types.
template <typename AssetType>
struct scr_specificasset :
    scr_specificasset<typename reflect::asset_type_info<AssetType>::base_type> {

    static inline PyTypeObject *type;

    using base_scr_specificasset = scr_specificasset<
        typename reflect::asset_type_info<AssetType>::base_type>;

    using base_scr_specificasset::tp_new;
    using base_scr_specificasset::tp_dealloc;

    static void install()
    {
        if (type)
            return;

        using type_info = reflect::asset_type_info<AssetType>;
        using base_type = typename type_info::base_type;

        static PyGetSetDef getset[] = {
            {}
        };

        scr_specificasset<base_type>::install();

        static PyType_Slot slots[] = {
            SLOT_FN(tp_new),
            SLOT_FN(tp_dealloc),
            { Py_tp_getset, getset },
            { Py_tp_base, scr_specificasset<base_type>::type },
            {}
        };

        std::string type_fullname = "drnsf.";
        type_fullname += type_info::name;

        PyType_Spec spec = {
            type_fullname.c_str(),
            sizeof(scr_specificasset<base_type>),
            0,
            Py_TPFLAGS_BASETYPE,
            slots
        };

        auto type_o = PyType_FromSpec(&spec);
        type = reinterpret_cast<PyTypeObject *>(type_o);
        if (!type) {
            PyErr_Print();
            std::abort();
        }

        if (PyDict_SetItemString(s_dict, type_info::name, type_o) == -1) {
            PyErr_Print();
            std::abort();
        }
    }
};

// (internal type) scr_asset
// Scripting type for asset types.
struct scr_asset : scr_base {
    using native_type = res::asset *;

    static inline PyTypeObject *type;

    native_type asset_p;

    static native_type from_python(PyObject *obj)
    {
        if (obj == Py_None)
            return nullptr;

        if (!PyObject_IsInstance(obj, reinterpret_cast<PyObject *>(type)))
            throw conversion_error("scr_asset: incompatible type");

        return static_cast<scr_asset *>(obj)->asset_p;
    }

    static PyObject *to_python(native_type value) noexcept
    {
        if (!value)
            Py_RETURN_NONE;

        if (value->m_scripthandle.p) {
            auto obj = static_cast<scr_asset *>(value->m_scripthandle.p);
            Py_INCREF(obj);
            return obj;
        }

        PyTypeObject *specific_type;

        auto handler = [&](auto asset) {
            specific_type = scr_specificasset<
                std::remove_pointer_t<decltype(asset)>
            >::type;
        };

        util::dynamic_call<
            const decltype(handler) &,
            res::asset,
            gfx::frame,
            gfx::anim,
            gfx::mesh,
            gfx::model,
            gfx::world,
            misc::raw_data,
            nsf::archive,
            nsf::spage,
            nsf::raw_entry,
            nsf::wgeo_v2,
            nsf::entry,
            res::asset>(handler, value);

        try {
            auto obj = new scr_asset();
            obj->ob_refcnt = 1;
            obj->ob_type = specific_type;
            obj->asset_p = value;
            obj->asset_p->m_scripthandle.p = obj;
            obj->asset_p->m_scripthandle.dtor = [](void *p) noexcept {
                auto obj = static_cast<scr_asset *>(p);
                obj->asset_p = nullptr;
            };
            return obj;
        } catch (std::bad_alloc &) {
            return PyErr_NoMemory();
        }
    }

    static PyObject *tp_new(
        PyObject *subtype,
        PyObject *args,
        PyObject *kwds) noexcept
    {
        PyErr_SetString(PyExc_TypeError, "drnsf.Asset cannot be constructed");
        return nullptr;
    }

    static void tp_dealloc(scr_asset *self) noexcept
    {
        self->asset_p->m_scripthandle = {};
        delete self;
    }

    DECLARE_GETTER(scr_asset, project);
    DECLARE_GETTER(scr_asset, name);

    static void install()
    {
        if (type)
            return;

        static PyGetSetDef getset[] = {
            GETSET_RO(project),
            GETSET_RO(name),
            {}
        };

        static PyType_Slot slots[] = {
            SLOT_FN(tp_new),
            SLOT_FN(tp_dealloc),
            { Py_tp_getset, getset },
            {}
        };

        static PyType_Spec spec = {
            "drnsf.Asset",
            sizeof(scr_asset),
            0,
            Py_TPFLAGS_BASETYPE,
            slots
        };

        auto type_o = PyType_FromSpec(&spec);
        type = reinterpret_cast<PyTypeObject *>(type_o);
        if (!type) {
            PyErr_Print();
            std::abort();
        }

        if (PyDict_SetItemString(s_dict, "Asset", type_o) == -1) {
            PyErr_Print();
            std::abort();
        }
    }
};

template <>
struct scr_specificasset<res::asset> : scr_asset {};

// (internal type) scr_globalfns
// Non-instantiated type which contains global functions.
struct scr_globalfns : scr_base {
    static inline bool installed = false;

    DECLARE_METHOD_NOARGS(scr_globalfns, getcontextproject);
    DECLARE_METHOD_O(scr_globalfns, pushproject);
    DECLARE_METHOD_NOARGS(scr_globalfns, popproject);
    DECLARE_METHOD_NOARGS(scr_globalfns, P);

    static void install()
    {
        if (installed)
            return;

        static PyMethodDef methods[] = {
            METHOD(getcontextproject, METH_NOARGS),
            METHOD(pushproject, METH_O),
            METHOD(popproject, METH_NOARGS),
            METHOD(P, METH_NOARGS),
            {}
        };

        for (auto p = methods; p->ml_name; p++) {
            auto fn = PyCFunction_New(p, nullptr);
            if (!fn) {
                PyErr_Print();
                std::abort();
            }

            if (PyDict_SetItemString(s_dict, p->ml_name, fn) == -1) {
                PyErr_Print();
                std::abort();
            }
            Py_DECREF(fn);
        }

        installed = true;
    }
};

DEFINE_GETTER(scr_project, root)
{
    return scr_atom::to_python(self->proj_p->get_asset_root());
}

DEFINE_GETTER(scr_atom, parent)
{
    if (self->v.get_depth() == 0)
        Py_RETURN_NONE;

    return scr_atom::to_python(self->v.get_parent());
}

DEFINE_GETTER(scr_atom, basename)
{
    return PyUnicode_FromString(self->v.basename().c_str());
}

DEFINE_GETTER(scr_atom, dirname)
{
    return PyUnicode_FromString(self->v.dirname().c_str());
}

DEFINE_GETTER(scr_atom, path)
{
    return PyUnicode_FromString(self->v.path().c_str());
}

DEFINE_GETTER(scr_atom, asset)
{
    return scr_asset::to_python(self->v.get());
}

DEFINE_METHOD_NOARGS(scr_atom, firstchild)
{
    return scr_atom::to_python(self->v.first_child());
}

DEFINE_METHOD_NOARGS(scr_atom, nextsibling)
{
    return scr_atom::to_python(self->v.next_sibling());
}

DEFINE_GETTER(scr_asset, project)
{
    Py_RETURN_NONE;
    // TODO
}

DEFINE_GETTER(scr_asset, name)
{
    if (!self->asset_p)
        // TODO - error
        Py_RETURN_NONE;

    return scr_atom::to_python(self->asset_p->get_name());
}

DEFINE_METHOD_NOARGS(scr_globalfns, getcontextproject)
{
    auto engp = engine_impl::get();
    if (!engp)
        // TODO - error
        Py_RETURN_NONE;

    if (!engp->m_ctx)
        Py_RETURN_NONE;

    std::shared_ptr<res::project> proj_p;
    // FIXME - run this section only on the main thread!
    proj_p = engp->m_ctx->get_proj();
    // FIXME - end above section
    return scr_project::to_python(proj_p);
}

DEFINE_METHOD_O(scr_globalfns, pushproject)
{
    auto engp = engine_impl::get();
    if (!engp)
        // TODO - error
        Py_RETURN_NONE;

    std::shared_ptr<res::project> proj;
    try {
        proj = scr_project::from_python(arg);
    } catch (conversion_error &) {
        // TODO - error
        Py_RETURN_NONE;
    }

    engp->m_project_stack.push_back(proj);

    Py_RETURN_NONE;
}

DEFINE_METHOD_NOARGS(scr_globalfns, popproject)
{
    auto engp = engine_impl::get();
    if (!engp)
        // TODO - error
        Py_RETURN_NONE;

    if (engp->m_project_stack.empty())
        // TODO - error
        Py_RETURN_NONE;

    engp->m_project_stack.pop_back();

    Py_RETURN_NONE;
}

DEFINE_METHOD_NOARGS(scr_globalfns, P)
{
    auto engp = engine_impl::get();
    if (!engp)
        // TODO - error
        Py_RETURN_NONE;

    if (!engp->m_project_stack.empty()) {
        return scr_project::to_python(engp->m_project_stack.back());
    }

    if (!engp->m_ctx)
        Py_RETURN_NONE;

    std::shared_ptr<res::project> proj_p;
    // FIXME - run this section only on the main thread!
    proj_p = engp->m_ctx->get_proj();
    // FIXME - end above section
    return scr_project::to_python(proj_p);

    Py_RETURN_NONE;
}

#undef SLOT_FN
#undef GETSET_RO
#undef GETSET_RW
#undef METHOD
#undef DECLARE_GETTER
#undef DECLARE_SETTER
#undef DEFINE_GETTER
#undef DEFINE_SETTER
#undef DECLARE_METHOD_NOARGS
#undef DEFINE_METHOD_NOARGS

}

// declared in scripting.hh
void init()
{
    if (s_init_state == init_state::ready)
        return; // no-op
    if (s_init_state == init_state::failed)
        throw std::runtime_error("scripting::init: init previously failed");
    if (s_init_state == init_state::finished)
        throw std::runtime_error("scripting::init: already shutdown");

    // Set the initialization state to 'failed'. If the function exits abruptly
    // such as from a thrown exception, this will be the resulting state.
    s_init_state = init_state::failed;

    // Setup builtin module initializers. This must be called prior to calling
    // `Py_Initialize'.
    int err = PyImport_AppendInittab("drnsf", drnsf_module_init);
    if (err == -1) {
        PyErr_Print();
        std::abort();
    }

    // If this function fails, the entire process is killed unfortunately. This
    // is solved in PEP 587 which is not available in any stable builds at time
    // of writing.
    Py_Initialize();

    PyEval_InitThreads();
    s_main_threadstate = PyThreadState_Get();

    auto module = PyImport_AddModule("drnsf");
    if (!module) {
        PyErr_Print();
        std::abort();
    }

    s_dict = PyModule_GetDict(module);
    if (!s_dict) {
        PyErr_Print();
        std::abort();
    }
    Py_INCREF(s_dict);

    scr_project::install();
    scr_atom::install();
    scr_globalfns::install();

    scr_specificasset<gfx::frame>::install();
    scr_specificasset<gfx::anim>::install();
    scr_specificasset<gfx::mesh>::install();
    scr_specificasset<gfx::model>::install();
    scr_specificasset<gfx::world>::install();
    scr_specificasset<misc::raw_data>::install();
    scr_specificasset<nsf::archive>::install();
    scr_specificasset<nsf::spage>::install();
    scr_specificasset<nsf::raw_entry>::install();
    scr_specificasset<nsf::wgeo_v2>::install();
    scr_specificasset<nsf::entry>::install();
    scr_specificasset<res::asset>::install();

    auto code = Py_CompileString(
        reinterpret_cast<const char *>(embed::drnsf_py::data),
        "drnsf.py",
        Py_file_input
    );
    if (!code) {
        PyErr_Print();
        std::abort();
    }

    auto nonnative_module = PyImport_ExecCodeModule("drnsf._nonnative", code);
    Py_DECREF(code);
    if (!nonnative_module) {
        PyErr_Print();
        std::abort();
    }

    s_nonnative_dict = PyModule_GetDict(nonnative_module);
    if (!s_nonnative_dict) {
        PyErr_Print();
        std::abort();
    }
    Py_INCREF(s_nonnative_dict);
    Py_DECREF(nonnative_module);

    Py_DECREF(module);
    PyEval_ReleaseThread(s_main_threadstate);

    s_init_state = init_state::ready;

    // FIXME temporary hack until scripting is thread-safe
    lock();
    // this hack lock is released during select()/WaitForMulitipleObjectsEx()
}

// declared in scripting.hh
void shutdown() noexcept
{
    if (s_init_state != init_state::ready)
        return;

    // One final lock. This is never released.
    lock();

    Py_Finalize();

    s_init_state = init_state::finished;
}

// declared in scripting.hh
bool is_init() noexcept
{
    return s_init_state == init_state::ready;
}

// declared in scripting.hh
void lock() noexcept
{
    if (s_init_state != init_state::ready)
        return;
    if (s_lockcount < 0)
        std::abort();

    if (s_lockcount == 0)
        PyEval_AcquireThread(s_main_threadstate);

    s_lockcount++;
}

// declared in scripting.hh
void unlock()
{
    if (s_init_state != init_state::ready)
        return;
    if (s_lockcount < 0)
        std::abort();
    if (s_lockcount == 0)
        throw std::runtime_error("scripting::unlock: not locked");

    if (s_lockcount == 1)
        PyEval_ReleaseThread(s_main_threadstate);

    s_lockcount--;
}

// declared in scripting.hh
engine::engine(edit::context *ctx)
{
    if (s_init_state != init_state::ready) {
        M = nullptr;
        return;
    }

    lock();
    DRNSF_ON_EXIT { unlock(); };

    M = new engine_impl;
    try {
        M->m_ctx = ctx;

        M->m_interp = Py_NewInterpreter();

        auto module = PyImport_ImportModule("drnsf");
        if (!module) {
            // TODO - proper python exception grabbing
            Py_EndInterpreter(M->m_interp);
            PyThreadState_Swap(s_main_threadstate);
            throw std::runtime_error("scripting::engine: python error");
        }

        auto pp = static_cast<engine_impl **>(PyModule_GetState(module));
        if (!pp) {
            // TODO - proper python exception grabbing
            Py_EndInterpreter(M->m_interp);
            PyThreadState_Swap(s_main_threadstate);
            throw std::runtime_error("scripting::engine: module bugged");
        }

        *pp = M;

        PyThreadState_Swap(s_main_threadstate);
    } catch (...) {
        delete M;
        throw;
    }
}

// declared in scripting.hh
engine::~engine()
{
    if (!M)
        return;

    lock();
    PyThreadState_Swap(M->m_interp);
    Py_EndInterpreter(M->m_interp);
    PyThreadState_Swap(s_main_threadstate);
    unlock();

    delete M;
}

// declared in scripting.hh
void engine::start_console()
{
    if (!M)
        throw std::logic_error("scripting::engine::start_console: not init");

    lock();
    DRNSF_ON_EXIT { unlock(); };

    PyThreadState_Swap(M->m_interp);
    DRNSF_ON_EXIT { PyThreadState_Swap(s_main_threadstate); };

    auto fn = PyDict_GetItemString(s_nonnative_dict, "startconsole");
    if (!fn) {
        // TODO
        throw std::runtime_error("scripting::engine::start_console: bugged");
    }
    Py_INCREF(fn);
    DRNSF_ON_EXIT { Py_DECREF(fn); };

    auto result = PyObject_CallObject(fn, nullptr);
    if (!result) {
        // TODO - better exception
        throw std::runtime_error("scripting::engine::start_console: bugged");
    }
    Py_DECREF(result);
}

// declared in scripting.hh
handle::~handle() noexcept
{
    lock();

    if (dtor)
        dtor(p);

    unlock();
}

}
}
