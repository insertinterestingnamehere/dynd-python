//
// Copyright (C) 2011-15 DyND Developers
// BSD 2-Clause License, see LICENSE.txt
//

#include <Python.h>

#include <dynd/shape_tools.hpp>
#include <dynd/types/fixed_string_type.hpp>
#include <dynd/types/struct_type.hpp>

#include "array_as_pep3118.hpp"
#include "array_functions.hpp"
#include "utility_functions.hpp"

using namespace std;
using namespace dynd;
using namespace pydynd;

static void debug_print_getbuffer_flags(std::ostream &o, int flags)
{
  cout << "Requested buffer flags " << flags << "\n";
  if ((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE)
    cout << "  PyBUF_WRITABLE\n";
  if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT)
    cout << "  PyBUF_FORMAT\n";
  if ((flags & PyBUF_ND) == PyBUF_ND)
    cout << "  PyBUF_ND\n";
  if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES)
    cout << "  PyBUF_STRIDES\n";
  if ((flags & PyBUF_C_CONTIGUOUS) == PyBUF_C_CONTIGUOUS)
    cout << "  PyBUF_C_CONTIGUOUS\n";
  if ((flags & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS)
    cout << "  PyBUF_F_CONTIGUOUS\n";
  if ((flags & PyBUF_ANY_CONTIGUOUS) == PyBUF_ANY_CONTIGUOUS)
    cout << "  PyBUF_ANY_CONTIGUOUS\n";
  if ((flags & PyBUF_INDIRECT) == PyBUF_INDIRECT)
    cout << "  PyBUF_INDIRECT\n";
}

static void debug_print_py_buffer(std::ostream &o, const Py_buffer *buffer, int flags)
{
  cout << "PEP 3118 buffer info:\n";
  cout << "  buf: " << buffer->buf << "\n";
  cout << "  obj: " << (void *)buffer->obj << "\n";
  cout << "  len: " << buffer->len << "\n";
  cout << "  itemsize: " << buffer->itemsize << "\n";
  cout << "  readonly: " << buffer->readonly << "\n";
  cout << "  ndim: " << buffer->ndim << "\n";
  cout << "  format: " << (buffer->format ? buffer->format : "<NULL>") << "\n";
  cout << "  shape: ";
  for (int i = 0; i < buffer->ndim; ++i)
    cout << buffer->shape[i] << " ";
  cout << "\n";
  cout << "  strides: ";
  for (int i = 0; i < buffer->ndim; ++i)
    cout << buffer->strides[i] << " ";
  cout << "\n";
  cout << "  internal: " << buffer->internal << endl;
}

