/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef GLOW_BACKENDS_OPENCL_OPENCL_H
#define GLOW_BACKENDS_OPENCL_OPENCL_H

#include "glow/Backends/Backend.h"
#include "glow/Backends/BackendUtils.h"
#include "glow/Backends/CompiledFunction.h"
#include "glow/Base/Tensor.h"
#include "glow/Base/Traits.h"
#include "glow/Graph/Node.h"
#include "glow/Graph/PlaceholderBindings.h"
#include "glow/IR/IR.h"
#include "llvm/ADT/ArrayRef.h"

#include <unordered_map>

#if defined(__APPLE__) || defined(__MACOSX)
#include "OpenCL/opencl.h"
#else
#include <CL/cl.h>
#endif

namespace glow {

class OCLConvolutionInst;
class Value;

/// A helper struct with information about kernels launches.
struct KernelLaunch {
  /// Kernel that was launched.
  cl_kernel kernel_;
  /// The name of the kernel that was launched.
  std::string name_;
  /// Event associated with the start of the kernel.
  /// Used only when profiling is enabled.
  cl_event event_;
  /// Constructor to be used by launching Glow's CL kernels.
  KernelLaunch(cl_kernel kernel, std::string name, cl_event event)
      : kernel_(kernel), name_(name), event_(event) {}
  /// Constructor to be used when launching an "external" CL kernel, e.g.
  /// provided by such libraries like CLBlast, etc.
  KernelLaunch(const std::string &name, cl_event event)
      : kernel_(nullptr), name_(name), event_(event) {}
};

/// A Glow IR function compiled for OpenCL.
class OpenCLFunction final : public CompiledFunction {
  /// A helper type representing a key for the program's cache.
  /// Each compiled program is uniquely identified by its source code, set of
  /// compiler options that were used and the device it was compiled for.
  using ProgramKey =
      std::tuple<const std::string, const std::string, const cl_device_id>;
  struct ProgramKeyHash {
    std::size_t operator()(const ProgramKey &K) const noexcept {
      return llvm::hash_combine(std::get<0>(K), std::get<1>(K), std::get<2>(K));
    }
  };
  /// The IR to be executed.
  std::unique_ptr<IRFunction> F_;
  /// CL compute device id.
  cl_device_id deviceId_;
  /// CL compute context.
  cl_context context_;
  /// CL compute command queue.
  cl_command_queue commands_;
  /// Cache of compiled programs.
  /// The same source code can be compile with different options (e.g. with
  /// different set of macro definitions) and/or for a different device and
  /// would result in different programs.
  std::unordered_map<ProgramKey, cl_program, ProgramKeyHash> programsCache_;
  /// A pointer to the on-device memory buffer.
  cl_mem deviceBuffer_{0};
  /// Information about kernel launches.
  std::vector<KernelLaunch> kernelLaunches_;

public:
  /// Ctor.
  explicit OpenCLFunction(std::unique_ptr<IRFunction> F,
                          const runtime::RuntimeBundle &bundle);

  /// @name CompiledFunction interface
  ///@{
  ~OpenCLFunction() override;

  void execute(PlaceholderBindings *bindings) override;
  /// Allocates on device buffer and copies Constant weights to device.
  void setupRuns() override;
  /// Per run setup, copies Inputs from \p bindings to on device memory.
  void beforeRun(const PlaceholderBindings &bindings) override;
  /// Copies outputs from device to tensors in \p bindings.
  void afterRun(const PlaceholderBindings &bindings) override;
  /// Final cleanup, currently an empty function in OpenCL.
  void tearDownRuns() override;

  /// Collects constants for runtime.
  void collectConstants(Module *module) override;

  /// \returns the Kind of Backend used to compile this function.
  virtual BackendKind getCompileBackendKind() const override {
    return BackendKind::OpenCL;
  }
  ///@}

  /// Returns IR function pointer.
  IRFunction *getIR() { return F_.get(); }

private:
  /// Copy the value from a device to a provided buffer.
  /// \returns number of copied bytes.
  uint64_t copyValueFromDevice(const Value *v, void *buf = nullptr);
  /// Copy value from the provided buffer to the device.
  /// \returns number of copied bytes.
  uint64_t copyValueToDevice(const Value *v, void *buf = nullptr);
  /// Fill the device \p buffer with a given \p value.
  /// \param len number of buffer elements to be filled by the \p value.
  /// Elements are considered to be of the type described by \p elemKind.
  void fillBuffer(cl_mem buffer, uint64_t start, uint64_t len, float value,
                  ElemKind elemKind);

  /// Execution a convolution instruction which uses NCHW format.
  void executeConvolution(const OCLConvolutionInst *CC);
  /// Allocate a device buffer of required \p size.
  cl_mem allocDeviceBuffer(uint64_t size);
  /// Frees a device buffer.
  void freeDeviceBuffer(cl_mem buf);

  /// Create kernel with a given \p name from a \p program.
  /// If \p program is nullptr, try to find the kernel with a given \p name
  /// in any of compiled programs.
  cl_kernel createKernel(const std::string &name, cl_program program = nullptr);

  /// Create a program from the \p source using provided \p options.
  cl_program createProgram(const std::string &source,
                           const std::vector<std::string> &options,
                           cl_command_queue queue);
  /// Enqueue a \p kernel on a provided \p commands queue.
  void enqueueKernel(cl_command_queue commands, cl_kernel kernel,
                     cl_device_id device, llvm::ArrayRef<size_t> global,
                     std::vector<KernelLaunch> &kernelLaunches);
  /// Enqueue a \p kernel on a provided \p commands queue using specified \p
  /// global and \p local work sizes.
  void enqueueKernel(cl_command_queue commands, cl_kernel kernel,
                     cl_device_id device, llvm::ArrayRef<size_t> global,
                     llvm::ArrayRef<size_t> local,
                     std::vector<KernelLaunch> &kernelLaunches);
};

/// This is the OpenCL backend.
class OCLBackend final : public BackendUsingGlowIR {
public:
  /// Ctor.
  OCLBackend() = default;

  /// @name Backend methods.
  /// This is the implementation of the Backend interface.
  ///@{
  ~OCLBackend() override = default;

  BackendKind getBackendKind() const override { return BackendKind::OpenCL; }

  std::unique_ptr<CompiledFunction>
  compileIR(std::unique_ptr<IRFunction> IR) const override;
  std::unique_ptr<CompiledFunction>
  compileIRWithoutConstants(std::unique_ptr<IRFunction> IR) const;

  std::unique_ptr<CompiledFunction>
  compile(Function *F, const CompilationOptions &opts) const override;

  bool transformPostLowering(Function *F,
                             const CompilationOptions &opts) const override;

  bool isOpSupported(const NodeInfo &NI) const override;

  bool shouldLower(const Node *N) const override {
    // The group convolution is supported in OpenCL slow convolution kernel.
    if (N->getKind() == Kinded::Kind::ConvolutionNodeKind)
      return false;
    return true;
  }

  /// @}
};

} // namespace glow

#endif // GLOW_BACKENDS_OPENCL_OPENCL_H
