// Copyright (C) 2023-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "openvino/genai/text2image/clip_text_model.hpp"

#include <fstream>

#include "openvino/runtime/core.hpp"

#include "utils.hpp"
#include "lora_helper.hpp"

namespace ov {
namespace genai {

CLIPTextModel::Config::Config(const std::string& config_path) {
    std::ifstream file(config_path);
    OPENVINO_ASSERT(file.is_open(), "Failed to open ", config_path);

    nlohmann::json data = nlohmann::json::parse(file);
    using utils::read_json_param;

    read_json_param(data, "max_position_embeddings", max_position_embeddings);
    read_json_param(data, "hidden_size", hidden_size);
}

CLIPTextModel::CLIPTextModel(const std::string root_dir) :
    m_clip_tokenizer(root_dir + "/../tokenizer"),
    m_config(root_dir + "/config.json") {
    m_model = ov::Core().read_model(root_dir + "/openvino_model.xml");
}

CLIPTextModel::CLIPTextModel(const std::string& root_dir,
                const std::string& device,
                const ov::AnyMap& properties) :
    CLIPTextModel(root_dir) {
    AdapterConfig adapters;
    if(auto filtered_properties = extract_adapters_from_properties(properties, &adapters)) {
        m_adapter_controller = AdapterController(m_model, adapters, "lora_te", device);
        compile(device, *filtered_properties);
    } else {
        compile(device, properties);
    }
}

CLIPTextModel::CLIPTextModel(const CLIPTextModel&) = default;

const CLIPTextModel::Config& CLIPTextModel::get_config() const {
    return m_config;
}

CLIPTextModel& CLIPTextModel::reshape(int batch_size) {
    OPENVINO_ASSERT(m_model, "Model has been already compiled. Cannot reshape already compiled model");

    ov::PartialShape input_shape = m_model->input(0).get_partial_shape();
    input_shape[0] = batch_size;
    input_shape[1] = m_config.max_position_embeddings;
    std::map<size_t, ov::PartialShape> idx_to_shape{{0, input_shape}};
    m_model->reshape(idx_to_shape);

    return *this;
}

CLIPTextModel& CLIPTextModel::compile(const std::string& device, const ov::AnyMap& properties) {
    OPENVINO_ASSERT(m_model, "Model has been already compiled. Cannot re-compile already compiled model");
    ov::CompiledModel compiled_model = ov::Core().compile_model(m_model, device, properties);
    m_request = compiled_model.create_infer_request();
    // release the original model
    m_model.reset();

    return *this;
}

void CLIPTextModel::set_adapters(const AdapterConfig& adapters) {
    m_adapter_controller.apply(m_request, adapters);
}

ov::Tensor CLIPTextModel::infer(const std::string& pos_prompt, const std::string& neg_prompt, bool do_classifier_free_guidance) {
    OPENVINO_ASSERT(m_request, "CLIP text encoder model must be compiled first. Cannot infer non-compiled model");

    const int32_t pad_token_id = m_clip_tokenizer.get_pad_token_id();
    const size_t text_embedding_batch_size = do_classifier_free_guidance ? 2 : 1;

    auto perform_tokenization = [&](const std::string& prompt, ov::Tensor input_ids) {
        std::fill_n(input_ids.data<int32_t>(), input_ids.get_size(), pad_token_id);

        ov::Tensor input_ids_token = m_clip_tokenizer.encode(prompt).input_ids;
        std::copy_n(input_ids_token.data<std::int64_t>(), input_ids_token.get_size(), input_ids.data<std::int32_t>());
    };

    ov::Tensor input_ids(ov::element::i32, {text_embedding_batch_size, m_config.max_position_embeddings});
    size_t current_batch_idx = 0;

    if (do_classifier_free_guidance) {
        perform_tokenization(neg_prompt,
                                ov::Tensor(input_ids, {current_batch_idx    , 0},
                                                    {current_batch_idx + 1, m_config.max_position_embeddings}));
        ++current_batch_idx;
    } else {
        // Negative prompt is ignored when --guidanceScale < 1.0
    }

    perform_tokenization(pos_prompt,
                            ov::Tensor(input_ids, {current_batch_idx    , 0},
                                                {current_batch_idx + 1, m_config.max_position_embeddings}));

    // text embeddings
    m_request.set_tensor("input_ids", input_ids);
    m_request.infer();

    return m_request.get_output_tensor(0);
}

} // namespace genai
} // namespace ov