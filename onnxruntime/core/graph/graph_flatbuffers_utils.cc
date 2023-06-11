// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "graph_flatbuffers_utils.h"

#include "flatbuffers/flatbuffers.h"

#include "core/common/narrow.h"
#include "core/flatbuffers/flatbuffers_utils.h"
#include "core/flatbuffers/schema/ort.fbs.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/tensor_external_data_info.h"
#include "core/graph/graph.h"
#include "core/providers/cpu/cpu_execution_provider.h"

using namespace ONNX_NAMESPACE;

namespace onnxruntime::fbs::utils {

#if !defined(ORT_MINIMAL_BUILD)

template <typename DimsFieldType>
inline flatbuffers::Offset<flatbuffers::Vector<int64_t>>
SaveDims(flatbuffers::FlatBufferBuilder& builder, const DimsFieldType& dims) {
  std::vector<int64_t> dims_data(dims.size());
  std::copy(dims.begin(), dims.end(), dims_data.begin());
  return builder.CreateVector(dims_data);
}

Status SaveInitializerOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                const TensorProto& initializer,
                                const Path& model_path,
                                flatbuffers::Offset<fbs::Tensor>& fbs_tensor) {
  auto name = SaveStringToOrtFormat(builder, initializer.has_name(), initializer.name());
  auto doc_string = SaveStringToOrtFormat(builder, initializer.has_doc_string(), initializer.doc_string());
  auto dims = SaveDims(builder, initializer.dims());

  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> string_data;
  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> raw_data;

  auto src_type = initializer.data_type();
  const bool has_string_data = src_type == ONNX_NAMESPACE::TensorProto_DataType_STRING;
  if (has_string_data) {
    std::vector<std::string> string_data_vec(initializer.string_data().size());
    std::copy(initializer.string_data().cbegin(), initializer.string_data().cend(), string_data_vec.begin());
    string_data = builder.CreateVectorOfStrings(string_data_vec);
  } else {
    std::vector<uint8_t> unpacked_tensor;
    ORT_RETURN_IF_ERROR(
        onnxruntime::utils::UnpackInitializerData(initializer, model_path, unpacked_tensor));
    raw_data = builder.CreateVector(unpacked_tensor.data(), unpacked_tensor.size());
  }

  fbs::TensorBuilder tb(builder);
  tb.add_name(name);
  tb.add_doc_string(doc_string);
  tb.add_dims(dims);
  tb.add_data_type(static_cast<fbs::TensorDataType>(src_type));
  if (has_string_data)
    tb.add_string_data(string_data);
  else
    tb.add_raw_data(raw_data);
  fbs_tensor = tb.Finish();
  return Status::OK();
}

#if !defined(DISABLE_SPARSE_TENSORS)
Status SaveSparseInitializerOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                                      const ONNX_NAMESPACE::SparseTensorProto& initializer,
                                      const Path& model_path,
                                      flatbuffers::Offset<fbs::SparseTensor>& fbs_sparse_tensor) {
  // values
  const auto& values = initializer.values();
  flatbuffers::Offset<fbs::Tensor> values_off;
  ORT_RETURN_IF_ERROR(SaveInitializerOrtFormat(builder, values, model_path, values_off));

  // Indicies
  const auto& indicies = initializer.indices();
  flatbuffers::Offset<fbs::Tensor> indicies_off;
  ORT_RETURN_IF_ERROR(SaveInitializerOrtFormat(builder, indicies, model_path, indicies_off));

  // Shape
  auto shape = SaveDims(builder, initializer.dims());

  fbs::SparseTensorBuilder stb(builder);
  stb.add_values(values_off);
  stb.add_indices(indicies_off);
  stb.add_dims(shape);

  fbs_sparse_tensor = stb.Finish();

  return Status::OK();
}
#endif  // !defined(DISABLE_SPARSE_TENSORS)

