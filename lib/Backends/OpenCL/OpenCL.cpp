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

// Silence Apple's warning about the deprecation of OpenCL.
#define CL_SILENCE_DEPRECATION

// Silence warnings about using deprecated OpenCL 1.2 functions.
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS

#include "OpenCL.h"

#include "glow/Backends/BackendUtils.h"
#include "glow/CodeGen/MemoryAllocator.h"
#include "glow/Graph/Graph.h"
#include "glow/Graph/Nodes.h"
#include "glow/IR/IRUtils.h"
#include "glow/IR/Instrs.h"
#include "glow/Quantization/Base/Base.h"
#include "glow/Support/Debug.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "opencl"

using namespace glow;
using llvm::format;

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

typedef uint32_t cl_size_t;

/// We convert the OpenCL source code of kernels into an include file using an
/// external utility (include-bin). The resulting file is included here to
/// compile these sources into our the OpenCL backend. During the execution
/// these sources are compiled by the OpenCL driver.

// This defines kernels for most operations.
static const unsigned char kernels_cl_src[] = {
#include "glow/kernels.inc"
};
static const size_t kernels_cl_src_size = sizeof(kernels_cl_src);

// This defines kernels for optimized convolutions.
static const unsigned char kernels_fwd_conv_cl_src[] = {
#include "glow/kernels_fwd_conv.inc"
};
static const size_t kernels_fwd_conv_cl_src_size =
    sizeof(kernels_fwd_conv_cl_src);

// This defines kernels for quantized optimized convolutions.
static const unsigned char kernels_fwd_quantized_conv_cl_src[] = {
#include "glow/kernels_fwd_quantized_conv.inc"
};
static const size_t kernels_fwd_quantized_conv_cl_src_size =
    sizeof(kernels_fwd_quantized_conv_cl_src);

llvm::cl::OptionCategory OpenCLBackendCat("Glow OpenCL Backend Options");

llvm::cl::opt<unsigned>
    clPlatformId("platform", llvm::cl::desc("OpenCL platform to be used"),
                 llvm::cl::init(0), llvm::cl::cat(OpenCLBackendCat));
llvm::cl::opt<unsigned> clDeviceId("device",
                                   llvm::cl::desc("OpenCL device to be used"),
                                   llvm::cl::init(0),
                                   llvm::cl::cat(OpenCLBackendCat));
llvm::cl::opt<bool> clDoProfile("opencl-profile",
                                llvm::cl::desc("Profile OpenCL kernels"),
                                llvm::cl::init(false),
                                llvm::cl::cat(OpenCLBackendCat));

namespace glow {
Backend *createOCLBackend() { return new OCLBackend(); }
} // namespace glow

static void dumpCompileLog(cl_device_id dev, cl_program prog) {
#ifndef NDEBUG
  // Determine the size of the log.
  size_t logSize;
  clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);

  // Allocate memory for the log.
  auto *log = (char *)malloc(logSize);

  // Get the log.
  clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, logSize, log, nullptr);

  // Print the log.
  llvm::outs() << log << "\n";
  free(log);
#endif
}

/// Add an macro definition with an integer value to the set of options.
template <typename T>
static void addIntOption(std::vector<std::string> &options,
                         const std::string &name, const T value) {
  options.push_back("-D" + name + "=" + std::to_string(value));
}

/// Add an macro definition with a string value to the set of options.
static void addStringOption(std::vector<std::string> &options,
                            const std::string &name, const std::string &value) {
  options.push_back("-D" + name + "=" + value);
}

OpenCLFunction::OpenCLFunction(std::unique_ptr<IRFunction> F,
                               const runtime::RuntimeBundle &bundle)
    : CompiledFunction(bundle), F_(std::move(F)) {
  cl_uint numPlatforms{0};
  cl_int err = clGetPlatformIDs(0, NULL, &numPlatforms);
  GLOW_ASSERT(err == CL_SUCCESS && "clGetPlatformIDs Failed.");
  GLOW_ASSERT(numPlatforms > clPlatformId &&
              "Should have at least one platform for running OpenCL");
  std::vector<cl_platform_id> platform_ids(numPlatforms);
  err = clGetPlatformIDs(numPlatforms, platform_ids.data(), NULL);
  cl_platform_id platform_id_used = platform_ids[clPlatformId];
  GLOW_ASSERT(err == CL_SUCCESS && "clGetPlatformIDs Failed.");

  cl_uint num{0};
  err = clGetDeviceIDs(platform_id_used, CL_DEVICE_TYPE_ALL, 0, nullptr, &num);
  GLOW_ASSERT(err == CL_SUCCESS && "clGetDeviceIDs Failed.");
  GLOW_ASSERT(num > clDeviceId &&
              "Should have at least one GPU/CPU/FPGA for running OpenCL");
  std::vector<cl_device_id> devices(num);
  err = clGetDeviceIDs(platform_id_used, CL_DEVICE_TYPE_ALL, num,
                       devices.data(), nullptr);
  GLOW_ASSERT(err == CL_SUCCESS && "clGetDeviceIDs Failed.");
  deviceId_ = devices[clDeviceId];
  context_ = clCreateContext(nullptr, 1, &deviceId_, nullptr, nullptr, nullptr);
  GLOW_ASSERT(context_ && "clCreateContext Failed.");
  commands_ = clCreateCommandQueue(
      context_, deviceId_, (clDoProfile) ? CL_QUEUE_PROFILING_ENABLE : 0, &err);
  GLOW_ASSERT(commands_ && "clCreateCommandQueue Failed.");

  err = CL_SUCCESS;
  std::vector<std::string> options;
  // Configure the kernels by providing the size of size_t on the host size.
  // This is required to e.g. properly pass struct parameters of types like
  // ShapeNHWC, ShapeNCHW, etc. The definitions of these types on the host side
  // use size_t for their members and they should be defined on the OpenCL's
  // side using integer types of the same width.
  addIntOption(options, "SIZEOF_HOST_SIZE_T", sizeof(size_t));
  // Create the program from the source.
  std::string source(reinterpret_cast<const char *>(kernels_cl_src),
                     kernels_cl_src_size);
  createProgram(source, options, commands_);
}

OpenCLFunction::~OpenCLFunction() {
  for (auto &kv : programsCache_) {
    auto prog = kv.second;
    clReleaseProgram(prog);
  }
  clReleaseCommandQueue(commands_);
  clReleaseContext(context_);
  if (deviceBuffer_) {
    freeDeviceBuffer(deviceBuffer_);
    deviceBuffer_ = nullptr;
  }
  tearDownRuns();
}

static std::string getKernelName(const char *baseName, ElemKind elemTy) {
  std::string name = baseName;
  switch (elemTy) {
  case ElemKind::FloatTy:
    return name + "W";
  case ElemKind::Int8QTy:
    return name + "_i8W";
  case ElemKind::Int32QTy:
    return name + "_i32W";
  case ElemKind::Int64ITy:
    return name + "_uW";
  case ElemKind::BoolTy:
    return name + "_bW";
  default:
    GLOW_UNREACHABLE("Unsupported element type");
  }
}

cl_kernel OpenCLFunction::createKernel(const std::string &name,
                                       cl_program program) {
  cl_int err = CL_SUCCESS;
  cl_kernel kernel = nullptr;
  if (program) {
    cl_kernel kernel = clCreateKernel(program, name.c_str(), &err);
    GLOW_ASSERT((kernel && err == CL_SUCCESS) && "clCreateKernel Failed.");
    return kernel;
  }
  // Inspect all programs.
  for (auto &kv : programsCache_) {
    auto prog = kv.second;
    cl_kernel kernel = clCreateKernel(prog, name.c_str(), &err);
    if (err == CL_SUCCESS) {
      return kernel;
    }
  }
  GLOW_ASSERT(kernel && "clCreateKernel Failed.");
  return kernel;
}

cl_program
OpenCLFunction::createProgram(const std::string &source,
                              const std::vector<std::string> &options,
                              cl_command_queue queue) {
  const char *src = source.c_str();
  cl_context ctx;
  cl_int err = clGetCommandQueueInfo(queue, CL_QUEUE_CONTEXT, sizeof(ctx), &ctx,
                                     nullptr);
  GLOW_ASSERT(err == CL_SUCCESS && "clGetCommandQueueInfo Failed.");
  cl_device_id deviceId;
  err = clGetCommandQueueInfo(queue, CL_QUEUE_DEVICE, sizeof(deviceId),
                              &deviceId, nullptr);
  GLOW_ASSERT(err == CL_SUCCESS && "clGetCommandQueueInfo Failed.");

  // Check if this program was compiled with the same parameters for the
  // provided context and device.
  std::string combinedOptions;
  for (auto &opt : options) {
    combinedOptions.append(opt).append(" ");
  }

  ProgramKey key = std::make_tuple(source, combinedOptions, deviceId);
  cl_program &program = programsCache_[key];
  if (program) {
    return program;
  }
  // Create a new compiled program.
  program = clCreateProgramWithSource(context_, 1, &src, nullptr, &err);
  GLOW_ASSERT(program && "clCreateProgramWithSource Failed.");
  err = clBuildProgram(program, 0, nullptr, combinedOptions.c_str(), nullptr,
                       nullptr);
  if (err) {
    dumpCompileLog(deviceId, program);
  }
  GLOW_ASSERT(err == CL_SUCCESS && "clBuildProgram Failed.");
  // Add this program to the program cache.
  return program;
}

