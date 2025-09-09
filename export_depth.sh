#!/bin/bash

# --- 配置区 ---
# 输入S57文件的目录
S57_DIR="/mnt/d/Maps/S-57_C1_base_24_WK43"
# 输出CSV文件的目录
OUTPUT_DIR="depth_output" 
# 输出的CSV文件名 (不含.csv后缀)
OUTPUT_FILENAME="depth"

# 定义一个关联数组，映射“水深图层”到它们的“水深字段名”
declare -A DEPTH_MAP=(
    [SOUNDG]="DEPTH"    # 特殊：由 -oo ADD_SOUNDG_DEPTH=ON 生成
    [DEPARE]="DRVAL1"
    [DRGARE]="DRVAL1"
    [DEPCNT]="VALDCO"
    [WRECKS]="VALSOU"
    [OBSTRN]="VALSOU"
    [UWTROC]="VALSOU"
)

# 将 LNDARE 添加到我们要处理的图层列表中
ALL_TARGET_LAYERS=("LNDARE" "${!DEPTH_MAP[@]}")
# ----------------

# 如果目录已存在，先删除它
if [ -d "$OUTPUT_DIR" ]; then
    rm -rf "$OUTPUT_DIR"
    echo "已删除旧的输出目录: $OUTPUT_DIR"
fi

# 标志位，用于可靠地处理CSV文件的首次创建和后续追加
IS_FIRST_WRITE=true

find "$S57_DIR" -name "*.000" -print0 | while IFS= read -r -d $'\0' s57_file; do
    echo "正在处理: $s57_file"

    sql_parts=() 

    # 遍历所有目标图层
    for layer in "${ALL_TARGET_LAYERS[@]}"; do
        if ogrinfo -q "$s57_file" "$layer" >/dev/null 2>&1; then
            if [ "$layer" == "LNDARE" ]; then
                echo "  - 发现陆地区域: '$layer', 设置深度为 -1"
                sql_parts+=("SELECT '$layer' AS LAYERS, -1 AS DEPTH FROM \"$layer\"")
            else
                depth_field=${DEPTH_MAP[$layer]}
                echo "  - 发现深度图层: '$layer', 使用字段 '$depth_field', 并过滤空值"
                # 新增: 在WHERE子句中过滤掉深度为空的要素
                sql_parts+=("SELECT '$layer' AS LAYERS, \"$depth_field\" AS DEPTH FROM \"$layer\" WHERE \"$depth_field\" IS NOT NULL AND \"$depth_field\" != ''")
            fi
        fi
    done

    if [ ${#sql_parts[@]} -gt 0 ]; then
        
        SQL_QUERY=""
        for i in "${!sql_parts[@]}"; do
            if [ $i -gt 0 ]; then
                SQL_QUERY+=" UNION ALL "
            fi
            SQL_QUERY+="${sql_parts[$i]}"
        done
        
        echo "  - 正在导出为 2D CSV..."

        if $IS_FIRST_WRITE; then
            # 首次写入
            ogr2ogr \
                -f CSV \
                "$OUTPUT_DIR" \
                "$s57_file" \
                -sql "$SQL_QUERY" \
                -nln "$OUTPUT_FILENAME" \
                -lco GEOMETRY=AS_WKT \
                -dim 2 \
                -oo SPLIT_MULTIPOINT=ON \
                -oo ADD_SOUNDG_DEPTH=ON
            
            if [ $? -eq 0 ]; then
                IS_FIRST_WRITE=false
            fi
        else
            # 后续写入
            ogr2ogr \
                -f CSV \
                -append \
                "$OUTPUT_DIR" \
                "$s57_file" \
                -sql "$SQL_QUERY" \
                -nln "$OUTPUT_FILENAME" \
                -lco GEOMETRY=AS_WKT \
                -dim 2 \
                -oo SPLIT_MULTIPOINT=ON \
                -oo ADD_SOUNDG_DEPTH=ON
        fi
    else
        echo "  - 未发现任何有效目标图层，跳过此文件。"
    fi
done

echo "所有文件处理完毕！最终的CSV数据已生成在目录 '$OUTPUT_DIR' 中。"