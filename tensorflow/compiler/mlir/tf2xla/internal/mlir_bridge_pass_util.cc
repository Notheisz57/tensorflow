/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/mlir/tf2xla/internal/mlir_bridge_pass_util.h"

#include <functional>
#include <memory>
#include <string>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Visitors.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/tf2xla/tf2xla_defs.h"
#include "tensorflow/core/common_runtime/function_body.h"
#include "tensorflow/core/common_runtime/function_def_utils.h"
#include "tensorflow/core/graph/graph.h"
#include "tsl/platform/status.h"

namespace tensorflow {

using ::mlir::failure;
using ::mlir::LogicalResult;
using ::mlir::success;

namespace {
LogicalResult HasAttr(
    const Graph& graph, const FunctionLibraryDefinition* function_library,
    const std::function<bool(const Graph& graph)>& predicate) {
  if (predicate(graph)) {
    return success();
  }

  // Check if any reachable functions from the graph has the target attribute.
  GraphDef graph_def;
  graph.ToGraphDef(&graph_def);
  if (!function_library) return failure();
  for (const std::string& func_name :
       function_library->ReachableDefinitions(graph_def).ListFunctionNames()) {
    const FunctionDef* func_def = function_library->Find(func_name);
    std::unique_ptr<FunctionBody> func_body;
    absl::Status status = FunctionDefToBodyHelper(
        *func_def, AttrSlice(&func_def->attr()), function_library, &func_body);
    // This is not expected to happen in practice
    if (!status.ok()) {
      LOG(ERROR) << "Failed to parse " << func_name << ": "
                 << tsl::NullTerminatedMessage(status);
      return failure();
    }
    if (predicate(*func_body->graph)) {
      return success();
    }
  }
  return failure();
}

bool IsNonReplicatedGraph(const Graph& graph,
                          const FunctionLibraryDefinition* function_library) {
  auto predicate = [](const Graph& graph) {
    const std::string kStatefulPartitionedCallOp = "StatefulPartitionedCall";
    for (const Node* node : graph.nodes()) {
      auto node_op = node->type_string();
      if (node_op == kStatefulPartitionedCallOp) {
        // Functions called by StatefulfulPartitionedCall ops with
        // _XlaMustCompile=true are compiled by XLA.
        auto attr = node->attrs().FindByString(std::string(kMustCompileAttr));
        if (attr != nullptr && attr->b() == true) {
          return true;
        }
      }
    }
    return false;
  };
  return HasAttr(graph, function_library, predicate).succeeded();
}

bool IsReplicatedGraph(const Graph& graph,
                       const FunctionLibraryDefinition* function_library) {
  auto predicate = [](const Graph& graph) {
    for (const Node* node : graph.nodes()) {
      // _tpu_replicate is used in replicated TPU graphs. It will be converted
      // to_replication_info and _xla_compile_device_type in phase 1 pipelines.
      if (node->attrs().FindByString(std::string(kTpuReplicateAttr))) {
        return true;
      }
    }
    return false;
  };
  return HasAttr(graph, function_library, predicate).succeeded();
}

bool IsSingleCoreTpuGraph(const Graph& graph,
                          const FunctionLibraryDefinition* function_library) {
  auto predicate = [](const Graph& graph) {
    for (const Node* node : graph.nodes()) {
      // _xla_compile_device_type=TPU is found in single-core TPU graphs.
      auto attr =
          node->attrs().FindByString(std::string(kCompileDeviceTypeAttr));
      if (attr && attr->s() == kTpuDevice) {
        return true;
      }
    }
    return false;
  };
  return HasAttr(graph, function_library, predicate).succeeded();
}

bool IsReplicatedGraph(mlir::ModuleOp module) {
  auto walk_result = module.walk([&](mlir::Operation* op) {
    // TODO(b/223677572): Once the scope for new compilation and replication
    // markers is expanded beyond bridge we can remove this check for
    // `kTPUReplicateAttr`, we will then always have a `kCompileDeviceTypeAttr`
    // in such cases (see above).
    // TODO(b/229028654): Remove string conversion once we have C++17.
    const llvm::StringRef tpu_replicate_attr_name(kTpuReplicateAttr.data(),
                                                  kTpuReplicateAttr.size());
    auto replicate_attr =
        op->getAttrOfType<mlir::StringAttr>(tpu_replicate_attr_name);
    if (replicate_attr) return mlir::WalkResult::interrupt();
    return mlir::WalkResult::advance();
  });
  return walk_result.wasInterrupted();
}

bool IsSingleCoreTPUGraph(mlir::ModuleOp module) {
  auto walk_result = module.walk([&](mlir::Operation* op) {
    // Check for ops with compile device type "TPU". This allows us to support
    // TPU compilation without replication. Note that currently the compile
    // device type is not set by default before bridge, only if eager context
    // attribute `jit_compile_rewrite` is true.
    // TODO(b/229028654): Remove string conversion once we have C++17.
    const llvm::StringRef compile_device_type_attr_name(
        kCompileDeviceTypeAttr.data(), kCompileDeviceTypeAttr.size());
    auto compilation_attr =
        op->getAttrOfType<mlir::StringAttr>(compile_device_type_attr_name);
    if (compilation_attr && compilation_attr.getValue().str() == kTpuDevice) {
      return mlir::WalkResult::interrupt();
    }
    return mlir::WalkResult::advance();
  });
  return walk_result.wasInterrupted();
}

}  // namespace

bool IsSupportedByNonReplicatedBridge(
    const Graph& graph, const FunctionLibraryDefinition* function_library) {
  return IsNonReplicatedGraph(graph, function_library);
}

bool IsSupportedByReplicatedBridge(
    const Graph& graph, const FunctionLibraryDefinition* function_library) {
  return IsReplicatedGraph(graph, function_library) ||
         IsSingleCoreTpuGraph(graph, function_library);
}

bool IsSupportedByReplicatedBridge(mlir::ModuleOp module) {
  return IsReplicatedGraph(module) || IsSingleCoreTPUGraph(module);
}

bool HasTPUPartitionedCallOpInModule(mlir::ModuleOp module) {
  bool has_tpu_partitioned_call = false;
  for (auto func_op : module.getOps<mlir::func::FuncOp>()) {
    func_op->walk([&](mlir::TF::TPUPartitionedCallOp op) {
      has_tpu_partitioned_call = true;
    });
    if (has_tpu_partitioned_call) break;
  }
  return has_tpu_partitioned_call;
}

}  // namespace tensorflow