template <class T>
void setKernelArg(cl_kernel kernel, unsigned argIdx, T value) {
  cl_int err = clSetKernelArg(kernel, argIdx, sizeof(T), &value);
  GLOW_ASSERT(err == CL_SUCCESS && "Unable to set parameter");
}

/// Set OpenCL \p kernel arguments using the buffer operands of the
/// instruction \p I. The first of these arguments should be passed to the \p
/// kernel at index \p nextKernelArgIdx. The \p bundle provides symbolTable, a
/// mapping from Values to on-device buffer offsets of these values.
///
/// \returns the index of the last set OpenCL kernel argument.
static size_t setKernelArgsForBuffers(cl_kernel kernel, const Instruction &I,
                                      size_t nextKernelArgIdx,
                                      runtime::RuntimeBundle &bundle) {
  // Number of instruction operands.
  auto numArgs = I.getNumOperands();
  // The predicate of the instruction if available.
  Value *predicate = I.hasPredicate() ? I.getPredicate() : nullptr;
  // The index of the kernel argument to be set.
  unsigned kernelArgIdx = nextKernelArgIdx;
  // Go over all operands and pass buffer operands to the kernel.
  for (unsigned arg = 0; arg < numArgs; arg++) {
    auto *value = I.getOperand(arg).first;
    // Ignore predicate operands as they are not supported by the OpenCL backend
    // yet.
    if (value == predicate)
      continue;
    // The value is a buffer that should be passed as a kernel argument.
    setKernelArg<cl_uint>(kernel, kernelArgIdx, bundle.getValueOffset(value));
    kernelArgIdx++;
  }
  return kernelArgIdx - 1;
}

void OpenCLFunction::fillBuffer(cl_mem buffer, uint64_t start, uint64_t len,
                                float value, ElemKind elemKind) {
  auto kernel = createKernel(getKernelName("splat", elemKind));
  setKernelArg(kernel, 0, buffer);
  setKernelArg<cl_uint>(kernel, 1, start);
  setKernelArg(kernel, 2, value);
  enqueueKernel(commands_, kernel, deviceId_, {(size_t)len}, kernelLaunches_);
}

/// \returns the max local workgroup size for each dimension, under the
/// opencl constraints, with the global workgroup sizes of \p global;
void getMaxLocalWorkgroupSize(cl_kernel kernel, cl_device_id device,
                              llvm::ArrayRef<size_t> global,
                              llvm::MutableArrayRef<size_t> local) {

  // Figure out the max size of the workgroup.
  size_t L;
  auto err = clGetKernelWorkGroupInfo(kernel, device, CL_KERNEL_WORK_GROUP_SIZE,
                                      sizeof(L), &L, nullptr);

  size_t WIS[3];
  clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_ITEM_SIZES, sizeof(WIS), &WIS,
                  nullptr);

  size_t WGS;
  clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(WGS), &WGS,
                  nullptr);

  GLOW_ASSERT(err == CL_SUCCESS && "Error in clGetKernelWorkGroupInfo.");
  // The global workgroup size must be a multiple of the local workgroup size,
  // and less than the max size for the specific dimension. Also, the
  // multiplication of all dimensions (size of total local work) needs to be
  // less than WSG. In here we find the highest L that divides the global
  // workgroup size. This is our naive implementation of gcd, with the other
  // constraints:
  size_t totalWorkPrevDims = 1;
  for (int i = 0, e = global.size(); i < e; i++) {
    local[i] = L;

    while (global[i] % local[i] || L % local[i] || local[i] > WIS[i] ||
           local[i] * totalWorkPrevDims > WGS) {
      local[i]--;
    }

    // Remember how much work we are doing in this dimension. Use it to make
    // sure that the next dimenstions don't exceed the total allowed workgroup
    // size.
    totalWorkPrevDims *= local[i];
  }
}

void OpenCLFunction::enqueueKernel(cl_command_queue commands, cl_kernel kernel,
                                   cl_device_id device,
                                   llvm::ArrayRef<size_t> global,
                                   llvm::ArrayRef<size_t> local,
                                   std::vector<KernelLaunch> &kernelLaunches) {
  char kernelName[128];
  size_t retSize;
  cl_int err = clGetKernelInfo(kernel, CL_KERNEL_FUNCTION_NAME,
                               sizeof(kernelName), &kernelName, &retSize);
  GLOW_ASSERT(err == CL_SUCCESS && "Error in clGetKernelInfo.");

  cl_event event{nullptr};
  err = clEnqueueNDRangeKernel(commands, kernel, global.size(), nullptr,
                               &global[0], &local[0], 0, nullptr,
                               clDoProfile ? &event : nullptr);
  GLOW_ASSERT(err == CL_SUCCESS && "Error in clEnqueueNDRangeKernel.");
  kernelLaunches.push_back(KernelLaunch(kernel, kernelName, event));
}

/// Enqueue a \p kernel for execution on the command queue \p commands on a
/// given \p device. The information about the launched kernel will be added to
/// \p kernelLaunches list.
void OpenCLFunction::enqueueKernel(cl_command_queue commands, cl_kernel kernel,
                                   cl_device_id device,
                                   llvm::ArrayRef<size_t> global,
                                   std::vector<KernelLaunch> &kernelLaunches) {
  llvm::SmallVector<size_t, 4> local(global.size(), 0);
  getMaxLocalWorkgroupSize(kernel, device, global, local);
  char kernelName[128];
  size_t retSize;
  cl_int err = clGetKernelInfo(kernel, CL_KERNEL_FUNCTION_NAME,
                               sizeof(kernelName), &kernelName, &retSize);
  GLOW_ASSERT(err == CL_SUCCESS && "Error in clGetKernelInfo.");

  cl_event event{nullptr};
  err = clEnqueueNDRangeKernel(commands, kernel, global.size(), nullptr,
                               &global[0], &local[0], 0, nullptr,
                               clDoProfile ? &event : nullptr);
  GLOW_ASSERT(err == CL_SUCCESS && "Error in clEnqueueNDRangeKernel.");
  kernelLaunches.push_back(KernelLaunch(kernel, kernelName, event));
}

/// Analyze and dump the collected profiling information about the execution of
/// OpenCL kernels.
static void dumpProfileInfo(const std::vector<KernelLaunch> &kernelLaunches) {
  if (!clDoProfile)
    return;
  cl_ulong total = 0;

  std::unordered_map<std::string, cl_ulong> kernelToDuration;

  for (auto &kl : kernelLaunches) {
    auto &event = kl.event_;
    clWaitForEvents(1, &event);
    auto name = kl.name_;
    assert(!name.empty() && "Kernel name cannot be empty");
    cl_ulong time_start;
    cl_ulong time_end;

    clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START,
                            sizeof(time_start), &time_start, NULL);
    clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(time_end),
                            &time_end, NULL);
    // Duration (in nanoseconds).
    double duration = time_end - time_start;
    kernelToDuration[name] += duration;
    total += duration;
    llvm::outs() << "OpenCl execution time for a launch of kernel " << name
                 << format(" is: %0.3f milliseconds\n", duration / 1000000.0);
  }
  llvm::outs() << format(
      "OpenCl total execution time is: %0.3f milliseconds \n",
      total / 1000000.0);

  // Build a sorted list of kernel durations.
  std::vector<std::pair<cl_ulong, std::string>> sortedKernelDurations;
  sortedKernelDurations.reserve(kernelToDuration.size());
  for (auto kv : kernelToDuration) {
    sortedKernelDurations.push_back(std::make_pair(kv.second, kv.first));
  }
  std::sort(sortedKernelDurations.begin(), sortedKernelDurations.end());

  llvm::outs() << "\n\nSummary information per kernel:\n";
  for (auto k : sortedKernelDurations) {
    llvm::outs() << "OpenCl total execution time for kernel " << k.second
                 << format(" is: %0.3f milliseconds (%lu%%)\n",
                           k.first / 1000000.0,
                           (unsigned long)(k.first * 100 / total));
  }
}

