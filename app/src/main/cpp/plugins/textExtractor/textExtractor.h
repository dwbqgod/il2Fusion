#ifndef IL2FUSION_PLUGINS_TEXT_EXTRACTOR_H
#define IL2FUSION_PLUGINS_TEXT_EXTRACTOR_H

#include <cstdint>
#include <string>
#include <vector>

namespace text_extractor {

// 设置进程名并异步启动文本拦截初始化（幂等）。
void Init(const std::string& process_name);

// 更新方法全名列表；若 libil2cpp 已就绪会立即重新安装 hook。
void UpdateTargets(const std::vector<std::string>& new_targets);

}  // namespace text_extractor

#endif  // IL2FUSION_PLUGINS_TEXT_EXTRACTOR_H