#define GET_FBS_ATTR(BUILDER, TYPE, DATA_NAME, DATA) \
  fbs::AttributeBuilder attr_builder(BUILDER);       \
  attr_builder.add_name(name);                       \
  attr_builder.add_doc_string(doc_string);           \
  attr_builder.add_type(TYPE);                       \
  attr_builder.add_##DATA_NAME(DATA);                \
  fbs_attr = attr_builder.Finish();

#define GET_DATA_VEC(TYPE, NAME, SRC_DATA) \
  std::vector<TYPE> NAME(SRC_DATA.size()); \
  std::copy(SRC_DATA.cbegin(), SRC_DATA.cend(), NAME.begin());

Status SaveAttributeOrtFormat(flatbuffers::FlatBufferBuilder& builder,
                              const AttributeProto& attr_proto,
                              flatbuffers::Offset<fbs::Attribute>& fbs_attr,
                              const Path& model_path,
                              const onnxruntime::Graph* subgraph) {
  auto name = SaveStringToOrtFormat(builder, attr_proto.has_name(), attr_proto.name());
  auto doc_string = SaveStringToOrtFormat(builder, attr_proto.has_doc_string(), attr_proto.doc_string());
  auto type = static_cast<fbs::AttributeType>(attr_proto.type());
  switch (type) {
    case fbs::AttributeType::FLOAT: {
      GET_FBS_ATTR(builder, type, f, attr_proto.f());
    } break;
    case fbs::AttributeType::INT: {
      GET_FBS_ATTR(builder, type, i, attr_proto.i());
    } break;
    case fbs::AttributeType::STRING: {
      auto s = builder.CreateString(attr_proto.s());
      GET_FBS_ATTR(builder, type, s, s);
    } break;
    case fbs::AttributeType::TENSOR: {
      flatbuffers::Offset<fbs::Tensor> fbs_tensor;
      ORT_RETURN_IF_ERROR(
          SaveInitializerOrtFormat(builder, attr_proto.t(), model_path, fbs_tensor));
      GET_FBS_ATTR(builder, type, t, fbs_tensor);
    } break;
    case fbs::AttributeType::GRAPH: {
      ORT_RETURN_IF(nullptr == subgraph, "Graph attribute value was null. Invalid ORT format model.");
      flatbuffers::Offset<fbs::Graph> fbs_graph;
      ORT_RETURN_IF_ERROR(subgraph->SaveToOrtFormat(builder, fbs_graph));
      GET_FBS_ATTR(builder, type, g, fbs_graph);
    } break;
    case fbs::AttributeType::FLOATS: {
      GET_DATA_VEC(float, floats_vec_, attr_proto.floats());
      auto floats = builder.CreateVector(floats_vec_);
      GET_FBS_ATTR(builder, type, floats, floats);
    } break;
    case fbs::AttributeType::INTS: {
      GET_DATA_VEC(int64_t, ints_vec_, attr_proto.ints());
      auto ints = builder.CreateVector(ints_vec_);
      GET_FBS_ATTR(builder, type, ints, ints);
    } break;
    case fbs::AttributeType::STRINGS: {
      GET_DATA_VEC(std::string, strings_vec_, attr_proto.strings());
      auto strings = builder.CreateVectorOfStrings(strings_vec_);
      GET_FBS_ATTR(builder, type, strings, strings);
    } break;
    case fbs::AttributeType::TENSORS: {
      std::vector<flatbuffers::Offset<fbs::Tensor>> fbs_tensors_vec;
      fbs_tensors_vec.reserve(attr_proto.tensors().size());
      for (const auto& tensor : attr_proto.tensors()) {
        flatbuffers::Offset<fbs::Tensor> fbs_tensor;
        ORT_RETURN_IF_ERROR(
            SaveInitializerOrtFormat(builder, tensor, model_path, fbs_tensor));
        fbs_tensors_vec.push_back(fbs_tensor);
      }
      auto tensors = builder.CreateVector(fbs_tensors_vec);
      GET_FBS_ATTR(builder, type, tensors, tensors);
    } break;
    default:
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "SaveAttributeOrtFormat: Unsupported attribute type: ", fbs::EnumNameAttributeType(type));
      break;
  }

  return Status::OK();
}