void OpenCLFunction::executeConvolution(const OCLConvolutionInst *CC) {
  auto input = CC->getSrc();
  auto output = CC->getDest();
  auto bias = CC->getBias();
  auto weights = CC->getFilter();
  auto odim = ShapeNCHW(CC->getDest()->getType()->dims());
  auto idim = ShapeNCHW(CC->getSrc()->getType()->dims());
  auto fdim = ShapeNCHW(CC->getFilter()->getType()->dims());
  bool isQuantized = output->getType()->isQuantizedType();
  PaddingTLBR pads(CC->getPads());
  ShapeHW kdim(CC->getKernels());
  ShapeHW sdim(CC->getStrides());
  unsigned group = CC->getGroup();
  // So far, we don't support fast convolution kernel if group > 1.
  // For group convolution, the slow convolution kernel should be invoked.
  // The following assertion should be removed once the group > 1 is supported
  // in fast convolution kernel.
  assert(group == 1 && "Group Convolution is not supported by OpenCL backend's "
                       "fast convolution kernel.");

  // Create options for compiling the program.
  // Don't use names M, N, K as they are defined in precompiled headers.

  std::vector<std::string> options;
  // Number of spacial axes.
  addIntOption(options, "v_nax", 2);
  // Number of groups.
  addIntOption(options, "v_g", group);
  // Parameters for kernel size, padding and stride
  addIntOption(options, "v_k_0", kdim.height);
  addIntOption(options, "v_k_1", kdim.width);
  addIntOption(options, "v_p_0", pads.top);
  addIntOption(options, "v_p_1", pads.left);
  addIntOption(options, "v_s_0", sdim.height);
  addIntOption(options, "v_s_1", sdim.width);

  // Dilation.
  addIntOption(options, "v_d_0", 1);
  addIntOption(options, "v_d_1", 1);

  // Number of kernel input channels.
  addIntOption(options, "v_fin", fdim.c);
  // Number of kernel output channels.
  addIntOption(options, "v_fout", fdim.n);
  // Bias multiplier.
  addStringOption(options, "v_bmul", "(float)1");

  // Spacial dimensions of input.
  addIntOption(options, "v_imsi_0", idim.h);
  addIntOption(options, "v_imsi_1", idim.w);

  // Spacial dimensions of output.
  addIntOption(options, "v_imso_0", odim.h);
  addIntOption(options, "v_imso_1", odim.w);

  // Padding required for tiles.
  addStringOption(options, "v_pad_A", "0");
  addStringOption(options, "v_pad_B", "0");

  // Determine the work groups sizes along h and w.
  size_t WIS[3];
  cl_int err = clGetDeviceInfo(deviceId_, CL_DEVICE_MAX_WORK_ITEM_SIZES,
                               sizeof(WIS), &WIS, nullptr);
  GLOW_ASSERT(err == CL_SUCCESS && "Could not execute clGetDeviceInfo");
  size_t wg_size[3];
  for (int id = 0; id < 2; ++id) {
    size_t defaultVal = 16;
    // Special case on CPUs devices, where a workgroup size could be 1,
    // e.g. in case of Apple's OpenCL driver for CPUs.
    if (WIS[id] < defaultVal || (id == 0 && WIS[1] < defaultVal)) {
      defaultVal = WIS[1];
    }
    addIntOption(options, "workgroup_size_" + std::to_string(id), defaultVal);
    wg_size[id] = defaultVal;
  }
  // The tile-size in dimension K.
  // Should be tunable.
  addStringOption(options, "TSK", "4");
  addIntOption(options, "TSK_UNROLL", 1);

  // WPTN and WPTM should be tunable.
  size_t WPTN = 4;
  size_t WPTM = 4;
  // The work-per-thread in dimension N.
  addIntOption(options, "WPTN", WPTN);
  // The work-per-thread in dimension M.
  addIntOption(options, "WPTM", WPTM);

  // Vector width in dimensions M and M.
  // VWN and VWM should be tunable.
  addStringOption(options, "VWM", "4");
  addStringOption(options, "VWN", "4");

  // Generate a tailor-made convolution kernel using the provided options based
  // on the parameters of the current convolution.
  std::string src;
  if (isQuantized) {
    src.append(
        reinterpret_cast<const char *>(kernels_fwd_quantized_conv_cl_src),
        kernels_fwd_quantized_conv_cl_src_size);
  } else {
    src.append(reinterpret_cast<const char *>(kernels_fwd_conv_cl_src),
               kernels_fwd_conv_cl_src_size);
  }
  auto prog = createProgram(src, options, commands_);
  auto kernelName = isQuantized ? "conv_forward_mem_i8" : "conv_forward_mem";
  auto kernel = createKernel(kernelName, prog);
  setKernelArg(kernel, 0, deviceBuffer_);
  setKernelArg<cl_uint>(kernel, 1, runtimeBundle_.getValueOffset(input));
  setKernelArg<cl_uint>(kernel, 2, runtimeBundle_.getValueOffset(weights));
  setKernelArg<cl_uint>(kernel, 3, runtimeBundle_.getValueOffset(bias));
  setKernelArg<cl_uint>(kernel, 4, runtimeBundle_.getValueOffset(output));

  // Extra options for quantized kernel
  if (isQuantized) {
    auto inputTy = CC->getSrc()->getType();
    auto outputTy = CC->getDest()->getType();
    auto biasTy = CC->getBias()->getType();
    auto weightsTy = CC->getFilter()->getType();
    setKernelArg(kernel, 5, weightsTy->getOffset());
    setKernelArg(kernel, 6, weightsTy->getScale());
    setKernelArg(kernel, 7, inputTy->getOffset());
    setKernelArg(kernel, 8, inputTy->getScale());
    setKernelArg(kernel, 9, outputTy->getOffset());
    setKernelArg(kernel, 10, outputTy->getScale());
    setKernelArg(kernel, 11, biasTy->getOffset());
    setKernelArg(kernel, 12, biasTy->getScale());
  }

  // Compute proper parameters for global work and workgroups.
  auto fw_wgs0 = wg_size[0];
  auto fw_wgs1 = wg_size[1];
  int fw_wptn = WPTN;
  int fw_wptm = WPTM;
  int fw_div_N = fw_wptn * fw_wgs0;
  int fw_div_M = fw_wptm * fw_wgs1;
  int N_FW_ = odim.h * odim.w;
  int M_FW_ = odim.c / group;
  size_t L;
  clGetKernelWorkGroupInfo(kernel, deviceId_, CL_KERNEL_WORK_GROUP_SIZE,
                           sizeof(L), &L, nullptr);
  GLOW_ASSERT(fw_wgs0 * fw_wgs1 <= L && "Bad workgroup size");

  // Set the size of a workgroup.
  std::vector<size_t> local = {fw_wgs0, fw_wgs1, 1};

  // Set the global work size.
  std::vector<size_t> global = {((N_FW_ - 1) / fw_div_N + 1) * fw_wgs0,
                                ((M_FW_ - 1) / fw_div_M + 1) * fw_wgs1,
                                idim.n * group};

  enqueueKernel(commands_, kernel, deviceId_, global, local, kernelLaunches_);
}

/// This method is copied from InterpreterNodes.cpp. Please be aware that
/// they should be in sync.
template <typename T>
static void topK(Tensor &outW, Tensor &indW, Tensor &inW, size_t k) {
  auto values = outW.getHandle<T>();
  auto indices = indW.getHandle<int64_t>();
  auto in = inW.getHandle<T>();
  size_t n = in.dims().back();

  size_t in_p = 0, out_p = 0;
  size_t tensor_end = in.size();
  using pairType = std::pair<float, size_t>;
  std::vector<pairType> buf(n);

  while (in_p < tensor_end) {
    for (size_t i = 0; i < n; i++) {
      buf[i].first = in.raw(in_p++);
      buf[i].second = i;
    }
    // NOTE: it's possible to do N + KlogK, while this version is NlogN
    std::sort(buf.begin(), buf.end(), [](const pairType &a, const pairType &b) {
      if (a.first != b.first)
        return a.first > b.first;
      return a.second < b.second;
    });
    for (size_t i = 0; i < k; i++) {
      values.raw(out_p) = buf[i].first;
      indices.raw(out_p) = buf[i].second;
      out_p++;
    }
  }
}