static void append_pep3118_format(intptr_t &out_itemsize, const ndt::type &tp, const char *arrmeta,
                                  std::stringstream &o)
{
  switch (tp.get_id()) {
  case bool_id:
    o << "?";
    out_itemsize = 1;
    return;
  case int8_id:
    o << "b";
    out_itemsize = 1;
    return;
  case int16_id:
    o << "h";
    out_itemsize = 2;
    return;
  case int32_id:
    o << "i";
    out_itemsize = 4;
    return;
  case int64_id:
    o << "q";
    out_itemsize = 8;
    return;
  case uint8_id:
    o << "B";
    out_itemsize = 1;
    return;
  case uint16_id:
    o << "H";
    out_itemsize = 2;
    return;
  case uint32_id:
    o << "I";
    out_itemsize = 4;
    return;
  case uint64_id:
    o << "Q";
    out_itemsize = 8;
    return;
  case float32_id:
    o << "f";
    out_itemsize = 4;
    return;
  case float64_id:
    o << "d";
    out_itemsize = 8;
    return;
  case complex_float32_id:
    o << "Zf";
    out_itemsize = 8;
    return;
  case complex_float64_id:
    o << "Zd";
    out_itemsize = 16;
    return;
  case fixed_string_id:
    switch (tp.extended<ndt::fixed_string_type>()->get_encoding()) {
    case string_encoding_ascii: {
      intptr_t element_size = tp.get_data_size();
      o << element_size << "s";
      out_itemsize = element_size;
      return;
    }
    // TODO: Couldn't find documentation for UCS-2 character code?
    case string_encoding_utf_32: {
      intptr_t element_size = tp.get_data_size();
      o << (element_size / 4) << "w";
      out_itemsize = element_size;
      return;
    }
    default:
      break;
    }
    // Pass through to error
    break;
  case fixed_dim_id: {
    ndt::type child_tp = tp;
    o << "(";
    do {
      const ndt::fixed_dim_type *tdt = child_tp.extended<ndt::fixed_dim_type>();
      intptr_t dim_size = tdt->get_fixed_dim_size();
      o << dim_size;
      if (child_tp.get_data_size() != tdt->get_element_type().get_data_size() * dim_size) {
        stringstream ss;
        ss << "Cannot convert dynd type " << tp << " into a PEP 3118 format because it is not C-order";
        throw dynd::type_error(ss.str());
      }
      o << ")";
      child_tp = tdt->get_element_type();
    } while (child_tp.get_id() == fixed_dim_id && (o << ","));
    append_pep3118_format(out_itemsize, child_tp, arrmeta, o);
    out_itemsize = tp.get_data_size();
    return;
  }
  case struct_id: {
    o << "T{";
    const ndt::struct_type *tdt = tp.extended<ndt::struct_type>();
    size_t num_fields = tdt->get_field_count();
    const uintptr_t *offsets = reinterpret_cast<const uintptr_t *>(arrmeta);
    const uintptr_t *arrmeta_offsets = tdt->get_arrmeta_offsets_raw();
    size_t format_offset = 0;
    for (size_t i = 0; i != num_fields; ++i) {
      size_t offset = offsets[i];
      // Add padding bytes
      while (offset > format_offset) {
        o << "x";
        ++format_offset;
      }
      if (offset < format_offset) {
        // DyND allows the order of fields in memory to differ from the logical
        // order, something not supported by PEP 3118
        stringstream ss;
        ss << "Cannot convert dynd type " << tp << " with out of order data layout into a PEP 3118 format string";
        throw type_error(ss.str());
      }
      // The field's type
      append_pep3118_format(out_itemsize, tdt->get_field_type(i), arrmeta ? (arrmeta + arrmeta_offsets[i]) : NULL, o);
      format_offset += out_itemsize;
      // Append the name
      o << ":" << tdt->get_field_name(i) << ":";
    }
    out_itemsize = format_offset;
    o << "}";
    return;
  }
  default:
    break;
  }
  stringstream ss;
  ss << "Cannot convert dynd type " << tp << " into a PEP 3118 format string";
  throw dynd::type_error(ss.str());
}

std::string pydynd::make_pep3118_format(intptr_t &out_itemsize, const ndt::type &tp, const char *arrmeta)
{
  std::stringstream result;
  // Specify native alignment/storage if it's a builtin scalar type
  if (tp.is_builtin()) {
    result << "@";
  }
  append_pep3118_format(out_itemsize, tp, arrmeta, result);
  return result.str();
}

static void array_getbuffer_pep3118_bytes(const ndt::type &tp, const char *arrmeta, char *data, Py_buffer *buffer,
                                          int flags)
{
  buffer->itemsize = 1;
  if (flags & PyBUF_FORMAT) {
    buffer->format = (char *)"c";
  }
  else {
    buffer->format = NULL;
  }
  buffer->ndim = 1;
#if PY_VERSION_HEX == 0x02070000
  buffer->internal = NULL;
  buffer->shape = &buffer->smalltable[0];
  buffer->strides = &buffer->smalltable[1];
#else
  buffer->internal = malloc(2 * sizeof(intptr_t));
  buffer->shape = reinterpret_cast<Py_ssize_t *>(buffer->internal);
  buffer->strides = buffer->shape + 1;
#endif
  buffer->strides[0] = 1;

  if (tp.get_id() == bytes_id) {
    // Variable-length bytes type
    buffer->buf = reinterpret_cast<bytes *>(data)->begin();
    buffer->len = reinterpret_cast<bytes *>(data)->size();
  }
  else {
    // Fixed-length bytes type
    buffer->len = tp.get_data_size();
  }
  buffer->shape[0] = buffer->len;
}

