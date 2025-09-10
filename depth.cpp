#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <map>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/join.hpp>

#include "gdal_priv.h"
#include "gdal_utils.h"
#include "ogrsf_frmts.h"

// miniz 相关的函数声明，如果不需要可以移除
#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace po = boost::program_options;
namespace fs = boost::filesystem;

// 自定义 unique_ptr deleter 用于 GDALDataset
void gdal_dataset_deleter(GDALDataset* ds) {
    if (ds) {
        GDALClose(ds);
    }
}
using GdalDatasetPtr = std::unique_ptr<GDALDataset, decltype(&gdal_dataset_deleter)>;


int main(int argc, char* argv[]) {
    // --- 1. 使用 Boost::program_options 解析命令行参数 ---
    po::options_description desc("S57 Depth Processor Options");
    desc.add_options()
        ("help,h", "显示帮助信息")
        ("input-dir,i", po::value<std::string>()->required(), "包含S57文件的输入目录")
        ("output-dir,o", po::value<std::string>()->required(), "输出CSV文件的目录")
        ("output-name,n", po::value<std::string>()->default_value("depth"), "输出的CSV文件名 (不含后缀)");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << desc << std::endl;
            return 0;
        }

        po::notify(vm); // 检查 "required" 选项是否存在
    } catch (const po::error& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        std::cerr << desc << std::endl;
        return 1;
    }

    const std::string inputDir = vm["input-dir"].as<std::string>();
    const std::string outputDir = vm["output-dir"].as<std::string>();
    const std::string outputName = vm["output-name"].as<std::string>();

    // --- 2. 初始化 GDAL ---
    GDALAllRegister();
    CPLSetConfigOption("OGR_WKT_PRECISION", "8");

    // --- 3. 定义图层到深度字段的映射关系 (脚本逻辑的C++实现) ---
    const std::map<std::string, std::string> depth_map = {
        {"SOUNDG", "DEPTH"}, // 特殊：由 -oo ADD_SOUNDG_DEPTH=ON 生成
        {"DEPARE", "DRVAL1"},
        {"DRGARE", "DRVAL1"},
        {"DEPCNT", "VALDCO"},
        {"WRECKS", "VALSOU"},
        {"OBSTRN", "VALSOU"},
        {"UWTROC", "VALSOU"}
    };

    std::vector<std::string> all_target_layers;
    all_target_layers.push_back("LNDARE");
    for(const auto& pair : depth_map) {
        all_target_layers.push_back(pair.first);
    }

    // --- 4. 准备输出目录 ---
    if (fs::exists(outputDir)) {
        std::cout << "已删除旧的输出目录: " << outputDir << std::endl;
        fs::remove_all(outputDir);
    }

    bool isFirstWrite = true;

    // --- 5. 遍历输入目录中的所有 .000 文件 ---
    try {
        for (const auto& entry : fs::recursive_directory_iterator(inputDir)) {
            if (entry.path().extension() == ".000") {
                const std::string s57_file = entry.path().string();
                std::cout << "正在处理: " << s57_file << std::endl;

                const char* papszOpenOptions[] = {
                    "SPLIT_MULTIPOINT=ON",
                    "ADD_SOUNDG_DEPTH=ON",
                    nullptr // 数组必须以NULL结尾
                };

                // 在 GDALOpenEx 调用中传入 papszOpenOptions
                GdalDatasetPtr poDS(
                    static_cast<GDALDataset*>(GDALOpenEx(s57_file.c_str(), GDAL_OF_VECTOR, nullptr, const_cast<char**>(papszOpenOptions), nullptr)),
                    &gdal_dataset_deleter
                );

                if (!poDS) {
                    std::cerr << "警告: 无法打开文件 " << s57_file << std::endl;
                    continue;
                }

                std::vector<std::string> sql_parts;
                for (const auto& layerName : all_target_layers) {
                    OGRLayer* poLayer = poDS->GetLayerByName(layerName.c_str());
                    if (poLayer) { // 图层存在
                        if (layerName == "LNDARE") {
                            std::cout << "  - 发现陆地区域: '" << layerName << "', 设置深度为 -1" << std::endl;
                            sql_parts.push_back("SELECT ST_MakeValid(ST_SimplifyPreserveTopology(geometry, 0.00025)) AS WKT, '" +
                                                layerName + "' AS LAYERS, CAST(-1 AS REAL) AS DEPTH FROM \"" +
                                                layerName + "\"");
                        } else {
                            auto it = depth_map.find(layerName);
                            if (it != depth_map.end()) {
                                const std::string& depthField = it->second;
                                std::cout << "  - 发现深度图层: '" << layerName << "', 使用字段 '" << depthField << "'" << std::endl;

                                std::stringstream ss;
                                ss << "SELECT ST_MakeValid(ST_SimplifyPreserveTopology(geometry, 0.00025)) AS WKT, '" << layerName
                                   << "' AS LAYERS, CAST(\"" << depthField << "\" AS REAL) AS DEPTH FROM \""
                                   << layerName << "\" WHERE \"" << depthField << "\" IS NOT NULL AND \"" << depthField
                                   << "\" != ''";
                                sql_parts.push_back(ss.str());
                            }
                        }
                    }
                }

                if (!sql_parts.empty()) {
                    std::string sqlQuery = boost::algorithm::join(sql_parts, " UNION ALL ");
                    std::cout << "  - 正在导出为 2D CSV..." << std::endl;

                    std::vector<const char*> opts;
                    opts.push_back("-f");
                    opts.push_back("CSV");

                    // 为 ST_MakeValid 启用 SQLite 方言
                    opts.push_back("-dialect");
                    opts.push_back("SQLite");

                    if (!isFirstWrite) {
                        opts.push_back("-append");
                    }

                    opts.push_back("-sql");
                    opts.push_back(sqlQuery.c_str());

                    opts.push_back("-nln");
                    opts.push_back(outputName.c_str());

                    // Layer Creation Options
                    opts.push_back("-lco");
                    opts.push_back("GEOMETRY=AS_WKT");

                    // General Options from script
                    opts.push_back("-dim");
                    opts.push_back("2");

                    opts.push_back(nullptr); // 数组结尾

                    char** papszOptions = const_cast<char**>(opts.data());
                    GDALVectorTranslateOptions* psOptions = GDALVectorTranslateOptionsNew(papszOptions, nullptr);

                    if (!psOptions) {
                        std::cerr << "错误：创建 GDALVectorTranslateOptions 失败。" << std::endl;
                        continue;
                    }

                    int nError = 0;
                    GDALDatasetH hSrcDataset = poDS.get();
                    GDALDatasetH pahSrcDS[] = { hSrcDataset };
                    GDALDatasetH hDstDS = GDALVectorTranslate(outputDir.c_str(), nullptr, 1, pahSrcDS, psOptions, &nError);

                    if (hDstDS) {
                        GDALClose(hDstDS);
                        if (isFirstWrite) isFirstWrite = false;
                    } else {
                        std::cerr << "错误：处理文件 " << s57_file << " 时发生错误。" << std::endl;
                    }

                    GDALVectorTranslateOptionsFree(psOptions);
                } else {
                     std::cout << "  - 未发现任何有效目标图层，跳过此文件。" << std::endl;
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "文件系统错误: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "所有文件处理完毕！最终的CSV数据已生成在目录 '" << outputDir << "' 中。" << std::endl;
    return 0;
}