void OpenCLFunction::execute(PlaceholderBindings *bindings) {
  (void)bindings;

  for (const auto &I : F_->getInstrs()) {
    // Skip memory allocation instructions as they are NOPs.
    if (isa<AllocActivationInst>(I) || isa<DeallocActivationInst>(I) ||
        isa<TensorViewInst>(I)) {
      continue;
    }

    // The kernels are named after the name of the instruction, plus the "W"
    // suffix to prevent name colissions for functions like 'tanh' that are also
    // a part of the OpenCL runtime.
    auto elemTy = I.getNumOperands() ? I.getOperand(0).first->getElementType()
                                     : ElemKind::FloatTy;

    // If ElementCmpLTEInst then the first operand is always bool, so instead
    // set the element type based on the LHS input.
    if (auto *LTE = dyn_cast<ElementCmpLTEInst>(&I)) {
      elemTy = LTE->getLHS()->getElementType();
    }

    std::string kernelName = getKernelName(I.getKindName(), elemTy);

    //  Check if the instruction is quantized. Consider an instruction to be
    //  quantized if its destination or source operands are quantized.
    bool isQuantized = I.getNumOperands() &&
                       (I.getOperand(0).first->getType()->isQuantizedType() ||
                        I.getOperand(I.getNumOperands() - 1)
                            .first->getType()
                            ->isQuantizedType());

    // Element-wise operations, except the copy instruction.
    if (I.isDataParallel() && !isa<CopyInst>(I)) {
      // Figure out how many element-wise elements are there to process:
      size_t global;
      if (I.isDataParallel()) {
        global = I.getOperand(0).first->getType()->size();
        // The check for quantization below is a temporary workaround until the
        // corresponding kernels are implemented for the quantized operations.
        if (!isQuantized) {
          if (global % 16 == 0) {
            // Start less kernels and let each kernel do more work using vector
            // instructions.
            global /= 16;
            kernelName += "16";
          } else if (global % 8 == 0) {
            // Start less kernels and let each kernel do more work using vector
            // instructions.
            global /= 8;
            kernelName += "8";
          }
        }
      } else {
        GLOW_UNREACHABLE("Invalid instruction.");
      }

      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);
      auto numMandatoryArgs = numArgs;
      (void)numMandatoryArgs;

      if (auto *SI = dyn_cast<SplatInst>(&I)) {
        // Pass the splat as a parameter.
        if (!isQuantized) {
          setKernelArg(kernel, ++numArgs, SI->getValue());
        } else {
          auto *destTy = SI->getDest()->getType();
          TensorQuantizationParams destQ{destTy->getScale(),
                                         destTy->getOffset()};
          float val = SI->getValue();
          int8_t int8Val = quantization::quantize(val, destQ);
          setKernelArg<float>(kernel, ++numArgs, static_cast<float>(int8Val));
        }
      }

      if (isQuantized) {
        if (isa<ElementAddInst>(I) || isa<ElementSubInst>(I) ||
            isa<ElementMulInst>(I) || isa<ElementDivInst>(I) ||
            isa<ElementMinInst>(I) || isa<ElementMaxInst>(I)) {
          int32_t destOffset = I.getOperand(0).first->getType()->getOffset();
          float destScale = I.getOperand(0).first->getType()->getScale();

          auto LHSTy = I.getOperand(1).first->getType();
          auto RHSTy = I.getOperand(2).first->getType();

          auto lhsScaleParams = quantization::quantizeScaleOffset32To8(
              LHSTy->getScale() / destScale, LHSTy->getOffset());
          auto rhsScaleParams = quantization::quantizeScaleOffset32To8(
              RHSTy->getScale() / destScale, RHSTy->getOffset());
          setKernelArg(kernel, ++numArgs, destOffset);
          setKernelArg(kernel, ++numArgs, lhsScaleParams);
          setKernelArg(kernel, ++numArgs, rhsScaleParams);
          if (isa<ElementMulInst>(I) || isa<ElementDivInst>(I)) {
            float resultScale =
                isa<ElementMulInst>(I)
                    ? LHSTy->getScale() * RHSTy->getScale() / destScale
                    : LHSTy->getScale() / (RHSTy->getScale() * destScale);
            auto resultScaleParams =
                quantization::quantizeScaleOffset32To8(resultScale, 0);
            setKernelArg(kernel, ++numArgs, resultScaleParams);
          }
        }
        // Quantize floating point tensor. Scale and Offset are based on return
        // type of the instruction \p I.
        if (auto *QI = dyn_cast<QuantizeInst>(&I)) {
          float destTensorQuantizationScale =
              QI->getDest()->getType()->getScale();
          int32_t destTensorQuantizationOffset =
              QI->getDest()->getType()->getOffset();
          setKernelArg(kernel, ++numArgs, destTensorQuantizationScale);
          setKernelArg(kernel, ++numArgs, destTensorQuantizationOffset);
        }
        // Rescale quantized tensor. Scale and Offset are based on return type
        // of the instruction \p I.
        if (auto *RQI = dyn_cast<RescaleQuantizedInst>(&I)) {
          auto *dest = RQI->getDest();
          auto *src = RQI->getSrc();
          auto *destType = dest->getType();
          auto *srcType = src->getType();
          auto rescaleParams = quantization::quantizeScaleOffset32To8(
              srcType->getScale() / destType->getScale(), srcType->getOffset());

          setKernelArg(kernel, ++numArgs, destType->getOffset());
          setKernelArg(kernel, ++numArgs, srcType->getOffset());
          setKernelArg(kernel, ++numArgs, rescaleParams);
        }
        // Dequantize integer tensor. Scale and Offset are based
        // on the source tensor type.
        if (auto *QI = dyn_cast<DequantizeInst>(&I)) {
          float srcTensorQuantizationScale =
              QI->getSrc()->getType()->getScale();
          int32_t srcTensorQuantizationOffset =
              QI->getSrc()->getType()->getOffset();
          setKernelArg(kernel, ++numArgs, srcTensorQuantizationScale);
          setKernelArg(kernel, ++numArgs, srcTensorQuantizationOffset);
        }
      }

      assert((!isQuantized) ||
             numArgs > numMandatoryArgs && "Not enough kernel arguments");
      enqueueKernel(commands_, kernel, deviceId_, {global}, kernelLaunches_);
      continue;
    }

    if (auto *SM = dyn_cast<SoftMaxInst>(&I)) {
      // Implement Softmax by parallelizing the batch dimension. Each sample in
      // the batch is processed by a different parallel 'thread'.
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      // This is the number of elements for each slice. There are N slices in
      // our batch.
      auto inputDims = SM->getSrc()->getType()->dims();
      size_t numSlices = inputDims[0];

      // Pass the slice size (size of each sample in the batch) as a parameter.
      setKernelArg<cl_uint>(kernel, numArgs + 1, flattenCdr(inputDims).second);

      enqueueKernel(commands_, kernel, deviceId_, {numSlices}, kernelLaunches_);
      continue;
    }

    if (auto *SM = dyn_cast<SoftMaxGradInst>(&I)) {
      // Implement Softmax by parallelizing the batch dimension. Each sample in
      // the batch is processed by a different parallel 'thread'.
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      // This is the number of elements for each slice. There are N slices in
      // our batch.
      auto inputDims = SM->getSrcGrad()->getType()->dims();
      size_t numSlices = inputDims[0];

      // Pass the slice size (size of each sample in the batch) as a parameter.
      setKernelArg<cl_uint>(kernel, numArgs + 1, flattenCdr(inputDims).second);

      enqueueKernel(commands_, kernel, deviceId_, {numSlices}, kernelLaunches_);
      continue;
    }

    if (auto *ET = dyn_cast<ExtractTensorInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      // Currently support tensors up to 4 dimensions.
      // TODO: Handle other dimensions.
      const size_t numDimensions = ET->getDest()->getType()->dims().size();
      ShapeNHWC odim = ShapeNHWC::empty();
      ShapeNHWC idim = ShapeNHWC::empty();
      ShapeNHWC offset = ShapeNHWC::empty();

      if (numDimensions == 1) {
        odim = ShapeNHWC::fromX(ET->getDest()->getType()->dims());
        idim = ShapeNHWC::fromX(ET->getSrc()->getType()->dims());
        offset = ShapeNHWC::fromXY(ET->getOffsets());
      } else if (numDimensions == 2) {
        odim = ShapeNHWC::fromXY(ET->getDest()->getType()->dims());
        idim = ShapeNHWC::fromXY(ET->getSrc()->getType()->dims());
        offset = ShapeNHWC::fromXY(ET->getOffsets());
      } else if (numDimensions == 3) {
        odim = ShapeNHWC::fromXYZ(ET->getDest()->getType()->dims());
        idim = ShapeNHWC::fromXYZ(ET->getSrc()->getType()->dims());
        offset = ShapeNHWC::fromXYZ(ET->getOffsets());
      } else if (numDimensions == 4) {
        odim = ShapeNHWC(ET->getDest()->getType()->dims());
        idim = ShapeNHWC(ET->getSrc()->getType()->dims());
        offset = ShapeNHWC(ET->getOffsets());
      } else {
        llvm_unreachable("Unsupported tensor dimension");
      }

      setKernelArg(kernel, numArgs + 1, odim);
      setKernelArg(kernel, numArgs + 2, idim);
      setKernelArg(kernel, numArgs + 3, offset);
      enqueueKernel(commands_, kernel, deviceId_, {odim.n, odim.h},
                    kernelLaunches_);
      continue;
    }

    if (auto *IT = dyn_cast<InsertTensorInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      // Currently support tensors of up to 4 dimensions.
      // TODO: Handle other dimensions.
      const size_t numDimensions = IT->getDest()->getType()->dims().size();
      ShapeNHWC odim = ShapeNHWC::empty();
      ShapeNHWC idim = ShapeNHWC::empty();
      ShapeNHWC offset = ShapeNHWC::empty();
      if (numDimensions == 1) {
        odim = ShapeNHWC::fromX(IT->getDest()->getType()->dims());
        idim = ShapeNHWC::fromX(IT->getSrc()->getType()->dims());
        offset = ShapeNHWC::fromX(IT->getOffsets());
      } else if (numDimensions == 2) {
        odim = ShapeNHWC::fromXY(IT->getDest()->getType()->dims());
        idim = ShapeNHWC::fromXY(IT->getSrc()->getType()->dims());
        offset = ShapeNHWC::fromXY(IT->getOffsets());
      } else if (numDimensions == 3) {
        odim = ShapeNHWC::fromXYZ(IT->getDest()->getType()->dims());
        idim = ShapeNHWC::fromXYZ(IT->getSrc()->getType()->dims());
        offset = ShapeNHWC::fromXYZ(IT->getOffsets());
      } else if (numDimensions == 4) {
        odim = ShapeNHWC(IT->getDest()->getType()->dims());
        idim = ShapeNHWC(IT->getSrc()->getType()->dims());
        offset = ShapeNHWC(IT->getOffsets());
      } else {
        llvm_unreachable("Unsupported tensor dimension");
      }

      setKernelArg(kernel, numArgs + 1, odim);
      setKernelArg(kernel, numArgs + 2, idim);
      setKernelArg(kernel, numArgs + 3, offset);
      setKernelArg<cl_uint>(kernel, numArgs + 4, IT->getCount());
      setKernelArg<cl_uint>(kernel, numArgs + 5, IT->getAxis());
      enqueueKernel(commands_, kernel, deviceId_, {idim.n, idim.h},
                    kernelLaunches_);
      continue;
    }

    if (auto *BMM = dyn_cast<MatMulInst>(&I)) {
// Size of the tile to be used for matrix multiplication.
#define TILE_DIM ((size_t)8)
      // Determine max work groups sizes.
      size_t WIS[3];
      cl_int err = clGetDeviceInfo(deviceId_, CL_DEVICE_MAX_WORK_ITEM_SIZES,
                                   sizeof(WIS), &WIS, nullptr);
      GLOW_ASSERT(err == CL_SUCCESS && "Could not execute clGetDeviceInfo");
      // True if the tiled matrix multiplication kernel can be used. This is
      // only possible if the device allows workgroups with sizes which are at
      // least as big as a tile.
      bool useTiledMatMul = (WIS[0] >= TILE_DIM && WIS[1] >= TILE_DIM);
      auto tiledKernelName = isQuantized ? "matmul_tiled_i8" : "matmul_tiled";
      cl_kernel kernel =
          createKernel(useTiledMatMul ? tiledKernelName : kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto ddim = ShapeNHWC::fromXY(BMM->getDest()->getType()->dims());
      auto ldim = ShapeNHWC::fromXY(BMM->getLHS()->getType()->dims());
      auto rdim = ShapeNHWC::fromXY(BMM->getRHS()->getType()->dims());

      setKernelArg(kernel, numArgs + 1, ddim);
      setKernelArg(kernel, numArgs + 2, ldim);
      setKernelArg(kernel, numArgs + 3, rdim);
      if (isQuantized) {
        auto lhsTy = BMM->getLHS()->getType();
        auto rhsTy = BMM->getRHS()->getType();
        auto destTy = BMM->getDest()->getType();
        auto destScaleParams = quantization::quantizeScaleOffset32To8(
            lhsTy->getScale() * rhsTy->getScale() / destTy->getScale(), 0);
        setKernelArg(kernel, numArgs + 4, lhsTy->getOffset());
        setKernelArg(kernel, numArgs + 5, rhsTy->getOffset());
        setKernelArg(kernel, numArgs + 6, destTy->getOffset());
        setKernelArg(kernel, numArgs + 7, destScaleParams);
      }

      if (useTiledMatMul) {
        std::vector<size_t> local{TILE_DIM, TILE_DIM};
        std::vector<size_t> global{(ddim.n / local[0] + 1) * local[0],
                                   (ddim.h / local[1] + 1) * local[1]};

        enqueueKernel(commands_, kernel, deviceId_, global, local,
                      kernelLaunches_);
      } else {
        enqueueKernel(commands_, kernel, deviceId_, {ddim.n, ddim.h, ddim.w},
                      kernelLaunches_);
      }
#undef TILE_DIM
      continue;
    }

    if (auto *BA = dyn_cast<BatchedAddInst>(&I)) {
      if (isQuantized &&
          BA->getSlice()->getType()->getElementType() == ElemKind::Int32QTy) {
        kernelName += "_32";
      }
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto bdim = flattenCdr(BA->getBatch()->dims());
      setKernelArg<cl_uint>(kernel, numArgs + 1, bdim.first);
      setKernelArg<cl_uint>(kernel, numArgs + 2, bdim.second);

      if (isQuantized) {
        auto *destTy = BA->getDest()->getType();
        auto *batchTy = BA->getBatch()->getType();
        auto *sliceTy = BA->getSlice()->getType();

        setKernelArg(kernel, numArgs + 3, destTy->getOffset());

        float destScale = destTy->getScale();
        auto batchScaleParams = quantization::quantizeScaleOffset32To8(
            batchTy->getScale() / destScale, batchTy->getOffset());
        auto sliceScaleParams = quantization::quantizeScaleOffset32To8(
            sliceTy->getScale() / destScale, sliceTy->getOffset());

        setKernelArg(kernel, numArgs + 4, batchScaleParams);
        setKernelArg(kernel, numArgs + 5, sliceScaleParams);
      }

      // Parallelize on each element in the slice.
      enqueueKernel(commands_, kernel, deviceId_, {bdim.second},
                    kernelLaunches_);
      continue;
    }

    if (auto *BRA = dyn_cast<BatchedReduceAddInst>(&I)) {
      assert(BRA->getAxis() == 0 && "No current support for non-zero axis.");

      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto bdim = flattenCdr(BRA->getBatch()->dims());
      setKernelArg<cl_uint>(kernel, numArgs + 1, bdim.first);
      setKernelArg<cl_uint>(kernel, numArgs + 2, bdim.second);

      // Parallelize on each element in the slice.
      enqueueKernel(commands_, kernel, deviceId_, {bdim.second},
                    kernelLaunches_);
      continue;
    }

    if (auto *CC = dyn_cast<OCLConvolutionInst>(&I)) {
      executeConvolution(CC);
      continue;
    }

    if (auto *CC = dyn_cast<ConvolutionInst>(&I)) {
      // This is a naive implementation that parallelizes using three dims:
      // the X and the Y in the output filter.
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);
      auto odim = ShapeNHWC(CC->getDest()->getType()->dims());
      auto idim = ShapeNHWC(CC->getSrc()->getType()->dims());
      auto pads = PaddingTLBR(CC->getPads());
      ShapeHW kdim(CC->getKernels());
      ShapeHW sdim(CC->getStrides());
      setKernelArg(kernel, numArgs + 1, kdim);
      setKernelArg(kernel, numArgs + 2, sdim);
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, CC->getGroup());
      setKernelArg(kernel, numArgs + 5, odim);
      setKernelArg(kernel, numArgs + 6, idim);
      setKernelArg(kernel, numArgs + 7,
                   ShapeNHWC(CC->getFilter()->getType()->dims()));
      if (isQuantized) {
        auto srcTy = CC->getSrc()->getType();
        auto destTy = CC->getDest()->getType();
        auto filterTy = CC->getFilter()->getType();
        auto biasTy = CC->getBias()->getType();
        setKernelArg(kernel, numArgs + 8, destTy->getOffset());
        setKernelArg(kernel, numArgs + 9, destTy->getScale());
        setKernelArg(kernel, numArgs + 10, srcTy->getOffset());
        setKernelArg(kernel, numArgs + 11, srcTy->getScale());
        setKernelArg(kernel, numArgs + 12, filterTy->getOffset());
        setKernelArg(kernel, numArgs + 13, filterTy->getScale());
        setKernelArg(kernel, numArgs + 14, biasTy->getOffset());
        setKernelArg(kernel, numArgs + 15, biasTy->getScale());
      }

      // Use a 3D grid where the first dimension is the depth and the second
      // dimension is the slice index in the batch.
      enqueueKernel(commands_, kernel, deviceId_, {odim.h, odim.w, odim.c},
                    kernelLaunches_);
      continue;
    }

    if (auto *CG = dyn_cast<ConvolutionGradInst>(&I)) {
      auto *src = CG->getSrc();
      auto *filter = CG->getFilter();
      auto *destGrad = CG->getDestGrad();
      auto *srcGrad = CG->getSrcGrad();
      auto *filterGrad = CG->getFilterGrad();
      auto *biasGrad = CG->getBiasGrad();
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto destGradDim = ShapeNHWC(destGrad->dims());
      auto srcDim = ShapeNHWC(src->dims());
      auto filterGradDim = ShapeNHWC(filterGrad->dims());
      auto pads = PaddingTLBR(CG->getPads());
      ShapeHW kdim(CG->getKernels());
      ShapeHW sdim(CG->getStrides());
      setKernelArg(kernel, numArgs + 1, kdim);
      setKernelArg(kernel, numArgs + 2, sdim);
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, CG->getGroup());
      setKernelArg(kernel, numArgs + 5, srcDim);
      setKernelArg(kernel, numArgs + 6, destGradDim);
      setKernelArg(kernel, numArgs + 7, filterGradDim);
      // Zero memory for the output buffers.
      fillBuffer(deviceBuffer_, runtimeBundle_.getValueOffset(srcGrad),
                 srcGrad->size(), 0, srcGrad->getElementType());
      fillBuffer(deviceBuffer_, runtimeBundle_.getValueOffset(filterGrad),
                 filterGrad->size(), 0, filterGrad->getElementType());
      fillBuffer(deviceBuffer_, runtimeBundle_.getValueOffset(biasGrad),
                 biasGrad->size(), 0, biasGrad->getElementType());

      (void)filter;
      assert(filter->dims() == filterGrad->dims() && "Dims should be the same");
      assert(src->dims() == srcGrad->dims() && "Dims should be the same");

      enqueueKernel(commands_, kernel, deviceId_,
                    {destGradDim.h, destGradDim.w, destGradDim.c},
                    kernelLaunches_);
      continue;
    }

    if (auto *PM = dyn_cast<MaxPoolInst>(&I)) {
      // This is a naive implementation that parallelizes using three dims:
      // the X and the Y in the output filter.
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto odim = ShapeNHWC(PM->getDest()->getType()->dims());
      auto idim = ShapeNHWC(PM->getSrc()->getType()->dims());
      auto pads = PaddingTLBR(PM->getPads());
      ShapeHW kdim(PM->getKernels());
      assert(kdim.isSquare() && "Only square kernel is supported");
      ShapeHW sdim(PM->getStrides());
      assert(sdim.isSquare() && "Only square stride is supported");
      setKernelArg<cl_uint>(kernel, numArgs + 1, kdim.height);
      setKernelArg<cl_uint>(kernel, numArgs + 2, sdim.height);
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, odim);
      setKernelArg(kernel, numArgs + 5, idim);

      enqueueKernel(commands_, kernel, deviceId_, {odim.h, odim.w, odim.c},
                    kernelLaunches_);
      continue;
    }

    if (auto *PM = dyn_cast<MaxPoolWithXYInst>(&I)) {
      // This is a naive implementation that parallelizes using three dims:
      // the X and the Y in the output filter.
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto odim = ShapeNHWC(PM->getDest()->getType()->dims());
      auto idim = ShapeNHWC(PM->getSrc()->getType()->dims());
      auto pads = PaddingTLBR(PM->getPads());
      ShapeHW kdim(PM->getKernels());
      assert(kdim.isSquare() && "Only square kernel is supported");
      ShapeHW sdim(PM->getStrides());
      assert(sdim.isSquare() && "Only square stride is supported");
      setKernelArg<cl_uint>(kernel, numArgs + 1, kdim.height);
      setKernelArg<cl_uint>(kernel, numArgs + 2, sdim.height);
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, odim);
      setKernelArg(kernel, numArgs + 5, idim);

      enqueueKernel(commands_, kernel, deviceId_, {odim.h, odim.w, odim.c},
                    kernelLaunches_);
      continue;
    }

    if (auto *PMG = dyn_cast<MaxPoolWithXYGradInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto destGradDim = ShapeNHWC(PMG->getDestGrad()->dims());
      auto srcGradDim = ShapeNHWC(PMG->getSrcGrad()->dims());
      auto pads = PaddingTLBR(PMG->getPads());
      ShapeHW kdim(PMG->getKernels());
      assert(kdim.isSquare() && "Only square kernel is supported");
      ShapeHW sdim(PMG->getStrides());
      assert(sdim.isSquare() && "Only square stride is supported");
      setKernelArg<cl_uint>(kernel, numArgs + 1, kdim.height);
      setKernelArg<cl_uint>(kernel, numArgs + 2, sdim.height);
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, srcGradDim);
      setKernelArg(kernel, numArgs + 5, destGradDim);

      assert(srcGradDim.n == destGradDim.n && "batch size is wrong");
      assert(srcGradDim.c == destGradDim.c && "depth size is wrong");

      enqueueKernel(commands_, kernel, deviceId_, {srcGradDim.n},
                    kernelLaunches_);
      continue;
    }

    if (auto *PA = dyn_cast<AvgPoolInst>(&I)) {
      // This is a naive implementation that parallelizes using three dims:
      // the X and the Y in the output filter.
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto odim = ShapeNHWC(PA->getDest()->getType()->dims());
      auto idim = ShapeNHWC(PA->getSrc()->getType()->dims());
      auto pads = PaddingTLBR(PA->getPads());
      ShapeHW kdim(PA->getKernels());
      assert(kdim.isSquare() && "Only square kernel is supported");
      ShapeHW sdim(PA->getStrides());
      assert(sdim.isSquare() && "Only square stride is supported");
      setKernelArg<cl_uint>(kernel, numArgs + 1, kdim.height);
      setKernelArg<cl_uint>(kernel, numArgs + 2, sdim.height);
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, odim);
      setKernelArg(kernel, numArgs + 5, idim);

      enqueueKernel(commands_, kernel, deviceId_, {odim.h, odim.w, odim.c},
                    kernelLaunches_);
      continue;
    }

    if (auto *TR = dyn_cast<TransposeInst>(&I)) {
      // This is a naive implementation that parallelizes using one dimension,
      // the N (batch size).
      GLOW_ASSERT(TR->getShuffle().size() <= 4 &&
                  "This code supports only 4 and lower dimensional transposes");

      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      // Temporary hack to support 3-dim transposes.
      // TODO: support any dimensional transposes.
      std::vector<size_t> odim_vec = TR->getDest()->getType()->dims();
      std::vector<size_t> idim_vec = TR->getSrc()->getType()->dims();
      std::vector<unsigned_t> mask = TR->getShuffle();
      while (mask.size() < 4) {
        odim_vec.push_back(1);
        idim_vec.push_back(1);
        mask.push_back(mask.size());
        continue;
      }

      auto odim = ShapeNHWC(llvm::makeArrayRef(odim_vec));
      auto idim = ShapeNHWC(llvm::makeArrayRef(idim_vec));

      setKernelArg(kernel, numArgs + 1, odim);
      setKernelArg(kernel, numArgs + 2, idim);

      ShapeNHWC shuff(mask[0], mask[1], mask[2], mask[3]);
      setKernelArg(kernel, numArgs + 3, shuff);
      enqueueKernel(commands_, kernel, deviceId_, {idim.n, idim.h},
                    kernelLaunches_);
      continue;
    }

    if (auto *C = dyn_cast<CopyInst>(&I)) {
      Value *dest, *src;
      dest = C->getDest();
      src = C->getSrc();
      if (src == dest) {
        continue;
      }
      size_t destOff = runtimeBundle_.getValueOffset(dest);
      size_t srcOff = runtimeBundle_.getValueOffset(src);
      size_t sizeInBytes = dest->getSizeInBytes();
      cl_event event{nullptr};
      cl_int err = clEnqueueCopyBuffer(commands_, deviceBuffer_, deviceBuffer_,
                                       srcOff, destOff, sizeInBytes, 0, nullptr,
                                       clDoProfile ? &event : nullptr);
      if (clDoProfile) {
        kernelLaunches_.emplace_back(KernelLaunch("copy", event));
      }
      GLOW_ASSERT(err == CL_SUCCESS && "Error in clEnqueueCopyBuffer.");
      continue;
    }

    if (auto *GI = dyn_cast<GatherInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);
      unsigned_t batchDims = GI->getBatchDims();

      auto *data = GI->getData();

      assert(data->getElementType() == ElemKind::FloatTy &&
             "At the moment only floats are supported");

      TypeRef dataType = data->getType();
      size_t numIndices = GI->getIndices()->size();

      // The size of the sample in the batch.
      size_t sliceSize = dataType->getSliceSize(batchDims + 1);
      // The size of the slices that we gather.
      size_t srcSampleSize = dataType->getSliceSize(batchDims);
      // The size of the slices that we pack.
      size_t destSampleSize = numIndices * sliceSize;
      // The size of each sample in the batch.
      size_t numSamples = dataType->size() / srcSampleSize;

      setKernelArg<cl_uint>(kernel, numArgs + 1, numIndices);
      setKernelArg<cl_uint>(kernel, numArgs + 2, sliceSize);

      // Batch arguments:
      setKernelArg<cl_uint>(kernel, numArgs + 3, numSamples);
      setKernelArg<cl_uint>(kernel, numArgs + 4, destSampleSize);
      setKernelArg<cl_uint>(kernel, numArgs + 5, srcSampleSize);

      enqueueKernel(commands_, kernel, deviceId_, {numIndices},
                    kernelLaunches_);
      continue;
    }

    if (auto *SAI = dyn_cast<ScatterAssignInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto *data = SAI->getData();
      size_t dataSliceSize = data->size() / data->dims()[0];
      size_t numIndices = SAI->getIndices()->size();
      setKernelArg<cl_uint>(kernel, numArgs + 1, dataSliceSize);

      enqueueKernel(commands_, kernel, deviceId_, {numIndices},
                    kernelLaunches_);
      continue;
    }

    if (auto *DP = dyn_cast<DebugPrintInst>(&I)) {
      clFinish(commands_);
      auto *V = DP->getSrc();
      // Allocate a temporary tensor to hold the value.
      Tensor T(V->getType());
      // Load the current value of the variable into host memory.
      copyValueFromDevice(V, T.getUnsafePtr());
      clFinish(commands_);
      llvm::outs() << I.getName() << ": ";
      // Dump the content of a value.
      V->dump();
      llvm::outs() << "\n";
      dumpImpl(&T);
      llvm::outs() << "\n";
      llvm::outs().flush();
      continue;
    }

    if (auto *TE = dyn_cast<TraceEventInst>(&I)) {
      llvm::outs() << "skipping TraceEvent on OpenCL: unimplemented\n";
      continue;
    }

    // For TopKInst, we perform the computation on the host side, as sorting on
    // GPU is complex and we may not get too much benefit from it. We copy the
    // tensor from GPU memory to host memory, perform the computation, and then
    // copy the results back to GPU memory.
    if (auto *TK = dyn_cast<TopKInst>(&I)) {
      clFinish(commands_);
      auto *destDev = TK->getValues();
      auto *indDev = TK->getIndices();
      auto *srcDev = TK->getInput();
      Tensor destT(destDev->getType());
      Tensor indT(indDev->getType());
      Tensor srcT(srcDev->getType());
      size_t k = TK->getK();

      copyValueFromDevice(srcDev, srcT.getUnsafePtr());
      clFinish(commands_);

      if (isQuantized) {
        topK<int8_t>(destT, indT, srcT, k);
      } else {
        topK<float>(destT, indT, srcT, k);
      }
      copyValueToDevice(destDev, destT.getUnsafePtr());
      copyValueToDevice(indDev, indT.getUnsafePtr());
      clFinish(commands_);
      continue;
    }

    if (auto PA = dyn_cast<OCLAvgPoolInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto odim = ShapeNCHW(PA->getDest()->getType()->dims());
      auto idim = ShapeNCHW(PA->getSrc()->getType()->dims());
      auto pads = PaddingTLBR(PA->getPads());

      setKernelArg<cl_uint>(kernel, numArgs + 1, PA->getKernel());
      setKernelArg<cl_uint>(kernel, numArgs + 2, PA->getStride());
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, odim);
      setKernelArg(kernel, numArgs + 5, idim);
      if (isQuantized) {
        auto srcTy = PA->getSrc()->getType();
        auto destTy = PA->getDest()->getType();
        auto destScaleParam = quantization::quantizeScaleOffset32To8(
            srcTy->getScale() / destTy->getScale() /
                (PA->getKernel() * PA->getKernel()),
            destTy->getOffset());
        setKernelArg(kernel, numArgs + 6, srcTy->getOffset());
        setKernelArg(kernel, numArgs + 7, destScaleParam);
      }

      enqueueKernel(commands_, kernel, deviceId_, {odim.h, odim.w, odim.c},
                    kernelLaunches_);
      continue;
    }

    if (auto *PM = dyn_cast<OCLMaxPoolInst>(&I)) {
      cl_kernel kernel = createKernel(kernelName);
      setKernelArg(kernel, 0, deviceBuffer_);
      auto numArgs = setKernelArgsForBuffers(kernel, I, 1, runtimeBundle_);

      auto odim = ShapeNCHW(PM->getDest()->getType()->dims());
      auto idim = ShapeNCHW(PM->getSrc()->getType()->dims());
      auto pads = PaddingTLBR(PM->getPads());

      setKernelArg<cl_uint>(kernel, numArgs + 1, PM->getKernel());
      setKernelArg<cl_uint>(kernel, numArgs + 2, PM->getStride());
      setKernelArg(kernel, numArgs + 3, pads);
      setKernelArg(kernel, numArgs + 4, odim);
      setKernelArg(kernel, numArgs + 5, idim);

      enqueueKernel(commands_, kernel, deviceId_, {odim.h, odim.w, odim.c},
                    kernelLaunches_);
      continue;
    }

    llvm::errs() << "Cannot select: " << I.getKindName() << "\n";
    GLOW_UNREACHABLE("compilation failed");
  }

  clFinish(commands_);
}

