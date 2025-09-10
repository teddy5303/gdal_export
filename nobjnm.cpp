#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <fstream>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/join.hpp>

#include "gdal_priv.h"
#include "gdal_utils.h"
#include "ogrsf_frmts.h"

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.h"

namespace po = boost::program_options;
namespace fs = boost::filesystem;

/**
 * @brief 使用 miniz 压缩单个文件到 ZIP 存档中
 *
 * @param source_filepath 要压缩的源文件路径
 * @param zip_filepath 目标 ZIP 文件的路径
 * @param filename_in_zip 存储在 ZIP 文件内部的文件名
 * @return true 如果压缩成功
 * @return false 如果压缩失败
 */
bool zip_single_file(const std::string& source_filepath, const std::string& zip_filepath, const std::string& filename_in_zip) {
    // 1. 将源文件完整读入内存
    std::ifstream file(source_filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "错误: 无法打开源文件 " << source_filepath << std::endl;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size);
    if (!file.read(buffer.data(), size)) {
        std::cerr << "错误: 无法读取源文件 " << source_filepath << std::endl;
        return false;
    }
    file.close();


    // 2. 调用 miniz 的核心函数来创建/追加到 ZIP 文件
    mz_bool status = mz_zip_add_mem_to_archive_file_in_place(
        zip_filepath.c_str(),          // 目标 ZIP 文件路径
        filename_in_zip.c_str(),       // 存档内的文件名
        buffer.data(),                 // 指向内存中文件数据的指针
        buffer.size(),                 // 文件数据的大小
        "File comment",                // 文件注释
        0,                             // 注释大小
        MZ_DEFAULT_COMPRESSION         // 使用默认压缩级别
    );

    if (!status) {
        std::cerr << "错误: miniz 无法将文件添加到 ZIP 存档。" << std::endl;
        return false;
    }

    std::cout << "成功将 " << source_filepath << " 压缩到 " << zip_filepath << std::endl;
    return true;
}

// 自定义 unique_ptr deleter 用于 GDALDataset
void gdal_dataset_deleter(GDALDataset* ds) {
    if (ds) {
        GDALClose(ds);
    }
}
using GdalDatasetPtr = std::unique_ptr<GDALDataset, decltype(&gdal_dataset_deleter)>;


