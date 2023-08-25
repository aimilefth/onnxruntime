// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contrib_ops/cuda/bert/paged_attention.h"
#include <dlfcn.h>
#include <algorithm>
#include <cstdint>

#include "contrib_ops/cuda/transformers/dump_cuda_tensor.h"
#include "core/common/common.h"
#include "core/common/status.h"
#include "core/framework/float16.h"
#include "core/providers/cuda/cuda_common.h"
#include "core/providers/cuda/shared_inc/fpgeneric.h"
#include "core/platform/env_var_utils.h"
#include "contrib_ops/cuda/bert/paged_attention_impl.h"
#include "contrib_ops/cuda/bert/bert_padding.h"
#include "contrib_ops/cuda/bert/cutlass_fmha/memory_efficient_attention.h"
#include "driver_types.h"
#include "gsl/narrow"

using namespace onnxruntime::cuda;
using namespace ::onnxruntime::common;
using namespace ONNX_NAMESPACE;

namespace onnxruntime {
namespace contrib {
namespace cuda {

// The three struct is used to represent InputMedata in the python side.
struct AttnBias {
  typedef struct {
    int64_t seqstart;
    int64_t max_seqlen;
    int64_t seqstart_py;
  } block_tables;
  block_tables k_seqinfo;
  block_tables q_seqinfo;
  int64_t batchsize;
};

struct THEvent {
  cudaEvent_t events[64];  // assume we have at most 64 layers.
};

struct InputMetadata {
  int64_t block_tables;
  int64_t max_num_blocks_per_seq;
  int64_t context_lens;
  int64_t max_context_len;
  int64_t num_prompt_tokens;
  int64_t num_valid_tokens;
  int64_t slot_mapping;
  int64_t num_generation_tokens;
  AttnBias attn_bias;
  THEvent cache_events;
  cudaStream_t cache_stream;
};

#define REGISTER_KERNEL_TYPED(T)                                  \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                  \
      PagedAttention,                                             \
      kMSDomain,                                                  \
      1,                                                          \
      T,                                                          \
      kCudaExecutionProvider,                                     \
      (*KernelDefBuilder::Create())                               \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      PagedAttention<T>);

REGISTER_KERNEL_TYPED(float)
REGISTER_KERNEL_TYPED(MLFloat16)

namespace at {
#pragma pack(8)
struct Tensor {
  int dtype_;
  const void* data_;
  std::vector<int64_t> shape_;
};
}  // namespace at

template <typename T>
PagedAttention<T>::PagedAttention(const OpKernelInfo& info) : CudaKernel(info) {
  int64_t num_heads = 0;
  int64_t head_size = 0;
  ORT_ENFORCE(info.GetAttr("num_heads", &num_heads).IsOK() && num_heads > 0);
  ORT_ENFORCE(info.GetAttr("head_size", &head_size).IsOK() && head_size > 0);
  num_heads_ = static_cast<int32_t>(num_heads);
  head_size_ = static_cast<int32_t>(head_size);
  ORT_ENFORCE(info.GetAttr("scale", &scale_).IsOK() && scale_ > 0);
  ORT_ENFORCE(info.GetAttr("mask_type", &mask_type_).IsOK() && (mask_type_ == "normal" || mask_type_ == "alibi" || mask_type_ == "RoPE"));
  const std::string lib_path = ParseEnvironmentVariableWithDefault<std::string>("flash_attention_v2", "/home/jicwen/work/flash-attention/build/Debug/libflashattn.so");

  void* fd = dlopen(lib_path.c_str(), RTLD_LOCAL | RTLD_NOW);

  flash_attention_v2_kernel_ = dlsym(fd, "mha_varlen_fwd_c");
  dlclose(fd);
}

void flash_attention_v2(const cudaDeviceProp& device_prop, cudaStream_t stream,
                        const Tensor* query, const Tensor* key, const Tensor* value,
                        float* work_space,
                        Tensor* output, const InputMetadata* input_metadata,
                        PackedAttentionParameters params, void* flash_attention_v2_kernel) {
  int32_t sm = device_prop.major * 10 + device_prop.minor;
  ORT_ENFORCE(sm >= 70);
  at::Tensor query_tensor = {1, query->DataRaw(), {input_metadata->num_prompt_tokens, params.num_heads, params.head_size}};
  at::Tensor key_tensor = {1, key->DataRaw(), {input_metadata->num_prompt_tokens, params.num_heads, params.head_size}};
  at::Tensor value_tensor = {1, value->DataRaw(), {input_metadata->num_prompt_tokens, params.num_heads, params.head_size}};

  at::Tensor softmax_lse = {2, work_space, {input_metadata->attn_bias.batchsize, params.num_heads, input_metadata->attn_bias.q_seqinfo.max_seqlen}};
  std::vector<int64_t> out_shape = query_tensor.shape_;
  {
  //auto v=output->Shape().GetDims();
  //out_shape.assign(v.begin(), v.end());
  }

  at::Tensor output_tensor = {1, output->MutableDataRaw(), out_shape};
  at::Tensor cu_seqlens_q = {4, reinterpret_cast<int32_t*>(input_metadata->attn_bias.q_seqinfo.seqstart), {input_metadata->attn_bias.batchsize}};
  typedef void mha_varlen_fwd_c(cudaStream_t stream, const at::Tensor& q,  // total_q x num_heads x head_size, total_q := \sum_{i=0}^{b} s_i
                                const at::Tensor& k,                       // total_k x num_heads_k x head_size, total_k := \sum_{i=0}^{b} s_i
                                const at::Tensor& v,                       // total_k x num_heads_k x head_size, total_k := \sum_{i=0}^{b} s_i
                                at::Tensor& softmax_lse,                   // b x num_heads x max_seqlen
                                at::Tensor& out_,                          // total_q x num_heads x head_size, total_k := \sum_{i=0}^{b} s_i
                                const at::Tensor& cu_seqlens_q,            // b+1
                                const at::Tensor& cu_seqlens_k,            // b+1
                                const int max_seqlen_q, const int max_seqlen_k,
                                const float softmax_scale, const bool is_causal);
  mha_varlen_fwd_c* func = reinterpret_cast<mha_varlen_fwd_c*>(flash_attention_v2_kernel);

  (*func)(stream, query_tensor, key_tensor, value_tensor, softmax_lse, output_tensor, cu_seqlens_q, cu_seqlens_q, input_metadata->attn_bias.q_seqinfo.max_seqlen,
          input_metadata->attn_bias.q_seqinfo.max_seqlen, params.scale, true);
}

template <typename T>
void memory_efficient_attn(const cudaDeviceProp& device_prop, cudaStream_t stream,
                           const Tensor* query, const Tensor* key, const Tensor* value,
                           Tensor* output, const InputMetadata* input_metadata,
                           PackedAttentionParameters params) {
  MemoryEfficientAttentionParams attn_param;
  attn_param.sm = device_prop.major * 10 + device_prop.minor;
  attn_param.is_half = sizeof(T) == 2;
  attn_param.batch_size = input_metadata->attn_bias.batchsize;
  attn_param.num_heads = params.num_heads;
  attn_param.sequence_length = input_metadata->attn_bias.q_seqinfo.max_seqlen;
  attn_param.kv_sequence_length = 0;
  attn_param.qk_head_size = params.head_size;
  attn_param.v_head_size = params.head_size;
  attn_param.causal = true;
  attn_param.scale = params.scale;
  attn_param.seqlen_k_ptr = nullptr;
  attn_param.seqstart_q_ptr = reinterpret_cast<int32_t*>(input_metadata->attn_bias.q_seqinfo.seqstart);
  attn_param.seqstart_k_ptr = reinterpret_cast<int32_t*>(input_metadata->attn_bias.q_seqinfo.seqstart);
  attn_param.q_strideB = params.head_size * params.num_heads * input_metadata->num_prompt_tokens;
  attn_param.k_strideB = params.head_size * params.num_heads * input_metadata->num_prompt_tokens;
  attn_param.v_strideB = params.head_size * params.num_heads * input_metadata->num_prompt_tokens;
  attn_param.query = query->DataRaw();
  attn_param.key = key->DataRaw();
  attn_param.value = value->DataRaw();
  attn_param.attn_bias = nullptr;
  attn_param.is_attn_bias_batched = false;
  attn_param.output = output->MutableDataRaw();
  attn_param.workspace = nullptr;
  attn_param.stream = stream;
  run_memory_efficient_attention(attn_param);
}

template <typename T>
Status PagedAttention<T>::CheckInputs(
    const Tensor* query,
    const Tensor* key,
    const Tensor* value,
    const InputMetadata* input_metadata,
    PackedAttentionParameters& parameters) const {
  ORT_UNUSED_PARAMETER(query);
  ORT_UNUSED_PARAMETER(key);
  ORT_UNUSED_PARAMETER(value);
  int64_t num_prompt_tokens = input_metadata->num_prompt_tokens;

  parameters.batch_size = 1;
  parameters.sequence_length = gsl::narrow<int>(num_prompt_tokens);
  parameters.head_size = head_size_;
  parameters.num_heads = num_heads_;
  parameters.scale = scale_;
  return Status::OK();
}


template <typename T>
Status PagedAttention<T>::ComputeInternal(OpKernelContext* context) const {
  const Tensor* query = context->Input<Tensor>(0);
  const Tensor* key = context->Input<Tensor>(1);
  const Tensor* value = context->Input<Tensor>(2);
  const Tensor* key_cache = context->Input<Tensor>(3);
  const Tensor* value_cache = context->Input<Tensor>(4);
  const Tensor* t_input_metadata = context->Input<Tensor>(5);
  const Tensor* positions = context->Input<Tensor>(6);
  const Tensor* cos_sin_cache = context->Input<Tensor>(7);

  InputMetadata* input_metadata = reinterpret_cast<InputMetadata*>(t_input_metadata->Data<int64_t>()[0]);

  TensorShape output_shape = query->Shape();
  Tensor* output = context->Output(0, output_shape);

  ORT_ENFORCE(output_shape[1] == num_heads_ * head_size_, "invlaid query shape");

  const auto& device_prop = GetDeviceProp();
  PackedAttentionParameters parameters;
  ORT_RETURN_IF_ERROR(CheckInputs(query, key, value, input_metadata, parameters));

  if (mask_type_ == "RoPE") {
    ORT_ENFORCE(positions != nullptr, "RoPE mask requires position input");
    ORT_ENFORCE(cos_sin_cache != nullptr, "RoPE mask requires position input");
    int64_t rot_dim = cos_sin_cache->Shape()[1];
    ORT_ENFORCE(rot_dim == head_size_, "RoPE mask requires position input with shape [seq_len, head_size]");
    rotary_embedding_neox(Stream(context), positions->Data<int64_t>(), const_cast<void*>(query->DataRaw()),
                          const_cast<void*>(key->DataRaw()), head_size_, cos_sin_cache->DataRaw(), output_shape[0],
                          rot_dim, num_heads_, num_heads_, 1);
  }

  int64_t num_prompt_tokens = std::min(query->Shape()[0], input_metadata->num_prompt_tokens);
  if (num_prompt_tokens > 0) {
    const bool use_flash_attn_v2 = ParseEnvironmentVariableWithDefault<bool>("use_flash_attn_v2", true);
    if (use_flash_attn_v2 && device_prop.major * 10 + device_prop.minor >= 80) {
      auto workspace = GetScratchBuffer<float>(static_cast<size_t>(
          input_metadata->attn_bias.batchsize * parameters.num_heads * input_metadata->attn_bias.q_seqinfo.max_seqlen),
                                               context->GetComputeStream());

      flash_attention_v2(device_prop, Stream(context),
                         query,
                         key,
                         value,
                         workspace.get(),
                         output,
                         input_metadata,
                         parameters,
                         flash_attention_v2_kernel_);
    } else {
      memory_efficient_attn<MLFloat16>(device_prop, Stream(context),
                                       query,
                                       key,
                                       value,
                                       output,
                                       input_metadata,
                                       parameters);
    }
  }

  auto key_cache_shape = key_cache->Shape();
  int64_t num_valid_tokens = std::min(key->Shape()[0], input_metadata->num_valid_tokens);
  if (num_valid_tokens > 0 && key_cache_shape.Size() > 3) {
    int64_t key_shape_r[3] = {num_valid_tokens, num_heads_, head_size_};
    int64_t value_shape_r[3] = {num_valid_tokens, num_heads_, head_size_};
    int block_size = gsl::narrow<int>(key_cache_shape[3]);
    reshape_and_cache(Stream(context),
                      key->Data<MLFloat16>(),
                      value->Data<MLFloat16>(),
                      key_cache->Data<MLFloat16>(),
                      value_cache->Data<MLFloat16>(),
                      reinterpret_cast<const int32_t*>(input_metadata->slot_mapping),
                      key_shape_r,
                      value_shape_r,
                      block_size,
                      key_cache_shape[4],
                      1);
  }

  if (input_metadata->cache_events.events[0]) {
    CUDA_CALL_THROW(cudaStreamWaitEvent(input_metadata->cache_stream, input_metadata->cache_events.events[0]));
    std::copy(input_metadata->cache_events.events + 1, input_metadata->cache_events.events + 32, input_metadata->cache_events.events);
  }

  if (input_metadata->num_generation_tokens > 0) {
    int64_t generation_qeury_shape[3] = {num_valid_tokens - num_prompt_tokens, num_heads_, head_size_};
    single_query_cached_kv_attention(Stream(context),
                                     output->MutableData<MLFloat16>() + num_prompt_tokens * num_heads_ * head_size_,
                                     query->Data<MLFloat16>() + num_prompt_tokens * num_heads_ * head_size_,
                                     key_cache->Data<MLFloat16>(),
                                     value_cache->Data<MLFloat16>(),
                                     scale_,
                                     reinterpret_cast<const int32_t*>(input_metadata->block_tables),
                                     input_metadata->max_num_blocks_per_seq,
                                     reinterpret_cast<const int32_t*>(input_metadata->context_lens),
                                     value_cache->Shape()[3],
                                     input_metadata->max_context_len,
                                     nullptr,
                                     generation_qeury_shape, 1);
  }
  return Status::OK();
}

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