uint64_t OpenCLFunction::copyValueToDevice(const Value *v, void *buf) {
  uint64_t copiedBytes = 0;
  auto symbolInfo = runtimeBundle_.getSymbolInfo(v);
  size_t sizeInBytes = v->getType()->getSizeInBytes();
  // Issue a non-blocking command to copy the buffer to the device.
  if (sizeInBytes) {
    size_t valueOffset = symbolInfo.offset;
    cl_event event{nullptr};
    cl_int err = clEnqueueWriteBuffer(
        commands_, deviceBuffer_, /* blocking_write */ CL_FALSE, valueOffset,
        sizeInBytes, buf, /* num_events_in_wait_list */ 0,
        /* event_list */ nullptr, /* event */ clDoProfile ? &event : nullptr);
    GLOW_ASSERT(err == CL_SUCCESS && "Unable to copy data to the device");
    if (clDoProfile) {
      kernelLaunches_.emplace_back(KernelLaunch("copyValueToDevice", event));
    }
    copiedBytes += sizeInBytes;
  }
  return copiedBytes;
}

uint64_t OpenCLFunction::copyValueFromDevice(const Value *v, void *buf) {
  uint64_t copiedBytes = 0;
  auto symbolInfo = runtimeBundle_.getSymbolInfo(v);
  size_t sizeInBytes = v->getType()->getSizeInBytes();
  // Issue a non-blocking command to copy the buffer from the device.
  if (sizeInBytes) {
    size_t valueOffset = symbolInfo.offset;
    cl_event event{nullptr};
    cl_int err = clEnqueueReadBuffer(
        commands_, deviceBuffer_, /* blocking_read */ CL_FALSE, valueOffset,
        sizeInBytes, buf, /* num_events_in_wait_list */ 0,
        /* event_list */ nullptr, /* event */ clDoProfile ? &event : nullptr);
    GLOW_ASSERT(err == CL_SUCCESS && "Unable to copy from the device");
    DEBUG_GLOW(llvm::dbgs()
               << "Copied the value from device: " << v->getName() << "\n");
    if (clDoProfile) {
      kernelLaunches_.emplace_back(KernelLaunch("copyValueFromDevice", event));
    }
    copiedBytes += sizeInBytes;
  }
  return copiedBytes;
}

