#!/bin/bash

# --- 配置区 ---
# 输入目录
S57_DIR="/mnt/d/Maps/S-57_C1_base_24_WK43"
# 输出目录
OUTPUT_DIR="nobjnm_output" 
# 输出的CSV中的表名
OUTPUT_TABLE="nobjnm"
# 您想要从中筛选数据的图层列表
TARGET_LAYERS=("LNDARE" "DEPARE" "SEAARE" "HRBFAC" "BRIDGE")
# 您想要筛选的字段（硬编码为NOBJNM，但保留为变量以便未来扩展）
FILTER_FIELD="NOBJNM"
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

    # ### ADDED ###: 从文件名提取地图等级 (第3个字符)
    filename=$(basename "$s57_file")
    level=${filename:2:1} # Bash的子字符串提取功能，从第2个索引开始，取1个字符

    sql_parts=() 

    # 遍历所有我们感兴趣的图层
    for layer in "${TARGET_LAYERS[@]}"; do
        # 步骤 1: 检查图层是否存在
        if ogrinfo -q "$s57_file" "$layer" >/dev/null 2>&1; then
            
            # 步骤 2: 检查该图层是否包含我们需要的筛选字段 (NOBJNM)
            if ogrinfo -so "$s57_file" "$layer" | grep -q "$FILTER_FIELD:"; then
                echo "  - 发现图层: '$layer', 包含 '$FILTER_FIELD' 字段，将应用过滤器"
                # ### MODIFIED ###: 在SELECT语句中增加了 '$level' AS LEVEL
                sql_parts+=("SELECT '$level' AS LEVEL, '$layer' AS LAYERS, $FILTER_FIELD FROM $layer WHERE $FILTER_FIELD IS NOT NULL AND $FILTER_FIELD != ''")
            else
                # 如果字段不存在，则此图层不满足筛选条件，直接跳过
                echo "  - 发现图层: '$layer', 但不包含 '$FILTER_FIELD' 字段，跳过"
            fi
        fi
    done

    # 如果找到了至少一个满足条件的图层，则继续处理
    if [ ${#sql_parts[@]} -gt 0 ]; then
        
        # 使用循环正确拼接 UNION ALL
        SQL_QUERY=""
        for i in "${!sql_parts[@]}"; do
            if [ $i -gt 0 ]; then
                SQL_QUERY+=" UNION ALL "
            fi
            SQL_QUERY+="${sql_parts[$i]}"
        done
        
        echo "  - 正在导出为 CSV..."

        # 使用标志位来区分第一次写入和后续追加
        if $IS_FIRST_WRITE; then
            ogr2ogr \
                -f CSV \
                "$OUTPUT_DIR" \
                "$s57_file" \
                -sql "$SQL_QUERY" \
                -nln "$OUTPUT_TABLE" \
                -lco GEOMETRY=AS_WKT
            
            if [ $? -eq 0 ]; then
                IS_FIRST_WRITE=false
            fi
        else
            ogr2ogr \
                -f CSV \
                -append \
                "$OUTPUT_DIR" \
                "$s57_file" \
                -sql "$SQL_QUERY" \
                -nln "$OUTPUT_TABLE" \
                -lco GEOMETRY=AS_WKT
        fi
    else
        echo "  - 未发现任何包含 '$FILTER_FIELD' 的目标图层，跳过此文件。"
    fi
done

echo "所有文件处理完毕！"