#undef GET_FBS_ATTR
#undef GET_DATA_VEC

#endif

Status LoadInitializerOrtFormat(const fbs::Tensor& fbs_tensor, TensorProto& initializer,
                                const OrtFormatLoadOptions& load_options) {
  initializer.Clear();

  LOAD_STR_FROM_ORT_FORMAT(initializer, name, fbs_tensor.name());
  LOAD_STR_FROM_ORT_FORMAT(initializer, doc_string, fbs_tensor.doc_string());

  auto fbs_dims = fbs_tensor.dims();
  ORT_RETURN_IF(nullptr == fbs_dims, "Missing dimensions for initializer. Invalid ORT format model.");
  initializer.mutable_dims()->Add(fbs_dims->cbegin(), fbs_dims->cend());

  auto fbs_data_type = fbs_tensor.data_type();
  initializer.set_data_type(static_cast<int32_t>(fbs_data_type));
  if (fbs_data_type == fbs::TensorDataType::STRING) {
    auto fbs_str_data = fbs_tensor.string_data();
    ORT_RETURN_IF(nullptr == fbs_str_data, "Missing string data for initializer. Invalid ORT format model.");
    auto mutable_str_data = initializer.mutable_string_data();
    mutable_str_data->Reserve(fbs_str_data->size());
    for (const auto* fbs_str : *fbs_str_data) {
      mutable_str_data->Add(fbs_str->str());
    }
  } else {
    const auto* fbs_raw_data = fbs_tensor.raw_data();
    ORT_RETURN_IF(nullptr == fbs_raw_data, "Missing raw data for initializer. Invalid ORT format model.");

    if (load_options.can_use_flatbuffer_for_initializers && fbs_raw_data->size() > 127) {
      initializer.set_data_location(ONNX_NAMESPACE::TensorProto_DataLocation_EXTERNAL);

      static_assert(sizeof(void*) <= sizeof(ExternalDataInfo::OFFSET_TYPE));
      const void* data_offset = fbs_raw_data->Data();
      // we reinterpret_cast this back to void* in tensorprotoutils.cc:GetExtDataFromTensorProto.
      // use intptr_t as OFFSET_TYPE is signed. in theory you could get a weird looking value if the address uses the
      // high bit, but that should be unlikely in a scenario where we care about memory usage enough to use this path.
      auto offset = narrow<ExternalDataInfo::OFFSET_TYPE>(reinterpret_cast<intptr_t>(data_offset));

      ONNX_NAMESPACE::StringStringEntryProto* entry = initializer.mutable_external_data()->Add();
      entry->set_key("location");
      entry->set_value(ToUTF8String(onnxruntime::utils::kTensorProtoMemoryAddressTag));
      entry = initializer.mutable_external_data()->Add();
      entry->set_key("offset");
      entry->set_value(std::to_string(offset));
      entry = initializer.mutable_external_data()->Add();
      entry->set_key("length");
      entry->set_value(std::to_string(fbs_raw_data->size()));
    } else {
      // fbs_raw_data is uint8_t vector, so the size is byte size
      initializer.set_raw_data(fbs_raw_data->Data(), fbs_raw_data->size());
    }
  }

  return Status::OK();
}