void OpenCLFunction::setupRuns() {
  if (!runsSetup_) {
    deviceBuffer_ = allocDeviceBuffer(runtimeBundle_.getConstantWeightSize() +
                                      runtimeBundle_.getMutableWeightSize() +
                                      runtimeBundle_.getActivationsSize());
    size_t sizeInBytes = runtimeBundle_.getConstantWeightSize();
    if (runtimeBundle_.getConstants()) {
      // Issue a non-blocking command to copy the buffer to the device.
      auto buf = runtimeBundle_.getConstants();
      size_t valueOffset = 0;
      cl_event event{nullptr};
      cl_int err = clEnqueueWriteBuffer(
          commands_, deviceBuffer_, /* blocking_write */ CL_FALSE, valueOffset,
          sizeInBytes, buf, /* num_events_in_wait_list */ 0,
          /* event_list */ nullptr, /* event */ clDoProfile ? &event : nullptr);
      GLOW_ASSERT(err == CL_SUCCESS && "Unable to copy data to the device");
      if (clDoProfile) {
        kernelLaunches_.emplace_back(
            KernelLaunch("copyConstantsToDevice", event));
      }
      // Do it!
      clFinish(commands_);
    }
    runsSetup_ = true;
  }
}

void OpenCLFunction::beforeRun(const PlaceholderBindings &bindings) {
  for (auto PH : bindings.pairs()) {
    auto symbolInfo = runtimeBundle_.getSymbolInfo(PH.first);
    auto addr = symbolInfo.offset;
    auto numBytes = symbolInfo.size;
    // Issue a non-blocking command to copy the buffer to the device.
    auto buf = PH.second->getUnsafePtr();
    cl_event event{nullptr};

    cl_int err = clEnqueueWriteBuffer(
        commands_, deviceBuffer_, /* blocking_write */ CL_FALSE, addr, numBytes,
        buf, /* num_events_in_wait_list */ 0,
        /* event_list */ nullptr, /* event */ clDoProfile ? &event : nullptr);
    GLOW_ASSERT(err == CL_SUCCESS && "Unable to copy data to the device");
    if (clDoProfile) {
      kernelLaunches_.emplace_back(KernelLaunch("copyInputsToDevice", event));
    }
  }
  // Do it!
  clFinish(commands_);
}

