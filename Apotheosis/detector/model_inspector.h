#ifndef DETECTOR_MODEL_INSPECTOR_H
#define DETECTOR_MODEL_INSPECTOR_H

#include <cstdint>
#include <string>
#include <vector>

namespace detector
{

enum class ClassNamesSource
{
    None = 0,
    OnnxCustomMetadata,
    TrtEngineHeader,
    SidecarJson,
    SidecarNames,
    Fallback
};

struct ModelMetadata
{
    int class_count = 0;
    std::vector<std::string> class_names;
    ClassNamesSource source = ClassNamesSource::None;
    bool fixed_input_size = false;
    bool fixed_input_size_known = false;

    int input_width  = 0;
    int input_height = 0;
    std::vector<int64_t> output_shape;
};

enum class ModelOutputKind { Unknown, End2End, RawChannelsFirst, RawRowsFirst };

inline ModelOutputKind classify_model_output(const std::vector<int64_t>& shape)
{
    if (shape.size() != 3 || (shape[0] != 1 && shape[0] > 0))
        return ModelOutputKind::Unknown;
    if (shape[2] == 6 && shape[1] != 0)
        return ModelOutputKind::End2End;
    if (shape[1] >= 5 && shape[2] > shape[1])
        return ModelOutputKind::RawChannelsFirst;
    if (shape[2] >= 5 && shape[1] > shape[2])
        return ModelOutputKind::RawRowsFirst;
    return ModelOutputKind::Unknown;
}

ModelMetadata inspect_onnx_model(const std::string& model_path, bool verbose = false);

std::vector<std::string> parse_python_dict_names(const std::string& blob);
std::vector<std::string> parse_json_names(const std::string& blob);

std::string read_ultralytics_engine_header(const std::string& engine_path);

std::vector<std::string> read_sidecar_class_names(const std::string& model_path,
                                                  ClassNamesSource* source_out);

void pad_class_names(ModelMetadata& md, int expected_count);

}

#endif // DETECTOR_MODEL_INSPECTOR_H