#if !defined(DISABLE_SPARSE_TENSORS)
Status LoadSparseInitializerOrtFormat(const fbs::SparseTensor& fbs_sparse_tensor,
                                      SparseTensorProto& initializer,
                                      const OrtFormatLoadOptions& load_options) {
  SparseTensorProto loaded_initializer;
  auto fbs_values_tensor = fbs_sparse_tensor.values();
  ORT_RETURN_IF(nullptr == fbs_values_tensor, "Missing values for sparse initializer. Invalid ORT format model.");
  auto* values_tensor = loaded_initializer.mutable_values();
  ORT_RETURN_IF_ERROR(LoadInitializerOrtFormat(*fbs_values_tensor, *values_tensor, load_options));
  ORT_RETURN_IF(values_tensor->name().empty(), "Missing name for SparseTensor initializer. Invalid ORT format model.");

  auto fbs_indicies_tensor = fbs_sparse_tensor.indices();
  ORT_RETURN_IF(nullptr == fbs_indicies_tensor, "Missing indicies for sparse initializer: ", "'", values_tensor->name(), "'",
                "Invalid ORT format model.");
  auto* indicies_tensor = loaded_initializer.mutable_indices();
  ORT_RETURN_IF_ERROR(LoadInitializerOrtFormat(*fbs_indicies_tensor, *indicies_tensor, load_options));

  auto fbs_dims = fbs_sparse_tensor.dims();
  ORT_RETURN_IF(nullptr == fbs_dims, "Missing dims for sparse initializer: ", "'", values_tensor->name(), "'",
                "Invalid ORT format model.");
  loaded_initializer.mutable_dims()->Add(fbs_dims->cbegin(), fbs_dims->cend());

  swap(loaded_initializer, initializer);
  return Status::OK();
}
#endif  // !defined(DISABLE_SPARSE_TENSORS)

Status LoadAttributeOrtFormat(const fbs::Attribute& fbs_attr,
                              ONNX_NAMESPACE::AttributeProto& attr_proto,
                              std::unique_ptr<onnxruntime::Graph>& sub_graph,
                              onnxruntime::Graph& graph, onnxruntime::Node& node,
                              const OrtFormatLoadOptions& load_options,
                              const logging::Logger& logger) {
  attr_proto.Clear();
  LOAD_STR_FROM_ORT_FORMAT(attr_proto, name, fbs_attr.name());
  LOAD_STR_FROM_ORT_FORMAT(attr_proto, doc_string, fbs_attr.doc_string());

  auto type = static_cast<AttributeProto_AttributeType>(fbs_attr.type());
  attr_proto.set_type(type);
  switch (type) {
    case AttributeProto_AttributeType_FLOAT: {
      attr_proto.set_f(fbs_attr.f());
    } break;
    case AttributeProto_AttributeType_INT: {
      attr_proto.set_i(fbs_attr.i());
    } break;
    case AttributeProto_AttributeType_STRING: {
      auto fbs_str = fbs_attr.s();
      ORT_RETURN_IF(nullptr == fbs_str, "Null string attribute. Invalid ORT format model.");
      attr_proto.set_s(fbs_str->str());
    } break;
    case AttributeProto_AttributeType_TENSOR: {
      auto fbs_tensor = fbs_attr.t();
      ORT_RETURN_IF(nullptr == fbs_tensor, "Null tensor attribute. Invalid ORT format model.");
      ORT_RETURN_IF_ERROR(LoadInitializerOrtFormat(*fbs_tensor, *attr_proto.mutable_t(),
                                                   load_options));
    } break;
    case AttributeProto_AttributeType_GRAPH: {
      // If the attribute type is a graph, we will create an empty graph in attr_proto so that the ONNX checker
      // is happy in a full build, and deserialize the ORT Graph instance into the 'graph' param.
      auto fbs_graph = fbs_attr.g();
      ORT_RETURN_IF(nullptr == fbs_graph, "Null graph attribute. Invalid ORT format model.");
      attr_proto.mutable_g()->set_name("Empty graph proto from deserialization of ORT format model");
      ORT_RETURN_IF_ERROR(onnxruntime::Graph::LoadFromOrtFormat(*fbs_graph, graph, node,
                                                                load_options,
                                                                logger, sub_graph));
    } break;
    case AttributeProto_AttributeType_FLOATS: {
      auto fbs_floats = fbs_attr.floats();
      ORT_RETURN_IF(nullptr == fbs_floats, "Null floats attribute. Invalid ORT format model.");
      auto floats = attr_proto.mutable_floats();
      floats->Reserve(fbs_floats->size());
      floats->Add(fbs_floats->cbegin(), fbs_floats->cend());
    } break;
    case AttributeProto_AttributeType_INTS: {
      auto fbs_ints = fbs_attr.ints();
      ORT_RETURN_IF(nullptr == fbs_ints, "Null ints attribute. Invalid ORT format model.");
      auto* ints = attr_proto.mutable_ints();
      ints->Reserve(fbs_ints->size());
      ints->Add(fbs_ints->cbegin(), fbs_ints->cend());
    } break;
    case AttributeProto_AttributeType_STRINGS: {
      auto fbs_strings = fbs_attr.strings();
      ORT_RETURN_IF(nullptr == fbs_strings, "Null strings attribute. Invalid ORT format model.");
      auto* strings = attr_proto.mutable_strings();
      strings->Reserve(fbs_strings->size());
      for (const auto* fbs_str : *fbs_strings) {
        ORT_RETURN_IF(nullptr == fbs_str, "Null string in strings attribute. Invalid ORT format model.");
        strings->Add(fbs_str->str());
      }
    } break;
    case AttributeProto_AttributeType_TENSORS: {
      auto fbs_tensors = fbs_attr.tensors();
      ORT_RETURN_IF(nullptr == fbs_tensors, "Null tensors attribute. Invalid ORT format model.");
      auto* tensors = attr_proto.mutable_tensors();
      tensors->Reserve(fbs_tensors->size());
      for (const auto* fbs_tensor : *fbs_tensors) {
        ORT_RETURN_IF(nullptr == fbs_tensor, "Null tensor in tensors attribute. Invalid ORT format model.");
        ORT_RETURN_IF_ERROR(LoadInitializerOrtFormat(*fbs_tensor, *tensors->Add(),
                                                     load_options));
      }
    } break;

    default:
      break;
  }

  return Status::OK();
}