int pydynd::array_getbuffer_pep3118(PyObject *ndo, Py_buffer *buffer, int flags)
{
  // debug_print_getbuffer_flags(cout, flags);
  try {
    buffer->shape = NULL;
    buffer->strides = NULL;
    buffer->suboffsets = NULL;
    buffer->format = NULL;
    buffer->obj = ndo;
    buffer->internal = NULL;
    Py_INCREF(ndo);
    if (!PyObject_TypeCheck(ndo, get_array_pytypeobject())) {
      throw runtime_error("array_getbuffer_pep3118 called on a non-array");
    }
    nd::array &n = pydynd::array_to_cpp_ref(ndo);
    ndt::type tp = n.get_type();

    // Check if a writable buffer is requested
    if ((flags & PyBUF_WRITABLE) && !(n.get_flags() & nd::write_access_flag)) {
      throw runtime_error("dynd array is not writable");
    }
    buffer->readonly = ((n.get_flags() & nd::write_access_flag) == 0);
    buffer->buf = const_cast<char *>(n.cdata());

    if (tp.get_id() == bytes_id || tp.get_id() == fixed_bytes_id) {
      array_getbuffer_pep3118_bytes(tp, n.get()->metadata(), const_cast<char *>(n.cdata()), buffer, flags);
      return 0;
    }

    buffer->ndim = (int)tp.get_ndim();
    if (((flags & PyBUF_ND) != PyBUF_ND) && buffer->ndim > 1) {
      stringstream ss;
      ss << "dynd type " << n.get_type() << " is multidimensional, but PEP 3118 request is not ND";
      throw dynd::type_error(ss.str());
    }

    // Create the format, and allocate the dynamic memory but Py_buffer needs
    char *uniform_arrmeta = n.get()->metadata();
    ndt::type uniform_tp = tp.get_type_at_dimension(&uniform_arrmeta, buffer->ndim);
    if ((flags & PyBUF_FORMAT) || uniform_tp.get_data_size() == 0) {
      // If the array data type doesn't have a fixed size, make_pep3118 fills
      // buffer->itemsize as a side effect
      std::string format = make_pep3118_format(buffer->itemsize, uniform_tp, uniform_arrmeta);
      if (flags & PyBUF_FORMAT) {
        buffer->internal = malloc(2 * buffer->ndim * sizeof(intptr_t) + format.size() + 1);
        buffer->shape = reinterpret_cast<Py_ssize_t *>(buffer->internal);
        buffer->strides = buffer->shape + buffer->ndim;
        buffer->format = reinterpret_cast<char *>(buffer->strides + buffer->ndim);
        memcpy(buffer->format, format.c_str(), format.size() + 1);
      }
      else {
        buffer->format = NULL;
        buffer->internal = malloc(2 * buffer->ndim * sizeof(intptr_t));
        buffer->shape = reinterpret_cast<Py_ssize_t *>(buffer->internal);
        buffer->strides = buffer->shape + buffer->ndim;
      }
    }
    else {
      buffer->format = NULL;
      buffer->itemsize = uniform_tp.get_data_size();
      buffer->internal = malloc(2 * buffer->ndim * sizeof(intptr_t));
      buffer->shape = reinterpret_cast<Py_ssize_t *>(buffer->internal);
      buffer->strides = buffer->shape + buffer->ndim;
    }

    // Fill in the shape and strides
    const char *arrmeta = n.get()->metadata();
    for (int i = 0; i < buffer->ndim; ++i) {
      switch (tp.get_id()) {
      case fixed_dim_id: {
        const ndt::fixed_dim_type *tdt = tp.extended<ndt::fixed_dim_type>();
        const fixed_dim_type_arrmeta *md = reinterpret_cast<const fixed_dim_type_arrmeta *>(arrmeta);
        buffer->shape[i] = md->dim_size;
        buffer->strides[i] = md->stride;
        arrmeta += sizeof(fixed_dim_type_arrmeta);
        tp = tdt->get_element_type();
        break;
      }
      default: {
        stringstream ss;
        ss << "Cannot get a strided view of dynd type " << n.get_type() << " for PEP 3118 buffer";
        throw runtime_error(ss.str());
      }
      }
    }

    // Get the total length of the buffer in bytes
    buffer->len = buffer->itemsize;
    for (int i = 0; i < buffer->ndim; ++i) {
      buffer->len *= buffer->shape[i];
    }

    // Check that any contiguity requirements are satisfied
    if ((flags & PyBUF_C_CONTIGUOUS) == PyBUF_C_CONTIGUOUS || (flags & PyBUF_STRIDES) != PyBUF_STRIDES) {
      if (!strides_are_c_contiguous(buffer->ndim, buffer->itemsize, buffer->shape, buffer->strides)) {
        throw runtime_error("dynd array is not C-contiguous as requested for PEP 3118 buffer");
      }
    }
    else if ((flags & PyBUF_F_CONTIGUOUS) == PyBUF_F_CONTIGUOUS) {
      if (!strides_are_f_contiguous(buffer->ndim, buffer->itemsize, buffer->shape, buffer->strides)) {
        throw runtime_error("dynd array is not F-contiguous as requested for PEP 3118 buffer");
      }
    }
    else if ((flags & PyBUF_ANY_CONTIGUOUS) == PyBUF_ANY_CONTIGUOUS) {
      if (!strides_are_c_contiguous(buffer->ndim, buffer->itemsize, buffer->shape, buffer->strides) &&
          !strides_are_f_contiguous(buffer->ndim, buffer->itemsize, buffer->shape, buffer->strides)) {
        throw runtime_error("dynd array is not C-contiguous nor F-contiguous "
                            "as requested for PEP 3118 buffer");
      }
    }

    // debug_print_py_buffer(cout, buffer, flags);

    return 0;
  }
  catch (const std::exception &e) {
    // Numpy likes to hide these errors and repeatedly try again, so it's useful
    // to see what's happening
    // cout << "ERROR " << e.what() << endl;
    Py_DECREF(ndo);
    buffer->obj = NULL;
    if (buffer->internal != NULL) {
      free(buffer->internal);
      buffer->internal = NULL;
    }
    PyErr_SetString(PyExc_BufferError, e.what());
    return -1;
  }
  catch (const dynd::dynd_exception &e) {
    // Numpy likes to hide these errors and repeatedly try again, so it's useful
    // to see what's happening
    // cout << "ERROR " << e.what() << endl;
    Py_DECREF(ndo);
    buffer->obj = NULL;
    if (buffer->internal != NULL) {
      free(buffer->internal);
      buffer->internal = NULL;
    }
    PyErr_SetString(PyExc_BufferError, e.what());
    return -1;
  }
}

int pydynd::array_releasebuffer_pep3118(PyObject *ndo, Py_buffer *buffer)
{
  try {
    if (buffer->internal != NULL) {
      free(buffer->internal);
      buffer->internal = NULL;
    }
    return 0;
  }
  catch (const std::exception &e) {
    PyErr_SetString(PyExc_BufferError, e.what());
    return -1;
  }
  catch (const dynd::dynd_exception &e) {
    PyErr_SetString(PyExc_BufferError, e.what());
    return -1;
  }
}