int main(int argc, char* argv[]) {
    // --- 1. 使用 Boost::program_options 解析命令行参数 ---
    po::options_description desc("S57 Processor Options");
    desc.add_options()
        ("help,h", "显示帮助信息")
        ("input-dir,i", po::value<std::string>()->required(), "包含S57文件的输入目录")
        ("output-dir,o", po::value<std::string>()->required(), "输出CSV文件的目录")
        ("layers,l", po::value<std::vector<std::string>>()->multitoken()->default_value({"LNDARE", "DEPARE", "SEAARE", "HRBFAC", "BRIDGE"}, "LNDARE DEPARE..."), "要处理的图层列表")
        ("field,f", po::value<std::string>()->default_value("NOBJNM"), "要筛选的字段名")
        ("output-name,n", po::value<std::string>()->default_value("nobjnm"), "输出的CSV文件名 (不含后缀)");

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
    const std::vector<std::string> targetLayers = vm["layers"].as<std::vector<std::string>>();
    const std::string filterField = vm["field"].as<std::string>();
    const std::string outputName = vm["output-name"].as<std::string>();

    // --- 2. 初始化 GDAL ---
    GDALAllRegister();
    CPLSetConfigOption("OGR_WKT_PRECISION", "8");

    // CPLSetConfigOption("GDAL_DATA", "D:/vcpkg/installed/x64-windows/share/gdal");

    // --- 3. 准备输出目录 ---
    if (fs::exists(outputDir)) {
        std::cout << "已删除旧的输出目录: " << outputDir << std::endl;
        fs::remove_all(outputDir);
    }
    // 注意：我们不创建目录，让GDAL在第一次写入时创建

    bool isFirstWrite = true;

    // --- 4. 遍历输入目录中的所有 .000 文件 ---
    try {
        for (const auto& entry : fs::recursive_directory_iterator(inputDir)) {
            if (entry.path().extension() == ".000") {
                const std::string s57_file = entry.path().string();
                std::cout << "正在处理: " << s57_file << std::endl;

                // 从文件名提取地图等级
                std::string filename = entry.path().filename().string();
                char level = (filename.length() >= 3) ? filename[2] : '0';

                // 打开S57文件，强制使用S57驱动
                GdalDatasetPtr poDS(
                    static_cast<GDALDataset*>(GDALOpenEx(s57_file.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)),
                    &gdal_dataset_deleter
                );

                if (!poDS) {
                    std::cerr << "警告: 无法打开文件 " << s57_file << std::endl;
                    continue;
                }

                std::vector<std::string> sql_parts;
                for (const auto& layerName : targetLayers) {
                    OGRLayer* poLayer = poDS->GetLayerByName(layerName.c_str());
                    if (poLayer) { // 图层存在
                        OGRFeatureDefn* poDefn = poLayer->GetLayerDefn();
                        if (poDefn->GetFieldIndex(filterField.c_str()) != -1) { // 字段存在
                            std::cout << "  - 发现图层: '" << layerName << "', 包含 '" << filterField << "' 字段，将应用过滤器" << std::endl;

                            std::stringstream ss;
                            ss << "SELECT ST_MakeValid(ST_SimplifyPreserveTopology(geometry, 0.00025)) AS WKT, '" << level
                               << "' AS LEVEL, '" << layerName << "' AS LAYERS, "
                               << "\"" << filterField << "\" FROM \"" << layerName << "\" WHERE \"" << filterField
                               << "\" IS NOT NULL AND \"" << filterField << "\" != ''";

                            sql_parts.push_back(ss.str());
                        } else {
                            std::cout << "  - 发现图层: '" << layerName << "', 但不包含 '" << filterField << "' 字段，跳过" << std::endl;
                        }
                    }
                }

                if (!sql_parts.empty()) {
                    std::string sqlQuery = boost::algorithm::join(sql_parts, " UNION ALL ");
                    std::cout << "  - 正在导出为 CSV..." << std::endl;

                    // 使用GDALVectorTranslate API (ogr2ogr的C++等效函数)
                    GDALVectorTranslateOptions* psOptions = GDALVectorTranslateOptionsNew(nullptr, nullptr);

                    std::vector<const char*> opts;
                    opts.push_back("-f");
                    opts.push_back("CSV");

                    // MODIFIED #1: 添加 dialect 选项以启用 ST_MakeValid 等空间函数
                    opts.push_back("-dialect");
                    opts.push_back("SQLite");

                    if (!isFirstWrite) {
                        opts.push_back("-append");
                    }

                    opts.push_back("-sql");
                    opts.push_back(sqlQuery.c_str());

                    opts.push_back("-nln");
                    opts.push_back(outputName.c_str());

                    opts.push_back("-lco");
                    opts.push_back("GEOMETRY=AS_WKT");

                    // C-style 字符串数组必须以 NULL 结尾
                    opts.push_back(nullptr);

                    char** papszOptions = const_cast<char**>(opts.data());

                    // 注意：GDALVectorTranslateOptionsSetOptions的最后一个参数需要为NULL
                    psOptions = GDALVectorTranslateOptionsNew(papszOptions, nullptr);
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
                     std::cout << "  - 未发现任何包含 '" << filterField << "' 的目标图层，跳过此文件。" << std::endl;
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "文件系统错误: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "所有文件处理完毕！" << std::endl;

    // 这里可以添加调用 zip_single_file 的逻辑
    // 例如:
    // const std::string csv_path = (fs::path(outputDir) / (outputName + ".csv")).string();
    // const std::string zip_path = (fs::path(outputDir) / (outputName + ".zip")).string();
    // if (fs::exists(csv_path)) {
    //     zip_single_file(csv_path, zip_path, outputName + ".csv");
    // }

    return 0;
}