// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/training_api/checkpoint.h"

#include <filesystem>

#include "core/flatbuffers/schema/ort_training.fbs.h"
#include "core/framework/framework_common.h"
#include "core/framework/tensorprotoutils.h"
#include "core/graph/graph_flatbuffers_utils.h"

namespace onnxruntime::training::api {

namespace {

/**
 * @brief Create flatbuffer tensors from OrtValue objects
 *
 * @param name_to_ort_value Name to OrtValue map.
 * @param data_transfer_manager Data transfer manager to copy the OrtValue tensor to a cpu buffer.
 * @param builder Builder to create flatbuffer tensors.
 * @param flatbuffer_tensors Flatbuffer tensors to be populated.
 * @return Status of the operation.
 */
Status FlatbufferTensorsFromOrtValues(
    const InlinedHashMap<std::string, OrtValue>& name_to_ort_value,
    const DataTransferManager& data_transfer_manager,
    flatbuffers::FlatBufferBuilder& builder,
    std::vector<flatbuffers::Offset<fbs::Tensor>>& flatbuffer_tensors) {
  for (const auto& [name, ort_value] : name_to_ort_value) {
    flatbuffers::Offset<fbs::Tensor> fbs_tensor;
    ORT_RETURN_IF_ERROR(fbs::utils::SaveOrtValueOrtFormat(name, ort_value, data_transfer_manager, builder, fbs_tensor));
    flatbuffer_tensors.emplace_back(fbs_tensor);
  }

  return Status::OK();
}

/**
 * @brief Create OrtValue objects from flatbuffer tensors.
 *
 * @param flatbuffer_tensors Flatbuffer tensors.
 * @param name_to_ort_value Name to OrtValue map to be populated.
 * @return Status of the operation.
 */
Status OrtValuesFromFlatbufferTensors(
    const flatbuffers::Vector<flatbuffers::Offset<onnxruntime::fbs::Tensor>>* flatbuffer_tensors,
    InlinedHashMap<std::string, OrtValue>& name_to_ort_value) {
  ORT_RETURN_IF_NOT(flatbuffer_tensors, "Expected: Both trainable and non trainable tensors must exist.",
                    " Actual: Encountered a nullptr. Checkpoint file is invalid");

  for (auto* fbs_tensor : *flatbuffer_tensors) {
    ORT_RETURN_IF_NOT(fbs_tensor, "Encountered a nullptr flatbuffer tensor. Checkpoint file is invalid.");

    std::string tensor_name;
    OrtValue ort_value;
    ORT_RETURN_IF_ERROR(fbs::utils::LoadOrtValueOrtFormat(*fbs_tensor, tensor_name, ort_value));
    name_to_ort_value.emplace(tensor_name, ort_value);
  }

  return Status::OK();
}

namespace Save {

/**
 * @brief Save from a checkpoint flatbuffer to file.
 * @param checkpoint_path Path to save the checkpoint file.
 * @param builder Flatbuffer builder containing the checkpoint buffer.
 * @return Status of the operation.
 *
 */
Status ToFile(const PathString& checkpoint_path, flatbuffers::FlatBufferBuilder& builder) {
  std::ofstream file(checkpoint_path, std::ios::binary);
  uint8_t* buf = builder.GetBufferPointer();
  int size = builder.GetSize();
  file.write(reinterpret_cast<const char*>(buf), size);
  ORT_RETURN_IF_NOT(file, "Failed to save ORT format model to file: ", ToUTF8String(checkpoint_path));

  return Status::OK();
}

#if !defined(ORT_MINIMAL_BUILD)
/**
 * @brief Save from ONNX initializers to a checkpoint file.
 *
 * @param trainable_tensor_protos trainable parameters in TensorProto format.
 * @param non_trainable_tensor_protos non-trainable parameters in TensorProto format.
 * @param checkpoint_path file where checkpoint is saved.
 * @return Status
 */
Status FromTensorProtos(
    const std::vector<ONNX_NAMESPACE::TensorProto>& trainable_tensor_protos,
    const std::vector<ONNX_NAMESPACE::TensorProto>& non_trainable_tensor_protos,
    const PathString& checkpoint_path) {
  // Make sure name unique across trainable and non-trainable lists.
  std::set<std::string> trainable_unique_names;
  std::set<std::string> non_trainable_unique_names;
  InlinedVector<std::string> inter_sec;
  auto check_unique = [](const std::vector<ONNX_NAMESPACE::TensorProto>& tensor_protos,
                         std::set<std::string>& unique_names) {
    for (auto& tensor_proto : tensor_protos) {
      ORT_RETURN_IF_NOT(unique_names.find(tensor_proto.name()) == unique_names.end(),
                        "Duplicated tensor proto named ", tensor_proto.name());
      unique_names.emplace(tensor_proto.name());
    }

    return Status::OK();
  };

  ORT_RETURN_IF_ERROR(check_unique(trainable_tensor_protos, trainable_unique_names));
  ORT_RETURN_IF_ERROR(check_unique(non_trainable_tensor_protos, non_trainable_unique_names));
  std::set_intersection(trainable_unique_names.begin(), trainable_unique_names.end(),
                        non_trainable_unique_names.begin(), non_trainable_unique_names.end(),
                        std::back_inserter(inter_sec));
  ORT_RETURN_IF_NOT(inter_sec.empty(), "Tensor name exists in both trainable param list and non-trainable param list.");

  constexpr size_t m_bytes = 1024 * 1024;
  size_t fbs_buffer_size = 0U;
  for (const auto& tensor_proto : trainable_tensor_protos) {
    fbs_buffer_size += tensor_proto.ByteSizeLong();
  }
  for (const auto& tensor_proto : non_trainable_tensor_protos) {
    fbs_buffer_size += tensor_proto.ByteSizeLong();
  }

  // Align buffer size to 1MB.
  fbs_buffer_size = std::max(fbs_buffer_size, m_bytes);
  fbs_buffer_size = ((fbs_buffer_size + m_bytes - 1) / m_bytes) * m_bytes;
  flatbuffers::FlatBufferBuilder builder(fbs_buffer_size);

  const auto tensor_protos_to_fbs_tensors = [&builder](const auto& tensor_protos, auto& fbs_tensors) {
    fbs_tensors.reserve(tensor_protos.size());
    for (const auto& tensor_proto : tensor_protos) {
      flatbuffers::Offset<fbs::Tensor> fbs_tensor;
      ORT_RETURN_IF_ERROR(
          fbs::utils::SaveInitializerOrtFormat(builder, tensor_proto, Path(), fbs_tensor));
      fbs_tensors.push_back(fbs_tensor);
    }

    return Status::OK();
  };

  std::vector<flatbuffers::Offset<fbs::Tensor>> trainable_tensors;
  ORT_RETURN_IF_ERROR(tensor_protos_to_fbs_tensors(trainable_tensor_protos, trainable_tensors));

  std::vector<flatbuffers::Offset<fbs::Tensor>> non_trainable_tensors;
  ORT_RETURN_IF_ERROR(tensor_protos_to_fbs_tensors(non_trainable_tensor_protos, non_trainable_tensors));

  fbs::ModuleStateBuilder module_state_builder(builder);
  module_state_builder.add_requires_grad(builder.CreateVector(trainable_tensors));
  module_state_builder.add_frozen_params(builder.CreateVector(non_trainable_tensors));
  flatbuffers::Offset<fbs::ModuleState> fbs_module_state = module_state_builder.Finish();

  // This function only stores the module state since the optimizer state and
  // user defined properties are not available.
  std::vector<flatbuffers::Offset<fbs::OptimizerGroup>> fbs_optimizer_groups;
  flatbuffers::Offset<fbs::PropertyBag> fbs_property_bag;

  fbs::CheckpointBuilder checkpoint_builder(builder);
  checkpoint_builder.add_module_state(fbs_module_state);
  checkpoint_builder.add_optimizer_groups(builder.CreateVector(fbs_optimizer_groups));
  checkpoint_builder.add_property_bag(fbs_property_bag);
  auto checkpoint = checkpoint_builder.Finish();
  builder.Finish(checkpoint, fbs::CheckpointIdentifier());

  ORT_RETURN_IF_ERROR(Save::ToFile(checkpoint_path, builder));

  return Status::OK();
}
#endif

/**
 * @brief Save from the module state to a flatbuffer checkpoint module state.
 *
 * @param module_state module state containing the model's trainable and non-trainable parameters.
 * @param builder Flatbuffer builder.
 * @param fbs_module_state Flatbuffer module state to be populated.
 * @return Status of the operation.
 */
Status FromModuleState(const ModuleCheckpointState& module_state,
                       flatbuffers::FlatBufferBuilder& builder,
                       flatbuffers::Offset<fbs::ModuleState>& fbs_module_state) {
  if (module_state.named_parameters.empty()) {
    std::vector<flatbuffers::Offset<fbs::Tensor>> trainable_tensors;
    std::vector<flatbuffers::Offset<fbs::Tensor>> non_trainable_tensors;
    fbs::ModuleStateBuilder module_state_builder(builder);
    module_state_builder.add_requires_grad(builder.CreateVector(trainable_tensors));
    module_state_builder.add_frozen_params(builder.CreateVector(non_trainable_tensors));
    fbs_module_state = module_state_builder.Finish();
    return Status::OK();
  }

  ORT_RETURN_IF_NOT(module_state.train_session_data_transfer_mgr,
                    "Cannot save module state to a checkpoint. Expected: A valid data transfer manager. ",
                    "Actual: nullptr.");

  InlinedHashMap<std::string, OrtValue> requires_grad;
  InlinedHashMap<std::string, OrtValue> frozen_params;
  for (auto& [name, value] : module_state.named_parameters) {
    if (value->RequiresGrad()) {
      requires_grad.emplace(name, value->Data());
    } else {
      frozen_params.emplace(name, value->Data());
    }
  }

  std::vector<flatbuffers::Offset<fbs::Tensor>> trainable_tensors;
  trainable_tensors.reserve(requires_grad.size());
  ORT_RETURN_IF_ERROR(FlatbufferTensorsFromOrtValues(
      requires_grad,
      *module_state.train_session_data_transfer_mgr,
      builder, trainable_tensors));

  std::vector<flatbuffers::Offset<fbs::Tensor>> non_trainable_tensors;
  non_trainable_tensors.reserve(frozen_params.size());
  ORT_RETURN_IF_ERROR(FlatbufferTensorsFromOrtValues(
      frozen_params,
      *module_state.train_session_data_transfer_mgr,
      builder, non_trainable_tensors));

  fbs::ModuleStateBuilder module_state_builder(builder);
  module_state_builder.add_requires_grad(builder.CreateVector(trainable_tensors));
  module_state_builder.add_frozen_params(builder.CreateVector(non_trainable_tensors));
  fbs_module_state = module_state_builder.Finish();

  return Status::OK();
}

/**
 * @brief Save from the optimizer state to a flatbuffer checkpoint optimizer state.
 *
 * @param optimizer_state optimizer state containing the optimizer's state (for example learning rate, step, first
 *                        and second order momentums ...).
 * @param builder Flatbuffer builder.
 * @param fbs_optimizer_groups Flatbuffer optimizer groups to be populated.
 * @return Status of the operation.
 */
Status FromOptimizerState(const OptimizerCheckpointState& optimizer_state,
                          flatbuffers::FlatBufferBuilder& builder,
                          std::vector<flatbuffers::Offset<fbs::OptimizerGroup>>& fbs_optimizer_groups) {
  if (optimizer_state.group_named_optimizer_states.empty()) {
    return Status::OK();
  }

  ORT_RETURN_IF_NOT(optimizer_state.optimizer_session_data_transfer_mgr,
                    "Cannot save optimizer state to a checkpoint. Expected: A valid data transfer manager. ",
                    "Actual: nullptr.");

  fbs_optimizer_groups.reserve(optimizer_state.group_named_optimizer_states.size());
  for (auto& group_named_optimizer_state : optimizer_state.group_named_optimizer_states) {
    const std::shared_ptr<GroupOptimizerState>& group_optimizer_state_ptr = group_named_optimizer_state.second;

    std::vector<flatbuffers::Offset<fbs::ParameterOptimizerState>> optimizer_states;
    optimizer_states.reserve(group_optimizer_state_ptr->param_named_optimizer_states.size());
    for (const auto& [param_name, param_optimizer_state] : group_optimizer_state_ptr->param_named_optimizer_states) {
      std::vector<flatbuffers::Offset<fbs::Tensor>> momentums;
      momentums.reserve(param_optimizer_state.size());
      ORT_RETURN_IF_ERROR(FlatbufferTensorsFromOrtValues(
          param_optimizer_state,
          *optimizer_state.optimizer_session_data_transfer_mgr,
          builder, momentums));

      fbs::ParameterOptimizerStateBuilder optimizer_state_builder(builder);
      optimizer_state_builder.add_param_name(builder.CreateString(param_name));
      optimizer_state_builder.add_momentums(builder.CreateVector(momentums));

      flatbuffers::Offset<fbs::ParameterOptimizerState> fbs_optimizer_state = optimizer_state_builder.Finish();
      optimizer_states.emplace_back(fbs_optimizer_state);
    }

    fbs::OptimizerGroupBuilder optimizer_state_builder(builder);
    optimizer_state_builder.add_group_name(builder.CreateString(group_named_optimizer_state.first));
    optimizer_state_builder.add_initial_learning_rate(group_optimizer_state_ptr->initial_lr);
    optimizer_state_builder.add_step(group_optimizer_state_ptr->step);
    optimizer_state_builder.add_optimizer_states(builder.CreateVector(optimizer_states));

    auto fbs_optimizer_group = optimizer_state_builder.Finish();
    fbs_optimizer_groups.emplace_back(fbs_optimizer_group);
  }

  return Status::OK();
}

/**
 * @brief Save from user defined properties to a flatbuffer checkpoint property bag.
 *
 * @param property_bag user defined properties.
 * @param builder Flatbuffer builder.
 * @param fbs_property_bag Flatbuffer property bag to be populated.
 * @return Status of the operation.
 */
Status FromPropertyBag(const PropertyBag& property_bag, flatbuffers::FlatBufferBuilder& builder,
                       flatbuffers::Offset<fbs::PropertyBag>& fbs_property_bag) {
  std::vector<flatbuffers::Offset<fbs::IntProperty>> ints;
  std::vector<flatbuffers::Offset<fbs::FloatProperty>> floats;
  std::vector<flatbuffers::Offset<fbs::StringProperty>> strings;
  for (const auto& [name, value] : property_bag) {
    if (std::holds_alternative<int64_t>(value)) {
      fbs::IntPropertyBuilder int_property_builder(builder);
      int_property_builder.add_name(builder.CreateString(name));
      int_property_builder.add_value(std::get<int64_t>(value));
      flatbuffers::Offset<fbs::IntProperty> property = int_property_builder.Finish();
      ints.emplace_back(property);
    } else if (std::holds_alternative<float>(value)) {
      fbs::FloatPropertyBuilder float_property_builder(builder);
      float_property_builder.add_name(builder.CreateString(name));
      float_property_builder.add_value(std::get<float>(value));
      flatbuffers::Offset<fbs::FloatProperty> property = float_property_builder.Finish();
      floats.emplace_back(property);
    } else if (std::holds_alternative<std::string>(value)) {
      fbs::StringPropertyBuilder string_property_builder(builder);
      string_property_builder.add_name(builder.CreateString(name));
      string_property_builder.add_value(builder.CreateString(std::get<std::string>(value)));
      flatbuffers::Offset<fbs::StringProperty> property = string_property_builder.Finish();
      strings.emplace_back(property);
    } else {
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT, "Unknown property type encountered in the property bag.");
    }
  }

