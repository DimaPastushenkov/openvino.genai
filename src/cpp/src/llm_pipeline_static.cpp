// Copyright (C) 2024-2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "llm_pipeline_static.hpp"

#include "sampler.hpp"

#include <fstream>
#include <regex>

#include "openvino/pass/stateful_to_stateless.hpp"

// NB: decompose SDPA
#include "openvino/pass/matcher_pass.hpp"
#include "openvino/pass/manager.hpp"
#include "openvino/pass/graph_rewrite.hpp"
#include "openvino/pass/pattern/op/wrap_type.hpp"

#include "openvino/runtime/core.hpp"
#include "openvino/opsets/opset13.hpp"
#include "openvino/core/preprocess/pre_post_process.hpp"
#include "openvino/runtime/properties.hpp"
#include "openvino/runtime/intel_npu/properties.hpp"
#include "openvino/core/parallel.hpp"
#include "openvino/genai/text_streamer.hpp"

#include <jinja2cpp/user_callable.h>


#include "json_utils.hpp"
#include "utils.hpp"

namespace {

namespace opp = ov::pass::pattern;
class TransposeValueTensors : public ov::pass::MatcherPass {
public:
    OPENVINO_MATCHER_PASS_RTTI("TransposeValueTensors");
    struct Context {
        std::vector<std::shared_ptr<ov::opset13::Parameter>> new_params;
        std::vector<std::shared_ptr<ov::opset13::Parameter>> old_params;
        using Ref = std::reference_wrapper<Context>;
    };