void OpenCLFunction::afterRun(const PlaceholderBindings &bindings) {
  for (auto PH : bindings.pairs()) {
    auto symbolInfo = runtimeBundle_.getSymbolInfo(PH.first);
    auto addr = symbolInfo.offset;
    auto numBytes = symbolInfo.size;
    // Issue a non-blocking command to copy the buffer to the device.
    auto buf = PH.second->getUnsafePtr();
    cl_event event{nullptr};

    cl_int err = clEnqueueReadBuffer(
        commands_, deviceBuffer_, /* blocking_read */ CL_FALSE, addr, numBytes,
        buf, /* num_events_in_wait_list */ 0,
        /* event_list */ nullptr, /* event */ clDoProfile ? &event : nullptr);
    GLOW_ASSERT(err == CL_SUCCESS && "Unable to copy data from the device");
    if (clDoProfile) {
      kernelLaunches_.emplace_back(
          KernelLaunch("copyOutputsFromDevice", event));
    }
  }
  // Do it!
  clFinish(commands_);

  // Output profiling information.
  dumpProfileInfo(kernelLaunches_);

  for (auto &kl : kernelLaunches_) {
    clReleaseKernel(kl.kernel_);
  }
  kernelLaunches_.clear();
}

void OpenCLFunction::tearDownRuns() {
  if (deviceBuffer_) {
    freeDeviceBuffer(deviceBuffer_);
    deviceBuffer_ = nullptr;
  }
  runsSetup_ = false;
}