  fbs::PropertyBagBuilder property_bag_builder(builder);
  property_bag_builder.add_ints(builder.CreateVector(ints));
  property_bag_builder.add_floats(builder.CreateVector(floats));
  property_bag_builder.add_strings(builder.CreateVector(strings));

  fbs_property_bag = property_bag_builder.Finish();

  return Status::OK();
}

/**
 * @brief Save from a checkpoint state to a checkpoint file.
 *
 * @param state parameter/optimizer and other user defined training states.
 * @param checkpoint_path file where checkpoint is saved.
 * @param include_optimizer_state Whether to include optimizer state in the checkpoint.
 * @return Status of the operation.
 */
Status FromCheckpointState(
    const CheckpointState& state, const PathString& checkpoint_path, const bool include_optimizer_state) {
  flatbuffers::FlatBufferBuilder builder(1024);

  // Write weight tensors files.
  flatbuffers::Offset<fbs::ModuleState> module_state;
  ORT_RETURN_IF_ERROR(FromModuleState(state.module_checkpoint_state, builder, module_state));

  // Write optimizer state tensors files.
  std::vector<flatbuffers::Offset<fbs::OptimizerGroup>> optimizer_groups;
  if (include_optimizer_state) {
    ORT_RETURN_IF_ERROR(FromOptimizerState(state.optimizer_checkpoint_state, builder, optimizer_groups));
  }

  flatbuffers::Offset<fbs::PropertyBag> property_bag;
  ORT_RETURN_IF_ERROR(FromPropertyBag(state.property_bag, builder, property_bag));

  fbs::CheckpointBuilder checkpoint_builder(builder);
  checkpoint_builder.add_module_state(module_state);
  checkpoint_builder.add_optimizer_groups(builder.CreateVector(optimizer_groups));
  checkpoint_builder.add_property_bag(property_bag);
  auto checkpoint = checkpoint_builder.Finish();
  builder.Finish(checkpoint, fbs::CheckpointIdentifier());

  ORT_RETURN_IF_ERROR(Save::ToFile(checkpoint_path, builder));

  return Status::OK();
}

}  // namespace Save

namespace Load {

/**
 * @brief Load checkpoint flatbuffer from file.
 * @param checkpoint_path Path to the checkpoint file.
 * @param checkpoint_bytes Contents of the checkpoint file in bytes.
 * @param checkpoint_span Checkpoint bytes represented as a span.
 * @return Status of the operation.
 *
 */
Status FromFile(const PathString& checkpoint_path, std::vector<uint8_t>& checkpoint_bytes,
                gsl::span<const uint8_t>& checkpoint_span) {
  ORT_RETURN_IF_NOT(std::filesystem::exists(checkpoint_path), "Checkpoint does not exist at provided path: ",
                    checkpoint_path);

  size_t num_bytes = 0;
  ORT_RETURN_IF_ERROR(Env::Default().GetFileLength(checkpoint_path.c_str(), num_bytes));
  checkpoint_bytes.resize(num_bytes);

  std::ifstream bytes_stream(checkpoint_path, std::ifstream::in | std::ifstream::binary);
  bytes_stream.read(reinterpret_cast<char*>(checkpoint_bytes.data()), num_bytes);

  ORT_RETURN_IF_NOT(bytes_stream, "Loading checkpoint from ", ToUTF8String(checkpoint_path), " failed. Only ",
                    bytes_stream.gcount(), "/", num_bytes, " bytes could be read.");

  checkpoint_span = gsl::span<const uint8_t>(checkpoint_bytes.data(), num_bytes);

  flatbuffers::Verifier verifier(checkpoint_span.data(), checkpoint_span.size());
  ORT_RETURN_IF_NOT(fbs::VerifyCheckpointBuffer(verifier), "Checkpoint verification failed.");

  return Status::OK();
}

/**
 * @brief Load from a flatbuffer checkpoint module state to a module state.
 *
 * @param fbs_module_state Flatbuffer module state.
 * @param module_state Module state to be populated.
 * @return Status of the operation.
 */
Status ToModuleState(
    const onnxruntime::fbs::ModuleState* fbs_module_state, ModuleCheckpointState& module_state) {
  ORT_RETURN_IF_NOT(fbs_module_state, "Checkpoint is invalid. Expected: Valid checkpoint module state flatbuffer. ",
                    "Acutal: nullptr.");

  const auto* requires_grad = fbs_module_state->requires_grad();
  flatbuffers::uoffset_t trainable_params_size = (requires_grad != nullptr ? requires_grad->size() : 0U);
  InlinedHashMap<std::string, OrtValue> trainable_params;
  trainable_params.reserve(trainable_params_size);
  ORT_RETURN_IF_ERROR(OrtValuesFromFlatbufferTensors(requires_grad, trainable_params));

  for (auto& [name, value] : trainable_params) {
    auto param = std::make_shared<Parameter>(name, value, true);
    module_state.named_parameters.emplace(name, param);
  }

  const auto* frozen_params = fbs_module_state->frozen_params();
  flatbuffers::uoffset_t non_trainable_params_size = (frozen_params != nullptr ? frozen_params->size() : 0U);
  InlinedHashMap<std::string, OrtValue> non_trainable_params;
  non_trainable_params.reserve(non_trainable_params_size);
  ORT_RETURN_IF_ERROR(OrtValuesFromFlatbufferTensors(frozen_params, non_trainable_params));

  for (auto& [name, value] : non_trainable_params) {
    auto param = std::make_shared<Parameter>(name, value, false);
    module_state.named_parameters.emplace(name, param);
  }

  return Status::OK();
}

/**
 * @brief Load from a flatbuffer checkpoint optimizer state to an optimizer state.
 *
 * @param optimizer_groups Flatbuffer optimizer groups.
 * @param optimizer_state Optimizer state to be populated.
 * @return Status of the operation.
 */
Status ToOptimizerState(
    const flatbuffers::Vector<flatbuffers::Offset<onnxruntime::fbs::OptimizerGroup>>* optimizer_groups,
    OptimizerCheckpointState& optimizer_state) {
  for (auto* optimizer_group : *optimizer_groups) {
    const std::string group_name = optimizer_group->group_name()->str();
    const int64_t step = optimizer_group->step();
    const float initial_learning_rate = optimizer_group->initial_learning_rate();

    auto* parameter_optimizer_states = optimizer_group->optimizer_states();

    [[maybe_unused]] auto [optimizer_state_it, inserted] =
        optimizer_state.group_named_optimizer_states.emplace(group_name, std::make_shared<GroupOptimizerState>());

    optimizer_state_it->second->step = step;
    optimizer_state_it->second->initial_lr = initial_learning_rate;
    for (auto* parameter_optimizer_state : *parameter_optimizer_states) {
      std::string param_name = parameter_optimizer_state->param_name()->str();
      auto* momentums = parameter_optimizer_state->momentums();
      ORT_RETURN_IF_ERROR(OrtValuesFromFlatbufferTensors(momentums, optimizer_state_it->second->param_named_optimizer_states[param_name]));
    }
  }

  return Status::OK();
}

/**
 * @brief Load from a flatbuffer checkpoint property bag to a property bag.
 *
 * @param fbs_property_bag Flatbuffer property bag.
 * @param property_bag Property bag to be populated.
 * @return Status of the operation.
 */
Status ToPropertyBag(const onnxruntime::fbs::PropertyBag* fbs_property_bag,
                     PropertyBag& property_bag) {
  if (nullptr == fbs_property_bag) {
    return Status::OK();
  }

  auto* ints = fbs_property_bag->ints();
  if (nullptr != ints) {
    for (auto* int_property : *ints) {
      std::string name = int_property->name()->str();
      auto value = int_property->value();
      property_bag.AddProperty(name, value);
    }
  }

  auto* floats = fbs_property_bag->floats();
  if (nullptr != floats) {
    for (auto* float_property : *floats) {
      std::string name = float_property->name()->str();
      auto value = float_property->value();
      property_bag.AddProperty(name, value);
    }
  }

  auto* strings = fbs_property_bag->strings();
  if (nullptr != strings) {
    for (auto* string_property : *strings) {
      std::string name = string_property->name()->str();
      std::string value = string_property->value()->str();
      property_bag.AddProperty(name, value);
    }
  }

  return Status::OK();
}

#if !defined(ORT_MINIMAL_BUILD)
/**
 * @brief Load checkpoint from a checkpoint file to initializers in a model proto.
 *
 * @param checkpoint_path Path to the checkpoint file.
 * @param model_proto Model proto to be populated.
 * @return Status of the operation.
 */
Status ToModelProto(const PathString& checkpoint_path,
                    ONNX_NAMESPACE::ModelProto& model_proto) {
  std::vector<uint8_t> checkpoint_bytes;
  gsl::span<const uint8_t> checkpoint_span;
  ORT_RETURN_IF_ERROR(Load::FromFile(checkpoint_path, checkpoint_bytes, checkpoint_span));

  const auto* fbs_checkpoint = fbs::GetCheckpoint(checkpoint_span.data());
  ORT_RETURN_IF_NOT(fbs_checkpoint, "Checkpoint is invalid. Expected: Valid checkpoint flatbuffer. Acutal: nullptr.");

  auto* module_state = fbs_checkpoint->module_state();
  if (nullptr == module_state) {
    return Status::OK();
  }

  InlinedHashMap<std::string, ONNX_NAMESPACE::TensorProto> param_tensor_protos;

  const auto flatbuffer_tensors_to_tensor_protos = [&param_tensor_protos](const auto* flatbuffer_tensors) {
    if (nullptr == flatbuffer_tensors) {
      return Status::OK();
    }

    for (auto* fbs_tensor : *flatbuffer_tensors) {
      ONNX_NAMESPACE::TensorProto tensor_proto;
      OrtFormatLoadOptions load_options{false, false};
      ORT_RETURN_IF_ERROR(fbs::utils::LoadInitializerOrtFormat(*fbs_tensor, tensor_proto, load_options));
      param_tensor_protos.insert({fbs_tensor->name()->str(), tensor_proto});
    }

    return Status::OK();
  };

  ORT_RETURN_IF_ERROR(flatbuffer_tensors_to_tensor_protos(module_state->requires_grad()));
  ORT_RETURN_IF_ERROR(flatbuffer_tensors_to_tensor_protos(module_state->frozen_params()));

  // Copy loaded tensor protos to the initializers in the ModelProto
  for (auto& init : *(model_proto.mutable_graph()->mutable_initializer())) {
    ORT_RETURN_IF_NOT(init.has_name(), "ModelProto is invalid. Expected: All initializers must have names.");
    auto it = param_tensor_protos.find(init.name());
    if (it == param_tensor_protos.end()) {
      continue;
    }
    init.CopyFrom(it->second);
  }

  return Status::OK();
}
#endif

/**
 * @brief Load checkpoint from a checkpoint file to a checkpoint state.
 *
 * @param checkpoint_path Path to the checkpoint file.
 * @param state Checkpoint state to be populated.
 * @return Status of the operation.
 */
Status ToCheckpointState(const PathString& checkpoint_path, CheckpointState& state) {
  std::vector<uint8_t> checkpoint_bytes;
  gsl::span<const uint8_t> checkpoint_span;
  ORT_RETURN_IF_ERROR(Load::FromFile(checkpoint_path, checkpoint_bytes, checkpoint_span));

  const auto* fbs_checkpoint = fbs::GetCheckpoint(checkpoint_span.data());
  ORT_RETURN_IF_NOT(fbs_checkpoint, "Checkpoint is invalid. Expected: Valid checkpoint flatbuffer. Acutal: nullptr.");

  ORT_RETURN_IF_ERROR(ToModuleState(fbs_checkpoint->module_state(), state.module_checkpoint_state));
  ORT_RETURN_IF_ERROR(ToOptimizerState(fbs_checkpoint->optimizer_groups(), state.optimizer_checkpoint_state));
  ORT_RETURN_IF_ERROR(ToPropertyBag(fbs_checkpoint->property_bag(), state.property_bag));

  return Status::OK();
}

}  // namespace Load

}  // namespace

#if !defined(ORT_MINIMAL_BUILD)
Status SaveCheckpoint(const std::vector<ONNX_NAMESPACE::TensorProto>& trainable_tensor_protos,
                      const std::vector<ONNX_NAMESPACE::TensorProto>& non_trainable_tensor_protos,
                      const PathString& checkpoint_path) {
  return Save::FromTensorProtos(trainable_tensor_protos, non_trainable_tensor_protos, checkpoint_path);
}
#endif

Status SaveCheckpoint(const CheckpointState& states, const PathString& checkpoint_path,
                      const bool include_optimizer_state) {
  return Save::FromCheckpointState(states, checkpoint_path, include_optimizer_state);
}

Status LoadCheckpoint(const PathString& checkpoint_path, CheckpointState& checkpoint_states) {
  return Load::ToCheckpointState(checkpoint_path, checkpoint_states);
}

#if !defined(ORT_MINIMAL_BUILD)
Status LoadCheckpointToModel(const PathString& checkpoint_path,
                             ONNX_NAMESPACE::ModelProto& model_proto) {
  return Load::ToModelProto(checkpoint_path, model_proto);
}
#endif

}  // namespace onnxruntime::training::api