Status SaveOrtValueOrtFormat(
    const std::string& tensor_name, const OrtValue& ort_value,
    const DataTransferManager& data_transfer_manager, flatbuffers::FlatBufferBuilder& builder,
    flatbuffers::Offset<fbs::Tensor>& fbs_tensor) {
  // Check if the OrtValue is a tensor.
  ORT_RETURN_IF_NOT(ort_value.IsTensor(), "Only tensor OrtValues can be saved to a checkpoint.");

  const onnxruntime::Tensor& src_tensor = ort_value.Get<onnxruntime::Tensor>();
  // Check if the tensor is on CPU. If not, we need to copy the tensor to CPU before saving it.
  if (const auto& tensor_location = src_tensor.Location();
      tensor_location.device.Type() != OrtDevice::CPU &&
      tensor_location.mem_type != OrtMemTypeCPUInput &&
      tensor_location.mem_type != OrtMemTypeCPUOutput &&
      tensor_location.device.Type() != OrtDevice::GPU) {
    ORT_MAKE_STATUS(ONNXRUNTIME, EP_FAIL, "Device type ", tensor_location.device.Type(),
                    " is not supported while saving a tensor to a checkpoint.");
  }

  InlinedVector<uint8_t> tensor_data_buffer{};
  tensor_data_buffer.resize(src_tensor.SizeInBytes());
  const OrtMemoryInfo cpu_alloc_info{onnxruntime::CPU, OrtDeviceAllocator};

  gsl::span<uint8_t> dst_span = gsl::make_span(tensor_data_buffer);
  ORT_RETURN_IF_NOT(src_tensor.SizeInBytes() == static_cast<size_t>(dst_span.size_bytes()),
                    "Size of src and dst buffer mismatch. Src size: ", src_tensor.SizeInBytes(),
                    " Dst size: ", dst_span.size_bytes());

  onnxruntime::Tensor dst_tensor{src_tensor.DataType(), src_tensor.Shape(), dst_span.data(), cpu_alloc_info};
  ORT_RETURN_IF_ERROR(data_transfer_manager.CopyTensor(src_tensor, dst_tensor));

  ORT_RETURN_IF(dst_tensor.IsDataTypeString(),
                "TensorProto_DataType_STRING is not supported while saving a tensor to ORT format.");

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> raw_data = builder.CreateVector(dst_span.data(), dst_span.size());

  fbs::TensorBuilder tb(builder);
  tb.add_name(builder.CreateString(tensor_name));
  tb.add_doc_string(builder.CreateString(std::string()));
  tb.add_dims(SaveDims(builder, dst_tensor.Shape().GetDims()));
  tb.add_data_type(static_cast<fbs::TensorDataType>(dst_tensor.GetElementType()));
  tb.add_raw_data(raw_data);
  fbs_tensor = tb.Finish();
  return Status::OK();
}