    TransposeValueTensors(Context::Ref ctx) {
        auto param = opp::wrap_type<ov::op::v0::Parameter>();
        auto transpose = opp::wrap_type<ov::op::v1::Transpose>({opp::any_input(), opp::any_input()});
        auto concat = opp::wrap_type<ov::op::v0::Concat>({param, transpose});
        auto softmax = opp::wrap_type<ov::op::v8::Softmax>({opp::any_input()});
        auto matmul = opp::wrap_type<ov::op::v0::MatMul>({softmax, concat});

        auto callback = [=](ov::pass::pattern::Matcher& m) {
            auto& node_to_output = m.get_pattern_value_map();

            auto matched_node_param     = node_to_output.at(param).get_node_shared_ptr();
            auto matched_node_concat    = node_to_output.at(concat).get_node_shared_ptr();
            auto matched_node_transpose = node_to_output.at(transpose).get_node_shared_ptr();
            auto matched_node_matmul    = node_to_output.at(matmul).get_node_shared_ptr();

            auto matched_param     = std::static_pointer_cast<ov::op::v0::Parameter>(matched_node_param);
            auto matched_concat    = std::static_pointer_cast<ov::op::v0::Concat>(matched_node_concat);
            auto matched_transpose = std::static_pointer_cast<ov::op::v1::Transpose>(matched_node_transpose);
            auto matched_matmul    = std::static_pointer_cast<ov::op::v0::MatMul>(matched_node_matmul);

            auto shape = matched_param->get_partial_shape();
            OPENVINO_ASSERT(shape.size() == 4u);
            // NB: Transpose Parameter that correspond to V-tensor it will
            // speed-up its multiplication with attention scores
            std::swap(shape[2], shape[3]);
            auto new_param = std::make_shared<ov::opset13::Parameter>(matched_param->get_element_type(), shape);
            new_param->set_friendly_name(matched_param->get_friendly_name());
            new_param->outputs().begin()->get_tensor().set_names(matched_param->outputs().begin()->get_tensor().get_names());
            ov::replace_node(matched_param, new_param);
            // NB: Save in order to add/remove to the model later on
            ctx.get().new_params.push_back(new_param);
            ctx.get().old_params.push_back(matched_param);

            auto order_cst = ov::op::v0::Constant::create(ov::element::i32, ov::Shape{4}, {0, 2, 3, 1});
            auto new_transpose = std::make_shared<ov::opset13::Transpose>(matched_transpose->input_value(0),
                                                                          order_cst->output(0));
            new_transpose->set_friendly_name(matched_transpose->get_friendly_name());
            ov::replace_node(matched_transpose, new_transpose);

            auto new_concat = std::make_shared<ov::opset13::Concat>(
                ov::OutputVector{new_param->output(0), new_transpose->output(0)}, 3u
            );
            new_concat->set_friendly_name(matched_concat->get_friendly_name());
            ov::replace_node(matched_concat, new_concat);

            matched_matmul->set_transpose_b(true);

            return true;
        };
        register_matcher(std::make_shared<opp::Matcher>(matmul, "TransposeValueTensors"), std::move(callback));
    }
};

class ScaledDotProductAttentionDecomposition : public ov::pass::MatcherPass {
public:
    OPENVINO_MATCHER_PASS_RTTI("ScaledDotProductAttentionDecomposition");
    ScaledDotProductAttentionDecomposition() {
        auto pattern_node = ov::pass::pattern::wrap_type<ov::op::v13::ScaledDotProductAttention>();

        ov::matcher_pass_callback callback = [=](ov::pass::pattern::Matcher& m) {
            auto& pattern_to_output = m.get_pattern_value_map();
            auto node = ov::as_type_ptr<ov::op::v13::ScaledDotProductAttention>(
                    pattern_to_output.at(pattern_node).get_node_shared_ptr());

            if (node == nullptr || transformation_callback(node)) {
                return false;
            }

            auto new_output_node = decompose(node);
            ov::replace_node(node, new_output_node);
            return true;
        };

        auto m = std::make_shared<ov::pass::pattern::Matcher>(pattern_node, "ScaledDotProductAttentionDecomposition");
        register_matcher(m, std::move(callback));
    }
    std::shared_ptr<ov::Node> decompose(std::shared_ptr<ov::op::v13::ScaledDotProductAttention> node) {
        using namespace ov::op;
        using namespace ov;
        auto query = node->input_value(0);
        auto key = node->input_value(1);
        auto value = node->input_value(2);
        auto q_shape = register_new_node<v3::ShapeOf>(query, element::i32);
        auto k_shape = register_new_node<v3::ShapeOf>(key, element::i32);
        auto minus_one = register_new_node(v0::Constant::create(element::i32, Shape{}, {-1}));
        auto minus_two = register_new_node(v0::Constant::create(element::i32, Shape{}, {-2}));
        auto zero_i = register_new_node(v0::Constant::create(element::i32, Shape{}, {0}));
        auto one_i = register_new_node(v0::Constant::create(element::i32, Shape{}, {1}));
        auto one_f = register_new_node<v1::ConvertLike>(one_i, query);
        auto zero_f = register_new_node<v1::ConvertLike>(zero_i, query);

        Output<Node> scale;
        if (node->get_input_size() < 5) {
            scale = register_new_node<v8::Gather>(q_shape, minus_one, zero_i)->output(0);
            scale = register_new_node<v1::ConvertLike>(scale, query);
            auto sqrt_scale = register_new_node<v0::Sqrt>(scale);
            scale = register_new_node<v1::Divide>(one_f, sqrt_scale);
        } else {
            scale = node->input_value(4);
        }

        auto q_scaled = register_new_node<v1::Multiply>(query, scale);
        auto k_rank = register_new_node<v3::ShapeOf>(k_shape, element::i32)->output(0);
        auto k_last_dim = register_new_node<v1::Add>(k_rank, minus_one);
        auto k_next_dim = register_new_node<v1::Add>(k_rank, minus_two)->output(0);
        k_rank = register_new_node<v0::Squeeze>(k_rank, zero_i);
        auto minus_inf =
            register_new_node(v0::Constant::create(element::f32, Shape{}, {-std::numeric_limits<float>::infinity()}))
            ->output(0);
        auto keep_dim_last = register_new_node<v0::Squeeze>(k_next_dim, zero_i);
        auto k_dims_before_transpose = register_new_node<v4::Range>(zero_i, keep_dim_last, one_i, element::i32);

        auto scaled_atten = register_new_node<v0::MatMul>(q_scaled, key, false, true)->output(0);
        minus_inf = register_new_node<v1::ConvertLike>(minus_inf, scaled_atten);

        if (node->get_causal() || node->get_input_size() > 3) {
            Output<Node> mask;
            Output<Node> atten_mask;
            if (!node->get_causal()) {
                mask = node->input_value(3);

                // two types of masks are supported. A boolean mask where a value of True indicates that the element should
                // take part in attention. A float mask of the same type as query, key, value that is added to the attention
                // score.
                if (mask.get_element_type() == element::boolean) {
                    atten_mask = register_new_node<v1::ConvertLike>(mask, scaled_atten);
                    auto inv_mask = register_new_node<v1::LogicalNot>(mask);
                    atten_mask = register_new_node<v1::Select>(inv_mask, atten_mask, minus_inf);
                } else {
                    atten_mask = mask;
                }
            } else {
                auto target_s_len = register_new_node<v8::Gather>(q_shape, minus_two, zero_i);
                auto source_s_len = register_new_node<v8::Gather>(k_shape, minus_two, zero_i);
                auto ssl = register_new_node<v0::Unsqueeze>(source_s_len, zero_i);
                auto tsl = register_new_node<v0::Unsqueeze>(target_s_len, zero_i);
                auto mask_shape = register_new_node<v0::Concat>(OutputVector{tsl, ssl}, 0);
                mask = register_new_node<v1::Broadcast>(minus_inf, mask_shape);
                auto horizontal_range = register_new_node<v4::Range>(zero_i, source_s_len, one_i, element::i32)->output(0);
                horizontal_range = register_new_node<v0::Unsqueeze>(horizontal_range, zero_i);
                auto stop = register_new_node<v1::Add>(target_s_len, one_i);
                auto vertical_range = register_new_node<v4::Range>(one_i, stop, one_i, element::i32)->output(0);
                vertical_range = register_new_node<v0::Unsqueeze>(vertical_range, one_i);
                auto triu = register_new_node<v1::GreaterEqual>(horizontal_range, vertical_range);
                atten_mask = register_new_node<v1::Select>(triu, mask, zero_f);
            }
            scaled_atten = register_new_node<v1::Add>(scaled_atten, atten_mask);
        }

        scaled_atten = register_new_node<v8::Softmax>(scaled_atten, -1);
        auto result = register_new_node<v0::MatMul>(scaled_atten, value);
        result->set_friendly_name(node->get_friendly_name());
        copy_runtime_info(node, get_new_nodes());
        return result;
    }
};

std::shared_ptr<ov::Model> cvt_value_tensors_layout(std::shared_ptr<ov::Model> model) {
    ov::preprocess::PrePostProcessor ppp(model);
    for (auto tensor : model->outputs()) {
        if (tensor.get_any_name().find("value") != std::string::npos) {
            // NB: [batch, num_heads, seq_len, emb_size] -> [batch, num_heads, emb_size, seq_len]
            ppp.output(tensor.get_any_name()).model().set_layout(ov::Layout("BHSE"));
            ppp.output(tensor.get_any_name()).tensor().set_layout(ov::Layout("BHES"));
        }
    }
    return ppp.build();
}

bool optimize_value_tensors(std::shared_ptr<ov::Model> model) {
    ov::pass::GraphRewrite rewr;
    rewr.add_matcher<ScaledDotProductAttentionDecomposition>();
    TransposeValueTensors::Context ctx;
    rewr.add_matcher<TransposeValueTensors>(std::ref(ctx));
    rewr.run_on_model(model);

    model->add_parameters(ctx.new_params);
    for (auto old_param : ctx.old_params) {
        model->remove_parameter(old_param);
    }
    ov::pass::Validate().run_on_model(model);

    // NB: if new_params is not empty - pass has been applied
    return !ctx.new_params.empty();
}

uint32_t align_to(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

enum class GenerateHint {
    FAST_COMPILE,
    BEST_PERF
};

std::string to_string(GenerateHint h) {
    switch(h) {
        case GenerateHint::FAST_COMPILE :
            return "FAST_COMPILE";
        case GenerateHint::BEST_PERF :
            return "BEST_PERF";
        default:
            OPENVINO_THROW("Unsupported value for type GenerateHint provided");
    }
}

GenerateHint str_to_hint(const std::string& str) {
    if (str == to_string(GenerateHint::FAST_COMPILE)) {
        return GenerateHint::FAST_COMPILE;
    }
    if (str == to_string(GenerateHint::BEST_PERF)) {
        return GenerateHint::BEST_PERF;
    }
    OPENVINO_THROW("Unsupported \"GENERATE_HINT\" provided: " +
                   str + ". Please select either \"" + to_string(GenerateHint::BEST_PERF) + "\" or \"" + to_string(GenerateHint::FAST_COMPILE) +"\".");
}

std::shared_ptr<ov::Model> cvt_kvcache_to_fp16(const std::shared_ptr<ov::Model>& model) {
    ov::preprocess::PrePostProcessor ppp(model);

    for (auto tensor : model->inputs()) {
        if (tensor.get_any_name().find("past_key") != std::string::npos) {
            ppp.input(tensor.get_any_name()).tensor().set_element_type(ov::element::Type_t::f16);
        }
    }

    for (auto tensor : model->outputs()) {
        if (tensor.get_any_name().find("present") != std::string::npos) {
            ppp.output(tensor.get_any_name()).tensor().set_element_type(ov::element::Type_t::f16);
        }
    }

    return ppp.build();
}

void align_u4_zp_constants(const std::shared_ptr<ov::Model>& model) {
    for (auto op : model->get_ops()) {
        if (ov::op::util::is_constant(op)) {
            auto cst_op = std::dynamic_pointer_cast<ov::op::v0::Constant>(op);
            const auto cst_op_out = cst_op->output(0);
            if (cst_op_out.get_element_type() == ov::element::u4 && ov::shape_size(cst_op_out.get_shape()) == 1u) {
                ov::Tensor cst_tensor(ov::element::u4, cst_op_out.get_shape());
                *static_cast<uint8_t*>(cst_tensor.data()) = cst_op->get_vector<uint8_t>()[0] & 0x0f;
                auto new_cst_op = std::make_shared<ov::op::v0::Constant>(cst_tensor);
                for (auto target_input : cst_op_out.get_target_inputs()) {
                    target_input.replace_source_output(new_cst_op);
                }
            }
        }
    }
}

bool is_cw_compressed(const std::shared_ptr<ov::Model>& model) {
    std::vector<std::string> rt_info_path = {"nncf", "weight_compression", "group_size"};
    if (!model->has_rt_info(rt_info_path)) {
        // NB: Model isn't compressed by NNCF - skip
        return false;
    }
    auto group_size = model->get_rt_info<int>(rt_info_path);
    if (group_size == -1) {
        // NB: Enable DQ for CW quantized models
        return true;
    }
    return false;
}

std::optional<ov::Any> pop_option(ov::AnyMap& config, const std::string& option_name) {
    if (auto it = config.find(option_name); it != config.end()) {
        std::optional<ov::Any> found = std::make_optional(it->second);
        config.erase(it);
        return found;
    }
    return std::nullopt;
}

template <typename T>
std::optional<T> get_option(const ov::AnyMap& config, const std::string& option_name) {
    if (auto it = config.find(option_name); it != config.end()) {
        return std::make_optional(it->second.as<T>());
    }
    return std::nullopt;
}

std::shared_ptr<ov::Model> redirect_new_kv_to_output(const std::shared_ptr<ov::Model>& model) {
    const auto kStartOutputKVCacheLayers = 1u;
    for (int i = kStartOutputKVCacheLayers; i < model->outputs().size(); ++i) {
        auto kvout  = model->output(i);
        auto kvrslt = kvout.get_node();
        auto kvcat  = kvrslt->inputs()[0].get_source_output().get_node();
        auto kvval  = kvcat->inputs()[1].get_source_output();
        kvval.set_names({kvout.get_any_name()});
        kvrslt->inputs()[0].replace_source_output(kvval);
    }
    model->validate_nodes_and_infer_types();
    return model;
}

std::shared_ptr<ov::Model> add_slices_to_kvcache_inputs(const std::shared_ptr<ov::Model>& model) {
    const auto kvcache_name_pattern = "past_key_values";
    std::vector<std::shared_ptr<ov::opset13::Parameter>> new_params;
    for (auto param : model->get_parameters()) {
        auto tensor_name = param->get_output_tensor(0).get_any_name();
        if (tensor_name.find(kvcache_name_pattern) == std::string::npos) {
            new_params.push_back(param);
            continue;
        }
        auto shape = param->get_output_shape(0);
        shape[2] += 1;

        auto new_param = std::make_shared<ov::opset13::Parameter>(param->get_element_type(), shape);
        new_param->set_friendly_name(tensor_name);
        new_param->outputs().begin()->get_tensor().set_names(param->outputs().begin()->get_tensor().get_names());

        auto slice_start = std::make_shared<ov::opset13::Constant>(
            ov::element::Type_t::i32, ov::Shape{1}, std::vector<int32_t>{1}
        );
        auto slice_stop = std::make_shared<ov::opset13::Constant>(
            ov::element::Type_t::i32, ov::Shape{1}, std::vector<int32_t>{static_cast<int32_t>(shape[2])}
        );
        auto slice_step = std::make_shared<ov::opset13::Constant>(
            ov::element::Type_t::i32, ov::Shape{1}, std::vector<int32_t>{1}
        );
        auto slice_axes = std::make_shared<ov::opset13::Constant>(
            ov::element::Type_t::i32, ov::Shape{1}, std::vector<int32_t>{2}
        );
        auto slice_node = std::make_shared<ov::opset13::Slice>(
            new_param, slice_start->output(0), slice_stop->output(0), slice_step->output(0), slice_axes->output(0)
        );
        slice_node->set_friendly_name(tensor_name + "_Slice");
        for (auto target_input : param->output(0).get_target_inputs()) {
            target_input.replace_source_output(slice_node->output(0));
        }
        new_params.push_back(new_param);
    }
    return std::make_shared<ov::Model>(model->get_results(), ov::SinkVector{}, new_params);
}

struct KVAxesPosition {
    uint32_t batch;
    uint32_t seq_len;
};

void reshape_to_static(std::shared_ptr<ov::Model> model,
                       const uint32_t input_size,
                       const uint32_t kvcache_size,
                       const KVAxesPosition& kv_axes_position) {
    std::map<std::string, ov::PartialShape> new_shapes;
    for (auto input : model->inputs()) {
        const auto& input_name = input.get_any_name();
        ov::PartialShape new_shape;
        if (input_name.find("input_ids") != std::string::npos) {
            new_shape = ov::PartialShape({1, input_size});
        } else if (input_name.find("attention_mask") != std::string::npos) {
            new_shape = ov::PartialShape({1, kvcache_size});
        } else if (input_name.find("position_ids") != std::string::npos) {
            new_shape = ov::PartialShape({1, input_size});
        } else {
            const auto& partial_shape = input.get_partial_shape();
            new_shape = partial_shape;
            new_shape[kv_axes_position.batch] = 1;
            new_shape[kv_axes_position.seq_len] = kvcache_size - input_size;
        }
        new_shapes.emplace(input_name, new_shape);
    }
    model->reshape(new_shapes);
}

template <typename T>
void fill_tensor(ov::Tensor tensor, T fill_val, size_t offset = 0u) {
    T* tensor_data = tensor.data<T>();
    std::fill(tensor_data + offset, tensor_data + tensor.get_size(), fill_val);
}

void copy_with_offset(const ov::Tensor& orig, const std::size_t offset, ov::Tensor& padded) {
    int64_t* orig_data = orig.data<int64_t>();
    int64_t* padded_data = padded.data<int64_t>();
    std::copy(orig_data, orig_data + orig.get_size(), padded_data + offset);
}

void merge_config_with(ov::AnyMap& lhs, const ov::AnyMap& rhs) {
    for (const auto& [key, value] : rhs) {
        // NB: Overwrite the value if key already exists
        if (auto it = lhs.find(key); it != lhs.end()) {
            it->second = value;
        } else {
            lhs.emplace(key, value);
        }
    }
}

struct NPUDesc {
    std::string arch;
    int64_t max_tiles;
    bool compiler_dq;
};

std::optional<NPUDesc> extract_npu_descriptor(ov::Core& core) {
    const auto all_devices = core.get_available_devices();
    if (std::find(all_devices.begin(), all_devices.end(), "NPU") == all_devices.end()) {
        return std::nullopt;
    }
    const auto arch = core.get_property("NPU", ov::device::architecture);
    const auto max_tiles = core.get_property("NPU", ov::intel_npu::max_tiles);
    bool compiler_dq = false;
    const auto supported_properties = core.get_property("NPU", ov::supported_properties);
    if (std::find(supported_properties.begin(), supported_properties.end(),
                  "NPU_COMPILER_DYNAMIC_QUANTIZATION") != supported_properties.end()) {
        compiler_dq = true;
    }
    return std::make_optional(NPUDesc{arch, max_tiles, compiler_dq});
}

ov::AnyMap get_baseline_common_config(const std::optional<NPUDesc>& npudesc) {
    ov::AnyMap config = {
        { "NPU_COMPILATION_MODE_PARAMS", "compute-layers-with-higher-precision=Sqrt,Power,ReduceMean,Add_RMSNorm" },
        { "NPUW_DEVICES", "NPU" },
        { "NPU_USE_NPUW",  "YES" },
        { "NPUW_FOLD", "YES" },
        { "NPUW_DCOFF_TYPE", "f16" },
        { "NPUW_DCOFF_SCALE", "YES"},
        { "NPUW_WEIGHTS_BANK", "shared" },
        { "NPUW_SLICE_OUT", "YES" },
        { "NPUW_FUNCALL_ASYNC", "YES" }
    };
    // FIXME: this config logic is getting more and more complex
    if (npudesc.has_value() && npudesc->compiler_dq) {
        config.emplace("NPUW_DQ", "YES");
        config.emplace("NPUW_DQ_FULL", "NO");
        config.emplace("NPU_COMPILER_DYNAMIC_QUANTIZATION", "YES");
        config.erase("NPUW_DCOFF_TYPE");
        config.erase("NPUW_DCOFF_SCALE");
    }
    return config;
}

ov::AnyMap get_default_common_config(const std::shared_ptr<ov::Model>& model,
                                     const std::optional<NPUDesc>& npudesc) {
    auto config = get_baseline_common_config(npudesc);
    const char* npu_l0 = std::getenv("DISABLE_OPENVINO_GENAI_NPU_L0");
    if (npu_l0 && std::atoi(npu_l0) == 1) {
        config.emplace("NPUW_WEIGHTS_BANK_ALLOC", "CPU");
    } else {
        config.emplace("NPUW_FUNCALL_FOR_ALL", "YES");
    }
    return config;
}

ov::AnyMap get_default_prefill_config(const std::shared_ptr<ov::Model>& model,
                                      const std::optional<NPUDesc>& npudesc) {
    auto config = get_default_common_config(model, npudesc);
    if (npudesc.has_value() &&
        npudesc->arch == "4000" &&
        npudesc->max_tiles != -1) {
        config.emplace("NPU_DPU_GROUPS", npudesc->max_tiles);
    }
    // Specify NPUW DQ if Compiler DQ is not enabled
    if (!npudesc.has_value() || !npudesc->compiler_dq) {
        if (is_cw_compressed(model)) {
            config.emplace("NPUW_DQ", "YES");
        } else {
            config.emplace("NPUW_PMM", "NO");
        }
    }
    return config;
}

ov::AnyMap get_default_generate_config(const std::shared_ptr<ov::Model>& model,
                                       const std::optional<NPUDesc>& npudesc,
                                       const GenerateHint hint) {
    auto config = get_default_common_config(model, npudesc);
    if (hint == GenerateHint::BEST_PERF) {
        config.emplace("NPUW_ONLINE_PIPELINE", "NONE");
    }
    if (npudesc.has_value() && npudesc->arch == "4000") {
        config.emplace("NPU_DPU_GROUPS", 4);
    }
    if (hint == GenerateHint::FAST_COMPILE) {
        config.emplace("NPUW_UNFOLD_IREQS", "YES");
    }
    // Specify NPUW DQ if Compiler DQ is not enabled
    if (!npudesc.has_value() || !npudesc->compiler_dq) {
        config.emplace("NPUW_DQ", "YES");
    }
    return config;
}

template <typename T>
T pop_or_default(ov::AnyMap& config, const std::string& key, const T& default_value) {
    auto anyopt = pop_option(config, key);
    if (anyopt.has_value()) {
        if (anyopt.value().empty()) {
            if (ov::genai::utils::is_container<T>)
                return T{};
            else {
                OPENVINO_THROW("Got empty ov::Any for key: " + key);
            }
        }
        return anyopt.value().as<T>();
    }
    return default_value;
}

std::optional<uint32_t> pop_int_and_cast(ov::AnyMap& config, const std::string& key) {
    auto anyopt = pop_option(config, key);
    if (anyopt.has_value()) {
        const auto any = anyopt.value();
        int64_t value;
        // NB: Integer value coming from python has int64_t datatype
        if (any.is<int64_t>()) {
            value = any.as<int64_t>();
        } else if (any.is<int>()) {
            value = any.as<int>();
        } else {
            OPENVINO_THROW("Failed to extract " + key + ". Type mismatch: expected types: int or int64_t");
        }
        if (value < 0) {
            OPENVINO_THROW(key + " cannot be negative!");
        }
        return std::make_optional(static_cast<uint32_t>(value));
    }
    return std::nullopt;
}

void update_config(ov::AnyMap& config, const std::pair<std::string, ov::Any>& pair) {
    if (config.count(pair.first) == 0) {
        config.insert(pair);
    }
}

void rename_key(ov::AnyMap& config, const std::string& old_key, const std::string& new_key) {
    if (config.count(old_key) != 0) {
        auto opt_value = pop_option(config, old_key);
        config[new_key] = opt_value.value();
    }
}

ov::Tensor make_tensor_slice(ov::Tensor tensor, size_t dim, size_t start_pos, size_t end_pos) {
    ov::Shape start_shape(std::vector<size_t>(tensor.get_shape().size(), 0u));
    start_shape[dim] = start_pos;
    ov::Shape end_shape = tensor.get_shape();
    end_shape[dim] = end_pos;
    return ov::Tensor(tensor, start_shape, end_shape);
}

void set_npuw_cache_dir(ov::AnyMap& config) {
    std::optional<std::string> cache_dir = get_option<std::string>(config, "CACHE_DIR");
    if (config.count("NPU_USE_NPUW") != 0u && cache_dir) {
        config.emplace("NPUW_CACHE_DIR", cache_dir.value());
        pop_option(config, "CACHE_DIR");
    }
}

void copy_columns_by_row_chunks(const ov::Tensor& src, ov::Tensor& dst) {
    const auto src_shape = src.get_shape();

    OPENVINO_ASSERT(src_shape.size() == 4u);
    OPENVINO_ASSERT(src_shape == dst.get_shape());
    OPENVINO_ASSERT(src.get_byte_size() == dst.get_byte_size());

    const auto src_strides = src.get_strides();
    const auto dst_strides = dst.get_strides();
    const auto elem_size   = src.get_byte_size() / src.get_size();

    const auto C = src_shape[1];
    const auto H = src_shape[2];
    const auto W = src_shape[3];

    const auto IS_H = src_strides[2];
    const auto OS_H = dst_strides[2];

    const size_t chunk_byte_size = W * elem_size;

    const auto* src_p  = static_cast<uint8_t*>(src.data());
          auto* dst_p  = static_cast<uint8_t*>(dst.data());

    for (size_t i = 0; i < C*H; ++i) {
        const size_t src_offset = i * IS_H;
        const size_t dst_offset = i * OS_H;
        std::copy_n(src_p + src_offset, chunk_byte_size, dst_p + dst_offset);
    }
}

void stream_generated_tokens(std::shared_ptr<ov::genai::StreamerBase> streamer_ptr,
                             ov::genai::GenerationHandle& handle) {
    if (streamer_ptr && handle->can_read()) {
        std::unordered_map<uint64_t, ov::genai::GenerationOutput> token = handle->read();
        for (const auto& gen_token : token.begin()->second.generated_ids) {
            auto streaming_status = streamer_ptr->write(gen_token);
            if (streaming_status != ov::genai::StreamingStatus::RUNNING) {
                streaming_status == ov::genai::StreamingStatus::CANCEL ? handle->cancel() : handle->stop();
                break;
            }
        }
    }
}

enum StaticPipelineKind {
    STATEFUL,
    STATELESS
};

StaticPipelineKind str_to_pipeline(const std::string& str) {
    if (str == "STATEFUL") {
        return StaticPipelineKind::STATEFUL;
    }
    OPENVINO_THROW("Unsupported \"PIPELINE\" provided: ",
                   str, ". Please select \"STATEFUL\".");
}
} // anonymous namespace

namespace ov {
namespace genai {
namespace static_llm {

StatefulLLMPipeline::StatefulLLMPipeline(
    const std::filesystem::path& models_path,
    const ov::genai::Tokenizer& tokenizer,
    const std::string& device,
    const ov::AnyMap& config
) : LLMPipelineImplBase(tokenizer,
                        utils::from_config_json_if_exists(models_path)),
    m_sampler(m_tokenizer) {
    ov::AnyMap properties = config;

    auto blob_path = pop_or_default(properties, "BLOB_PATH", std::string{});
    const auto export_blob = pop_or_default(properties, "EXPORT_BLOB", false);

    bool do_import = (!blob_path.empty() && !export_blob);

    if (do_import) {
        if (!std::filesystem::exists(blob_path)) {
            OPENVINO_THROW("Blob file is not found at: " + blob_path);
        }
        std::ifstream fin(blob_path, std::ios::in | std::ios::binary);
        if (!fin.is_open()) {
            OPENVINO_THROW("Blob file can't be opened: " + blob_path);
        }
        auto compiled = genai::utils::singleton_core().import_model(fin, device, config);
        m_max_prompt_len = compiled.get_property("NPUW_LLM_MAX_PROMPT_LEN").as<uint32_t>();
        auto min_resp_len = compiled.get_property("NPUW_LLM_MIN_RESPONSE_LEN").as<uint32_t>();
        m_kvcache_total = m_max_prompt_len + min_resp_len;
        m_request = compiled.create_infer_request();
    } else {
        ov::AnyMap properties = config;
        std::shared_ptr<ov::CompiledModel> compiled;
        // CACHE_DIR + weightless flow support
        auto cache_mode = get_option<CacheMode>(config, "CACHE_MODE");
        if (cache_mode.has_value() && *cache_mode == CacheMode::OPTIMIZE_SPEED) {
            auto model = genai::utils::singleton_core().read_model(models_path / "openvino_model.xml", {}, config);
            compiled = setupAndCompileModel(model, properties);
        } else {
            compiled = setupAndCompileModel(models_path / "openvino_model.xml", properties);
        }
        // Also export compiled model if required
        if (export_blob) {
            if (blob_path.empty()) {
                blob_path = (models_path / "openvino_model.blob").string();
            }
            // Check the path is full
            const int EXT_SIZE = 5; // ".blob"
            if (blob_path.size() < EXT_SIZE) {
                OPENVINO_THROW("Please provide a full path to blob file in BLOB_PATH: " + blob_path);
            }
            if (strncmp(".blob", &blob_path[blob_path.size() - EXT_SIZE], EXT_SIZE) != 0) {
                OPENVINO_THROW("Please provide a full path to blob file in BLOB_PATH: " + blob_path);
            }
            std::ofstream fout(blob_path, std::ios::out | std::ios::binary);
            if (!fout.is_open()) {
                OPENVINO_THROW("Blob file can't be exported to: " + blob_path);
            }
            compiled->export_model(fout);
        }
        m_request = compiled->create_infer_request();
        m_sampler.set_seed(m_generation_config.rng_seed);
    }
}


StatefulLLMPipeline::StatefulLLMPipeline(
    const std::shared_ptr<ov::Model>& model,
    const ov::genai::Tokenizer& tokenizer,
    const std::string&,
    const ov::AnyMap& properties,
    const ov::genai::GenerationConfig& generation_config
) : LLMPipelineImplBase(tokenizer, generation_config),
    m_sampler(m_tokenizer) {
    ov::AnyMap properties_copy = properties;
    auto compiled = setupAndCompileModel(model, properties_copy);
    m_request = compiled->create_infer_request();
    m_sampler.set_seed(m_generation_config.rng_seed);
}

void StatefulLLMPipeline::updateStatefulConfig(ov::AnyMap& pipeline_config,
                                               const ov::genai::utils::KVAxesPosition& kv_pos) {
    const uint32_t kMaxPromptLen = pop_int_and_cast(pipeline_config, "MAX_PROMPT_LEN").value_or(1024u);
    const uint32_t kMinResponseLen = pop_int_and_cast(pipeline_config, "MIN_RESPONSE_LEN").value_or(128u);
    m_max_prompt_len = kMaxPromptLen;
    m_kvcache_total = kMaxPromptLen + kMinResponseLen;

    update_config(pipeline_config, {"NPU_USE_NPUW", "YES"});
    update_config(pipeline_config, {"NPUW_LLM", "YES"});

    update_config(pipeline_config, {"NPUW_LLM_BATCH_DIM", kv_pos.batch});
    update_config(pipeline_config, {"NPUW_LLM_SEQ_LEN_DIM", kv_pos.seq_len});

    update_config(pipeline_config, {"NPUW_LLM_MAX_PROMPT_LEN", kMaxPromptLen});
    update_config(pipeline_config, {"NPUW_LLM_MIN_RESPONSE_LEN", kMinResponseLen});

    rename_key(pipeline_config, "++PREFILL_CONFIG", "++NPUW_LLM_PREFILL_CONFIG");
    rename_key(pipeline_config, "++GENERATE_CONFIG", "++NPUW_LLM_GENERATE_CONFIG");
    rename_key(pipeline_config, "PREFILL_CONFIG", "NPUW_LLM_PREFILL_CONFIG");
    rename_key(pipeline_config, "GENERATE_CONFIG", "NPUW_LLM_GENERATE_CONFIG");
    rename_key(pipeline_config, "GENERATE_HINT", "NPUW_LLM_GENERATE_HINT");
}

std::shared_ptr<ov::CompiledModel> StatefulLLMPipeline::setupAndCompileModel(
    const std::shared_ptr<ov::Model>& model,
    ov::AnyMap& pipeline_config) {
    auto kv_pos = ov::genai::utils::get_kv_axes_pos(model);
    updateStatefulConfig(pipeline_config, kv_pos);
    return std::make_shared<ov::CompiledModel>(genai::utils::singleton_core().compile_model(model, "NPU", pipeline_config));
}

std::shared_ptr<ov::CompiledModel> StatefulLLMPipeline::setupAndCompileModel(
    const std::filesystem::path& model_path,
    ov::AnyMap& pipeline_config) {
    auto kv_pos = ov::genai::utils::get_kv_axes_pos(
        // NB: Only read model to identify seq_len and batch axes
        genai::utils::singleton_core().read_model(model_path)
    );
    updateStatefulConfig(pipeline_config, kv_pos);
    return std::make_shared<ov::CompiledModel>(genai::utils::singleton_core().compile_model(model_path, "NPU", pipeline_config));
}

DecodedResults StatefulLLMPipeline::generate(
    StringInputs inputs,
    OptionalGenerationConfig generation_config,
    StreamerVariant streamer
) {
    auto start_time = std::chrono::steady_clock::now();

    GenerationConfig config = (generation_config.has_value()) ? *generation_config : m_generation_config;
    std::string prompt;
    if (auto input_vector = std::get_if<std::vector<std::string>>(&inputs)) {
        OPENVINO_ASSERT(input_vector->size() == 1u, "Currently only batch size=1 is supported");
        prompt = std::move(input_vector->front());
    } else {
        OPENVINO_ASSERT(std::holds_alternative<std::string>(inputs));
        prompt = std::get<std::string>(inputs);
    }

    ov::genai::TokenizedInputs tokenized_input;
    if (m_is_chat_conversation) {
        m_history.push_back({{"role", "user"}, {"content", prompt}});
        constexpr bool add_generation_prompt = true;
        prompt = m_tokenizer.apply_chat_template(m_history, add_generation_prompt);
        // for chat ov::genai::add_special_tokens(false) is aligned with stateful pipeline and HF
        tokenized_input = m_tokenizer.encode(prompt, ov::genai::add_special_tokens(false));
    } else {
        if (config.apply_chat_template && !m_tokenizer.get_chat_template().empty()) {
            ChatHistory history({{{"role", "user"}, {"content", prompt}}});
            constexpr bool add_generation_prompt = true;
            auto templated_prompt = m_tokenizer.apply_chat_template(history, add_generation_prompt);
            tokenized_input = m_tokenizer.encode(templated_prompt, ov::genai::add_special_tokens(false));
        } else {
            // in case when chat_template was not found in tokenizer_config.json or set
            tokenized_input = m_tokenizer.encode(prompt, ov::genai::add_special_tokens(true));
        }
    }

    auto encode_stop_time =  std::chrono::steady_clock::now();
    auto encoded_results = generate(tokenized_input, config, streamer);

    auto decode_start_time =  std::chrono::steady_clock::now();
    DecodedResults decoded_results = {m_tokenizer.decode(encoded_results.tokens), encoded_results.scores};
    auto decode_stop_time =  std::chrono::steady_clock::now();

    if (m_is_chat_conversation) {
        auto answer = decoded_results.texts[0];
        if (m_chat_generation_finish_status == GenerationStatus::CANCEL)
            // If chat generation process was cancelled by user, let's rollback to previous state of history
            m_history.pop_back();
        else
            m_history.push_back({{"role", "assistant"}, {"content", answer}});
    }

    // generate_durations
    decoded_results.perf_metrics = encoded_results.perf_metrics;
    auto& raw_counters = decoded_results.perf_metrics.raw_metrics;
    auto stop_time = std::chrono::steady_clock::now();
    raw_counters.generate_durations.clear();
    raw_counters.generate_durations.emplace_back(PerfMetrics::get_microsec(stop_time - start_time));
    raw_counters.tokenization_durations.emplace_back(PerfMetrics::get_microsec(encode_stop_time - start_time));
    raw_counters.detokenization_durations.emplace_back(PerfMetrics::get_microsec(decode_stop_time - decode_start_time));
    decoded_results.perf_metrics.m_evaluated = false;
    decoded_results.perf_metrics.evaluate_statistics(start_time);
    return decoded_results;
}

EncodedResults StatefulLLMPipeline::generate(
    const EncodedInputs& inputs,
    OptionalGenerationConfig generation_config,
    StreamerVariant streamer
) {
    auto start_time = std::chrono::steady_clock::now();
    ov::Tensor input_ids;
    ov::Tensor attention_mask;

    if (auto data = std::get_if<ov::Tensor>(&inputs)) {
        input_ids = *data;
        attention_mask = ov::genai::utils::init_attention_mask(input_ids);
    } else if (auto data = std::get_if<TokenizedInputs>(&inputs)) {
        input_ids = data->input_ids;
        attention_mask = data->attention_mask;
    }

    ov::Shape prompts_shape = input_ids.get_shape();
    const size_t batch_size = prompts_shape[0];
    OPENVINO_ASSERT(batch_size == 1u, "Currently only batch size=1 is supported");

    GenerationConfig config = (generation_config.has_value()) ? *generation_config : m_generation_config;
    // If stop_token_ids were not provided, take value from default m_generation_config
    if (config.stop_token_ids.empty())
        config.stop_token_ids = m_generation_config.stop_token_ids;
    // If eos_token_id was not provided, take value from default m_generation_config
    if (config.eos_token_id == -1)
        config.set_eos_token_id(m_generation_config.eos_token_id);
    config.validate();

    std::shared_ptr<StreamerBase> streamer_ptr = ov::genai::utils::create_streamer(streamer, m_tokenizer);

    OPENVINO_ASSERT(config.is_greedy_decoding() || config.is_multinomial(),
        "Currently only greedy and multinomial decoding are supported");

    OPENVINO_ASSERT(config.num_return_sequences == 1u,
        "Currently only \"num_return_sequences\" equal to 1 is supported!");

    ov::genai::EncodedResults results;
    auto& raw_perf_counters = results.perf_metrics.raw_metrics;
    // NB: Only batch=1 is supported now
    results.scores.resize(1u);
    results.scores[0] = 0u;
    results.tokens.resize(1u);

    // NB: Check if there is enough space in KV-cache to process input prompt
    auto prompt_len = input_ids.get_size();
    if (prompt_len > m_max_prompt_len) {
        OPENVINO_THROW("Static Stateful LLM pipeline may only process prompts up to "
                       + std::to_string(m_max_prompt_len) + " tokens. "
                       + "Set the \"MAX_PROMPT_LEN\" config option to increase the limit.");
    }

    ov::Tensor position_ids{ov::element::i64, input_ids.get_shape()};
    utils::initialize_position_ids(position_ids, attention_mask);

    m_request.set_tensor("input_ids", input_ids);
    m_request.set_tensor("attention_mask", attention_mask);
    m_request.set_tensor("position_ids", position_ids);

    m_request.infer();

    auto padded_logits = m_request.get_tensor("logits");
    // FIXME: Here is workaround to get only useful units of returned logits.
    //        If SliceOut is applied, there will be only 1 useful logit returned,
    //        nothing is required here.
    //        Other way, model will return logits of full context length,
    //        as internally prefill model is specially reshaped to return them.
    //        Fix should be done on OpenVINO side, so the model should return only
    //        useful logits of input prompt length, dropping the implementation-related
    //        padding ones.
    auto logits = padded_logits;
    auto padded_sequence_len = padded_logits.get_shape()[1];
    if (padded_sequence_len > 1) {
        // If SliceOut is not applied:
        logits = make_tensor_slice(padded_logits, 1, padded_sequence_len - prompt_len, padded_sequence_len);
    }
    int64_t output_sequence_len = logits.get_shape().at(1);

    auto sequence_group = std::make_shared<SequenceGroup>(
        0 /* request_id */, input_ids, config, 1 /* block_size */);
    sequence_group->schedule_tokens(sequence_group->get_prompt_len());
    sequence_group->set_output_seq_len(output_sequence_len);

    // NB: Controls what tokens are ready to be pushed into the streamer
    GenerationHandle handle = std::make_shared<GenerationHandleImpl>(
        sequence_group->get_generation_stream(), sequence_group->get_sampling_parameters());

    SamplerOutput sampler_output = m_sampler.sample({sequence_group}, logits);
    stream_generated_tokens(streamer_ptr, handle);

    int64_t input_ids_data = -1;
    int64_t position_ids_data = prompt_len - 1;
    std::vector<int64_t> attention_mask_data(prompt_len - 1, 1);
    m_request.set_tensor("input_ids", ov::Tensor(ov::element::i64, ov::Shape{1,1},  reinterpret_cast<void*>(&input_ids_data)));
    m_request.set_tensor("position_ids", ov::Tensor(ov::element::i64, ov::Shape{1,1}, reinterpret_cast<void*>(&position_ids_data)));

    while (sequence_group->is_running() && !sequence_group->handle_stopped() && !sequence_group->handle_cancelled()) {
        // KV Cache is full, no further generation is possible
        if (position_ids_data + 1 == m_kvcache_total) {
            sequence_group->set_out_of_memory();
            break;
        }

        sequence_group->schedule_tokens(1);
        const auto running_sequences = sequence_group->get_running_sequences();
        OPENVINO_ASSERT(running_sequences.size() == 1u);
        auto last_token = running_sequences.front()->get_generated_ids().back();

        // Just change the variables here, as pointers to them are already set to corresponding tensors
        input_ids_data = last_token;
        ++position_ids_data;
        // However, attention_mask changes its shape on each iteration, it should be re-set explicitly
        attention_mask_data.push_back(1);
        m_request.set_tensor("attention_mask", ov::Tensor(ov::element::i64, ov::Shape{1,attention_mask_data.size()}, (void*)&attention_mask_data[0]));

        m_request.infer();

        raw_perf_counters.m_new_token_times.emplace_back(std::chrono::steady_clock::now());
        raw_perf_counters.m_batch_sizes.emplace_back(batch_size);

        SamplerOutput sampler_output = m_sampler.sample({sequence_group}, m_request.get_tensor("logits"));
        stream_generated_tokens(streamer_ptr, handle);
    }

    if (streamer_ptr) { // push streamer's cache
        streamer_ptr->end();
    }

    OPENVINO_ASSERT(sequence_group->get_finished_sequences().size() == 1u);
    auto sequence = sequence_group->get_finished_sequences().front();
    results.tokens[0] = sequence->get_generated_ids();
    results.scores[0] = sequence->get_cumulative_log_prob();
    m_chat_generation_finish_status = sequence_group->get_generation_stream()->get_status();
    m_sampler.clear_request_info(sequence_group->get_request_id());

    auto stop_time = std::chrono::steady_clock::now();
    // If is called without tokenization then that stat will not be reported.
    auto& metrics = results.perf_metrics;
    metrics.num_input_tokens = batch_size * input_ids.get_shape().at(1);
    metrics.load_time = this->m_load_time_ms;
    metrics.raw_metrics.generate_durations.emplace_back(PerfMetrics::get_microsec(stop_time - start_time));
    metrics.evaluate_statistics(start_time);
    return results;
}

void StatefulLLMPipeline::start_chat(const std::string& system_message) {
    if (!system_message.empty()) {
        m_history.push_back({{"role", "system"}, {"content", system_message}});
    }
    m_is_chat_conversation = true;
};

void StatefulLLMPipeline::finish_chat() {
    m_is_chat_conversation = false;
    m_history.clear();
};


std::unique_ptr<LLMPipelineImplBase>
LLMPipelineFactory::create(const std::filesystem::path& models_path,
                                 const std::string& device,
                                 const ov::AnyMap& config) {
    return create(models_path, Tokenizer(models_path), device, config);
}

std::unique_ptr<LLMPipelineImplBase> LLMPipelineFactory::create(const std::shared_ptr<ov::Model>& model,
                                                                const ov::genai::Tokenizer& tokenizer,
                                                                const std::string& device,
                                                                const ov::AnyMap& properties,
                                                                const ov::genai::GenerationConfig& generation_config) {
    auto properties_copy = properties;
    const auto pipeline_mode = str_to_pipeline(pop_or_default(properties_copy, "STATIC_PIPELINE", std::string("STATEFUL")));
    if (pipeline_mode == StaticPipelineKind::STATEFUL) {
        return std::make_unique<ov::genai::static_llm::StatefulLLMPipeline>(model,
                                                                            tokenizer,
                                                                            device,
                                                                            properties_copy,
                                                                            generation_config);
    }
    OPENVINO_ASSERT(false);
}

std::unique_ptr<LLMPipelineImplBase>
LLMPipelineFactory::create(const std::filesystem::path& models_path,
                           const ov::genai::Tokenizer& tokenizer,
                           const std::string& device,
                           const ov::AnyMap& config) {
    auto properties = config;
    const auto pipeline_mode = str_to_pipeline(pop_or_default(properties, "STATIC_PIPELINE", std::string("STATEFUL")));
    if (pipeline_mode == StaticPipelineKind::STATEFUL) {
        return std::make_unique<ov::genai::static_llm::StatefulLLMPipeline>(models_path, tokenizer, device, properties);
    }
    OPENVINO_ASSERT(false);
}

}  // namespace static_llm
}  // namespace genai
}  // namespace ov