cl_mem OpenCLFunction::allocDeviceBuffer(uint64_t size) {
  const uint64_t alignment = 128;
  // Always allocate buffers properly aligned to hold values of any type.
  size = alignedSize(size, alignment);
  auto buf =
      clCreateBuffer(context_, CL_MEM_READ_WRITE, size, nullptr, nullptr);
  GLOW_ASSERT(buf && "Allocation failed!");
  return buf;
}

void OpenCLFunction::freeDeviceBuffer(cl_mem buf) { clReleaseMemObject(buf); }

void OpenCLFunction::collectConstants(Module *module) {
  runtimeBundle_.collectConstants(module);
}
std::unique_ptr<CompiledFunction>
OCLBackend::compileIR(std::unique_ptr<IRFunction> IR) const {
  auto *module = IR->getGraph()->getParent();
  auto function = compileIRWithoutConstants(std::move(IR));
  auto OCLFunction = static_cast<OpenCLFunction *>(function.get());
  OCLFunction->collectConstants(module);
  return function;
}

std::unique_ptr<CompiledFunction>
OCLBackend::compileIRWithoutConstants(std::unique_ptr<IRFunction> IR) const {
  MemoryAllocator allocator("GPU", 0xFFFFFFFF);
  runtime::RuntimeBundle bundle =
      generateRuntimeBundle(*IR, allocator, allocator, allocator);
  return llvm::make_unique<OpenCLFunction>(std::move(IR), bundle);
}

std::unique_ptr<CompiledFunction>
OCLBackend::compile(Function *F, const CompilationOptions &opts) const {
  if (opts.autoInstrument) {
    GLOW_UNREACHABLE("Instrumentation not supported on this Backend");
  }

  auto IR = generateAndOptimizeIR(F, *this, shouldShareBuffers());

  if (opts.collectConstants) {
    return compileIR(std::move(IR));
  }

  return compileIRWithoutConstants(std::move(IR));
}

bool OCLBackend::isOpSupported(const NodeInfo &NI) const {
  switch (NI.getKind()) {
  case Kinded::Kind::SplatNodeKind:
  case Kinded::Kind::TransposeNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int64ITy});

  case Kinded::Kind::PowNodeKind:
  case Kinded::Kind::BatchedReduceAddNodeKind:
  case Kinded::Kind::TanhNodeKind:
  case Kinded::Kind::SigmoidNodeKind:
  case Kinded::Kind::MaxPoolGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});

  case Kinded::Kind::AddNodeKind:
  case Kinded::Kind::SubNodeKind:
  case Kinded::Kind::MulNodeKind:
  case Kinded::Kind::DivNodeKind:
  case Kinded::Kind::MaxNodeKind:
  case Kinded::Kind::MinNodeKind:
  case Kinded::Kind::MatMulNodeKind:
  case Kinded::Kind::ConcatNodeKind:
  case Kinded::Kind::SliceNodeKind:
  case Kinded::Kind::InsertTensorNodeKind:
  case Kinded::Kind::OCLAvgPoolNodeKind:
  case Kinded::Kind::OCLMaxPoolNodeKind:
    // Note: Pools/Conv support Int8QTy because they're always transformed via
    // the backend to be an OCLPool/OCLConv.
  case Kinded::Kind::AvgPoolNodeKind:
  case Kinded::Kind::MaxPoolNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy});

  case Kinded::Kind::ConvolutionNodeKind:
    if (!NI.getInTy(ConvolutionNode::InputIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});
    }
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy},
                                                  {ConvolutionNode::BiasIdx}) &&
           (NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int32QTy);

  case Kinded::Kind::OCLConvolutionNodeKind:
    if (!NI.getInTy(OCLConvolutionNode::InputIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});
    }
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::Int8QTy}, {OCLConvolutionNode::BiasIdx}) &&
           (NI.getInElemTy(OCLConvolutionNode::BiasIdx) == ElemKind::Int32QTy);

  case Kinded::Kind::TopKNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy}, {},
               {TopKNode::IndicesIdx}) &&
           (NI.getOutElemTy(TopKNode::IndicesIdx) == ElemKind::Int64ITy);

  case Kinded::Kind::BatchedAddNodeKind:
    if (!NI.getInTy(BatchedAddNode::BatchIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});
    }
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy},
                                                  {BatchedAddNode::SliceIdx}) &&
           ((NI.getInElemTy(BatchedAddNode::SliceIdx) == ElemKind::Int8QTy) ||
            (NI.getInElemTy(BatchedAddNode::SliceIdx) == ElemKind::Int32QTy));

  case Kinded::Kind::GatherNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy},
                                                  {GatherNode::IndicesIdx}) &&
           (NI.getInElemTy(GatherNode::IndicesIdx) == ElemKind::Int64ITy);

  case Kinded::Kind::ScatterAssignNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {ScatterAssignNode::IndicesIdx}) &&
           (NI.getInElemTy(ScatterAssignNode::IndicesIdx) ==
            ElemKind::Int64ITy);

  case Kinded::Kind::QuantizeNodeKind:
    return (NI.getInElemTy(QuantizeNode::InputIdx) == ElemKind::FloatTy) &&
           ((NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int8QTy) ||
            (NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int32QTy));

  case Kinded::Kind::DequantizeNodeKind:
    return (NI.getInElemTy(DequantizeNode::InputIdx) == ElemKind::Int8QTy) &&
           (NI.getOutElemTy(DequantizeNode::ResultIdx) == ElemKind::FloatTy);

  case Kinded::Kind::RescaleQuantizedNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy});

  case Kinded::Kind::SelectNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy},
                                                  {SelectNode::CondIdx}) &&
           (NI.getInElemTy(SelectNode::CondIdx) == ElemKind::BoolTy);

  case Kinded::Kind::CmpLTENodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy}, {},
                                                  {CmpLTENode::ResultIdx}) &&
           (NI.getOutElemTy(CmpLTENode::ResultIdx) == ElemKind::BoolTy);

  case Kinded::Kind::SoftMaxNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy},
                                                  {SoftMaxNode::SelectedIdx}) &&
           (NI.getInElemTy(SoftMaxNode::SelectedIdx) == ElemKind::Int64ITy);

  case Kinded::Kind::ConvolutionGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy}, {},
        {ConvolutionGradNode::GradOfInputNamedInputIdx});

  case Kinded::Kind::SoftMaxGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {SoftMaxGradNode::SelectedIdx},
               {SoftMaxGradNode::GradOfInputNamedSelectedIdx}) &&
           (NI.getInElemTy(SoftMaxGradNode::SelectedIdx) == ElemKind::Int64ITy);

  case Kinded::Kind::SaveNodeKind:
  case Kinded::Kind::ReshapeNodeKind:
    // These work regardless of the underlying type.
    return true;

  default:
    return false;
  }
}