Status LoadOrtValueOrtFormat(const fbs::Tensor& fbs_tensor,
                             std::string& tensor_name, OrtValue& ort_value) {
  // The assumption is that the flatbuffer buffer will be destructed once the checkpoint has been loaded.
  // And so, we must allocate a buffer where the tensor data can be copied using the cpu allocator.
  // This buffer is owned by the OrtValue.
  static const CPUExecutionProviderInfo info;
  static const CPUExecutionProvider cpu_provider(info);
  static const AllocatorPtr cpu_allocator = cpu_provider.GetAllocator(OrtMemTypeDefault);

  auto* fbs_tensor_name = fbs_tensor.name();
  ORT_RETURN_IF_NOT(fbs_tensor_name, "Checkpoint is invalid. Expected: A valid tensor name. Acutal: nullptr.");
  tensor_name = fbs_tensor_name->str();

  auto* tensor_dims = fbs_tensor.dims();
  ORT_RETURN_IF_NOT(tensor_dims, "Checkpoint is invalid. Expected: Valid tensor dims. Acutal: nullptr.");

  auto tensor_data_type = fbs_tensor.data_type();
  const DataTypeImpl* tensor_dtype = DataTypeImpl::TensorTypeFromONNXEnum(
                                         static_cast<int>(tensor_data_type))
                                         ->GetElementType();
  auto dst_tensor = std::make_unique<onnxruntime::Tensor>(
      tensor_dtype, TensorShape(InlinedVector<int64_t>{tensor_dims->begin(), tensor_dims->end()}), cpu_allocator);

  // The tensor proto is used a dummy here. The actual data is stored in the raw_data field of the flatbuffer.
  // The data is copied from the raw_data field to the dst_tensor.
  ONNX_NAMESPACE::TensorProto unused_tensor_proto;
  unused_tensor_proto.set_data_type(static_cast<int>(tensor_data_type));

  auto unpack_tensor_with_type = [&unused_tensor_proto, &fbs_tensor, &dst_tensor]<typename T>() {
    return onnxruntime::utils::UnpackTensor(unused_tensor_proto, fbs_tensor.raw_data()->Data(),
                                            fbs_tensor.raw_data()->size(),
                                            dst_tensor->MutableData<T>(),
                                            dst_tensor->Shape().Size());
  };

  switch (static_cast<int>(tensor_data_type)) {
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<float>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_BOOL:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<bool>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_DOUBLE:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<double>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_STRING:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<std::string>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_INT8:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<int8_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT8:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<uint8_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_INT16:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<int16_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT16:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<uint16_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_INT32:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<int32_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT32:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<uint32_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_INT64:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<int64_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_UINT64:
      ORT_RETURN_IF_ERROR(unpack_tensor_with_type.template operator()<uint64_t>());
      break;
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT16:
    case ONNX_NAMESPACE::TensorProto_DataType_BFLOAT16:
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E4M3FN:
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E4M3FNUZ:
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E5M2:
    case ONNX_NAMESPACE::TensorProto_DataType_FLOAT8E5M2FNUZ:
    default:
      return ORT_MAKE_STATUS(ONNXRUNTIME, NOT_IMPLEMENTED, "Cannot unpack tensor with type ", static_cast<int>(tensor_data_type));
  }

  ort_value.Init(dst_tensor.release(), DataTypeImpl::GetType<onnxruntime::Tensor>(),
                 DataTypeImpl::GetType<onnxruntime::Tensor>()->GetDeleteFunc());

  return Status::OK();
}

}  // namespace onnxruntime::fbs::utils
