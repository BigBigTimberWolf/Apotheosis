#ifndef SCR_FILE_PICKER_H
#define SCR_FILE_PICKER_H

#include <optional>
#include <string>
#include <vector>

namespace file_picker
{
struct FilterSpec
{
    std::wstring name;
    std::wstring pattern;
};

std::optional<std::string> open_file(const std::wstring& title,
                                     const std::vector<FilterSpec>& filters,
                                     void* parent_hwnd = nullptr);

std::optional<std::string> import_onnx_into_models_dir(const std::string& source_path);
}

#endif // SCR_FILE_PICKER_H
