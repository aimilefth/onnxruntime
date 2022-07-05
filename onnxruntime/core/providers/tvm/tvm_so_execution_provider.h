// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#ifndef ONNXRUNTIME_CORE_PROVIDERS_TVM_TVM_SO_EXECUTION_PROVIDER_H_
#define ONNXRUNTIME_CORE_PROVIDERS_TVM_TVM_SO_EXECUTION_PROVIDER_H_

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

#include "core/common/logging/logging.h"
#include "core/framework/execution_provider.h"
#include "core/platform/ort_mutex.h"

#include "tvm_compiler.h"  // NOLINT(build/include_subdir)
#include "tvm_runner.h"  // NOLINT(build/include_subdir)


namespace onnxruntime {
class Graph;
class NodeArg;
namespace tvm {

class TvmSoExecutionProvider : public IExecutionProvider {
  using Compiler = TVMCompilerBase;
  using Compilers = std::unordered_map<std::string, std::shared_ptr<Compiler>>;
  using Runner = TVMRunner;
  using Runners = std::unordered_map<std::string, std::shared_ptr<Runner>>;

 public:
  explicit TvmSoExecutionProvider(const TvmEPOptions& options);
  virtual ~TvmSoExecutionProvider();

  std::vector<std::unique_ptr<ComputeCapability>>
  GetCapability(const onnxruntime::GraphViewer& graph,
                const std::vector<const KernelRegistry*>& /*kernel_registries*/,
                const KernelTypeStrResolver& /*kernel_type_str_resolver*/) const override;

  common::Status Compile(const std::vector<FusedNodeAndGraph>& fused_nodes_and_graphs,
                         std::vector<NodeComputeInfo>& node_compute_funcs) override;
  std::unique_ptr<onnxruntime::IDataTransfer> GetDataTransfer() const override;
  AllocatorPtr GetAllocator(int id, OrtMemType mem_type) const override;

 private:
  void printOptions();
  bool checkHash(const std::string& onnx_path) const;
  std::shared_ptr<TvmModule> compileModel(const std::string& func_name,
                                          const GraphViewer& graph_viewer,
                                          InputsInfoMap& inputs_info);    // NOLINT
  void setInputShapesForFreezedNN(const GraphViewer& graph_viewer,
                                  TVMTensorShapes& input_shapes,          // NOLINT
                                  InputsInfoMap& all_input_shapes);       // NOLINT
  void setInputShapesForUnfreezedNN(const GraphViewer& graph_viewer,
                                    TVMTensorShapes& input_shapes,        // NOLINT
                                    InputsInfoMap& all_input_shapes);     // NOLINT
  TensorShapeVector getInputShape(const NodeArg* node);
  TensorShapeVector convertTensorShape(const ONNX_NAMESPACE::TensorShapeProto& shape_proto);
  void prepareOutputTensors(std::vector<DLTensor>& output_tensors);  // NOLINT
  NodeComputeInfo prepareComputeInfo(const std::string& func_name);
  int createStateFunc(ComputeContext*, FunctionState*);

 private:
  TvmEPOptions options_;
  Compilers compilers_;
  Runners runners_;
  AllocatorPtr allocator_;
};

}  // namespace tvm
}  // namespace onnxruntime

#endif  // ONNXRUNTIME_CORE_PROVIDERS_TVM_TVM_SO_EXECUTION_PROVIDER_H_
